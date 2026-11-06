#include "launcher.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/version.h>

#include "trg/launcher.h"
#include "trg/platform.h"

#include "bug_report.h"
#include "dpi.h"
#include "frame_limiter.h"
#include "game_files.h"
#include "graphics_adapters.h"
#include "imgui_gamepad.h"
#include "keybinds.h"
#include "language_setting.h"
#include "launcher_achievements.h"
#include "launcher_quit.h"
#include "log_filter.h"
#include "log_path.h"
#include "online_page.h"
#include "online_runtime.h"
#include "platform.h"
#include "player_names.h"
#include "port_version.h"
#include "settings.h"
#include "system_info.h"
#include "ui_text.h"
#include "window_host.h"
#if defined(_WIN32)
#include "update_config.h"
#include "updater/update_ui.h"
#endif

namespace eck {
namespace {

// Logical pixels.
constexpr std::pair<int, int> kLauncherWindowSize = {1280, 720};
// Share of the primary display's work area the launcher may take.
constexpr double kLauncherMaxShare = 0.9;
// How long the saved message stays on screen before the game starts.
constexpr double kPlayFeedbackSeconds = 0.4;

constexpr ImGuiConfigFlags kNavFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

ImVec4 Rgb(int r, int g, int b) {
  return ImVec4(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, 1.0f);
}

ImVec4 Mixed(const ImVec4& from, const ImVec4& to, float amount) {
  return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
                from.z + (to.z - from.z) * amount, 1.0f);
}

const ImVec4 kAccent = Rgb(0xF2, 0xC3, 0x18);
const ImVec4 kActionBorder = Rgb(0xD9, 0x48, 0x1F);
// Lighter than the logo's #E4572E, which is too dark for text on the blue panels.
const ImVec4 kActionLink = Rgb(0xFF, 0x70, 0x4A);
const ImVec4 kLeaf = ImVec4(0.12f, 0.40f, 0.22f, 1.0f);

constexpr const char* kAuthorUrl = "https://github.com/saulob";
constexpr const char* kRepositoryUrl = "https://github.com/saulob/el-chavo-kart-recompiled";

const char* TranslateLauncherText(const char* english) {
  return Tr(english);
}

trg::Theme LauncherTheme() {
  trg::Theme theme = trg::Theme::Ocean().WithAccent(kAccent);
  theme.accent_hot = Mixed(kAccent, ImVec4(1, 1, 1, 1), 0.10f);
  theme.accent_pressed = Mixed(kAccent, ImVec4(0, 0, 0, 1), 0.20f);
  theme.accent_border = kActionBorder;
  theme.link = kActionLink;
  theme.good = kLeaf;
  return theme;
}

constexpr float kHeaderArtAspect = 1600.0f / 180.0f;

trg::HeaderPainter LauncherHeader(ImTextureID art, float art_aspect) {
  return [art, art_aspect](const trg::HeaderContext& c) {
    c.draw->AddRectFilledMultiColor(c.min, c.max, IM_COL32(18, 40, 92, 255), IM_COL32(28, 82, 150, 255),
                                    IM_COL32(16, 64, 118, 255), IM_COL32(10, 24, 60, 255));
    if (art == ImTextureID{}) {
      return;
    }
    const float height = c.max.y - c.min.y;
    const float width = height * art_aspect;
    const ImVec2 top_left(c.max.x - width, c.min.y);
    c.draw->AddImage(ImTextureRef(art), top_left, c.max);
  };
}

// Optional: the header keeps its gradient when no art is installed.
std::unique_ptr<rex::ui::ImmediateTexture> LoadHeaderArt(rex::ui::ImmediateDrawer* drawer) {
  if (!drawer) {
    return nullptr;
  }
  const std::filesystem::path path =
      rex::filesystem::GetExecutableFolder() / "assets" / "launcher" / "header-art.png";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return nullptr;
  }
  const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  int width = 0;
  int height = 0;
  const std::vector<uint8_t> pixels = rex::ui::DecodeImageRGBA(encoded.data(), encoded.size(), width, height);
  if (pixels.empty() || width <= 0 || height <= 0) {
    REXLOG_WARN("Launcher header art could not be decoded: {}", Utf8(path.filename()));
    return nullptr;
  }
  return drawer->CreateTexture(uint32_t(width), uint32_t(height), rex::ui::ImmediateTextureFilter::kLinear, false,
                               pixels.data());
}

trg::Fonts g_fonts;
float g_font_density = 1.0f;

GameFiles CheckAndLogGameFiles(const std::filesystem::path& game_dir) {
  const GameCheck check = CheckGameFiles(game_dir);
  if (check.state != GameFiles::kReady && check.state != GameFiles::kMissing) {
    REXLOG_WARN("{}", GameFilesLogText(check));
  }
  return check.state;
}

// Written by the install job, read once the Task has finished.
struct InstallOutcome {
  InstallResult result;
  GameFiles files = GameFiles::kMissing;
};

std::string Size(int width, int height) {
  return std::to_string(width) + " \xC3\x97 " + std::to_string(height);
}

std::string DisplayUrl(const char* url) {
  const std::string text = url;
  const size_t scheme = text.find("://");
  return scheme == std::string::npos ? text : text.substr(scheme + 3);
}

std::vector<trg::Option> RenderResolutionOptions() {
  std::vector<trg::Option> options;
  for (int scale = 1; scale <= 4; ++scale) {
    std::string label = scale == 1 ? std::string(Tr("Original")) : std::to_string(scale) + "x";
    label += " \xE2\x80\x94 " + std::to_string(kGuestWidth * scale) + "x" + std::to_string(kGuestHeight * scale);
    if (scale == 2) {
      label += std::string(" \xE2\x80\x94 ") + Tr("Recommended for 1440p");
    }
    options.push_back({std::to_string(scale), label});
  }
  return options;
}

