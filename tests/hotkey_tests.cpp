// The SDK's developer keys stay out of a release and F3 owns the frame counter. The binds themselves live in the game
// build, so the wiring is checked in the sources; the toggle is pure.

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

void TestF3TogglesTheCounter() {
  Check(std::string_view(eck::ToggledFrameCounterFlag(false)) == "true", "F3 shows a hidden counter");
  Check(std::string_view(eck::ToggledFrameCounterFlag(true)) == "false", "F3 hides a shown counter");

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
  Check(Contains(counter, "RegisterBind(\"bind_eck_frame_counter\", \"F3\""), "F3 is the bind");
  Check(!Contains(counter, "\"F2\""), "F2 no longer toggles the counter");
  Check(counter.find("class FrameCounterOverlay") == counter.rfind("class "), "there is one counter overlay");
  Check(!Contains(Source("ping_display.cpp"), "RegisterBind"), "the ping panel takes no key");
  Check(!Contains(Source("window_host.cpp"), "RegisterBind"), "the window host takes no bind");
  Check(Contains(Source("window_host.cpp"), "e.virtual_key() == rex::ui::VirtualKey::kF3"),
        "holding F3 does not strobe the counter");
}

void TestF2AndF4HaveNoPortAction() {
  for (const char* file : {"frame_counter.cpp", "window_host.cpp", "el_chavo_kart_app.h", "launcher.cpp"}) {
    const std::string text = Source(file);
    Check(!text.empty(), std::string("read ") + file);
    Check(!Contains(text, "VirtualKey::kF2") && !Contains(text, "VirtualKey::kF4"), std::string(file) + " binds neither");
  }
}

void TestLauncherSettingIsTheStartingState() {
  Check(Contains(Source("settings.cpp"), "REXCVAR_DEFINE_BOOL(eck_show_frame_counter, false"), "hidden by default");
  Check(Contains(Source("settings.cpp"), "\"eck_show_frame_counter\","), "the launcher saves it");
  const std::string launcher = Source("launcher.cpp");
  Check(Contains(launcher, "\"eck_show_frame_counter\", false, \"Hidden\", \"Shown\""), "Frame counter is the toggle");
  Check(Contains(launcher, "F3 toggles it while playing."), "the launcher names F3");
  const std::string counter = Source("frame_counter.cpp");
  Check(Contains(counter, "REXCVAR_GET(eck_show_frame_counter)"), "the overlay reads the setting");
  Check(!Contains(counter, "SaveNow") && !Contains(counter, "settings_"), "F3 never rewrites settings.toml");
}

}  // namespace

int main() {
  TestSdkBindsRemoved();
  TestF3TogglesTheCounter();
  TestF2AndF4HaveNoPortAction();
  TestLauncherSettingIsTheStartingState();
  return online_test::Finish();
}
