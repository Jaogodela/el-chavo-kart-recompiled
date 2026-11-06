// Local input follows window focus, so two game processes never consume the same controller. The gate and the ImGui
// navigation reading are pure; the source checks hold the wiring the game build adds around them.

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "input_focus.h"
#include "online_test_support.h"

using online_test::Check;
using eck::CombinePads;
using eck::GameReadsInput;
using eck::InputFocus;
using eck::MouseAction;
using eck::MouseReachesUi;

namespace {

constexpr size_t kButtons = 4;
constexpr size_t kSticks = 2;
constexpr size_t kButtonA = 0;
constexpr size_t kButtonB = 1;
constexpr size_t kStickLeft = 0;

struct Pad {
  std::array<bool, kButtons> buttons{};
  std::array<float, kSticks> sticks{};
};

eck::NavInput<kButtons, kSticks> Read(const InputFocus& focus, const std::vector<Pad>& pads) {
  return CombinePads<kButtons, kSticks>(
      focus.accepted(), pads, [](const Pad& pad, size_t i) { return pad.buttons[i]; },
      [](const Pad& pad, size_t i) { return pad.sticks[i]; });
}

bool Neutral(const eck::NavInput<kButtons, kSticks>& input) {
  for (const bool down : input.down) {
    if (down) return false;
  }
  for (const float amount : input.amount) {
    if (amount != 0.0f) return false;
  }
  return true;
}

Pad Holding(size_t button, float stick = 0.0f) {
  Pad pad;
  pad.buttons[button] = true;
  pad.sticks[kStickLeft] = stick;
  return pad;
}

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

void TestGate() {
  InputFocus focus;
  Check(!focus.accepted(), "a window starts without input: it may have opened behind another one");
  Check(focus.Update(true), "gaining focus is a change");
  Check(focus.accepted(), "the focused window accepts input");
  Check(!focus.Update(true), "a repeated focus report is not a transition");
  Check(focus.Update(false), "losing focus is a change");
  Check(!focus.accepted(), "the unfocused window rejects input");
  Check(!focus.Update(false), "a repeated loss is not a transition");
}

void TestGameInput() {
  Check(GameReadsInput(true, false), "focused game reads the keyboard and the controller");
  Check(!GameReadsInput(false, false), "unfocused game reads neither");
  Check(!GameReadsInput(true, true), "an open overlay still keeps the game from them");
  Check(!GameReadsInput(false, true), "unfocused with an overlay open stays closed");
}

void TestMouse() {
  for (const MouseAction action : {MouseAction::kMove, MouseAction::kDown, MouseAction::kUp, MouseAction::kWheel}) {
    Check(MouseReachesUi(true, action), "the focused window receives every mouse event");
  }
  Check(!MouseReachesUi(false, MouseAction::kMove), "an unfocused window ignores cursor movement");
  Check(!MouseReachesUi(false, MouseAction::kDown), "an unfocused window ignores mouse buttons");
  Check(!MouseReachesUi(false, MouseAction::kWheel), "an unfocused window ignores the wheel");
  Check(MouseReachesUi(false, MouseAction::kUp), "a release still arrives, so no press sticks");
}

void TestPadFollowsFocus() {
  InputFocus focus;
  std::vector<Pad> pads{Holding(kButtonA, 0.75f)};
  Check(Neutral(Read(focus, pads)), "before any focus the pad reads neutral");

  focus.Update(true);
  auto input = Read(focus, pads);
  Check(input.down[kButtonA], "the focused window reads the button");
  Check(input.amount[kStickLeft] == 0.75f, "the focused window reads the stick");

  focus.Update(false);
  input = Read(focus, pads);
  Check(!input.down[kButtonA], "focus loss releases the held button at once");
  Check(input.amount[kStickLeft] == 0.0f, "focus loss clears the stick at once");
  Check(Neutral(input), "unfocused reads as an untouched controller");
}

void TestReleaseWhileUnfocused() {
  InputFocus focus;
  focus.Update(true);
  std::vector<Pad> pads{Holding(kButtonA, 1.0f)};
  Check(Read(focus, pads).down[kButtonA], "A is held with focus");

  focus.Update(false);
  pads[0] = Pad{};
  Check(Neutral(Read(focus, pads)), "released while another window has focus");

  focus.Update(true);
  Check(Neutral(Read(focus, pads)), "on return the released button is not still held");
}

void TestRegainReadsCurrentInput() {
  InputFocus focus;
  focus.Update(true);
  std::vector<Pad> pads{Holding(kButtonA)};
  focus.Update(false);
  pads[0] = Holding(kButtonB, 0.5f);
  focus.Update(true);
  const auto input = Read(focus, pads);
  Check(!input.down[kButtonA], "nothing from before the focus loss survives");
  Check(input.down[kButtonB], "focus regain reads what is held now");
  Check(input.amount[kStickLeft] == 0.5f, "focus regain reads the stick as it is now");
}

void TestHotplugWithoutFocus() {
  InputFocus focus;
  std::vector<Pad> pads;
  Check(Neutral(Read(focus, pads)), "no pad, no focus: neutral");
  pads.push_back(Holding(kButtonA));
  Check(Neutral(Read(focus, pads)), "a pad added without focus stays neutral");
  focus.Update(true);
  Check(Read(focus, pads).down[kButtonA], "the pad added without focus works once focused");
  pads.clear();
  Check(Neutral(Read(focus, pads)), "unplugging while held releases it");
}

void TestTwoInstances() {
  InputFocus a;
  InputFocus b;
  const std::vector<Pad> same_controller{Holding(kButtonA, 1.0f)};

  a.Update(true);
  Check(Read(a, same_controller).down[kButtonA], "A consumes the controller while focused");
  Check(Neutral(Read(b, same_controller)), "B stays neutral on the same controller");

  a.Update(false);
  b.Update(true);
  Check(Neutral(Read(a, same_controller)), "after the click A stops consuming it");
  Check(Read(b, same_controller).down[kButtonA], "B now consumes it");

  b.Update(false);
  Check(Neutral(Read(a, same_controller)) && Neutral(Read(b, same_controller)), "desktop focused: neither reacts");
}

void TestLocalSplitScreen() {
  InputFocus focus;
  focus.Update(true);
  const std::vector<Pad> pads{Holding(kButtonA), Holding(kButtonB, 0.25f), Pad{}, Holding(2, 0.5f)};
  const auto input = Read(focus, pads);
  Check(input.down[kButtonA] && input.down[kButtonB] && input.down[2], "every pad counts in the focused window");
  Check(input.amount[kStickLeft] == 0.5f, "the furthest stick of any pad counts");
  focus.Update(false);
  Check(Neutral(Read(focus, pads)), "every pad goes neutral together: the gate is per window, not per pad");
}

void TestLauncherToGameWindow() {
  InputFocus focus;
  focus.Update(true);
  focus.Update(false);
  Check(!focus.accepted(), "the launcher closes");
  Check(focus.Update(true), "the game window takes over with no window to re-point");
  Check(focus.accepted(), "input follows the new window");

  InputFocus relaunched;
  Check(!relaunched.accepted(), "a relaunched process waits for its own window's focus");
  Check(relaunched.Update(true) && relaunched.accepted(), "and accepts input once it has it");
}

void TestDebugLines() {
  Check(eck::InputFocusLogLine(true, "a", 0x1234) ==
            "[INPUT] focus=active controller_input=enabled input_state_reset=yes instance=a window=0x1234",
        "gain line");
  Check(eck::InputFocusLogLine(false, "b", 0xAB) ==
            "[INPUT] focus=inactive controller_input=suppressed instance=b window=0xAB",
        "loss line");
  Check(Contains(eck::InputFocusLogLine(true, "", 0), "instance=none"), "no instance outside the instance script");
}

void TestWiring() {
  const std::string app = Source("el_chavo_kart_app.h");
  Check(Contains(app, "GameReadsInput(LocalInputAccepted(), eck::ModalDialog::AnyOpen())"),
        "the game's input drivers are gated by focus");
  Check(Contains(app, "void OnWindowFocusChanged(bool focused) override"), "the app follows the window's focus");
  Check(Contains(app, "AttachMouseFocusGate(window())"), "the mouse gate is on the window");
  Check(Contains(Source("modal_dialog.cpp"), "gamepad_.Update(io, ::LocalInputAccepted())"),
        "the in-game overlays are gated by focus");
  Check(Contains(Source("launcher.cpp"), "gamepad_.Update(io, ::LocalInputAccepted())"),
        "the launcher is gated by focus");
}

// The online pump runs on every ImGui frame and must keep running behind another window.
void TestNetworkIsNotGated() {
  const std::string overlay = Source("ping_display.cpp");
  Check(Contains(overlay, "PumpOnlineService()"), "the ping overlay pumps the online service");
  for (const char* file : {"ping_display.cpp", "online_runtime.cpp", "online_race_telemetry_guest.cpp"}) {
    const std::string text = Source(file);
    Check(!text.empty(), std::string("read ") + file);
    Check(!Contains(text, "LocalInputAccepted") && !Contains(text, "input_focus.h"),
          std::string(file) + " does not depend on focus");
  }
}

// Focus decides who reads the pads and has no say in how the game runs.
void TestFocusMovingBetweenInstances() {
  InputFocus focus_a, focus_b;
  focus_a.Update(true);
  const std::vector<Pad> pads = {Holding(kButtonA, 1.0f)};
  for (int i = 0; i < 20; ++i) {
    const bool a_focused = i % 2 == 1;
    focus_a.Update(a_focused);
    focus_b.Update(!a_focused);
    Check(Neutral(Read(focus_a, pads)) != Neutral(Read(focus_b, pads)), "one process reads the held pad, never both or neither");
    Check(GameReadsInput(focus_a.accepted(), false) == a_focused, "the unfocused process stays suppressed");
  }
}

void TestPacingIgnoresFocus() {
  Check(Contains(Source("online_runtime.cpp"), "UpdateLocalInstancePacing();"), "the online pump keeps the pacing in step");
  Check(Contains(Source("online_runtime.cpp"), "std::getenv(\"ECK_INSTANCE\")"), "only the test launcher's instances are paced");
  Check(Contains(Source("../tools/online-instance.ps1"), "$env:ECK_INSTANCE = $label"), "and that launcher does name them");
  for (const char* file : {"frame_limiter.cpp", "frame_limiter.h", "online_race_sync.h", "online_race_sync_guest.cpp", "online_race_start.h"}) {
    const std::string text = Source(file);
    Check(!text.empty(), std::string("read ") + file);
    Check(!Contains(text, "LocalInputAccepted") && !Contains(text, "input_focus.h"), std::string(file) + ": pacing and the race never see focus");
  }
}

}  // namespace

int main() {
  TestGate();
  TestGameInput();
  TestMouse();
  TestPadFollowsFocus();
  TestReleaseWhileUnfocused();
  TestRegainReadsCurrentInput();
  TestHotplugWithoutFocus();
  TestTwoInstances();
  TestLocalSplitScreen();
  TestLauncherToGameWindow();
  TestDebugLines();
  TestWiring();
  TestNetworkIsNotGated();
  TestFocusMovingBetweenInstances();
  TestPacingIgnoresFocus();
  return online_test::Finish();
}
