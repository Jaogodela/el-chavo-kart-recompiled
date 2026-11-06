#include "settings.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/logging.h>

#include "trg/settings.h"

#include "crash_report.h"
#include "online_setting.h"
#include "diagnostics.h"
#include "dpi.h"
#include "frame_limiter.h"
#include "game_files.h"
#include "graphics_adapters.h"
#include "internal_render_scale.h"
#include "input_summary.h"
#include "language_setting.h"
#include "keybinds.h"
#include "multisampling.h"
#include "player_names.h"
#include "system_info.h"
#include "ui_text.h"

#include "platform.h"

REXCVAR_DEFINE_BOOL(eck_launcher, true, "El Chavo Kart",
                    "Show the launcher before the game starts (hold Shift at start to force it)");
REXCVAR_DEFINE_BOOL(eck_skip_launcher, false, "El Chavo Kart",
                    "Skip the launcher once (set when the launcher restarts the game)");
REXCVAR_DEFINE_STRING(eck_language, eck::kDefaultLanguage, "El Chavo Kart",
                      "Game and launcher language: pt-BR, en-US, es-MX or es-ES (sets user_language and user_country "
                      "at start)");
REXCVAR_DEFINE_BOOL(eck_skip_intro, false, "El Chavo Kart", "Skip the startup logos and intro movies");
REXCVAR_DEFINE_BOOL(eck_unlock_secret_characters, false, "El Chavo Kart",
                    "Let every character be selected while on; the save is not changed");
REXCVAR_DEFINE_BOOL(eck_unlock_race_tracks, false, "El Chavo Kart",
                    "Let every race track be selected in a free race while on; the save is not changed");
REXCVAR_DEFINE_BOOL(eck_show_fps, true, "El Chavo Kart", "Show the frame rate in the window title");
REXCVAR_DEFINE_BOOL(eck_show_render_resolution, false, "El Chavo Kart",
                    "Show the render resolution in the window title");
REXCVAR_DEFINE_BOOL(eck_show_frame_counter, false, "El Chavo Kart",
                    "Show the frame counter in the corner (F2 toggles it)");
REXCVAR_DEFINE_BOOL(eck_msaa_compatibility, false, "El Chavo Kart",
                    "Draw the game's 2x MSAA render targets with 4x MSAA, for GPUs or drivers whose "
                    "native 2x MSAA misrenders (sets native_2x_msaa)");
REXCVAR_DEFINE_STRING(eck_graphics_adapter, "", "El Chavo Kart",
                      "Graphics card: the index d3d12_adapter takes, empty for the first usable card (applied at start)");
REXCVAR_DEFINE_STRING(eck_player_1_name, "Player 1", "El Chavo Kart",
                      "Name shown for local player 1; save data is not affected");
REXCVAR_DEFINE_STRING(eck_player_2_name, "Player 2", "El Chavo Kart",
                      "Name shown for local player 2; save data is not affected");
REXCVAR_DEFINE_STRING(eck_player_3_name, "Player 3", "El Chavo Kart",
                      "Name shown for local player 3; save data is not affected");
REXCVAR_DEFINE_STRING(eck_player_4_name, "Player 4", "El Chavo Kart",
                      "Name shown for local player 4; save data is not affected");
REXCVAR_DEFINE_BOOL(eck_online_enabled, false, "El Chavo Kart",
                    "Online play: friends and rooms of 2 to 4 players over the internet");
REXCVAR_DEFINE_STRING(eck_online_name, "Player", "El Chavo Kart",
                      "Name other players see during online play (letters, digits, - and _)");
REXCVAR_DEFINE_BOOL(eck_online_ping_display, false, "El Chavo Kart",
                    "Shows the round trip to the other players during online play (F3 toggles it)");
REXCVAR_DEFINE_STRING(eck_online_connection_test, "", "El Chavo Kart",
                      "UDP configuration of the last connection test finished with Online play on, written by the "
                      "launcher; empty: none since Online play was turned on");