class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, LauncherPaths paths, GameSettings& settings,
                 LauncherCallbacks callbacks)
      : ImGuiDialog(drawer),
        paths_(std::move(paths)),
        cb_(std::move(callbacks)),
        settings_(settings),
        game_files_(CheckAndLogGameFiles(paths_.game_dir)),
        monitors_(ListMonitors()),
        graphics_adapters_(ListGraphicsAdapters()) {
    trg::SetTranslator(&TranslateLauncherText);
    SetActiveUiLanguage(settings_.Get(kLanguageSetting, kDefaultLanguage));
    header_art_ = LoadHeaderArt(paths_.immediate_drawer);
    ConfigureOnline(paths_.user_dir);
    online_page_.SetSaveNow([this](std::string_view key, const std::string& value) {
      return settings_.SaveNow({{std::string(key), value}});
    });
    launcher_ = std::make_unique<trg::Launcher>(MakeConfig());
    StartUpdateCheck();
    if (WindowFullscreen()) {
      restore_when_windowed_ = true;
      RequestFullscreen(false);
    } else {
      StartInSavedState();
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (done_) {
      return;
    }
    SetActiveUiLanguage(settings_.Get(kLanguageSetting, kDefaultLanguage));
    PollInstall();
    PollUpdate();
    TrackPlacement();
    if (!nav_taken_) {
      saved_nav_flags_ = io.ConfigFlags & kNavFlags;
      saved_has_gamepad_ = io.BackendFlags & ImGuiBackendFlags_HasGamepad;
      io.ConfigFlags |= kNavFlags;
      nav_taken_ = true;
    }
    gamepad_.Update(io, ::LocalInputAccepted());
    if (!starting_ && ImGui::IsKeyPressed(ImGuiKey_F11, false) && !fullscreen_gate_.Pending()) {
      ToggleLauncherFullscreen();
    }
    if (starting_ && ImGui::GetTime() - play_started_ >= kPlayFeedbackSeconds && !LeavingFullscreen()) {
      StartGame();
      return;
    }
    if (starting_) {
      // TRG's hotkeys read the same key state, so release it before Frame() runs.
      io.ClearInputKeys();
      io.ClearInputMouse();
    }
    online_page_.SetOwnerWindow(WindowHandle());
    for (const std::string& notice : online_page_.Tick(settings_)) {
      launcher_->SetStatus(notice, 6.0);
    }
    ImGui::BeginDisabled(starting_);
    const trg::Result result = launcher_->Frame();
    ImGui::EndDisabled();
    if (result == trg::Result::kNone && !starting_) {
      DrawUpdateBanner();
    }
    if (starting_) {
      DrawStartingMessage();
    }
    switch (result) {
      case trg::Result::kPlay:
        RequestPlay();
        break;
      case trg::Result::kQuit:
        if (!starting_ && eck::LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
          Finish(cb_.quit);
        }
        break;
      case trg::Result::kNone:
        break;
    }
  }

  // Every PLAY (mouse, Enter, Xbox Start) ends here. The one save happens here; StartGame never saves.
  void RequestPlay() {
    if (starting_ || !GameCanStart(game_files_, install_.running())) {
      return;
    }
    if (!launcher_->Save()) {
      return;
    }
    starting_ = true;
    play_started_ = ImGui::GetTime();
    launcher_->SetStatus("", 0.0);
  }

  void StartGame() {
    // The game builds its own runtime next; the one that read the achievements must be gone.
    achievements_.reset();
    achievements_tried_ = false;
    if (cb_.window_changed && cb_.window_changed()) {
      if (cb_.restart && cb_.restart()) {
        Finish(nullptr);
        return;
      }
      starting_ = false;
      launcher_->SetStatus("Could not start the game in a new window. Press Play to try again.", 8);
      return;
    }
    RestoreStartupWindow();
    Finish(cb_.play);
  }

  // Drawn outside the disabled frame so the message keeps full brightness.
  void DrawStartingMessage() {
    const char* text = Tr("Settings saved. Starting game...");
    ImFont* font = g_fonts.semibold ? g_fonts.semibold : ImGui::GetFont();
    const float size = g_fonts.size * 1.25f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - text_size.x) * 0.5f,
                     vp->Pos.y + (vp->Size.y - text_size.y) * 0.5f);
    ImGui::GetForegroundDrawList()->AddText(font, size, pos, ImGui::GetColorU32(kAccent), text);
  }

  // Runs inside the last frame; the in-game overlays share this ImGui context.
  void OnClose() override {
    ReleaseUpdater();
    ImGuiIO& io = GetIO();
    gamepad_.Release(io);
    io.ConfigFlags = (io.ConfigFlags & ~kNavFlags) | saved_nav_flags_;
    io.BackendFlags = (io.BackendFlags & ~ImGuiBackendFlags_HasGamepad) | saved_has_gamepad_;
  }

 private:
  void Finish(std::function<void()> action) {
    done_ = true;
    ReleaseUpdater();
    if (!skip_finish_save_) {
      SavePlacement(placement_.Flush());
    }
    REXLOG_INFO("Launcher closed");
    Close();
    if (action) {
      action();
    }
  }

