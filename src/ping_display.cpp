#include "ping_display.h"

#include <cfloat>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

#include "online_runtime.h"
#include "settings.h"

namespace eck {
namespace {

constexpr ImU32 kPanelColor = IM_COL32(14, 12, 4, 190);
constexpr ImU32 kBorderColor = IM_COL32(250, 204, 41, 255);
constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 255);

class PingOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PingOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    PumpOnlineService();
    if (!REXCVAR_GET(eck_online_enabled)) return;
    const online::Snapshot state = Online().snapshot();
    if (!PingVisible(true, state.room.has_value())) return;
    const std::string text = PingText(MeasuredRtts(state.peers));
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    const ImVec2 pad(size * 0.6f, size * 0.3f);
    // Below the frame counter's slot in the top right corner.
    const float top = size * 2 + text_size.y + pad.y * 2;
    const ImVec2 p1(io.DisplaySize.x - size, top + text_size.y + pad.y * 2);
    const ImVec2 p0(p1.x - text_size.x - pad.x * 2, top);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(p0, p1, kPanelColor, size * 0.5f);
    draw->AddRect(p0, p1, kBorderColor, size * 0.5f, 0, 1.0f);
    draw->AddText(font, size, ImVec2(p0.x + pad.x, p0.y + pad.y), kTextColor, text.c_str());
  }
};

}  // namespace

void CreatePingDisplay(rex::ui::ImGuiDrawer* drawer) {
  new PingOverlay(drawer);  // retained by the drawer for the session
}

}  // namespace eck