REXCVAR_DEFINE_INT32(eck_launcher_x, 0, "El Chavo Kart", "Launcher window left edge, written by the launcher");
REXCVAR_DEFINE_INT32(eck_launcher_y, 0, "El Chavo Kart", "Launcher window top edge, written by the launcher");
REXCVAR_DEFINE_INT32(eck_launcher_width, 0, "El Chavo Kart", "Launcher window restored width, written by the launcher");
REXCVAR_DEFINE_INT32(eck_launcher_height, 0, "El Chavo Kart",
                     "Launcher window restored height, written by the launcher");
REXCVAR_DEFINE_BOOL(eck_launcher_maximized, false, "El Chavo Kart", "Launcher window was maximized when closed");
REXCVAR_DEFINE_BOOL(eck_launcher_fullscreen, false, "El Chavo Kart",
                    "Launcher was left in its own F11 fullscreen, written by the launcher");

namespace eck {
namespace {

// Every setting the launcher captures, saves, applies and resets. The keyboard bindings come from
// the launcher's own table so their names live in one place.
std::vector<std::string> GameSettingKeys() {
  std::vector<std::string> keys = {
      "eck_launcher",
      "fullscreen",
      "window_width",
      "window_height",
      "monitor",
      kTearingSetting,
      "eck_graphics_adapter",
      "resolution_scale",
      "eck_internal_render_scale",
      "swap_post_effect",
      "eck_msaa_compatibility",
      "anisotropic_override",
      "eck_frame_rate",
      "eck_skip_intro",
      "eck_unlock_secret_characters",
      "eck_unlock_race_tracks",
      "eck_show_fps",
      "eck_show_render_resolution",
      "eck_show_frame_counter",
      kLanguageSetting,
      "detailed_logging",
      "mnk_mode",
      "eck_player_1_name",
      "eck_player_2_name",
      "eck_player_3_name",
      "eck_player_4_name",
      "eck_online_enabled",
      "eck_online_name",
      "eck_online_ping_display",
      "eck_online_connection_test",
  };
  for (const KeyboardAction& action : kKeyboardActions) {
    keys.emplace_back(action.key);
  }
  return keys;
}

const std::vector<std::string> kGameSettingKeys = GameSettingKeys();

const std::vector<std::string> kWindowKeys = {
    "window_width",
    "window_height",
    "monitor",
    kTearingSetting,
    "eck_graphics_adapter",
};

bool IsWindowKey(std::string_view key) {
  return std::find(kWindowKeys.begin(), kWindowKeys.end(), key) != kWindowKeys.end();
}

// Monitor 0 (Default) is the primary display, as monitor 1 is.
std::string WindowValue(std::string_view key, const std::string& value) {
  if (key == "monitor") {
    return std::to_string(std::max(1, std::atoi(value.c_str())));
  }
  return value;
}

bool g_graphics_adapter_stale = false;

std::string TomlValue(std::string_view key, const std::string& value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
  if (!info || info->type != rex::cvar::FlagType::String) {
    return value;
  }
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') {
      quoted += '\\';
    }
    quoted += c;
  }
  return quoted + '"';
}

std::optional<std::string> ConfigValue(const toml::table& table, std::string_view key) {
  const toml::node* node = table.get(key);
  if (!node) {
    return std::nullopt;
  }
  if (auto v = node->value_exact<bool>()) {
    return *v ? "true" : "false";
  }
  if (auto v = node->value_exact<int64_t>()) {
    return std::to_string(*v);
  }
  if (auto v = node->value_exact<double>()) {
    return std::to_string(*v);
  }
  return node->value_exact<std::string>();
}

trg::TextFileSettings OpenPlayerFile(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (!std::filesystem::exists(path, ec)) {
    std::ofstream(path) << "# El Chavo Kart player settings, written by the launcher.\n"
                           "# Loaded after el_chavo_kart.toml. Remove a line to use the shipped value.\n";
  }
  return trg::TextFileSettings(Utf8(path));
}