#if defined(_WIN32)
  void StartUpdateCheck() {
    updater_ = std::make_unique<trg::update::UpdateController>(MakeUpdateConfig(PortFolder()));
    updater_->StartCheck();
  }

  void PollUpdate() {
    if (updater_) {
      updater_->Poll();
    }
  }

  void ReleaseUpdater() {
    if (!updater_) {
      return;
    }
    updater_->Cancel();
    if (updater_->Busy()) {
      // Do not join a network call on the UI thread when the launcher leaves.
      updater_.release();
    }
  }

  void DrawUpdateBanner() {
    if (!updater_) {
      return;
    }
    switch (trg::update::DrawUpdateBanner(updater_->snapshot(), launcher_->config().theme, g_fonts)) {
      case trg::update::UpdateUiAction::kDownload:
        updater_->StartDownload();
        break;
      case trg::update::UpdateUiAction::kRetry:
        if (updater_->RetryNeedsInstall()) {
          RestartToUpdate([this] { return updater_->Retry(); });
        } else {
          updater_->Retry();
        }
        break;
      case trg::update::UpdateUiAction::kRestart:
        RestartToUpdate([this] { return updater_->BeginInstall(); });
        break;
      case trg::update::UpdateUiAction::kNone:
        break;
    }
  }

  template <typename Begin>
  void RestartToUpdate(Begin begin) {
    if (install_.running()) {
      launcher_->SetStatus("Wait for the current install to finish.", 5.0);
      return;
    }
    if (!launcher_->Save()) {
      return;
    }
    SavePlacement(placement_.Flush());
    if (begin()) {
      skip_finish_save_ = true;
      Finish(cb_.quit);
    }
  }
#else
  void StartUpdateCheck() {}
  void PollUpdate() {}
  void ReleaseUpdater() {}
  void DrawUpdateBanner() {}
