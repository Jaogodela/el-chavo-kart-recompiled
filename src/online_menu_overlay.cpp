#include "online_menu_overlay.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "language_setting.h"
#include "launcher.h"
#include "modal_dialog.h"
#include "online_menu.h"
#include "online_runtime.h"
#include "overlay_rules.h"
#include "settings.h"

REXCVAR_DECLARE(std::string, eck_language);

namespace eck {
namespace {

constexpr float kWidth = 560.0f;
constexpr double kInvitePollSeconds = 0.25;
constexpr ImVec4 kToastColor(0.98f, 0.8f, 0.16f, 1.0f);
constexpr ImVec4 kInfoColor(0.85f, 0.85f, 0.85f, 1.0f);
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoTitleBar;

std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_menu_seen{false};

TextPackage GamePackage() { return PackageForLanguage(REXCVAR_GET(eck_language)); }

online_start::Verdict StartRace(const online_start::Plan& plan) {
  const online_start::Decision decision = online_start::Decide(plan, online_start::DebugEnabled());
  for (const std::string& line : decision.lines) REXLOG_INFO("{}", line);  // DEBUG_SAULO
  return decision.verdict;
}

bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat) {
  for (const ImGuiKey key : keys) {
    if (ImGui::IsKeyPressed(key, repeat)) return true;
  }
  return false;
}

class OnlineMenuOverlay final : public ModalDialog {
 public:
  explicit OnlineMenuOverlay(rex::ui::ImGuiDrawer* drawer)
      : ModalDialog(drawer), api_(Online()), menu_(api_, GamePackage(), StartRace) {
    REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] opened page={}", int(menu_.page()));
  }

  ~OnlineMenuOverlay() override { REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] closed"); }

 protected:
  bool InterceptBack() override {
    menu_.Back();
    Finish();
    return true;
  }

  void DrawContent(ImGuiIO& io) override {
    PumpOnlineService();
    const double now = ImGui::GetTime();
    menu_.Update(now);
    Input();
    if (Finish()) return;

    const ImVec2 viewport = io.DisplaySize;
    ImGui::PushFont(LauncherRegularFont());
    ImGui::SetNextWindowPos(ImVec2(viewport.x * 0.5f, viewport.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(FitWindowExtent(viewport.x, 320.0f, kWidth), 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!focused_) {
      focused_ = true;
      ImGui::SetNextWindowFocus();
    }
    if (ImGui::Begin("##eck_online_menu", nullptr, kWindowFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kToastColor, "%s", menu_.Title().c_str());
      ImGui::Separator();
      Rows();
      if (const std::string toast = menu_.Toast(now); !toast.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(kToastColor, "%s", toast.c_str());
      }
      ImGui::Spacing();
      ImGui::TextDisabled("A  OK    B  %s", menu_.Tx("Back"));
    }
    ImGui::End();
    ImGui::PopFont();
  }

 private:
  void Input() {
    if (Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}, true)) menu_.Move(-1);
    if (Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}, true)) menu_.Move(1);
    if (Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft}, true)) menu_.Adjust(-1);
    if (Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight}, true)) menu_.Adjust(1);
    if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown}, false)) menu_.Activate();
  }

  void Rows() {
    const std::vector<menu::Row> rows = menu_.Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const menu::Row& row = rows[i];
      if (row.act == menu::Act::kNone) {
        ImGui::TextColored(kInfoColor, "%s", row.text.c_str());
        continue;
      }
      const std::string label = row.act == menu::Act::kPlayers ? "<  " + row.text + "  >" : row.text;
      ImGui::PushID(int(i));
      ImGui::BeginDisabled(!row.enabled);
      if (ImGui::Selectable(label.c_str(), int(i) == menu_.cursor())) {
        menu_.SetCursor(int(i));
        menu_.Activate();
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }

  bool Finish() {
    switch (menu_.TakeOutcome()) {
      case menu::Outcome::kLocal:
        g_menu_seen.store(false);
        REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] local chosen: original Free Race flow continues");
        RequestClose();
        return true;
      case menu::Outcome::kClose:
        RequestClose();
        return true;
      case menu::Outcome::kNone: break;
    }
    return false;
  }

  menu::ServiceApi api_;
  menu::OnlineMenu menu_;
  bool focused_ = false;
};

// Always alive: opens the menu when asked, and for an invite that arrives while the main menu is up.
class OnlineMenuDriver final : public rex::ui::ImGuiDialog {
 public:
  OnlineMenuDriver(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer), drawer_(drawer), defer_(std::move(defer)) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    const bool enabled = REXCVAR_GET(eck_online_enabled);
    if (!enabled) {
      g_open_requested.store(false);
      return;
    }
    if (ModalDialog::AnyOpen() || opening_) return;
    bool open = g_open_requested.exchange(false);
    const double now = ImGui::GetTime();
    if (!open && g_menu_seen.load() && now - last_poll_ >= kInvitePollSeconds) {
      last_poll_ = now;
      open = Online().snapshot().invites.size() > 0 && !Online().snapshot().room;
      if (open) REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] invite received at the main menu: opening");
    }
    if (!open) return;
    opening_ = true;
    defer_([this] {
      opening_ = false;
      if (!ModalDialog::AnyOpen()) new OnlineMenuOverlay(drawer_);  // retained by the drawer until it closes
    });
  }

 private:
  rex::ui::ImGuiDrawer* drawer_;
  std::function<void(std::function<void()>)> defer_;
  double last_poll_ = 0;
  bool opening_ = false;
};

}  // namespace

void InstallOnlineMenu(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer) {
  new OnlineMenuDriver(drawer, std::move(defer));  // retained by the drawer for the session
}

void RequestOnlineMenu() {
  if (!REXCVAR_GET(eck_online_enabled)) {
    REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] free race confirmed with Online play off: original flow only");
    return;
  }
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-MENU] free race confirmed: Local/Online choice requested");
  g_open_requested.store(true);
}

void NoteMainMenuShown() { g_menu_seen.store(true); }

}  // namespace eck
