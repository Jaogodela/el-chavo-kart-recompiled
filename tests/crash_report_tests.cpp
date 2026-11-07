#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "bug_report.h"
#include "crash_report.h"
#include "diagnostics.h"
#include "log_path.h"
#include "online_test_support.h"

using online_test::Check;

namespace {

namespace fs = std::filesystem;

constexpr char kRepository[] = "https://github.com/saulob/el-chavo-kart-recompiled";

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Has(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

void Write(const fs::path& path, std::string_view text) {
  std::ofstream(path, std::ios::binary) << text;
}

template <size_t N>
void Copy(char (&out)[N], const char* text) {
  std::snprintf(out, N, "%s", text);
}

eck::CrashReport FullReport() {
  eck::CrashReport report;
  report.code = 0xC0000005u;
  report.address = 0x140001000;
  report.product = "El Chavo Kart";
  report.game_version = "0.1.0";
  report.build_config = "Release";
  report.architecture = "x86-64";
  Copy(report.faulting_module.name, "rexruntime.dll");
  report.faulting_module.known = true;
  report.faulting_module.base = 0x140000000;
  Copy(report.system_sdk, "0.10.0");
  Copy(report.system_os, "Windows 11 64-bit (build 26200)");
  Copy(report.system_cpu, "AMD Ryzen 7 7800X3D");
  Copy(report.runtime_gpu, "NVIDIA GeForce RTX 4070");
  Copy(report.runtime_selected_adapter, "NVIDIA GeForce RTX 4070");
  Copy(report.runtime_display, "2560x1440 | borderless fullscreen | window=1920x1080 | output=2560x1440");
  Copy(report.runtime_timing, "frame_rate=60 | immediate flip | host VBlank (VSync On)");
  Copy(report.runtime_renderer, "render=2560x1440 | internal=100% | msaa=2x native");
  Copy(report.runtime_language, "pt-BR");
  Copy(report.runtime_online, "enabled");
  return report;
}

std::string Format(const eck::CrashReport& report) {
  char buffer[8192];
  eck::FormatCrashReport(report, buffer, sizeof(buffer));
  return buffer;
}

void TestRequiredFields() {
  const std::string text = Format(FullReport());
  for (const std::string_view field :
       {"CRASH REPORT", "Version: 0.1.0", "Build: Release x86-64", "ReXGlue SDK: 0.10.0",
        "OS: Windows 11 64-bit (build 26200)", "CPU: AMD Ryzen 7 7800X3D", "gpu=NVIDIA GeForce RTX 4070",
        "selected_adapter=NVIDIA GeForce RTX 4070", "display=2560x1440 | borderless fullscreen", "frame_rate=60",
        "renderer=render=2560x1440", "language=pt-BR", "online=enabled", "Exception code: 0xC0000005",
        "ACCESS_VIOLATION", "Faulting module: rexruntime.dll", "Exception address: 0x0000000140001000"}) {
    Check(Has(text, field), "the report states " + std::string(field));
  }
}

void TestEmptyFieldsAreLeftOut() {
  eck::CrashReport report;
  report.code = 0xC0000005u;
  const std::string text = Format(report);
  Check(!Has(text, "OS: ") && !Has(text, "CPU: "), "unset system fields print no empty line");
  Check(!Has(text, "Last runtime state"), "a crash before the game started has no runtime section");
  Check(Has(text, "Faulting module: unknown"), "an unknown module is stated");
}

void TestPrivacy() {
  Check(std::wstring_view(eck::crash_detail::ModuleBaseName(L"C:\\Users\\Saulo\\Games\\rexruntime.dll")) ==
            L"rexruntime.dll",
        "a module path keeps only the file name");
  Check(std::wstring_view(eck::crash_detail::ModuleBaseName(L"/home/saulo/game/rexruntime.so")) == L"rexruntime.so",
        "with either separator");
  Check(std::wstring_view(eck::crash_detail::ModuleBaseName(L"plain.dll")) == L"plain.dll", "a bare name stays");

  const std::string text = Format(FullReport());
  for (const std::string_view secret : {"Users", "saulo", "Saulo", "token", "friend", "user_data", "\\"}) {
    Check(!Has(text, secret), "the report has no " + std::string(secret));
  }

  const std::string settings = Source("settings.cpp");
  const size_t begin = settings.find("const std::string crash_display");
  const size_t end = settings.find("SetCrashRuntimeState(crash_state)");
  Check(begin != std::string::npos && end != std::string::npos, "the crash state is built in one place");
  const std::string state = settings.substr(begin, end - begin);
  for (const std::string_view field : {"game_dir", "Utf8(", "eck_online_name", "user_data", "eck_player"}) {
    Check(!Has(state, field), "the crash state reads no " + std::string(field));
  }
}

void TestNormalExitIsNotACrash(const fs::path& dir) {
  const std::string clean = eck::FormatCleanExit("00:01:00");
  Check(!Has(clean, "CRASH"), "the clean exit line is not a crash");

  const fs::path normal = dir / "normal.log";
  Write(normal, "[info] Window closing, hard-exiting process.\n[info] " + clean + "\n");
  Check(!eck::LogContainsCrashReport(normal), "a normal session is not reported as a crash");

  const fs::path crashed = dir / "crashed.log";
  Write(crashed, "[info] Launcher closed\n" + Format(FullReport()));
  Check(eck::LogContainsCrashReport(crashed), "a crash block is found");
  Check(!eck::LogContainsCrashReport(dir / "missing.log"), "a missing log is no crash");

  std::string long_log(10000, 'x');
  long_log.replace(4090, 12, "CRASH REPORT");
  const fs::path across = dir / "across.log";
  Write(across, long_log);
  Check(eck::LogContainsCrashReport(across), "the marker is found across a read boundary");

  const std::string session = Source("session_log.cpp");
  Check(session.find("LogContainsCrashReport(file)") < session.find("std::filesystem::remove(file, ec)"),
        "Detailed logging off still keeps a log with a crash");
  Check(Has(Source("diagnostics.cpp"), "Execution complete: clean exit"), "a clean exit says so");
}

void TestBugReportLog(const fs::path& dir) {
  const fs::path logs = dir / "logs";
  fs::create_directories(logs);
  Check(eck::BugReportLog(logs, "", eck::kLogName).empty(), "no logs, nothing to show");
  Check(eck::BugReportLog(dir / "absent", "", eck::kLogName).empty(), "a missing folder is not an error");

  Write(logs / "el_chavo_kart_003.log", "active");
  Check(eck::BugReportLog(logs, logs / "el_chavo_kart_003.log", eck::kLogName) == logs / "el_chavo_kart_003.log",
        "the running session's log when it is the only one");

  Write(logs / "el_chavo_kart_001.log", "old");
  Write(logs / "el_chavo_kart_002.log", "crashed");
  Write(logs / "el_chavo_kart_notes.log", "not a session log");
  Write(logs / "other_game_009.log", "not this game");
  Check(eck::BugReportLog(logs, logs / "el_chavo_kart_003.log", eck::kLogName) == logs / "el_chavo_kart_002.log",
        "the newest finished session, not the launcher's own empty log");
  Check(eck::BugReportLog(logs, "", eck::kLogName) == logs / "el_chavo_kart_003.log", "the newest without an active log");

  Check(eck::NextLogFile(logs, eck::kLogName) == logs / "el_chavo_kart_004.log", "the numbering is unchanged");
  Check(eck::LogsFolder(dir, dir / "user_data", {}, false) == dir / "logs", "the portable logs folder");
  Check(eck::LogsFolder(dir, dir / "user_data", logs / "el_chavo_kart_003.log", false) == logs,
        "the active log's folder wins");
}

void TestBugReportUrl() {
  const std::string url = eck::BugReportUrl(kRepository, "0.1.0", "Windows 11 64-bit (build 26200)");
  Check(url.starts_with(std::string(kRepository) + "/issues/new?body="), "the project's new issue form");
  Check(Has(url, "0.1.0") && Has(url, "Windows%2011"), "it states the version and the system");
  Check(!Has(url, " ") && !Has(url, "\n"), "the query is encoded");
  Check(eck::PercentEncoded("a b&c=d/e?\xC3\xA9") == "a%20b%26c%3Dd%2Fe%3F%C3%A9", "reserved and UTF-8 bytes escape");
  Check(eck::PercentEncoded("Az09-._~") == "Az09-._~", "unreserved bytes stay");

  const std::string launcher = Source("launcher.cpp");
  const size_t begin = launcher.find("void ReportBug()");
  const std::string action = launcher.substr(begin, launcher.find('}', begin) - begin);
  Check(Has(action, "trg::OpenInFileManager") && Has(action, "trg::OpenUrl"), "it shows the log and opens the page");
  Check(!Has(launcher, "/issues\"") && Has(launcher, "kRepositoryUrl"), "the page comes from the repository link");
  Check(Has(launcher, "\"Report a bug\""), "About has Report a bug");
  Check(Has(launcher, "\"Open logs folder\""), "About has Open logs folder");
  for (const std::string_view upload : {"WinHttp", "POST", "UploadFile", "curl"}) {
    Check(!Has(action, upload), "nothing is uploaded: " + std::string(upload));
  }
}

#if defined(_WIN32)
int CrashChild(const char* log) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  eck::SetCrashGameVersion("0.1.0");
  eck::SetCrashSystemInfo("0.10.0", "Windows 11", "Test CPU");
  eck::CrashRuntimeState state;
  state.gpu = "Test GPU";
  state.selected_adapter = "Test GPU";
  state.language = "en-US";
  state.online = "disabled";
  eck::SetCrashRuntimeState(state);
  eck::InstallCrashReporting(log);
  RaiseException(0xC0000005u, 0, 0, nullptr);
  return 0;
}

// A real unhandled exception, in a child process: the handler's own path, not only its formatter.
void TestRealCrash(const fs::path& dir, const char* self) {
  const fs::path log = dir / "real_crash.log";
  Write(log, "[info] session\n");
  const std::string command = "\"\"" + fs::absolute(self).string() + "\" --crash \"" + log.string() + "\"\"";
  const int status = std::system(command.c_str());
  Check(status != 0, "the child did not exit cleanly");
  const std::string text = ReadFile(log);
  Check(Has(text, "[info] session"), "the block is appended to the session log");
  Check(eck::LogContainsCrashReport(log), "the handler wrote a report");
  for (const std::string_view field : {"Version: 0.1.0", "ReXGlue SDK: 0.10.0", "OS: Windows 11", "CPU: Test CPU",
                                       "gpu=Test GPU", "selected_adapter=Test GPU", "language=en-US",
                                       "online=disabled", "Exception code: 0xC0000005", "Faulting module: "}) {
    Check(Has(text, field), "the real report states " + std::string(field));
  }
  Check(!Has(text, fs::absolute(self).parent_path().string()), "module paths did not reach the report");
}
#else
enum class ChildAction { kSegfault, kAbort, kExit };

// The handlers run in a child process: this one keeps its own signal state and its report is read from the log.
int RunChild(const fs::path& log, ChildAction action) {
  const pid_t child = fork();
  if (child == 0) {
    eck::SetCrashGameVersion("0.1.0");
    eck::SetCrashSystemInfo("0.10.0", "Ubuntu 20.04.6 LTS", "Test CPU");
    eck::CrashRuntimeState state;
    state.gpu = "Test GPU";
    state.selected_adapter = "Test GPU";
    state.language = "en-US";
    state.online = "disabled";
    eck::SetCrashRuntimeState(state);
    eck::InstallCrashReporting(log.c_str());
    if (action == ChildAction::kSegfault) {
      raise(SIGSEGV);
    } else if (action == ChildAction::kAbort) {
      std::abort();
    }
    std::_Exit(0);
  }
  int status = 0;
  return child > 0 && waitpid(child, &status, 0) == child ? status : -1;
}

void TestRealSignals(const fs::path& dir) {
  Check(!eck::CrashReportingInstalled(), "nothing is installed before the first call");

  const fs::path segfault = dir / "segfault.log";
  Write(segfault, "[info] session\n");
  const int segfault_status = RunChild(segfault, ChildAction::kSegfault);
  Check(WIFSIGNALED(segfault_status) && WTERMSIG(segfault_status) == SIGSEGV, "the process still dies by the signal");
  const std::string text = ReadFile(segfault);
  Check(Has(text, "[info] session"), "the block is appended to the session log");
  Check(eck::LogContainsCrashReport(segfault), "the handler wrote a report");
  for (const std::string_view field :
       {"Linux fatal signal", "Signal: 11", "Exception code: 0xC0000005", "Version: 0.1.0", "ReXGlue SDK: 0.10.0",
        "OS: Ubuntu 20.04.6 LTS", "CPU: Test CPU", "gpu=Test GPU", "selected_adapter=Test GPU", "language=en-US",
        "online=disabled", "Build: ", "Stack trace: "}) {
    Check(Has(text, field), "the Linux report states " + std::string(field));
  }
  Check(!Has(text, fs::current_path().string() + "/"), "module paths did not reach the report");

  const fs::path aborted = dir / "abort.log";
  Write(aborted, "[info] session\n");
  const int abort_status = RunChild(aborted, ChildAction::kAbort);
  Check(WIFSIGNALED(abort_status) && WTERMSIG(abort_status) == SIGABRT, "abort still ends the process");
  Check(Has(ReadFile(aborted), "Signal: 6") && Has(ReadFile(aborted), "STATUS_FATAL_APP_EXIT"), "abort is reported");

  const fs::path clean = dir / "clean.log";
  Write(clean, "[info] session\n");
  const int clean_status = RunChild(clean, ChildAction::kExit);
  Check(WIFEXITED(clean_status) && WEXITSTATUS(clean_status) == 0, "a hard exit is not a crash");
  Check(!eck::LogContainsCrashReport(clean), "a hard exit writes no report");

  eck::InstallCrashReporting("");
  Check(!eck::CrashReportingInstalled(), "an empty log path installs nothing");
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  if (argc == 3 && std::string_view(argv[1]) == "--crash") {
    return CrashChild(argv[2]);
  }
#else
  (void)argc;
  (void)argv;
#endif
  const fs::path dir = fs::temp_directory_path() / "eck_crash_report_tests";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);

  TestRequiredFields();
  TestEmptyFieldsAreLeftOut();
  TestPrivacy();
  TestNormalExitIsNotACrash(dir);
  TestBugReportLog(dir);
  TestBugReportUrl();
#if defined(_WIN32)
  TestRealCrash(dir, argv[0]);
#else
  TestRealSignals(dir);
#endif

  fs::remove_all(dir, ec);
  return online_test::Finish();
}