#endif

  void* WindowHandle() const { return cb_.window_handle ? cb_.window_handle() : nullptr; }

  bool WindowFullscreen() const { return cb_.window_fullscreen && cb_.window_fullscreen(); }

  void StartInSavedState() {
    RestorePlacement();
    if (fullscreen_pref_.wanted()) {
      RequestFullscreen(true);
    }
  }

  void ToggleLauncherFullscreen() {
    const bool fullscreen = !WindowFullscreen();
    RequestFullscreen(fullscreen);
    fullscreen_pref_.UserSet(fullscreen);
    if (!settings_.SaveNow(LauncherFullscreenValues(fullscreen))) {
      REXLOG_WARN("Could not save the launcher fullscreen state");
    }
  }

  void RequestFullscreen(bool fullscreen) {
    fullscreen_gate_.Request(fullscreen);
    if (cb_.set_window_fullscreen) {
      cb_.set_window_fullscreen(fullscreen);
    }
  }

  // The game gets the window out of the launcher's temporary fullscreen.
  bool LeavingFullscreen() {
    if (!WindowFullscreen() || leave_gave_up_) {
      return false;
    }
    if (!leave_requested_) {
      leave_requested_ = true;
      leave_started_ = ImGui::GetTime();
      RequestFullscreen(false);
    }
    leave_gave_up_ = ImGui::GetTime() - leave_started_ >= FullscreenGate::kTimeoutSeconds;
    return !leave_gave_up_;
  }

  void RestorePlacement() {
    if (const auto placed = TakePreShowResult()) {
      startup_ = placed->startup;
      placement_ = PlacementTracker(placed->applied);
      return;
    }
    void* window = WindowHandle();
    const auto state = QueryWindowState(window);
    if (!state || WindowFullscreen()) {
      return;
    }
    const WindowPlacement saved = SavedLauncherPlacement();
    const bool restorable = PlacementIsVisible(saved, WorkAreas());
    startup_ = WindowPlacement{state->rect, state->maximized};
    placement_ = PlacementTracker(restorable ? std::optional<WindowPlacement>(saved) : std::nullopt);
    if (restorable) {
      ApplyWindowPlacement(window, saved);
    }
  }

  void TrackPlacement() {
    const double now = ImGui::GetTime();
    const FullscreenGate::Step step = fullscreen_gate_.Update(WindowFullscreen(), now);
    if (restore_when_windowed_) {
      if (!WindowFullscreen() && !fullscreen_gate_.Pending()) {
        restore_when_windowed_ = false;
        StartInSavedState();
      }
      return;
    }
    if (!startup_) {
      return;
    }
    if (step.restore) {
      if (const auto current = placement_.Current()) {
        ApplyWindowPlacement(WindowHandle(), *current);
      }
    }
    if (!step.observe) {
      return;
    }
    if (const auto state = QueryWindowState(WindowHandle())) {
      placement_.Observe(*state, WindowFullscreen(), now);
      SavePlacement(placement_.TakeSettled(now));
    }
  }

  void SavePlacement(const std::optional<WindowPlacement>& placement) {
    if (placement && !settings_.SaveNow(PlacementValues(*placement))) {
      REXLOG_WARN("Could not save the launcher window placement");
    }
  }

  // The game keeps the window it was created with, not the launcher's saved placement.
  void RestoreStartupWindow() {
    if (startup_ && !WindowFullscreen()) {
      ApplyWindowPlacement(WindowHandle(), *startup_);
    }
  }

  trg::LauncherConfig MakeConfig() {
    trg::LauncherConfig c;
    c.settings = &settings_;
    // PLAY saves in RequestPlay, so a failed save keeps the launcher open.
    c.save_on_play = false;
    c.theme = LauncherTheme();
    c.note = "Play applies the changes. Save keeps them without starting.";
    c.hints = "Enter  Play     Alt+F4  Quit     Ctrl+S  Save";
    c.fonts = g_fonts;
    c.branding.title = "EL CHAVO KART";
    c.branding.subtitle_segments = {
        {"PC PORT", ""},
        {"   \xC2\xB7   ", ""},
        {"v" + PortVersion() + "   \xC2\xB7   ", ""},
        {"BY SAULOB", kAuthorUrl},
    };
    const float art_aspect =
        header_art_ ? float(header_art_->width) / float(header_art_->height) : kHeaderArtAspect;
    c.branding.background = LauncherHeader(header_art_ ? reinterpret_cast<ImTextureID>(header_art_.get())
                                                       : ImTextureID{},
                                           art_aspect);
    c.pages = {
        {"Play", "Check the game files and start playing.", [this](trg::Ui& ui) { PagePlay(ui); },
         [this] { return game_files_ != GameFiles::kReady; }},
        {"Display", "Window, monitor and VSync.", [this](trg::Ui& ui) { PageDisplay(ui); }},
        {"Graphics", "Graphics card, render resolution, anti-aliasing and texture filtering.",
         [this](trg::Ui& ui) { PageGraphics(ui); }},
        {"Gameplay", "Language, and what the window shows while you play.", [](trg::Ui& ui) { PageGameplay(ui); }},
        {"Achievements", "Your achievements on this PC.", [this](trg::Ui& ui) { PageAchievements(ui); }},
        {"Online", "Online play, friends and the connection test.", [this](trg::Ui& ui) { online_page_.Draw(ui); },
         [this] { return online_page_.NeedsAttention(); }},
        {"Controls", "Controller and keyboard.", [](trg::Ui& ui) { PageControls(ui); }},
        {"About", "About this port, and where your files and settings live.",
         [this](trg::Ui& ui) { PageAbout(ui); }},
    };
    c.can_play = [this] {
      if (install_.running()) {
        return trg::PlayCheck{false, "Wait for the install to finish.", 0};
      }
      if (GameCanStart(game_files_, false)) {
        if (online_page_.PlayNeedsTest(settings_)) {
          return trg::PlayCheck{false, kConnectionTestRequired, launcher_ ? launcher_->FindPage("Online") : 0};
        }
        return trg::PlayCheck{};
      }
      return trg::PlayCheck{false, "The game files are not ready: see the Play page.", 0};
    };
    return c;
  }

  void PagePlay(trg::Ui& ui) {
    const std::string dir = Utf8(paths_.game_dir);
    if (install_.running()) {
      ui.StatusCard(trg::Status::kBusy, "Installing game files", nullptr, std::max(0.0f, install_.fraction()));
    } else {
      switch (game_files_) {
        case GameFiles::kReady:
          ui.StatusCard(trg::Status::kReady, "Ready to play", dir.c_str());
          break;
        case GameFiles::kUnsupported:
          ui.StatusCard(trg::Status::kAttention, "Different version of the game",
                        DescribeInstall(InstallStatus::kWrongVersion).text);
          break;
        case GameFiles::kIncomplete:
          ui.StatusCard(trg::Status::kAttention, "Game files incomplete",
                        "Some game files are missing or damaged. Install the game again from your disc image.");
          break;
        case GameFiles::kMissing:
          ui.StatusCard(trg::Status::kAttention, "Game files needed",
                        "This port requires your own El Chavo Kart Xbox 360 disc image.");
          break;
      }
    }
    if (!install_failure_.empty()) {
      DrawFailure(ui, install_failure_, install_failure_text_);
    }
    if (paths_.game_dir.empty()) {
      ui.Help("No game folder was given: start the game with --game_data_root.");
    } else {
      ui.Row("Game disc image",
             "El Chavo Kart (USA), title 475807D7. No game files are included with this port; the disc's files "
             "(about 2 GB) are copied next to the program.");
      if (!ui.TaskProgress(install_)) {
        const bool clicked = game_files_ == GameFiles::kReady
                                 ? ImGui::Button(Tr("Reinstall from disc image..."), ImVec2(-FLT_MIN, 0))
                                 : ui.AccentButton("Install from disc image...");
        if (clicked) {
          BrowseAndInstall();
        }
      }
    }
    ui.LauncherVisibilityRow("eck_launcher");
    ui.EndRows();
  }

  static void DrawFailure(trg::Ui& ui, const std::string& title, const std::string& text) {
    ui.Spacer(4);
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
    ImGui::TextUnformatted(Tr(title).c_str());
    ImGui::PopStyleColor();
    ui.Help(text.c_str());
    ui.Spacer(4);
  }

  void BrowseAndInstall() {
    PollInstall();
    if (install_.running()) {
      return;
    }
    const std::string picked =
        trg::BrowseForFile(Tr("Select your El Chavo Kart Xbox 360 disc image"), {{Tr("Xbox 360 disc image"), "*.iso"}});
    if (picked.empty()) {
      return;
    }
    install_failure_.clear();
    install_failure_text_.clear();
    achievements_.reset();
    achievements_tried_ = false;
    outcome_ = std::make_shared<InstallOutcome>();
    const bool started = install_.Start(
        "Reading disc image...",
        [outcome = outcome_, image = PathFromUtf8(picked), game_dir = paths_.game_dir,
         user_dir = paths_.user_dir](trg::Task& task) {
          InstallHooks hooks;
          hooks.user_data = user_dir;
          hooks.phase = [&task](const char* text) {
            task.SetLabel(text);
            task.Progress(0, 0);
          };
          hooks.progress = [&task](uint64_t done, uint64_t total) {
            task.Progress(static_cast<long long>(done), static_cast<long long>(total));
            return !task.cancelled();
          };
          outcome->result = InstallFromDiscImage(image, game_dir, hooks);
          if (outcome->result.ok()) {
            outcome->files = CheckGameFiles(game_dir).state;
          }
          return outcome->result.ok() ? std::string() : std::string(DescribeInstall(outcome->result.status).title);
        });
    if (started) {
      REXLOG_INFO("Installing game files from {}", Utf8(std::filesystem::path(PathFromUtf8(picked)).filename()));
    }
  }

  // Runs every frame so a finished install is picked up whichever page is open.
  void PollInstall() {
    std::string error;
    if (install_.Finish(&error) == trg::Task::State::kIdle) {
      return;
    }
    const std::shared_ptr<InstallOutcome> outcome = std::move(outcome_);
    if (!outcome) {
      return;
    }
    const InstallResult& result = outcome->result;
    const InstallMessage message = DescribeInstall(result.status);
    if (result.ok() || result.status == InstallStatus::kCancelled) {
      if (result.ok()) {
        game_files_ = outcome->files;
        REXLOG_INFO("Game files installed: {} files, {} bytes", result.files, result.bytes);
      } else {
        REXLOG_INFO("Game install cancelled");
      }
      launcher_->SetStatus(Tr(std::string(message.title)) + ". " + Tr(std::string(message.text)), 6.0);
      return;
    }
    REXLOG_WARN("Game install failed: {} ({})", message.title, result.detail);
    install_failure_ = message.title;
    install_failure_text_ = message.text;
    launcher_->SetStatus(Tr(std::string("Install failed:")) + " " + Tr(install_failure_), 8.0);
  }

  void PageDisplay(trg::Ui& ui) {
    ui.Toggle("Display mode",
              "How the game starts. Borderless Fullscreen fills the monitor. F11 switches while playing.", "fullscreen",
              false, "Windowed", "Borderless Fullscreen");
    ui.Row("Window size", "The game window's size in Windowed mode.");
    WindowSizeCombo();
    ui.Row("Monitor", "The display the game opens on.");
    MonitorCombo();
    ui.Row("VSync", "On keeps the picture free of tearing. Off allows tearing for G-Sync/FreeSync displays.");
    {
      const bool tearing = settings_.GetBool(kTearingSetting, false);
      if (int i = ui.Segmented("vsync", {"Off", "On"}, tearing ? 0 : 1); i >= 0) {
        settings_.SetBool(kTearingSetting, i == 0);
      }
    }
  }

  const MonitorInfo* SelectedMonitor() const {
    const int index = settings_.GetInt("monitor", 0);
    if (index >= 1 && index <= int(monitors_.size())) {
      return &monitors_[size_t(index - 1)];
    }
    for (const MonitorInfo& m : monitors_) {
      if (m.primary) {
        return &m;
      }
    }
    return monitors_.empty() ? nullptr : &monitors_.front();
  }

  static bool WindowSizeFits(int width, int height, const MonitorInfo* monitor) {
    return !monitor || FitClientToWorkArea({width, height}, {monitor->work_width, monitor->work_height},
                                           WindowFrameForScale(monitor->scale)) == DpiWindowSize{width, height};
  }

  void WindowSizeCombo() {
    const int width = settings_.GetInt("window_width", 0);
    const int height = settings_.GetInt("window_height", 0);
    const MonitorInfo* monitor = SelectedMonitor();
    auto label = [](int w, int h) {
      return w <= 0 || h <= 0 ? std::string(Tr("Default")) : Size(w, h);
    };
    if (ImGui::BeginCombo("##window_size", label(width, height).c_str())) {
      for (const WindowSize& size : kWindowSizes) {
        const bool current = size.width == width && size.height == height;
        if (!current && !WindowSizeFits(size.width, size.height, monitor)) {
          continue;
        }
        if (ImGui::Selectable(label(size.width, size.height).c_str(), current) && !current) {
          settings_.SetInt("window_width", size.width);
          settings_.SetInt("window_height", size.height);
        }
      }
      ImGui::EndCombo();
    }
  }

  void MonitorCombo() {
    const int current = settings_.GetInt("monitor", 0);
    auto label = [this](int index) {
      if (index <= 0) {
        return std::string(Tr("Default"));
      }
      const std::string name = std::string(Tr("Display")) + " " + std::to_string(index);
      if (index > int(monitors_.size())) {
        return name + " " + Tr("(not connected)");
      }
      const MonitorInfo& m = monitors_[size_t(index - 1)];
      return name + (m.primary ? std::string(" ") + Tr("(primary)") : std::string()) + "   " + Size(m.width, m.height);
    };
    if (ImGui::BeginCombo("##monitor", label(current).c_str())) {
      for (int i = 1; i <= int(monitors_.size()); ++i) {
        if (ImGui::Selectable(label(i).c_str(), i == current) && i != current) {
          settings_.SetInt("monitor", i);
        }
      }
      ImGui::EndCombo();
    }
  }

  void PageGraphics(trg::Ui& ui) {
    if (const std::optional<int> adapter =
            ResolveGraphicsAdapter(settings_.Get("eck_graphics_adapter"), graphics_adapters_)) {
      const std::string value = std::to_string(*adapter);
      settings_.Set("eck_graphics_adapter", value);
      std::vector<trg::Option> adapters;
      for (const auto& [option_value, label] : GraphicsAdapterOptions(graphics_adapters_)) {
        adapters.push_back({option_value, label});
      }
      ui.Combo("Graphics card",
               "Selects which GPU renders the game. If the selected GPU is no longer available, the first available "
               "GPU is used.",
               "eck_graphics_adapter", value.c_str(), adapters);
    }
    ui.Combo("Render resolution",
             "Draws the game at a multiple of its own picture, then scales it to the window. Higher is sharper and "
             "costs more GPU time. Window size is separate.",
             "resolution_scale", "1", RenderResolutionOptions());
    ui.Combo("Internal render resolution",
             "Renders the game internally at a lower resolution, then scales it to the selected render resolution. "
             "Lower values improve GPU performance but reduce image sharpness.",
             "eck_internal_render_scale", "100",
             {{"100", "100% Native"}, {"75", "75% Performance"}, {"50", "50% Low-end"}});
    ui.Choice("Anti-aliasing", "Smooths jagged edges after the frame is drawn. Extreme is softer but cleaner.",
              "swap_post_effect", "none", {{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA Extreme"}});
    ui.Toggle("Multisampling",
              "Use 2\xC3\x97 MSAA for best performance. Try 4\xC3\x97 only if you see graphics problems with 2\xC3\x97.",
              "eck_msaa_compatibility", false, "2\xC3\x97 MSAA", "4\xC3\x97 MSAA (Compatibility)");
    ui.Choice("Texture filtering",
              "Anisotropic filtering keeps textures sharp at oblique angles. Game leaves it to the game.",
              "anisotropic_override", "-1",
              {{"-1", "Game"},
               {"0", "Off"},
               {"2", "2\xC3\x97"},
               {"3", "4\xC3\x97"},
               {"4", "8\xC3\x97"},
               {"5", "16\xC3\x97"}},
              6);
  }

  static void PageGameplay(trg::Ui& ui) {
    std::vector<trg::Option> languages;
    for (const GameLanguage& language : kGameLanguages) {
      languages.push_back({language.key, language.label});
    }
    ui.Combo(kLanguageLabel, kLanguageHelp, kLanguageSetting, kDefaultLanguage, languages);
    ui.Toggle("Skip intro", "Start directly at the title screen", "eck_skip_intro", false, "Off", "On");
    ui.Toggle("Unlock secret characters",
              "Every character can be selected while this is on. Your save and achievements do not change.",
              "eck_unlock_secret_characters", false, "Off", "On");
    ui.Toggle("Unlock race tracks",
              "Every race track can be selected in a free race while this is on. Your save and achievements do not change.",
              "eck_unlock_race_tracks", false, "Off", "On");
    std::vector<trg::Option> frame_rates;
    for (auto& [value, label] : FrameRateChoices()) {
      frame_rates.push_back({std::move(value), std::move(label)});
    }
    ui.Choice("Frame rate",
              "The maximum frame rate the game renders. The game's clock follows real time, so game speed does not "
              "change. 30 is the console rate. Unlimited stops at 300.",
              "eck_frame_rate", "60", frame_rates);
    ui.Toggle("Frame rate in title",
              "Shows the game's frame rate in the window title: El Chavo Kart Recompiled | FPS: 60.00", "eck_show_fps",
              true, "Off", "On");
    ui.Toggle("Render resolution in title", "Shows the game's render resolution in the window title",
              "eck_show_render_resolution", false, "Off", "On");
    ui.Toggle("Frame counter", "Shows the game's frame rate in the corner. F3 toggles it while playing.",
              "eck_show_frame_counter", false, "Hidden", "Shown");
    if (ui.Section("Local player names", true)) {
      ui.Help(kPlayerNamesHelp);
      for (const PlayerNameSetting& player : kPlayerNames) {
        PlayerNameRow(ui, player);
      }
    }
  }

  static void PlayerNameRow(trg::Ui& ui, const PlayerNameSetting& player) {
    static std::map<std::string, PlayerNameResult> blocked;
    struct Edit {
      std::string previous;
      PlayerNameResult blocked = PlayerNameResult::Ok;
    } edit{ui.settings().Get(player.key)};
    const auto reject_invalid = [](ImGuiInputTextCallbackData* data) {
      auto* state = static_cast<Edit*>(data->UserData);
      const std::string kept = rex::system::xam::AcceptUserDisplayNameEdit(
          state->previous, {data->Buf, size_t(data->BufTextLen)}, &state->blocked);
      if (state->blocked != PlayerNameResult::Ok) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, kept.c_str());
      }
      return 0;
    };
    ui.Row(player.label, kPlayerNameHint);
    char text[64];
    std::snprintf(text, sizeof text, "%s", edit.previous.c_str());
    ImGui::PushID(player.key);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##text", player.default_name, text, sizeof text, ImGuiInputTextFlags_CallbackEdit,
                                 reject_invalid, &edit)) {
      ui.settings().Set(player.key, text);
      blocked[player.key] = edit.blocked;
    }
    ImGui::PopID();
    PlayerNameResult shown = blocked[player.key];
    if (shown == PlayerNameResult::Ok) {
      shown = CheckPlayerName(ui.settings().Get(player.key));
    }
    if (shown != PlayerNameResult::Ok) {
      ui.Help(PlayerNameMessage(shown));
    }
  }

  void PageAchievements(trg::Ui& ui) {
    // An open icon file would keep a reinstall from swapping the game folder.
    if (!GameCanStart(game_files_, install_.running())) {
      ui.Paragraph("Install the game to see its achievements.");
      return;
    }
    const uint32_t language = LanguageFor(settings_.Get(kLanguageSetting, kDefaultLanguage)).xconfig_language;
    if (achievements_ && achievements_->language() != language) {
      achievements_.reset();
      achievements_tried_ = false;
    }
    if (!achievements_ && !achievements_tried_) {
      achievements_tried_ = true;
      achievements_ = std::make_unique<LauncherAchievements>(paths_.game_dir, paths_.user_dir, language,
                                                             paths_.immediate_drawer);
    }
    if (!achievements_ || !achievements_->ok()) {
      ui.Paragraph("The achievements could not be read from the game files.");
      return;
    }
    DrawAchievementSummary(ui, Summarize(achievements_->entries()));
    std::vector<trg::AchievementCard> cards;
    for (const AchievementEntry& entry : achievements_->entries()) {
      trg::AchievementCard card;
      card.icon = achievements_->Icon(entry.info);
      card.title = entry.info.label;
      card.description = EntryDescription(entry);
      card.points = static_cast<int>(entry.info.gamerscore);
      card.unlocked = entry.unlocked;
      cards.push_back(std::move(card));
    }
    ui.AchievementGrid(cards, 1);
  }

  static void DrawAchievementSummary(trg::Ui& ui, const AchievementSummary& summary) {
    ImGui::PushFont(ui.fonts().semibold, 0.0f);
    ImGui::Text(Tr("%d / %d unlocked"), summary.unlocked, summary.total);
    ImGui::SameLine();
    ImGui::TextColored(kAccent, "%dG / %dG", summary.earned_gamerscore, summary.total_gamerscore);
    ImGui::PopFont();
    const float fraction = summary.total > 0 ? float(summary.unlocked) / float(summary.total) : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 6.0f * ui.scale()), "");
    ImGui::PopStyleColor();
    ui.Spacer(8);
  }

  static void PageControls(trg::Ui& ui) {
    ui.Info("Controller", "Automatic: Xbox controllers work without setup.");
    ui.Toggle("Keyboard", "Use the keyboard for gameplay controls together with the controller.", "mnk_mode", true,
              "Off", "On");
    if (ui.ConfirmRow("Keyboard defaults", "Puts the keyboard controls back to their standard layout.",
                      "Reset to defaults", "Put every key back to its default?", "Reset")) {
      ResetKeyboardDefaults(ui.settings());
      ui.SetStatus("Keyboard controls reset to defaults.");
    }
    if (ui.Section("Keyboard controls", true)) {
      ui.Help("Click a key, then press the new one. Esc cancels.");
      const bool enabled = ui.settings().GetBool("mnk_mode", true);
      if (!enabled) {
        ImGui::BeginDisabled();
      }
      for (const KeyboardAction& action : kKeyboardActions) {
        KeyboardRow(ui, action);
      }
      if (!enabled) {
        ImGui::EndDisabled();
      }
    }
  }

  static void KeyboardRow(trg::Ui& ui, const KeyboardAction& action) {
    const std::string shown = KeyboardBindingLabel(SavedKeyboardBinding(ui.settings(), action));
    const std::optional<ImGuiKey> key = ui.KeyBindRow(action.label, nullptr, shown);
    if (!key) {
      return;
    }
    if (*key == ImGuiKey_Enter || *key == ImGuiKey_KeypadEnter) {
      // The launcher's Play hotkey reads Enter later in this frame.
      ImGui::SetKeyOwner(*key, ImGuiKeyOwner_Any, ImGuiInputFlags_LockThisFrame);
    }
    const ImGuiIO& io = ImGui::GetIO();
    const std::string binding = KeyboardBindingFor(*key, io.KeyShift, io.KeyCtrl, io.KeyAlt);
    if (binding.empty()) {
      ui.SetStatus("That key cannot be used.");
      return;
    }
    ui.settings().Set(action.key, binding);
  }

  void PageAbout(trg::Ui& ui) {
    ui.Paragraph(
        "El Chavo Kart for Xbox 360, statically recompiled to run natively on PC with the ReXGlue SDK. No game code "
        "or assets are included: the game runs from your own game files.");
    ui.Spacer(4);
    if (!paths_.game_dir.empty()) {
      ui.FolderRow("Game files", "Where the game data is read from.", "Open game folder", Utf8(paths_.game_dir));
    }
    ui.FolderRow("Saves and settings", UserDataFolderHelp(paths_.user_data_outcome), "Open data folder",
                 Utf8(paths_.user_dir));
    if (ui.ButtonRow("Settings file", "Your launcher choices, as plain text. Loaded after el_chavo_kart.toml.",
                     "Show settings file") &&
        launcher_->Save()) {
      std::error_code ec;
      const bool exists = std::filesystem::exists(paths_.settings_file, ec);
      trg::OpenInFileManager(Utf8(exists ? paths_.settings_file : paths_.user_dir));
    }
    ui.ResetAllRow();
    ui.Heading("Diagnostics");
    if (ui.Toggle("Detailed logging", "Keeps logs for normal sessions. Crash diagnostics are always preserved.",
                  "detailed_logging", true)) {
      rex::cvar::SetFlagByName("detailed_logging", settings_.Get("detailed_logging", "true"));
      eck::ApplyDetailedLogging();
    }
    if (ui.ButtonRow("Logs folder", "Open the folder containing session and crash logs", "Open logs folder")) {
      trg::OpenInFileManager(Utf8(ReadyLogsFolder()));
    }
    if (ui.ButtonRow("Bug report",
                     "Shows the newest log and opens the project's issue page. Nothing is uploaded: check the log, "
                     "then attach it yourself.",
                     "Report a bug")) {
      ReportBug();
    }
    ui.Info("Port version", PortVersion().c_str());
    ui.Info("Libraries", "ReXGlue SDK " REXGLUE_VERSION_STRING "   \xC2\xB7   TRG Launcher " ECK_TRG_VERSION);
    ui.Info("Game", "El Chavo Kart (USA), Xbox 360 disc, title 475807D7, game version 0.0.0.3");
    ui.Link(DisplayUrl(kRepositoryUrl).c_str(), kRepositoryUrl);
  }

  std::filesystem::path ReadyLogsFolder() {
    std::error_code ec;
    std::filesystem::path folder = paths_.logs_dir;
    std::filesystem::create_directories(folder, ec);
    if (!std::filesystem::is_directory(folder, ec)) {
      REXLOG_WARN("Could not create the logs folder: {}", ec.message());
      folder = folder.parent_path();
    }
    return folder;
  }

  void ReportBug() {
    const std::filesystem::path folder = ReadyLogsFolder();
    const std::filesystem::path log = BugReportLog(folder, paths_.active_log, kLogName);
    trg::OpenInFileManager(Utf8(log.empty() ? folder : log));
    trg::OpenUrl(BugReportUrl(kRepositoryUrl, PortVersion(), OsDescription()));
  }

  LauncherPaths paths_;
  LauncherCallbacks cb_;
  GameSettings& settings_;
  GameFiles game_files_;
  std::vector<MonitorInfo> monitors_;
  std::vector<GraphicsAdapter> graphics_adapters_;
  std::unique_ptr<rex::ui::ImmediateTexture> header_art_;
  std::unique_ptr<trg::Launcher> launcher_;
  ImGuiGamepad gamepad_;
  bool nav_taken_ = false;
  bool starting_ = false;
  double play_started_ = 0.0;
  ImGuiConfigFlags saved_nav_flags_ = 0;
  ImGuiBackendFlags saved_has_gamepad_ = 0;
  bool done_ = false;
  bool skip_finish_save_ = false;
