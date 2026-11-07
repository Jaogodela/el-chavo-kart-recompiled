#include "window_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include "cursor_input.h"
#include "debug_telemetry.h"
#include "frame_limiter.h"
#include "input_activity.h"
#include "input_focus.h"
#include "settings.h"
#include "window_title.h"

// The SDK's VdSwap body, exported by rexruntime. The SDK has no header for it.
namespace rex::kernel::xboxkrnl {

void VdSwap_entry(mapped_void buffer_ptr, mapped_void fetch_ptr, mapped_void unk2, mapped_void unk3,
                  mapped_void unk4, mapped_u32 frontbuffer_ptr, mapped_u32 texture_format_ptr,
                  mapped_u32 color_space_ptr, mapped_u32 width, mapped_u32 height);

}  // namespace rex::kernel::xboxkrnl

namespace {

constexpr double kSampleSeconds = 1.0;
// Above the app's own input listener (z-order 0), so F11 never reaches the game.
constexpr size_t kWindowHostZOrder = 1000;
// Above the ImGui drawer (64) and the listeners that act on what reaches it.
constexpr size_t kMouseFocusGateZOrder = 2000;
constexpr char kInstanceEnv[] = "ECK_INSTANCE";

using Clock = std::chrono::steady_clock;

eck::InputFocus g_input_focus;
std::atomic<bool> g_active{false};
rex::ui::Window* g_window = nullptr;
rex::ui::WindowedAppContext* g_app_context = nullptr;
std::function<bool()> g_ui_wants_mouse;
// UI thread only.
eck::CursorHider g_cursor;
// Guarded by the input system's callback lock.
std::unordered_map<uint64_t, eck::ControllerActivityFilter> g_controller_filters;
std::optional<double> g_title_fps;
std::string g_shown_title;

std::mutex g_sample_mutex;
std::optional<Clock::time_point> g_sample_start;
uint64_t g_sample_frames = 0;
uint64_t g_total_frames = 0;
GuestFrameStats g_stats;

std::optional<std::string> TitleResolution() {
  if (!REXCVAR_GET(eck_show_render_resolution)) {
    return std::nullopt;
  }
  const int scale = std::max(1, std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()));
  return eck::RenderResolutionText(scale);
}

// Composes all title parts together, so one update never drops another.
void RefreshTitle() {
  const std::optional<double> fps = REXCVAR_GET(eck_show_fps) ? g_title_fps : std::nullopt;
  std::string title = eck::WindowTitleText(TitleResolution(), fps);
  if (title != g_shown_title) {
    g_shown_title = title;
    g_window->SetTitle(title);
  }
}

void ShowFps(double fps) {
  if (g_active.load(std::memory_order_relaxed)) {
    g_title_fps = fps;
    RefreshTitle();
  }
}

