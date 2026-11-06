#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "cursor_input.h"
#include "online_test_support.h"

using eck::ControllerActivityFilter;
using eck::CursorHider;
using online_test::Check;
using rex::input::X_INPUT_GAMEPAD;

namespace {

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

void TestLastDeviceDecides() {
  CursorHider cursor;
  Check(cursor.visible(), "the cursor starts visible");
  Check(cursor.ControllerActivity() == std::optional<bool>(false), "controller activity hides the cursor");
  Check(cursor.MouseActivity() == std::optional<bool>(true), "mouse activity shows the cursor");
  Check(cursor.KeyboardActivity() == std::optional<bool>(false), "keyboard activity hides the cursor");
  Check(cursor.MouseActivity() == std::optional<bool>(true), "mouse activity shows it again");
}

void TestOnlyChangesAreReported() {
  CursorHider cursor;
  Check(!cursor.MouseActivity(), "visible stays visible: nothing to apply");
  Check(cursor.KeyboardActivity().has_value(), "the first hide is a change");
  Check(!cursor.KeyboardActivity(), "a repeated hide asks for nothing");
  Check(!cursor.ControllerActivity(), "another hiding device asks for nothing");
}

void TestOverlayKeepsCursor() {
  CursorHider cursor;
  cursor.ControllerActivity();
  Check(cursor.SetOverlayOpen(true) == std::optional<bool>(true), "a mouse-driven overlay shows the cursor");
  Check(!cursor.ControllerActivity(), "controller input does not hide it under the overlay");
  Check(!cursor.KeyboardActivity(), "keyboard input does not hide it under the overlay");
  Check(cursor.visible(), "the overlay keeps it");
  Check(cursor.SetOverlayOpen(false) == std::optional<bool>(false), "closing returns to the last device");
}

void TestFocusIsolation() {
  CursorHider unfocused;
  unfocused.SetFocused(false);
  Check(unfocused.visible(), "an unfocused window never hides the cursor");
  Check(!unfocused.ControllerActivity(), "input meant for another window changes nothing");
  Check(!unfocused.KeyboardActivity(), "keyboard input without focus changes nothing");
  Check(!unfocused.SetFocused(true), "regaining focus keeps the cursor: that input was not ours");
  Check(unfocused.visible(), "the cursor is still visible after alt-tabbing back");

  CursorHider focused;
  focused.ControllerActivity();
  Check(focused.SetFocused(false) == std::optional<bool>(true), "losing focus gives the desktop its cursor");
  Check(focused.SetFocused(true) == std::optional<bool>(false), "regaining focus restores the hidden state");
}

void TestTwoInstances() {
  CursorHider a;
  CursorHider b;
  b.SetFocused(false);
  a.ControllerActivity();
  b.ControllerActivity();
  Check(!a.visible(), "the focused instance hides its cursor");
  Check(b.visible(), "the instance behind it does not follow the shared controller");
}

X_INPUT_GAMEPAD Pad(uint16_t buttons, uint8_t trigger = 0, int16_t thumb = 0) {
  X_INPUT_GAMEPAD pad;
  pad.buttons.value = buttons;
  pad.right_trigger = trigger;
  pad.thumb_lx.value = thumb;
  return pad;
}

void TestControllerActivityFilter() {
  ControllerActivityFilter filter;
  Check(!filter.Update(Pad(0)), "an idle pad is no activity");
  Check(filter.Update(Pad(1)), "a new button is activity");
  Check(!filter.Update(Pad(1)), "a held button is not");
  Check(filter.Update(Pad(3)), "a second button is");
  Check(!filter.Update(Pad(0)), "a release is not");
  Check(!filter.Update(Pad(0, rex::input::kTriggerThreshold - 1)), "a trigger below its threshold is not");
  Check(filter.Update(Pad(0, rex::input::kTriggerThreshold)), "a pulled trigger is");
  Check(!filter.Update(Pad(0, 255)), "a held trigger is not");
  Check(!filter.Update(Pad(0, 0, int16_t(rex::input::kThumbDeadzone - 1))), "a resting stick is not");
  Check(filter.Update(Pad(0, 0, int16_t(rex::input::kThumbDeadzone))), "a pushed stick is");
  Check(filter.Update(Pad(0, 0, 0)) == false, "a released stick is not");
  Check(filter.Update(Pad(0, 0, int16_t(-rex::input::kThumbDeadzone))), "the other direction is");
}

void TestWiring() {
  const std::string host = Source("window_host.cpp");
  Check(Contains(host, "if (g_window->GetCursorVisibility() != wanted)"), "the cursor API is called only on a change");
  Check(Contains(host, "!g_window->HasFocus()"), "keyboard and mouse notes follow this window's focus");
  Check(Contains(host, "g_cursor.visible() || UiWantsMouse()"), "ImGui overlays keep the cursor");
  Check(Contains(host, "g_active.load(std::memory_order_relaxed)"), "the launcher, before attach, is left alone");
  const std::string app = Source("el_chavo_kart_app.h");
  const size_t attach = app.find("AttachWindowHost(window()");
  Check(attach != std::string::npos && attach > app.find("void OnPostSetup() override"),
        "the cursor logic attaches with the game, not the launcher");
}

}  // namespace

int main() {
  TestLastDeviceDecides();
  TestOnlyChangesAreReported();
  TestOverlayKeepsCursor();
  TestFocusIsolation();
  TestTwoInstances();
  TestControllerActivityFilter();
  TestWiring();
  return online_test::Finish();
}
