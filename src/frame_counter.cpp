#include "frame_counter.h"

#include <cfloat>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "settings.h"

namespace eck {
namespace {

constexpr ImU32 kPanelColor = IM_COL32(14, 12, 4, 190);
constexpr ImU32 kBorderColor = IM_COL32(250, 204, 41, 255);
constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 255);

class FrameCounterOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FrameCounterOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const GuestFrameStats stats = GetGuestFrameStats();
    if (!FrameCounterVisible(REXCVAR_GET(eck_show_frame_counter), stats)) {
      return;
    }
    const std::string text = FrameCounterText(stats);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const ImVec2 pad(size * 0.6f, size * 0.3f);
    const ImVec2 p1(io.DisplaySize.x - size, size + text_size.y + pad.y * 2);
    const ImVec2 p0(p1.x - text_size.x - pad.x * 2, size);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(p0, p1, kPanelColor, size * 0.5f);
    draw->AddRect(p0, p1, kBorderColor, size * 0.5f, 0, 1.0f);
    draw->AddText(font, size, ImVec2(p0.x + pad.x, p0.y + pad.y), kTextColor, text.c_str());
  }
};

}  // namespace

void CreateFrameCounter(rex::ui::ImGuiDrawer* drawer) {
  new FrameCounterOverlay(drawer);  // retained by the drawer for the session
  rex::ui::RegisterBind("bind_eck_frame_counter", "F3", "Toggle frame counter", [] {
    rex::cvar::SetFlagByName("eck_show_frame_counter", ToggledFrameCounterFlag(REXCVAR_GET(eck_show_frame_counter)));
  });
}

}  // namespace eck
