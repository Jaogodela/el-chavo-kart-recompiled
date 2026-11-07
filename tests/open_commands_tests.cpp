// What the launcher asks the system to run for "Open logs folder", "Report a bug", links and folder buttons: the real
// trg::OpenUrl, trg::OpenInFileManager and Report a bug plan, with the command capture in place of Explorer, xdg-open
// and the browser. On Linux the real runner is also driven, with a stand-in program, to prove that no shell reads the
// arguments. Nothing here starts a browser, Explorer or a file manager. Run: eck_open_commands_tests

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <trg/platform.h>

#include "bug_report.h"
#include "log_path.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::vector<trg::OpenCommand> g_seen;
void Record(const trg::OpenCommand& command) { g_seen.push_back(command); }

std::string Utf8(const fs::path& path) {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

fs::path FromUtf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Touch(const fs::path& path, const std::string& text = "log") {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string PercentDecoded(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// The command a URL becomes: Windows hands the address itself to the shell's protocol handler, elsewhere xdg-open gets it
// as the one argument.
bool IsUrlCommand(const trg::OpenCommand& command, const std::string& url) {
  if (kWindows) return command.program == url && command.args.empty() && command.parameters.empty();
  return command.program == "xdg-open" && command.args == std::vector<std::string>{url} && command.parameters.empty();
}

// The command a folder becomes.
bool IsFolderCommand(const trg::OpenCommand& command, const fs::path& folder) {
  if (kWindows) return command.program == Utf8(folder) && command.args.empty() && command.parameters.empty();
  return command.program == "xdg-open" && command.args == std::vector<std::string>{Utf8(folder)};
}

// The command a file becomes: Windows selects it in Explorer, elsewhere its folder is opened.
bool IsFileCommand(const trg::OpenCommand& command, const fs::path& file) {
  if (kWindows) {
    return command.program == "explorer.exe" && command.args.empty() &&
           command.parameters == "/select,\"" + Utf8(file) + "\"";
  }
  return IsFolderCommand(command, file.parent_path());
}

struct Folders {
  fs::path root;
  Folders() {
    root = fs::temp_directory_path() / "eck-open-commands-tests";
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~Folders() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void TestUrls() {
  std::printf("addresses reach the system as one argument, whatever they hold\n");
  const std::vector<std::string> urls = {
      "https://github.com/saulob/el-chavo-kart-recompiled",
      "https://github.com/saulob/el-chavo-kart-recompiled/releases/tag/v0.1.0",
      "http://example.com/a?b=c&d=e%20f#frag",
      "https://example.com/issues/new?body=%2A%2Aa%2A%2A&title=x y",
      "https://example.com/?q=1;touch%20pwned&r=$(id)&s=`id`&t='x'&u=\"y\"",
      "https://example.com/Jo\xC3\xA3o/A\xC3\xA7\xC3\xA3o?name=Ana Mar\xC3\xAD" "a",
  };
  for (const std::string& url : urls) {
    const auto command = trg::BuildOpenUrlCommand(url);
    CHECK(command.has_value());
    CHECK(command && IsUrlCommand(*command, url));
    g_seen.clear();
    trg::OpenUrl(url);
    CHECK(g_seen.size() == 1);
    CHECK(g_seen.size() == 1 && IsUrlCommand(g_seen[0], url));
  }

  std::printf("anything that is not a web address is not opened\n");
  const std::vector<std::string> refused = {
      "",
      "file:///etc/passwd",
      "file:///C:/Windows/System32/cmd.exe",
      "javascript:alert(1)",
      "ftp://example.com/x",
      "ms-msdt:/id PCWDiagnostic",
      "-o",
      "--help",
      "xdg-open https://example.com",
      " https://example.com",
      "https:/example.com",
      "https://example.com/\nrm -rf ~",
      "https://example.com/\rx",
      std::string("https://example.com/\0x", 21),
  };
  for (const std::string& url : refused) {
    CHECK(!trg::BuildOpenUrlCommand(url).has_value());
    g_seen.clear();
    trg::OpenUrl(url);
    CHECK(g_seen.empty());
  }
}

void TestFolders() {
  std::printf("folders and files in paths with spaces and accents\n");
  Folders f;
  const fs::path logs = f.root / FromUtf8("Jo\xC3\xA3o Portugu\xC3\xAAs") / FromUtf8("A\xC3\xA7\xC3\xA3o") / "logs folder";
  const fs::path log = logs / "el_chavo_kart_002.log";
  Touch(log);

  const auto folder = trg::BuildOpenInFileManagerCommand(Utf8(logs));
  CHECK(folder && IsFolderCommand(*folder, logs));
  const auto file = trg::BuildOpenInFileManagerCommand(Utf8(log));
  CHECK(file && IsFileCommand(*file, log));
  g_seen.clear();
  trg::OpenInFileManager(Utf8(logs));
  trg::OpenInFileManager(Utf8(log));
  CHECK(g_seen.size() == 2);
  CHECK(g_seen.size() == 2 && IsFolderCommand(g_seen[0], logs) && IsFileCommand(g_seen[1], log));

  std::printf("shell metacharacters in a folder name stay inside one argument\n");
  const fs::path odd = f.root / "a; touch pwned $(id) `id` 'q' & (b) x";
  const fs::path odd_file = odd / "it's a log; rm -rf ~.log";
  Touch(odd_file);
  const auto odd_folder = trg::BuildOpenInFileManagerCommand(Utf8(odd));
  CHECK(odd_folder && IsFolderCommand(*odd_folder, odd));
  if (!kWindows) CHECK(odd_folder && odd_folder->args.size() == 1);
  const auto odd_log = trg::BuildOpenInFileManagerCommand(Utf8(odd_file));
  CHECK(odd_log && IsFileCommand(*odd_log, odd_file));
  CHECK(!fs::exists("pwned") && !fs::exists(f.root / "pwned") && !fs::exists(odd / "pwned"));

  std::printf("a missing path opens the folder above it, or nothing\n");
  const auto missing_file = trg::BuildOpenInFileManagerCommand(Utf8(logs / "new.log"));
  CHECK(missing_file && IsFolderCommand(*missing_file, logs));
  CHECK(!trg::BuildOpenInFileManagerCommand(Utf8(f.root / "absent" / "deeper" / "new.log")).has_value());
  CHECK(!trg::BuildOpenInFileManagerCommand("").has_value());
  g_seen.clear();
  trg::OpenInFileManager(Utf8(f.root / "absent" / "deeper"));
  trg::OpenInFileManager("");
  CHECK(g_seen.empty());
  trg::OpenInFileManager(Utf8(logs / "new.log"));
  CHECK(g_seen.size() == 1 && IsFolderCommand(g_seen[0], logs));

  std::printf("a relative path is made absolute, so it is never read as an option\n");
  const fs::path before = fs::current_path();
  fs::create_directories(f.root / "-evil");
  fs::current_path(f.root);
  const auto relative = trg::BuildOpenInFileManagerCommand("-evil");
  fs::current_path(before);
  CHECK(relative.has_value());
  if (relative) {
    const std::string target = kWindows ? relative->program : relative->args.at(0);
    CHECK(!target.empty() && target[0] != '-');
    CHECK(fs::path(FromUtf8(target)).is_absolute());
  }
}

void TestReportBug() {
  std::printf("Report a bug: shows the newest log, opens the project's issue form, uploads nothing\n");
  Folders f;
  const fs::path logs = f.root / FromUtf8("Jo\xC3\xA3o Portugu\xC3\xAAs") / FromUtf8("A\xC3\xA7\xC3\xA3o") / "logs";
  Touch(logs / "el_chavo_kart_001.log");
  Touch(logs / "el_chavo_kart_002.log");
  Touch(logs / "el_chavo_kart_003.log", "this run's own log");
  Touch(logs / "el_chavo_kart_note.log");
  const fs::path active = logs / "el_chavo_kart_003.log";
  const std::string os = "Windows 11 (build 26100)";
  const eck::BugReportPlan plan = eck::PlanBugReport(logs, active, "0.1.0", os);
  CHECK(plan.reveal == logs / "el_chavo_kart_002.log");

  g_seen.clear();
  trg::OpenInFileManager(Utf8(plan.reveal));
  trg::OpenUrl(plan.url);
  CHECK(g_seen.size() == 2);
  if (g_seen.size() == 2) {
    CHECK(IsFileCommand(g_seen[0], logs / "el_chavo_kart_002.log"));
    CHECK(IsUrlCommand(g_seen[1], plan.url));
  }

  const std::string prefix = "https://github.com/saulob/el-chavo-kart-recompiled/issues/new?body=";
  CHECK(plan.url.compare(0, prefix.size(), prefix) == 0);
  CHECK(plan.url == eck::BugReportUrl(eck::kRepositoryUrl, "0.1.0", os));
  CHECK(plan.url.find(' ') == std::string::npos && plan.url.find('\n') == std::string::npos);
  CHECK(trg::BuildOpenUrlCommand(plan.url).has_value());

  std::printf("the form carries the version and the system and nothing else\n");
  const std::string body = PercentDecoded(plan.url.substr(prefix.size()));
  CHECK(body ==
        "**Port version:** 0.1.0\n**Operating system:** Windows 11 (build 26100)\n\n**What happened?**\n\n\n"
        "**Steps to reproduce**\n\n\nPlease attach the log file the launcher just showed you.\n");
  const std::vector<std::string> secrets = {"token", "friend", "AB12-CD34", "player_id", Utf8(f.root), "Jo\xC3\xA3o",
                                            "el_chavo_kart_00"};
  for (const std::string& secret : secrets) {
    CHECK(plan.url.find(secret) == std::string::npos);
    CHECK(PercentDecoded(plan.url).find(secret) == std::string::npos);
  }

  std::printf("with no session log yet the logs folder is shown; with only the active one, that log\n");
  const eck::BugReportPlan empty = eck::PlanBugReport(f.root / "none", f.root / "none" / "el_chavo_kart_001.log", "0.1.0", os);
  CHECK(empty.reveal == f.root / "none");
  const fs::path lone = f.root / "lone";
  Touch(lone / "el_chavo_kart_001.log");
  CHECK(eck::PlanBugReport(lone, lone / "el_chavo_kart_001.log", "0.1.0", os).reveal == lone / "el_chavo_kart_001.log");

  std::printf("the Linux version and system are in the form as typed\n");
  const eck::BugReportPlan linux_plan = eck::PlanBugReport(logs, active, "0.1.0", "Ubuntu 24.04 (Linux 6.8)");
  CHECK(PercentDecoded(linux_plan.url).find("**Operating system:** Ubuntu 24.04 (Linux 6.8)\n") != std::string::npos);
}

#if !defined(_WIN32)
void TestRealRunnerHasNoShell() {
  std::printf("the real runner starts the program with the arguments as they are, through no shell\n");
  Folders f;
  const fs::path out = f.root / "argv.txt";
  const fs::path program = f.root / "stand-in";
  {
    std::ofstream script(program);
    script << "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done > '" << out.string() << "'\n";
  }
  chmod(program.c_str(), 0755);

  const fs::path odd = f.root / "a; touch pwned $(touch pwned2) `touch pwned3` 'q' \"d\" & x" / FromUtf8("A\xC3\xA7\xC3\xA3o");
  fs::create_directories(odd);
  auto command = trg::BuildOpenInFileManagerCommand(Utf8(odd));
  CHECK(command.has_value());
  if (!command) return;
  CHECK(command->program == "xdg-open");
  const std::vector<std::string> expected_folder = command->args;
  command->program = program.string();
  trg::RunOpenCommand(*command);
  for (int i = 0; i < 100 && !fs::exists(out); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(Read(out) == expected_folder.at(0) + "\n");

  fs::remove(out);
  auto url = trg::BuildOpenUrlCommand("https://example.com/?a=1;touch pwned&b=$(touch pwned2)&c=`touch pwned3`&d='x' y");
  CHECK(url.has_value());
  if (!url) return;
  const std::string address = url->args.at(0);
  url->program = program.string();
  trg::RunOpenCommand(*url);
  for (int i = 0; i < 100 && !fs::exists(out); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(Read(out) == address + "\n");

  CHECK(!fs::exists("pwned") && !fs::exists("pwned2") && !fs::exists("pwned3"));
  CHECK(!fs::exists(f.root / "pwned") && !fs::exists(odd.parent_path() / "pwned"));

  std::printf("a program that is not installed is not an error\n");
  trg::OpenCommand missing;
  missing.program = (f.root / "no-such-program").string();
  missing.args = {"x"};
  trg::RunOpenCommand(missing);
  CHECK(true);
}
#endif

}  // namespace

int main() {
  const trg::OpenCommandHook previous = trg::SetOpenCommandHook(Record);
  CHECK(previous == nullptr);
  TestUrls();
  TestFolders();
  TestReportBug();
#if !defined(_WIN32)
  trg::SetOpenCommandHook(nullptr);
  TestRealRunnerHasNoShell();
#endif
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