void CountGuestFrame() {
  if (!g_active.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(g_sample_mutex);
  const auto now = Clock::now();
  if (!g_sample_start) {
    g_sample_start = now;
    return;
  }
  ++g_sample_frames;
  ++g_total_frames;
  const double elapsed = std::chrono::duration<double>(now - *g_sample_start).count();
  if (elapsed < kSampleSeconds) {
    return;
  }
  const double fps = g_sample_frames / elapsed;
  g_stats = {fps, elapsed * 1000.0 / g_sample_frames, g_total_frames};
  g_sample_start = now;
  g_sample_frames = 0;
  if (g_app_context) {
    g_app_context->CallInUIThread([fps] { ShowFps(fps); });
  }
}

bool UiWantsMouse() { return g_ui_wants_mouse && g_ui_wants_mouse(); }

void UpdateCursor() {
  if (!g_window || !g_active.load(std::memory_order_relaxed)) {
    return;
  }
  const bool visible = g_cursor.visible() || UiWantsMouse();
  const auto wanted = visible ? rex::ui::Window::CursorVisibility::kVisible
                              : rex::ui::Window::CursorVisibility::kHidden;
  if (g_window->GetCursorVisibility() != wanted) {
    g_window->SetCursorVisibility(wanted);
  }
}

void RestoreCursor() {
  if (g_window && g_active.load(std::memory_order_relaxed)) {
    g_window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
  }
}

void OnControllerActivity(rex::input::DeviceId device,
                          const rex::input::X_INPUT_GAMEPAD& gamepad) {
  if (!g_active.load(std::memory_order_relaxed)) {
    return;
  }
  const uint64_t id = static_cast<uint64_t>(device);
  if (!g_controller_filters[id].Update(gamepad)) {
    return;
  }
  if (g_app_context) {
    g_app_context->CallInUIThread([] {
      if (!g_active.load(std::memory_order_relaxed)) {
        return;
      }
      g_cursor.ControllerActivity();
      UpdateCursor();
    });
  }
}

class WindowHost final : public rex::ui::WindowListener, public rex::ui::WindowInputListener {
 public:
  void OnClosing(rex::ui::UIEvent&) override {
    rex::input::SetInputActivityCallback(nullptr);
    RestoreCursor();
    g_active.store(false, std::memory_order_relaxed);
  }

  void OnGotFocus(rex::ui::UISetupEvent&) override {
    g_cursor.SetFocused(true);
    UpdateCursor();
  }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    g_cursor.SetFocused(false);
    UpdateCursor();
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override { NoteMouse(e); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { NoteMouse(e); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override { NoteMouse(e); }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (e.target() != g_window || !g_window->HasFocus()) {
      return;
    }
    if (e.virtual_key() == rex::ui::VirtualKey::kF11) {
      e.set_handled(true);
      if (e.prev_state()) {
        return;
      }
      // SetFullscreen(true) is SDL's default fullscreen mode, which is borderless desktop.
      const bool fullscreen = !g_window->IsFullscreen();
      g_window->SetFullscreen(fullscreen);
      REXLOG_INFO("Window mode: {}", fullscreen ? "borderless fullscreen" : "windowed");
      UpdateCursor();
      return;
    }
    if (e.prev_state()) {
      // ReXGlue's binds fire on every repeat, which would strobe a toggle while the key is held.
      if (e.virtual_key() == rex::ui::VirtualKey::kF2 || e.virtual_key() == rex::ui::VirtualKey::kF3 ||
          e.virtual_key() == rex::ui::VirtualKey::kF7) {
        e.set_handled(true);
      }
      return;
    }
    if (g_cursor.overlay_open()) {
      return;
    }
    g_cursor.KeyboardActivity();
    UpdateCursor();
  }

 private:
  static void NoteMouse(rex::ui::MouseEvent& e) {
    if (e.target() != g_window || !g_window->HasFocus()) {
      return;
    }
    if (!g_cursor.overlay_open() && !UiWantsMouse()) {
      g_cursor.MouseActivity();
    }
    UpdateCursor();
  }
};

WindowHost window_host;

class MouseFocusGate final : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override { Gate(e, eck::MouseAction::kMove); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Gate(e, eck::MouseAction::kDown); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { Gate(e, eck::MouseAction::kUp); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override { Gate(e, eck::MouseAction::kWheel); }

 private:
  static void Gate(rex::ui::MouseEvent& e, eck::MouseAction action) {
    if (!eck::MouseReachesUi(g_input_focus.accepted(), action)) {
      e.set_handled(true);
    }
  }
};

MouseFocusGate mouse_focus_gate;

// Through the drawer's own release, which also drops its mouse capture; the position leaves the window, so nothing
// stays hovered.
void ReleaseImGuiMouse(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  rex::ui::WindowInputListener& listener = *drawer;
  for (const auto button : {rex::ui::MouseEvent::Button::kLeft, rex::ui::MouseEvent::Button::kRight}) {
    rex::ui::MouseEvent release(window, button, -1, -1);
    listener.OnMouseUp(release);
  }
}

// Runtime overlays are not ModalDialog, so ImGui's mouse capture is checked each frame.
class CursorSync final : public rex::ui::ImGuiDialog {
 public:
  explicit CursorSync(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override { UpdateCursor(); }
};

}  // namespace

void AttachWindowHost(rex::ui::Window* window, rex::ui::WindowedAppContext& app_context,
                      std::function<bool()> ui_wants_mouse) {
  g_window = window;
  g_app_context = &app_context;
  g_ui_wants_mouse = std::move(ui_wants_mouse);
  g_cursor = eck::CursorHider{};
  g_cursor.SetFocused(window->HasFocus());
  g_controller_filters.clear();
  RefreshTitle();
  g_active.store(true, std::memory_order_relaxed);
  window->AddListener(&window_host);
  window->AddInputListener(&window_host, kWindowHostZOrder);
  rex::input::SetInputActivityCallback(OnControllerActivity);
  UpdateCursor();
}

void SetCursorOverlayOpen(bool open) {
  if (!g_active.load(std::memory_order_relaxed)) {
    return;
  }
  g_cursor.SetOverlayOpen(open);
  UpdateCursor();
}

bool LocalInputAccepted() { return g_input_focus.accepted(); }

void OnWindowFocus(bool focused, rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  if (!g_input_focus.Update(focused)) {
    return;
  }
  if (!focused && drawer) {
    ReleaseImGuiMouse(window, drawer);
  }
  const char* instance = std::getenv(kInstanceEnv);
  REXLOG_INFO("{}", eck::InputFocusLogLine(focused, instance ? instance : "",
                                           reinterpret_cast<uintptr_t>(window->GetNativeWindowHandle())));
}

void AttachMouseFocusGate(rex::ui::Window* window) {
  window->AddInputListener(&mouse_focus_gate, kMouseFocusGateZOrder);
}

eck::WindowSize CurrentDrawableSize(rex::ui::Window* window) {
  if (window == nullptr) {
    return {};
  }
#if defined(_WIN32)
  if (const HWND handle = static_cast<HWND>(window->GetNativeWindowHandle()); handle != nullptr) {
    RECT rect{};
    if (GetClientRect(handle, &rect)) {
      const int width = rect.right - rect.left;
      const int height = rect.bottom - rect.top;
      if (width > 0 && height > 0) {
        return {width, height};
      }
    }
  }
#endif
  return {int(window->GetActualPhysicalWidth()), int(window->GetActualPhysicalHeight())};
}

void CreateCursorSync(rex::ui::ImGuiDrawer* drawer) {
  new CursorSync(drawer);  // retained by the drawer for the session
}

// The guest can end without the window receiving OnClosing.
void DetachWindowHost() {
  rex::input::SetInputActivityCallback(nullptr);
  RestoreCursor();
  g_active.store(false, std::memory_order_relaxed);
  g_controller_filters.clear();
  g_ui_wants_mouse = nullptr;
  {
    std::lock_guard lock(g_sample_mutex);
    g_app_context = nullptr;
  }
  g_window = nullptr;
}

GuestFrameStats GetGuestFrameStats() {
  std::lock_guard lock(g_sample_mutex);
  return g_stats;
}

// Named like the SDK's export, so REX_EXTERN would call this hook again: the original is called by its entry point.
REX_HOOK_RAW(__imp__VdSwap) {
  eck::PaceGuestFrame();
  const auto swap_begin = Clock::now();
  rex::ppc::HostToGuestFunction<rex::kernel::xboxkrnl::VdSwap_entry>(ctx, base);
  eck::debug::OnSwap(std::chrono::duration<double, std::milli>(Clock::now() - swap_begin).count());
  CountGuestFrame();
}
