#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "internal_render_scale.h"
#include "launcher.h"
#include "player_names.h"
#include "window_title.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void InternalRenderScale() {
  using namespace eck;
  Check(NormalizeInternalRenderScale(100) == 100 && NormalizeInternalRenderScale(75) == 75 &&
            NormalizeInternalRenderScale(50) == 50,
        "the three choices are kept");
  Check(NormalizeInternalRenderScale(0) == 100 && NormalizeInternalRenderScale(60) == 100 &&
            NormalizeInternalRenderScale(-5) == 100 && NormalizeInternalRenderScale(200) == 100,
        "anything else draws at 100%");

  const InternalRenderSplit native = SplitInternalRenderScale(2, 100);
  Check(native.host_quarters == 8 && native.resolve_scale == 2, "2x at 100% keeps the whole scale");
  const InternalRenderSplit performance = SplitInternalRenderScale(2, 75);
  Check(performance.host_quarters == 6 && performance.resolve_scale == 2, "2x at 75% resolves at 2x");
  const InternalRenderSplit low = SplitInternalRenderScale(1, 50);
  Check(low.host_quarters == 2 && low.resolve_scale == 1, "1x at 50% resolves at 1x");
  const InternalRenderSplit four_low = SplitInternalRenderScale(4, 50);
  Check(four_low.host_quarters == 8 && four_low.resolve_scale == 2, "4x at 50% draws like 2x");

  Check(RequiredRenderTargetPath(100, "") == std::nullopt, "native needs no render target path");
  Check(RequiredRenderTargetPath(75, "") == std::string_view("rtv"), "a reduced scale needs host render targets");
  Check(RequiredRenderTargetPath(50, "rov") == std::nullopt, "a path the player chose is kept");

  Check(InternalRenderWidth(2, 75) == 1920 && InternalRenderHeight(2, 75) == 1080, "2x at 75% is 1920x1080");
  Check(InternalRenderWidth(1, 50) == 640 && InternalRenderHeight(1, 50) == 360, "1x at 50% is 640x360");
  Check(DescribeInternalRenderResolution(2, 75) == "75% 1920x1080 -> 2560x1440", "the description names both sizes");
}

void WindowTitle() {
  using namespace eck;
  const std::string base = kWindowTitle;
  Check(WindowTitleText(std::nullopt, std::nullopt) == base, "the base title stays plain");
  Check(WindowTitleText(std::nullopt, 59.996) == base + " | FPS: 60.00", "the frame rate follows the title");
  Check(WindowTitleText(RenderResolutionText(2), std::nullopt) == base + " | 2560x1440",
        "the render resolution follows the title");
  Check(WindowTitleText(RenderResolutionText(2), 60.0) == base + " | 2560x1440 | FPS: 60.00",
        "both parts compose in a fixed order");
  Check(RenderResolutionText(1) == "1280x720" && RenderResolutionText(4) == "5120x2880",
        "the render resolution is the guest size times the scale");
}

}  // namespace

void LocalPlayers() {
  using namespace eck;
  Check(kPlayerNames.size() == 4, "four local player slots");
  for (size_t i = 0; i < kPlayerNames.size(); ++i) {
    const PlayerNameSetting& player = kPlayerNames[i];
    const std::string number = std::to_string(i + 1);
    Check(std::string(player.key) == "eck_player_" + number + "_name", "launcher key " + number);
    Check(std::string(player.sdk_key) == "local_player_" + number + "_name", "SDK key " + number);
    Check(std::string(player.default_name) == "Player " + number, "default name " + number);
    Check(SdkPlayerName(player, "Chaves") == "Chaves", "a valid name is sent " + number);
    Check(SdkPlayerName(player, "") == (i == 0 ? "Player 1" : ""), "empty or default name policy " + number);
    Check(SdkPlayerName(player, "Player " + number) == (i == 0 ? "Player 1" : ""), "default is not forced " + number);
    Check(SdkPlayerName(player, "123456789012345678") == (i == 0 ? "Player 1" : ""), "too long falls back " + number);
  }
  Check(IsValidPlayerName("123456789012345") && !IsValidPlayerName("1234567890123456"), "15 characters at most");
  Check(!IsValidPlayerName("") && !IsValidPlayerName("a	b"), "empty and control characters are rejected");
}