#if defined(_WIN32)
  std::unique_ptr<trg::update::UpdateController> updater_;
#endif
  std::optional<WindowPlacement> startup_;
  PlacementTracker placement_;
  FullscreenGate fullscreen_gate_;
  LauncherFullscreenPreference fullscreen_pref_{SavedLauncherFullscreen()};
  bool restore_when_windowed_ = false;
  bool leave_requested_ = false;
  bool leave_gave_up_ = false;
  double leave_started_ = 0.0;
  std::string install_failure_;
  std::string install_failure_text_;
  std::shared_ptr<InstallOutcome> outcome_;
  trg::Task install_;
  OnlinePage online_page_;
  std::unique_ptr<LauncherAchievements> achievements_;
  bool achievements_tried_ = false;
};

}  // namespace

void LoadLauncherFonts(ImFontAtlas* atlas, float density) {
  // Latin-1 plus General Punctuation.
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x205E, 0};
  density = ValidDpiScale(density);
  g_font_density = density;
  ImFontConfig config;
  config.RasterizerDensity = density;
  auto load = [&](const std::filesystem::path& path) -> ImFont* {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
      return nullptr;
    }
    return atlas->AddFontFromFileTTF(Utf8(path).c_str(), g_fonts.size, &config, kRanges);
  };
  const UiFontFiles fonts = FindUiFonts();
  g_fonts.regular = load(fonts.regular);
  g_fonts.semibold = load(fonts.semibold);
  g_fonts.bold = load(fonts.bold);
}

