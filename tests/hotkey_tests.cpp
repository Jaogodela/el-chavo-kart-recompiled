// The SDK's developer keys stay out of a release, F2 owns the frame counter and F3 the Ping display. The binds
// themselves live in the game build, so the wiring is checked in the sources; the toggle and the corner layout are pure.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "corner_overlay.h"
#include "frame_counter.h"
#include "online_test_support.h"

using online_test::Check;

namespace {

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

bool SdkBindRemoved(std::string_view bind) {
  return std::any_of(std::begin(eck::kRemovedSdkBinds), std::end(eck::kRemovedSdkBinds),
                     [bind](const char* removed) { return removed == bind; });
}

void TestSdkBindsRemoved() {
  Check(SdkBindRemoved("bind_console"), "the console (Backtick, the apostrophe key on ABNT2) is removed");
  Check(SdkBindRemoved("bind_debug_overlay"), "F3 opens no SDK debug overlay");
  Check(SdkBindRemoved("bind_settings"), "F4 opens no SDK settings overlay");
  Check(!SdkBindRemoved("bind_achievements"), "F7 stays: the port overrides it");

  const std::string app = Source("el_chavo_kart_app.h");
  Check(Contains(app, "void OnCreateDialogs(rex::ui::ImGuiDrawer*) override"), "removed where the SDK just made them");
  Check(Contains(app, "rex::ui::UnregisterBind(bind)"), "through the SDK's own call");

  const std::string patch = Source("../patches/rexglue-sdk/0025-disable-debug-and-settings-overlay-binds.patch");
  Check(Contains(patch, "-  rex::ui::RegisterBind(\"bind_debug_overlay\", \"F3\""), "the release runtime has no F3 bind");
  Check(Contains(patch, "-  rex::ui::RegisterBind(\"bind_settings\", \"F4\""), "the release runtime has no F4 bind");
}

void TestF2TogglesTheCounter() {
  Check(std::string_view(eck::kFrameCounterKey) == "F2", "F2 is the frame counter key");
  Check(std::string_view(eck::ToggledFrameCounterFlag(false)) == "true", "F2 shows a hidden counter");
  Check(std::string_view(eck::ToggledFrameCounterFlag(true)) == "false", "F2 hides a shown counter");

  bool shown = false;
  for (int press = 0; press < 4; ++press) {
    shown = std::string_view(eck::ToggledFrameCounterFlag(shown)) == "true";
    Check(shown == (press % 2 == 0), "each press flips the state");
  }

  const GuestFrameStats measured{60.0, 16.7, 100};
  Check(eck::FrameCounterVisible(true, measured), "the counter draws while on");
  Check(!eck::FrameCounterVisible(false, measured), "and not while off");
  Check(!eck::FrameCounterVisible(true, GuestFrameStats{}), "nor before a rate is measured");

  const std::string counter = Source("frame_counter.cpp");
  Check(Contains(counter, "RegisterBind(\"bind_eck_frame_counter\", kFrameCounterKey"), "the counter binds its key constant");
  Check(!Contains(counter, "\"F3\""), "the counter is no longer on F3");
  Check(counter.find("class FrameCounterOverlay") == counter.rfind("class "), "there is one counter overlay");
  Check(Contains(counter, "DrawCornerText(FrameCounterText(stats), FrameCounterSlot())"), "the counter takes slot 0");
  Check(!Contains(Source("window_host.cpp"), "RegisterBind"), "the window host takes no bind");

  const std::string host = Source("window_host.cpp");
  Check(Contains(host, "e.virtual_key() == rex::ui::VirtualKey::kF2 || e.virtual_key() == rex::ui::VirtualKey::kF3"),
        "holding F2 or F3 does not strobe a toggle");
}

void TestF3TogglesPing() {
  const std::string header = Source("ping_display.h");
  const std::string ping = Source("ping_display.cpp");
  Check(Contains(header, "inline constexpr char kPingDisplayKey[] = \"F3\";"), "F3 is the Ping display key");
  Check(Contains(ping, "RegisterBind(\"bind_eck_ping_display\", kPingDisplayKey"), "the Ping display binds its key constant");
  Check(Contains(ping, "if (!REXCVAR_GET(eck_online_enabled)) return;"), "F3 does nothing with Online play off");
  Check(Contains(ping, "SetFlagByName(\"eck_online_ping_display\""), "F3 switches only the Ping display");
  Check(!Contains(ping, "\"eck_online_enabled\""), "F3 never turns Online play on");
  Check(!Contains(ping, "eck_show_frame_counter\", "), "F3 leaves the frame counter alone");
  Check(Contains(ping, "PingSlot(counter)"), "Ping sits under the counter when both show");
  Check(ping.find("class PingOverlay") == ping.rfind("class "), "there is one Ping overlay");
  for (const char* file : {"ping_display.cpp", "ping_display.h"}) {
    const std::string text = Source(file);
    Check(!Contains(text, "SaveNow") && !Contains(text, "settings_"), std::string(file) + " never rewrites settings.toml");
  }
  Check(!Contains(Source("frame_counter.cpp"), "kPingDisplayKey"), "the frame counter takes no Ping key");
}

void TestF4HasNoPortAction() {
  for (const char* file : {"frame_counter.cpp", "ping_display.cpp", "ping_display.h", "window_host.cpp",
                           "el_chavo_kart_app.h", "launcher.cpp"}) {
    const std::string text = Source(file);
    Check(!text.empty(), std::string("read ") + file);
    Check(!Contains(text, "VirtualKey::kF4") && !Contains(text, "\"F4\""), std::string(file) + " binds no F4");
  }
  for (const char* file : {"frame_counter.cpp", "ping_display.cpp", "el_chavo_kart_app.h", "launcher.cpp"}) {
    Check(!Contains(Source(file), "VirtualKey::kF2") && !Contains(Source(file), "VirtualKey::kF3"),
          std::string(file) + " has no key listener of its own");
  }
}

void TestCornerSlots() {
  Check(eck::FrameCounterSlot() == 0, "the frame counter alone is slot 0");
  Check(eck::PingSlot(false) == 0, "Ping alone is slot 0");
  Check(eck::PingSlot(true) == 1, "Ping goes under a shown counter");

  const eck::CornerPanel fps = eck::CornerPanelAt(1920, 18, 120, 18, eck::FrameCounterSlot());
  const eck::CornerPanel alone = eck::CornerPanelAt(1920, 18, 90, 18, eck::PingSlot(false));
  const eck::CornerPanel below = eck::CornerPanelAt(1920, 18, 90, 18, eck::PingSlot(true));
  Check(alone.y0 == fps.y0 && alone.y1 == fps.y1, "Ping alone takes the counter's place");
  Check(below.y0 > fps.y1, "Ping under the counter does not overlap it");
  Check(below.y0 - fps.y1 < fps.y1 - fps.y0, "and sits immediately below");
  Check(below.x1 == fps.x1, "the right edges line up");
}

void TestKeysAreReservedFromGameplay() {
  const std::string keybinds = Source("keybinds.h");
  const size_t begin = keybinds.find("inline bool KeyboardBindingIsReserved(ImGuiKey key)");
  const size_t end = keybinds.find("return true;", begin);
  Check(begin != std::string::npos && end != std::string::npos, "the reservation list is found");
  const std::string reserved = keybinds.substr(begin, end - begin);
  for (const char* key : {"ImGuiKey_F2", "ImGuiKey_F3", "ImGuiKey_F4", "ImGuiKey_F7", "ImGuiKey_F11", "ImGuiKey_GraveAccent"}) {
    Check(Contains(reserved, std::string("case ") + key + ":"), std::string(key) + " is reserved from keyboard emulation");
  }
}

void TestLauncherSettingIsTheStartingState() {
  Check(Contains(Source("settings.cpp"), "REXCVAR_DEFINE_BOOL(eck_show_frame_counter, false"), "hidden by default");
  Check(Contains(Source("settings.cpp"), "\"eck_show_frame_counter\","), "the launcher saves it");
  const std::string launcher = Source("launcher.cpp");
  Check(Contains(launcher, "\"eck_show_frame_counter\", false, \"Hidden\", \"Shown\""), "Frame counter is the toggle");
  Check(Contains(launcher, "F2 toggles it while playing."), "the launcher names F2");
  Check(!Contains(launcher, "F3 toggles it while playing."), "and no longer F3");
  const std::string counter = Source("frame_counter.cpp");
  Check(Contains(counter, "REXCVAR_GET(eck_show_frame_counter)"), "the overlay reads the setting");
  Check(!Contains(counter, "SaveNow") && !Contains(counter, "settings_"), "F2 never rewrites settings.toml");
}

}  // namespace

int main() {
  TestSdkBindsRemoved();
  TestF2TogglesTheCounter();
  TestF3TogglesPing();
  TestF4HasNoPortAction();
  TestCornerSlots();
  TestKeysAreReservedFromGameplay();
  TestLauncherSettingIsTheStartingState();
  return online_test::Finish();
}