void OriginalDisplay() {
  using namespace eck;
  Check(kGuestWidth == 1280 && kGuestHeight == 720, "the guest base is 1280x720");
  Check(RenderResolutionText(1) == "1280x720" && RenderResolutionText(2) == "2560x1440" &&
            RenderResolutionText(3) == "3840x2160" && RenderResolutionText(4) == "5120x2880",
        "render resolution derives from 1280x720");
  Check(InternalRenderWidth(1, 100) == 1280 && InternalRenderHeight(1, 100) == 720, "internal 100% is 1280x720");
  Check(InternalRenderWidth(1, 75) == 960 && InternalRenderHeight(1, 75) == 540, "internal 75% is 960x540");
  Check(InternalRenderWidth(1, 50) == 640 && InternalRenderHeight(1, 50) == 360, "internal 50% is 640x360");
  Check(InternalRenderWidth(2, 75) == 1920 && InternalRenderHeight(2, 75) == 1080, "2x at 75% is 1920x1080");
  Check(DescribeInternalRenderResolution(2, 50) == "50% 1280x720 -> 2560x1440", "2x at 50% names both sizes");
  Check(std::size(kWindowSizes) >= 5, "the window size list is kept");
  for (const WindowSize& size : kWindowSizes) {
    Check(std::fabs(double(size.width) / size.height - 16.0 / 9.0) < 0.02, "window sizes are 16:9");
  }
}

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

void ShowThisLauncher() {
  using eck::LauncherWanted;
  constexpr bool kReady = true;
  constexpr bool kSkip = true;
  constexpr bool kShift = true;
  constexpr bool kOn = true;
  Check(LauncherWanted(kReady, !kSkip, !kShift, kOn), "on by choice: the launcher opens");
  Check(!LauncherWanted(kReady, !kSkip, !kShift, !kOn), "off: the game starts directly");
  Check(LauncherWanted(kReady, !kSkip, kShift, !kOn), "off, Shift held: the launcher opens");
  Check(LauncherWanted(kReady, !kSkip, kShift, kOn), "on, Shift held: the launcher opens");
  Check(!LauncherWanted(kReady, kSkip, !kShift, kOn), "the relaunch after Play skips the launcher");
  Check(!LauncherWanted(kReady, kSkip, kShift, kOn), "Shift still held on the relaunch does not reopen it");
  Check(LauncherWanted(!kReady, !kSkip, !kShift, !kOn), "off, but no game: it is the only way to install");
  Check(LauncherWanted(!kReady, kSkip, !kShift, !kOn), "the relaunch flag cannot hide a missing game");

  const std::string settings = Source("settings.cpp");
  Check(Contains(settings, "REXCVAR_DEFINE_BOOL(eck_launcher, true"), "Show this launcher defaults on");
  Check(Contains(settings, "\"eck_launcher\","), "the choice is saved with the other settings");
  const std::string launcher = Source("launcher.cpp");
  Check(Contains(launcher, "ui.LauncherVisibilityRow(\"eck_launcher\")"), "the row is on the Play page");
  Check(Contains(launcher, "rex::cvar::ResetToDefault(\"eck_skip_launcher\")"), "the skip lasts one start");
  Check(Contains(launcher, "ShiftHeld()"), "Shift is read at start");

  const std::string app = Source("el_chavo_kart_app.h");
  Check(Contains(app, "eck::RelaunchSelf(L\"--eck_skip_launcher=true\")"), "the window relaunch skips the launcher");
  Check(Contains(app, "game_window_ = eck::FitWindowToDisplay();"),
        "the game window keeps the player's display settings when the launcher is skipped");
}

int main() {
  InternalRenderScale();
  WindowTitle();
  LocalPlayers();
  OriginalDisplay();
  ShowThisLauncher();
  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