std::string CvarText(std::string_view name, std::string_view fallback) {
  const std::string value = rex::cvar::GetFlagByName(name);
  return value.empty() ? std::string(fallback) : value;
}

std::string OnOff(bool value) {
  return value ? "On" : "Off";
}

std::string ChoiceLabel(std::initializer_list<std::pair<std::string_view, std::string_view>> choices,
                        std::string_view value, std::string_view fallback) {
  for (const auto& [key, label] : choices) {
    if (key == value) {
      return std::string(label);
    }
  }
  return std::string(fallback);
}

std::string SelectedGraphicsCardLabel(const GameSettings& settings) {
  const std::vector<GraphicsAdapter> adapters = ListGraphicsAdapters();
  const std::optional<int> index = ResolveGraphicsAdapter(settings.Get("eck_graphics_adapter", ""), adapters);
  if (!index) {
    return "Unavailable";
  }
  for (const GraphicsAdapter& adapter : adapters) {
    if (adapter.index != *index) {
      continue;
    }
    const auto same_name = std::count_if(adapters.begin(), adapters.end(), [&](const GraphicsAdapter& other) {
      return other.name == adapter.name;
    });
    return same_name > 1 ? adapter.name + " (" + std::to_string(adapter.index) + ")" : adapter.name;
  }
  return "Unavailable";
}

std::string TextureFilteringName(std::string_view value) {
  if (value == "-1") {
    return "Game";
  }
  if (value == "0") {
    return "Off";
  }
  if (value == "2") {
    return "2x";
  }
  if (value == "3") {
    return "4x";
  }
  if (value == "4") {
    return "8x";
  }
  if (value == "5") {
    return "16x";
  }
  return std::string(value);
}

std::string InternalRenderLabel(int percent) {
  switch (NormalizeInternalRenderScale(percent)) {
    case kInternalRenderScalePerformance:
      return "75% Performance";
    case kInternalRenderScaleLowEnd:
      return "50% Low-end";
    default:
      return "100% Native";
  }
}

std::string GameStateName(GameFiles state) {
  switch (state) {
    case GameFiles::kReady:
      return "verified";
    case GameFiles::kUnsupported:
      return "unsupported version";
    case GameFiles::kIncomplete:
      return "incomplete";
    case GameFiles::kMissing:
      return "missing";
  }
  return "unknown";
}

}  // namespace

void LoadPlayerSettings(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }
  rex::cvar::LoadConfig(path);
  REXLOG_INFO("Player settings: {}", Utf8(path.filename()));
}

WindowPlacement SavedLauncherPlacement() {
  struct Cvars {
    int GetInt(const char* key, int) const { return std::atoi(rex::cvar::GetFlagByName(key).c_str()); }
    bool GetBool(const char* key, bool) const { return rex::cvar::GetFlagByName(key) == "true"; }
  };
  return ReadPlacement(Cvars{});
}

bool SavedLauncherFullscreen() {
  return REXCVAR_GET(eck_launcher_fullscreen);
}

std::optional<GameWindowStartup> FitWindowToDisplay() {
  int width = std::atoi(rex::cvar::GetFlagByName("window_width").c_str());
  int height = std::atoi(rex::cvar::GetFlagByName("window_height").c_str());
  if (width <= 0 || height <= 0) {
    width = kGuestWidth;
    height = kGuestHeight;
  }
  const MonitorInfo display = MonitorForSetting(std::atoi(rex::cvar::GetFlagByName("monitor").c_str()));
  if (display.width == 0) {
    return std::nullopt;
  }
  GameWindowStartup startup;
  startup.scale = display.scale;
  startup.monitor = {display.width, display.height};
  startup.work = {display.work_width, display.work_height};
  startup.frame = WindowFrameForScale(display.scale);
  startup.plan = PlanGameWindow({width, height}, startup.work, startup.frame, MonitorForSetting(0).scale);
  rex::cvar::SetFlagByName("window_width", std::to_string(startup.plan.logical.width));
  rex::cvar::SetFlagByName("window_height", std::to_string(startup.plan.logical.height));
  return startup;
}

