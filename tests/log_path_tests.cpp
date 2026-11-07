// The logs folder follows the active log, falls back to <user data>/logs for an AppImage, and is
// created before its first use. Run: eck_log_path_tests

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "log_path.h"

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

}  // namespace

int main() {
  std::error_code ec;
  const std::filesystem::path base = std::filesystem::temp_directory_path(ec) / "eck_log_path_tests";
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);

  std::printf("the active log's folder wins\n");
  const std::filesystem::path active = base / "active" / "el_chavo_kart_001.log";
  std::filesystem::create_directories(active.parent_path(), ec);
  CHECK(eck::LogsFolder(base / "port", base / "user", active, true) == active.parent_path());
  CHECK(!std::filesystem::exists(base / "port" / "logs", ec));

  std::printf("without an active log the port folder holds the logs\n");
  CHECK(eck::LogsFolder(base / "port2", base / "user2", {}, false) == base / "port2" / "logs");

  std::printf("an AppImage's first launch creates the logs folder\n");
  const std::filesystem::path created = eck::LogsFolder(base / "port3", base / "user3", {}, true);
  CHECK(created == base / "port3" / "logs");
  CHECK(std::filesystem::is_directory(created, ec));

  std::printf("an AppImage falls back to user data when the port folder cannot hold logs\n");
  std::filesystem::create_directories(base / "port4", ec);
  std::ofstream(base / "port4" / "logs") << "not a directory";
  CHECK(eck::LogsFolder(base / "port4", base / "user4", {}, true) == base / "user4" / "logs");

  std::printf("sequential session logs start at 001 and skip unrelated names\n");
  const std::filesystem::path numbered = base / "numbered";
  std::filesystem::create_directories(numbered, ec);
  CHECK(eck::NextLogFile(numbered, "el_chavo_kart") == numbered / "el_chavo_kart_001.log");

  std::ofstream(numbered / "el_chavo_kart_001.log") << "one";
  std::ofstream(numbered / "el_chavo_kart_007.log") << "seven";
  std::ofstream(numbered / "el_chavo_kart_note.log") << "not numbered";
  std::ofstream(numbered / "other_012.log") << "not this port";
  CHECK(eck::NextLogFile(numbered, "el_chavo_kart") == numbered / "el_chavo_kart_008.log");

  std::ofstream(numbered / "el_chavo_kart_009.log") << "nine";
  CHECK(eck::NextLogFile(numbered, "el_chavo_kart") == numbered / "el_chavo_kart_010.log");
  CHECK(eck::NextLogFile(base / "missing" / "logs", "el_chavo_kart") ==
        base / "missing" / "logs" / "el_chavo_kart_001.log");

  std::printf("logs in a folder with spaces and accents are numbered and found like any other\n");
  const std::filesystem::path accented = base / std::filesystem::path(u8"João Português") / std::filesystem::path(u8"Ação");
  const std::filesystem::path logs = eck::LogsFolder(accented, base / "user5", {}, true);
  CHECK(logs == accented / "logs" && std::filesystem::is_directory(logs, ec));
  const std::filesystem::path first = eck::NextLogFile(logs, "el_chavo_kart");
  CHECK(first == logs / "el_chavo_kart_001.log");
  std::ofstream(first) << "first";
  std::ofstream(eck::NextLogFile(logs, "el_chavo_kart")) << "second";
  CHECK(eck::BugReportLog(logs, logs / "el_chavo_kart_002.log", "el_chavo_kart") == first);

  std::filesystem::remove_all(base, ec);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
