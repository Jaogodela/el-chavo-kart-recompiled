#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/version.h>
#include <rex/system.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/flags.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include "achievements_menu.h"
#include "achievements_overlay.h"
#include "active_log.h"
#include "crash_report.h"
#include "debug_telemetry.h"
#include "diagnostics.h"
#include "frame_counter.h"
#include "online_menu_overlay.h"
#include "online_runtime.h"
#include "ping_display.h"
#include "game_files.h"
#include "gpu_plugin_check.h"
#include "guest_functions.h"
#include "hard_exit.h"
#include "input_focus.h"
#include "input_summary.h"
#include "launcher.h"
#include "log_filter.h"
#include "log_path.h"
#include "main_menu_hooks.h"
#include "online_game_hook.h"
#include "modal_dialog.h"
#include "platform.h"
#include "port_version.h"
#include "session_log.h"
#include "settings.h"
#include "system_info.h"
#include "trg/game_helpers.h"
#include "user_data.h"
#include "window_host.h"

class ElChavoKartApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    eck::SetApplicationId();
    eck::InstallGuestFunctionTable(PPCImageConfig.func_mappings);
    return std::unique_ptr<ElChavoKartApp>(new ElChavoKartApp(ctx, "el_chavo_kart", PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    shipped_config_ = paths.config_path;
    const std::filesystem::path port_folder = eck::PortFolder();
    if (paths.game_data_root.empty()) {
      paths.game_data_root = eck::DefaultGameFolder(port_folder);
    }
    ChooseUserData(paths, port_folder);
    if (const auto inherited =
            eck::TakeInheritedSessionLog({port_folder / "logs", paths.user_data_root / "logs"})) {
      rex::cvar::SetFlagFromCommandLine("log_file", eck::Utf8(*inherited));
      eck::InstallCrashReports();
    }
    // ReXGlue writes its logs beside the executable, which is read-only inside an AppImage.
    const bool appimage = port_folder != rex::filesystem::GetExecutableFolder();
    if (appimage && rex::cvar::GetFlagByName("log_file").empty()) {
      const std::filesystem::path logs = eck::LogsFolder(port_folder, paths.user_data_root, {}, appimage);
      if (const std::filesystem::path file = eck::NextLogFile(logs, eck::kLogName); !file.empty()) {
        rex::cvar::SetFlagByName("log_file", eck::Utf8(file));
      }
    }
  }

  bool SetupPresentation() override {
    if (const std::string problem = eck::MissingGpuPluginMessage(rex::cvar::GetFlagByName("gpu_plugin"), shipped_config_);
        !problem.empty()) {
      REXLOG_ERROR("{}", problem);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, problem);
      return false;
    }
    if (rex::ReXApp::SetupPresentation()) {
      if (window()) {
        AttachMouseFocusGate(window());
      }
      return true;
    }
    REXLOG_WARN("Graphics unavailable; opening the launcher without graphics");
    eck::RunLauncherWithoutGraphics(DescribeGameFiles());
    return false;
  }

  void OnPostInitLogging() override {
    eck::InitializeProcessUptime();
    eck::InstallCrashReports();
    const eck::UserDataMessage message = eck::DescribeUserData(user_data_);
    if (message.warning) {
      REXLOG_WARN("{}", message.text);
    } else {
      REXLOG_INFO("{}", message.text);
    }
    eck::LoadPlayerSettings(eck::PlayerSettingsPath(user_data_root()));
    eck::ApplyDetailedLogging();
    eck::InstallSessionLogCleanup();
    const eck::ApplicationIdentity identity{
        .product = "El Chavo Kart",
        .version = eck::PortVersion(),
        .build_config = eck::BuildConfigurationName(),
        .architecture = eck::ArchitectureName(),
        .sdk = REXGLUE_VERSION_STRING,
        .backend = eck::GraphicsBackendName(),
    };
    REXLOG_INFO("{}", eck::FormatApplicationVersion(identity));
    REXLOG_INFO("{}", eck::FormatBuildIdentity(identity));
    const std::string os = eck::OsDescription();
    const std::string cpu = eck::CpuDescription();
    eck::SetCrashSystemInfo(REXGLUE_VERSION_STRING, os.c_str(), cpu.c_str());
    eck::LogMainMenuHooks();
    eck::LogOnlineGameHooks();
    if (eck::RecoverInterruptedInstall(game_data_root())) {
      REXLOG_WARN("Restored the game files after an interrupted install");
    }
    REXLOG_INFO("{}", eck::GameFilesLogText(eck::CheckGameFiles(game_data_root())));
    REXLOG_INFO("Profile settings: <user data>/{:08X}/profile/User; achievements: <user data>/achievements/{:08X}.toml",
                eck::kTitleId, eck::kTitleId);
    eck::ApplyGraphicsAdapter();
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    for (const char* bind : eck::kRemovedSdkBinds) {
      rex::ui::UnregisterBind(bind);
    }
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override {
    eck::LoadLauncherFonts(atlas, window() ? float(window()->GetDpi()) / float(window()->GetMediumDpi()) : 1.0f);
  }

  void OnDpiScaleChanged(float scale) override {
    eck::UpdateLauncherFonts(imgui_drawer(), immediate_drawer(), scale);
  }

  void OnWindowFocusChanged(bool focused) override { OnWindowFocus(focused, window(), imgui_drawer()); }

  void OnPreSetup(rex::RuntimeConfig&) override {
    game_settings_.Capture(eck::PlayerSettingsPath(user_data_root()), shipped_config_);
    show_launcher_ = eck::ShouldShowLauncher(game_data_root());
    if (show_launcher_) {
      eck::PrepareLauncherWindow();
    }
    game_settings_.MarkWindowCreated(show_launcher_);
    if (!show_launcher_) {
      game_window_ = eck::FitWindowToDisplay();
    }
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (window()) {
      window()->SetTitle(kWindowTitle);
    }
    eck::ApplyWindowIcon();
    game_settings_.CaptureLateRegistered();
    if (!show_launcher_ && eck::ShiftHeldAfterWindowOpened()) {
      show_launcher_ = true;
    }
    if (!show_launcher_) {
      eck::ApplyMultisampling();
      eck::ApplyInternalRenderPath();
      eck::ApplyLanguage();
      eck::ApplyPlayerNames();
      return defaults;
    }
    if (!imgui_drawer()) {
      game_settings_.Apply();
      return defaults;
    }
    REXLOG_INFO("Launcher shown");
    eck::LauncherCallbacks callbacks;
    callbacks.play = [this, resume, defaults] {
      app_context().CallInUIThreadDeferred([this, resume, defaults] {
        game_settings_.Apply();
        eck::ApplyDetailedLogging();
        resume(defaults);
      });
    };
    callbacks.window_changed = [this] { return game_settings_.WindowChanged(); };
    callbacks.restart = [this] {
      if (!eck::RelaunchSelf(L"--eck_skip_launcher=true")) {
        return false;
      }
      app_context().CallInUIThreadDeferred([] {
        REXLOG_INFO("Restarted in a new window; hard-exiting this process.");
        rex::FlushLogging();
        std::_Exit(0);
      });
      return true;
    };
    callbacks.quit = [this] { app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); }); };
    callbacks.window_handle = [this] { return window() ? window()->GetNativeWindowHandle() : nullptr; };
    callbacks.window_fullscreen = [this] { return window() && window()->IsFullscreen(); };
    callbacks.set_window_fullscreen = [this](bool fullscreen) {
      if (window()) {
        window()->SetFullscreen(fullscreen);
      }
    };
    const std::filesystem::path port_folder = eck::PortFolder();
    const std::filesystem::path logs_dir =
        eck::LogsFolder(port_folder, defaults.user_data_root, eck::ActiveLogFile(),
                        port_folder != rex::filesystem::GetExecutableFolder());
    eck::ShowLauncher(imgui_drawer(),
                      {defaults.game_data_root, defaults.user_data_root,
                       eck::PlayerSettingsPath(defaults.user_data_root), logs_dir, eck::ActiveLogFile(),
                       user_data_.outcome, immediate_drawer()},
                      game_settings_, std::move(callbacks));
    return std::nullopt;
  }

  void LaunchModule() override {
    const int monitor = game_settings_.GetInt("monitor", std::atoi(rex::cvar::GetFlagByName("monitor").c_str()));
    eck::StartupWindowInfo startup_window;
    startup_window.fullscreen = window() != nullptr && window()->IsFullscreen();
    startup_window.selected_width = game_settings_.GetInt(
        "window_width", std::atoi(rex::cvar::GetFlagByName("window_width").c_str()));
    startup_window.selected_height = game_settings_.GetInt(
        "window_height", std::atoi(rex::cvar::GetFlagByName("window_height").c_str()));
    startup_window.configured_width = game_window_ ? game_window_->plan.client.width : startup_window.selected_width;
    startup_window.configured_height = game_window_ ? game_window_->plan.client.height : startup_window.selected_height;
    const eck::WindowSize drawable = CurrentDrawableSize(window());
    startup_window.output_width = drawable.width;
    startup_window.output_height = drawable.height;
    startup_window.controller = eck::ControllerSummary();
    eck::LogStartupDiagnostics(game_settings_, eck::MonitorForSetting(monitor), monitor, game_data_root(),
                               startup_window);
    if (game_window_ && window()) {
      const eck::DpiWindowSize actual{int(window()->GetActualPhysicalWidth()), int(window()->GetActualPhysicalHeight())};
      REXLOG_INFO("{}", eck::FormatGameWindowLine(*game_window_, actual, {drawable.width, drawable.height}));
    }
    ReXApp::LaunchModule();
  }

  void OnPostSetup() override {
#if defined(_WIN32)
    REXLOG_INFO("Scheduling: {}", trg::TuneProcessScheduling());
#endif
    AttachWindowHost(window(), app_context(), [this] {
      rex::ui::ImGuiDrawer* drawer = imgui_drawer();
      return drawer && drawer->GetIO().WantCaptureMouse;
    });
    if (window()) {
      eck::CheckWindowDisplay(window()->GetNativeWindowHandle(), REXCVAR_GET(monitor));
    }
    if (window() && imgui_drawer()) {
      eck::CreateFrameCounter(imgui_drawer());
      eck::ConfigureOnline(user_data_root());
      eck::CreatePingDisplay(imgui_drawer());
      eck::InstallOnlineMenu(imgui_drawer(), [this](std::function<void()> run) {
        app_context().CallInUIThreadDeferred(std::move(run));
      });
      CreateCursorSync(imgui_drawer());
      if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
        input->SetActiveCallback([] { return eck::GameReadsInput(LocalInputAccepted(), eck::ModalDialog::AnyOpen()); });
      }
      eck::ModalDialog::AttachEscapeKey(window());
      eck::SetAchievementsMenuHandler([this] { app_context().CallInUIThreadDeferred([this] { OpenAchievements(); }); });
    }
  }

  void OnShutdown() override {
    // ReXApp destroys the window before the runtime; the input drivers must detach while it still exists.
    if (auto* input = runtime() ? runtime()->input_system() : nullptr) {
      input->Shutdown();
    }
    eck::debug::FlushSessionSummary();
    eck::SetAchievementsMenuHandler({});
    eck::ModalDialog::DetachEscapeKey();
    delete achievements_;
    DetachWindowHost();
    REXLOG_INFO("{}", eck::FormatCleanExit(eck::ProcessUptimeText()));
  }

  // Window close is a hard exit: the base TerminateTitle would call StopTitleThreads and wait on guest
  // threads that are still running guest code. The OS reclaims the process.
  void OnClosing(rex::ui::UIEvent&) override { eck::HardExit("Window closing, hard-exiting process."); }

  // F7 toggles the achievements overlay. The overlay is owned here, not by ReXApp.
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override {
    if (achievements_) {
      achievements_->RequestClose();
    } else {
      OpenAchievements();
    }
    return nullptr;
  }

 private:
  eck::GraphicsFallbackInfo DescribeGameFiles() const {
    switch (eck::CheckGameFiles(game_data_root()).state) {
      case eck::GameFiles::kReady:
        return {eck::Utf8(game_data_root()), "Ready", true};
      case eck::GameFiles::kUnsupported:
        return {eck::Utf8(game_data_root()), "Unsupported version", false};
      default:
        return {eck::Utf8(game_data_root()), "Setup required", false};
    }
  }

  // The cache folder ReXGlue derived from the system root follows the user data root unless it was set explicitly.
  void ChooseUserData(rex::PathConfig& paths, const std::filesystem::path& port_folder) {
    user_data_ = eck::SelectUserData({port_folder, paths.user_data_root, !REXCVAR_GET(user_data_root).empty()});
    if (user_data_.root == paths.user_data_root) {
      return;
    }
    paths.user_data_root = user_data_.root;
    if (REXCVAR_GET(cache_root).empty()) {
      paths.cache_root = user_data_.root / "cache";
    }
  }

  void OpenAchievements() {
    if (achievements_ || eck::ModalDialog::AnyOpen() || !runtime() || !runtime()->kernel_state() || !imgui_drawer() ||
        !immediate_drawer()) {
      return;
    }
    REXLOG_INFO("[Achievement] overlay open");
    achievements_ = new eck::AchievementsOverlay(imgui_drawer(), immediate_drawer(), runtime(), &achievements(),
                                                 [this] {
                                                   REXLOG_INFO("[Achievement] overlay closed");
                                                   achievements_ = nullptr;
                                                 });
  }

  eck::GameSettings game_settings_;
  eck::AchievementsOverlay* achievements_ = nullptr;
  std::filesystem::path shipped_config_;
  eck::UserDataSelection user_data_;
  bool show_launcher_ = false;
  std::optional<eck::GameWindowStartup> game_window_;
};