void GameSettings::Capture(const std::filesystem::path& player_file, const std::filesystem::path& shipped_config) {
  player_file_ = player_file;
  shipped_config_ = shipped_config;
  saved_.clear();
  unregistered_.clear();
  for (const std::string& key : kGameSettingKeys) {
    saved_[key] = rex::cvar::GetFlagByName(key);
    if (!rex::cvar::GetFlagInfo(key)) {
      unregistered_.insert(key);
    }
  }
  pending_ = saved_;
  // Captured after the correction, so Save would never see that the file still holds the old value.
  if (g_graphics_adapter_stale) {
    g_graphics_adapter_stale = false;
    const std::string key = "eck_graphics_adapter";
    SaveNow({{key, saved_[key]}});
  }
}

void GameSettings::CaptureLateRegistered() {
  for (auto it = unregistered_.begin(); it != unregistered_.end();) {
    const std::string& key = *it;
    if (!rex::cvar::GetFlagInfo(key)) {
      ++it;
      continue;
    }
    if (pending_[key].empty()) {
      saved_[key] = pending_[key] = rex::cvar::GetFlagByName(key);
    }
    it = unregistered_.erase(it);
  }
}

std::string GameSettings::Get(std::string_view key, std::string_view def) const {
  const auto it = pending_.find(std::string(key));
  return it == pending_.end() || it->second.empty() ? std::string(def) : it->second;
}

void GameSettings::Set(std::string_view key, std::string value) {
  pending_[std::string(key)] = std::move(value);
}

bool GameSettings::Save() {
  for (const PlayerNameSetting& player : kPlayerNames) {
    const auto name = pending_.find(player.key);
    if (name != pending_.end() && !IsValidPlayerName(name->second)) {
      name->second = player.default_name;
    }
  }
  std::vector<std::pair<std::string, std::string>> changes;
  for (const auto& [key, value] : pending_) {
    const auto saved = saved_.find(key);
    if (saved == saved_.end() || saved->second != value) {
      changes.emplace_back(key, value);
    }
  }
  if (changes.empty()) {
    return true;
  }
  for (const auto& [key, value] : changes) {
    if (!rex::cvar::GetFlagInfo(key)) {
      REXLOG_ERROR("Cannot save {}: the setting is not registered", key);
      return false;
    }
  }

  trg::TextFileSettings file = OpenPlayerFile(player_file_);
  for (const auto& [key, value] : changes) {
    file.Set(key, TomlValue(key, value));
  }
  if (!file.Save()) {
    REXLOG_ERROR("Cannot write player settings to {}", Utf8(player_file_.filename()));
    return false;
  }
  saved_ = pending_;
  return true;
}

bool GameSettings::SaveNow(const SettingValues& values) {
  trg::TextFileSettings file = OpenPlayerFile(player_file_);
  for (const auto& [key, value] : values) {
    file.Set(key, TomlValue(key, value));
  }
  if (!file.Save()) {
    REXLOG_ERROR("Cannot write player settings to {}", Utf8(player_file_.filename()));
    return false;
  }
  return true;
}

void GameSettings::ResetToDefaults() {
  toml::table shipped;
  try {
    shipped = toml::parse_file(Utf8(shipped_config_));
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("Cannot read {}: {}", Utf8(shipped_config_.filename()), e.what());
  }
  for (const std::string& key : kGameSettingKeys) {
    const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(key);
    pending_[key] = ConfigValue(shipped, key).value_or(info ? info->default_value : std::string());
  }
}

void GameSettings::Apply() const {
  for (const auto& [key, value] : pending_) {
    if (IsWindowKey(key) || rex::cvar::GetFlagByName(key) == value) {
      continue;
    }
    rex::cvar::SetFlagByName(key, value);
  }
  ApplyMultisampling();
  ApplyInternalRenderPath();
  ApplyLanguage();
  ApplyPlayerNames();
}