void UpdateLauncherFonts(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer, float density) {
  if (!drawer || !immediate_drawer || !g_fonts.regular) {
    return;
  }
  density = ValidDpiScale(density);
  if (std::abs(density - g_font_density) < 0.001f) {
    return;
  }

  ImGuiIO& io = drawer->GetIO();
  for (ImFont* font : {g_fonts.regular, g_fonts.semibold, g_fonts.bold}) {
    if (!font) {
      continue;
    }
    const ImWchar fallback = font->FallbackChar;
    const ImWchar ellipsis = font->EllipsisChar;
    font->ClearOutputData();
    font->FallbackChar = fallback;
    font->EllipsisChar = ellipsis;
    font->CurrentRasterizerDensity = density;
    for (ImFontConfig* source : font->Sources) {
      source->RasterizerDensity = density;
    }
  }
  io.Fonts->TexIsBuilt = false;
  // The drawer holds the uploaded atlas until the immediate drawer is replaced.
  drawer->SetImmediateDrawer(nullptr);
  drawer->SetImmediateDrawer(immediate_drawer);
  g_font_density = density;
}

ImFont* LauncherRegularFont() {
  return g_fonts.regular;
}

ImFont* LauncherSemiboldFont() {
  return g_fonts.semibold;
}

namespace {
bool g_skipped_once = false;
}  // namespace

