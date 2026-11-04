#pragma once

#include <cstdint>
#include <functional>

#include "window_sizes.h"

inline constexpr char kWindowTitle[] = "El Chavo Kart Recompiled";

// The latest one-second sample of the game's swaps; all zero until the first one is measured.
struct GuestFrameStats {
  double fps = 0;
  double frame_time_ms = 0;
  uint64_t frame_count = 0;
};

namespace rex::ui {
class ImGuiDrawer;
class Window;
class WindowedAppContext;
}  // namespace rex::ui

// `ui_wants_mouse` reports whether ImGui currently captures the mouse, so
// runtime menus force the cursor visible like the project's own overlays.
void AttachWindowHost(rex::ui::Window* window, rex::ui::WindowedAppContext& app_context,
                      std::function<bool()> ui_wants_mouse);
void DetachWindowHost();

// The current native client area in physical pixels: the extent the presenter draws into now, not the
// last size event cached by the window.
eck::WindowSize CurrentDrawableSize(rex::ui::Window* window);

// Called by ModalDialog while an in-game overlay is open.
void SetCursorOverlayOpen(bool open);

// Whether this process owns local physical input: its window has the keyboard focus. Callable from any thread.
bool LocalInputAccepted();

// Follows the window's focus from the moment it opens, so the launcher and the game share one source.
void OnWindowFocus(bool focused, rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);

// The system sends mouse movement and the wheel to the window under the pointer, focused or not.
void AttachMouseFocusGate(rex::ui::Window* window);

// Keeps the cursor in step with ImGui overlays that do not use ModalDialog.
void CreateCursorSync(rex::ui::ImGuiDrawer* drawer);
GuestFrameStats GetGuestFrameStats();