void ApplyLanguage() {
  const GameLanguage& language = LanguageFor(REXCVAR_GET(eck_language));
  SetActiveUiLanguage(language.key);
  rex::cvar::SetFlagByName("user_language", std::to_string(language.xconfig_language));
  if (!rex::cvar::SetFlagByName("user_country", std::to_string(language.xconfig_country))) {
    REXLOG_WARN("Language: user_country could not be set; Spanish shows the Latin American text");
  }
  REXLOG_INFO("Language: {} (package {}, user_language {}, user_country {})", language.label, language.package,
              language.xconfig_language, language.xconfig_country);
}

// Also replaces a native_2x_msaa = false written by launchers that labelled it Off.
void ApplyMultisampling() {
  rex::cvar::SetFlagByName("native_2x_msaa",
                           Native2xMsaaFor(REXCVAR_GET(eck_msaa_compatibility)) ? "true" : "false");
}

void ApplyPlayerNames() {
  for (const PlayerNameSetting& player : kPlayerNames) {
    rex::cvar::SetFlagByName(player.sdk_key, SdkPlayerName(player, rex::cvar::GetFlagByName(player.key)));
  }
}

void ApplyInternalRenderPath() {
#if defined(_WIN32)
  const std::optional<std::string_view> path = RequiredRenderTargetPath(
      std::atoi(rex::cvar::GetFlagByName("eck_internal_render_scale").c_str()),
      rex::cvar::GetFlagByName("render_target_path_d3d12"));
  if (path && rex::cvar::SetFlagByName("render_target_path_d3d12", *path)) {
    REXLOG_INFO("Internal render resolution: using D3D12 host render targets");
  }
#endif
}

void ApplyGraphicsAdapter() {
#if defined(_WIN32)
  const std::string setting = REXCVAR_GET(eck_graphics_adapter);
  const std::vector<GraphicsAdapter> adapters = ListGraphicsAdapters();
  const std::optional<int> index = ResolveGraphicsAdapter(setting, adapters);
  if (!index) {
    REXLOG_WARN("No usable Direct3D 12 graphics card found");
    return;
  }
  const auto chosen = std::find_if(adapters.begin(), adapters.end(),
                                   [&](const GraphicsAdapter& adapter) { return adapter.index == *index; });
  if (ParseGraphicsAdapter(setting) && ParseGraphicsAdapter(setting) != index) {
    REXLOG_WARN("Graphics card {} is not available, using {}", setting, chosen->name);
  } else {
    REXLOG_INFO("Graphics card: {}", chosen->name);
  }
  g_graphics_adapter_stale = GraphicsAdapterSettingIsStale(setting, adapters);
  rex::cvar::SetFlagByName("d3d12_adapter", std::to_string(*index));
  rex::cvar::SetFlagByName("eck_graphics_adapter", std::to_string(*index));
#endif
}