bool ShouldShowLauncher(const std::filesystem::path& game_dir) {
  g_skipped_once = REXCVAR_GET(eck_skip_launcher);
  rex::cvar::ResetToDefault("eck_skip_launcher");
  return LauncherWanted(CheckAndLogGameFiles(game_dir) == GameFiles::kReady, g_skipped_once, ShiftHeld(),
                        REXCVAR_GET(eck_launcher));
}

bool ShiftHeldAfterWindowOpened() {
#if defined(_WIN32)
  return false;
#else
  return !g_skipped_once && ShiftHeld();
#endif
}

namespace {

std::pair<int, int> LauncherWindowSize() {
  const MonitorInfo primary = MonitorForSetting(1);
  if (primary.width == 0) {
    return kLauncherWindowSize;
  }
  const DpiWindowSize size =
      FitWindowToWorkArea(kLauncherWindowSize.first, kLauncherWindowSize.second, primary.work_width,
                          primary.work_height, primary.scale, kLauncherMaxShare);
  return {size.width, size.height};
}

}  // namespace

void PrepareLauncherWindow() {
  const auto [width, height] = LauncherWindowSize();
  rex::cvar::SetFlagByName("fullscreen", "false");
  rex::cvar::SetFlagByName("window_width", std::to_string(width));
  rex::cvar::SetFlagByName("window_height", std::to_string(height));
  // Monitor 0 lets SDL centre the window on the primary display, whatever the display order.
  rex::cvar::SetFlagByName("monitor", "0");
  const WindowPlacement saved = SavedLauncherPlacement();
  InstallPreShowPlacement(PlanPreShow(saved, WorkAreas()), saved);
}

void ShowLauncher(rex::ui::ImGuiDrawer* drawer, LauncherPaths paths, GameSettings& settings,
                  LauncherCallbacks callbacks) {
  new LauncherDialog(drawer, std::move(paths), settings, std::move(callbacks));
}

}  // namespace eck