void LogStartupDiagnostics(const GameSettings& settings, const MonitorInfo& display, int monitor,
                           const std::filesystem::path& game_dir, const StartupWindowInfo& window) {
  const int frame_rate = FrameRateFromText(settings.Get("eck_frame_rate", "60"));
  const int scale = std::max(1, std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()));
  const int percent = NormalizeInternalRenderScale(
      std::atoi(rex::cvar::GetFlagByName("eck_internal_render_scale").c_str()));
  const InternalRenderSplit split = SplitInternalRenderScale(scale, percent);
  const std::string selected_card = SelectedGraphicsCardLabel(settings);
  const bool selected_fullscreen = settings.GetBool("fullscreen", false);
  const bool selected_tearing = settings.GetBool(kTearingSetting, false);
  const bool runtime_tearing = CvarText(kTearingSetting, "false") == "true";
  const GameLanguage& language = LanguageFor(settings.Get(kLanguageSetting, kDefaultLanguage));

  StartupDiagnostics diagnostics;
  diagnostics.launcher_display.mode = selected_fullscreen ? "Borderless Fullscreen" : "Windowed";
  const int selected_width = window.selected_width >= 0 ? window.selected_width : window.configured_width;
  const int selected_height = window.selected_height >= 0 ? window.selected_height : window.configured_height;
  diagnostics.launcher_display.window_size =
      selected_width > 0 && selected_height > 0
          ? std::to_string(selected_width) + "x" + std::to_string(selected_height)
          : "Default";
  diagnostics.launcher_display.monitor =
      monitor > 0 ? "Display " + std::to_string(monitor) : "Default (primary)";
  diagnostics.launcher_display.vsync = selected_tearing ? "Off" : "On";

  diagnostics.launcher_gameplay.language = language.label;
  diagnostics.launcher_gameplay.skip_intro = OnOff(settings.GetBool("eck_skip_intro", false));
  diagnostics.launcher_gameplay.unlock_secret_characters = OnOff(settings.GetBool("eck_unlock_secret_characters", false));
  diagnostics.launcher_gameplay.unlock_race_tracks = OnOff(settings.GetBool("eck_unlock_race_tracks", false));
  diagnostics.launcher_gameplay.frame_rate = FrameRateLabel(frame_rate);
  diagnostics.launcher_gameplay.frame_rate_in_title = OnOff(settings.GetBool("eck_show_fps", true));
  diagnostics.launcher_gameplay.render_resolution_in_title =
      OnOff(settings.GetBool("eck_show_render_resolution", false));
  diagnostics.launcher_gameplay.frame_counter = settings.GetBool("eck_show_frame_counter", false) ? "Shown" : "Hidden";

  diagnostics.launcher_graphics.graphics_card = selected_card;
  diagnostics.launcher_graphics.render_resolution =
      (scale == 1 ? std::string("Original") : std::to_string(scale) + "x") + " (" +
      std::to_string(kGuestWidth * scale) + "x" + std::to_string(kGuestHeight * scale) + ")";
  diagnostics.launcher_graphics.internal_render_resolution = InternalRenderLabel(percent);
  diagnostics.launcher_graphics.anti_aliasing =
      ChoiceLabel({{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA Extreme"}},
                  settings.Get("swap_post_effect", "none"), "Off");
  diagnostics.launcher_graphics.multisampling =
      settings.GetBool("eck_msaa_compatibility", false) ? "4x MSAA (Compatibility)" : "2x MSAA";
  diagnostics.launcher_graphics.texture_filtering =
      TextureFilteringName(settings.Get("anisotropic_override", "-1"));

  diagnostics.system.os = OsDescription();
  diagnostics.system.cpu = CpuDescription();
  diagnostics.system.logical_processors = LogicalProcessorCount();

  const GraphicsAdapterDetails adapter =
      EffectiveGraphicsAdapter(std::atoi(CvarText("d3d12_adapter", "-1").c_str()));
  diagnostics.gpu.resolved = adapter.name;
  diagnostics.gpu.adapter = adapter.index;
  diagnostics.gpu.vendor_id = adapter.vendor_id;
  diagnostics.gpu.device_id = adapter.device_id;
  diagnostics.gpu.resolved_known = adapter.known;

  diagnostics.display.monitor = monitor;
  diagnostics.display.width = display.width;
  diagnostics.display.height = display.height;
  diagnostics.display.refresh_hz = display.refresh_hz;
  diagnostics.display.scale_percent = int(std::lround(display.scale * 100.0f));
  diagnostics.display.fullscreen = window.fullscreen;
  diagnostics.display.configured_width = window.configured_width;
  diagnostics.display.configured_height = window.configured_height;
  diagnostics.display.output_width = window.output_width;
  diagnostics.display.output_height = window.output_height;

  const GameCheck game = CheckGameFiles(game_dir);
  diagnostics.game.title_id = kTitleId;
  diagnostics.game.executable = "default.xex";
  std::error_code size_ec;
  const std::uintmax_t file_size = std::filesystem::file_size(game_dir / "default.xex", size_ec);
  diagnostics.game.size = size_ec ? 0 : file_size;
  diagnostics.game.fingerprint = std::string("version ") + kGameVersion;
  diagnostics.game.status = GameStateName(game.state);
  diagnostics.game.sha256 = game.xex_sha256;

  diagnostics.language.label = language.label;
  diagnostics.language.package = language.package;
  diagnostics.language.user_language = uint32_t(std::atoi(CvarText("user_language", "0").c_str()));
  diagnostics.language.user_country = uint32_t(std::atoi(CvarText("user_country", "0").c_str()));

  diagnostics.gameplay.frame_rate = FrameRateLabel(frame_rate);
  diagnostics.gameplay.present = PresentationText(runtime_tearing);

  diagnostics.graphics.render_width = kGuestWidth * scale;
  diagnostics.graphics.render_height = kGuestHeight * scale;
  diagnostics.graphics.guest_width = kGuestWidth;
  diagnostics.graphics.guest_height = kGuestHeight;
  diagnostics.graphics.render_percent = percent;
  diagnostics.graphics.host_quarters = split.host_quarters;
  diagnostics.graphics.resolve_scale = split.resolve_scale;
  diagnostics.graphics.msaa = settings.GetBool("eck_msaa_compatibility", false) ? "4x compatibility" : "2x native";
  diagnostics.graphics.anti_aliasing = diagnostics.launcher_graphics.anti_aliasing;
  diagnostics.graphics.texture_filtering = diagnostics.launcher_graphics.texture_filtering;
  diagnostics.graphics.render_target_path = CvarText("render_target_path_d3d12", "default");

  diagnostics.input.keyboard_mode = settings.GetBool("mnk_mode", true) ? "On" : "Off";
  diagnostics.input.controller = window.controller;

  for (const std::string& line : FormatStartupDiagnostics(diagnostics)) {
    REXLOG_INFO("{}", line);
  }
  const std::string crash_display =
      std::to_string(display.width) + "x" + std::to_string(display.height) +
      (window.fullscreen ? " | borderless fullscreen" : " | windowed") + " | window=" +
      std::to_string(window.configured_width) + "x" + std::to_string(window.configured_height) + " | output=" +
      std::to_string(window.output_width) + "x" + std::to_string(window.output_height);
  const std::string crash_timing = "frame_rate=" + FrameRateLabel(frame_rate) + " | " + diagnostics.gameplay.present;
  const std::string crash_renderer =
      "render=" + std::to_string(diagnostics.graphics.render_width) + "x" +
      std::to_string(diagnostics.graphics.render_height) + " | internal=" + std::to_string(percent) +
      "% | msaa=" + diagnostics.graphics.msaa + " | anti-aliasing=" + diagnostics.graphics.anti_aliasing +
      " | texture filtering=" + diagnostics.graphics.texture_filtering;
  CrashRuntimeState crash_state;
  crash_state.gpu = diagnostics.gpu.resolved_known ? diagnostics.gpu.resolved.c_str() : "unknown";
  crash_state.selected_adapter = selected_card.c_str();
  crash_state.display = crash_display.c_str();
  crash_state.timing = crash_timing.c_str();
  crash_state.renderer = crash_renderer.c_str();
  crash_state.language = language.key;
  crash_state.online = settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled) ? "enabled" : "disabled";
  SetCrashRuntimeState(crash_state);
}

void GameSettings::MarkWindowCreated(bool launcher_window) {
  const float scale = launcher_window ? MonitorForSetting(0).scale : 1.0f;
  window_created_.clear();
  for (const std::string& key : kWindowKeys) {
    std::string value = rex::cvar::GetFlagByName(key);
    if (launcher_window && (key == "window_width" || key == "window_height")) {
      value = std::to_string(ScaleLogicalToPhysical(std::atoi(value.c_str()), scale));
    }
    window_created_[key] = std::move(value);
  }
}

bool GameSettings::WindowChanged() const {
  for (const std::string& key : kWindowKeys) {
    const auto created = window_created_.find(key);
    const std::string created_value = created == window_created_.end() ? std::string() : created->second;
    if (WindowValue(key, Get(key)) != WindowValue(key, created_value)) {
      return true;
    }
  }
  return false;
}

}  // namespace eck
