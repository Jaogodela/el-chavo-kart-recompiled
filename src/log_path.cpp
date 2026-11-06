#include "log_path.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

namespace eck {
namespace {

bool IsNumber(std::string_view text) {
  return !text.empty() && text.size() <= 9 &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// NNN of a <name>_NNN.log file name, 0 for anything else.
int LogNumber(std::string_view file, std::string_view name) {
  const std::string prefix = std::string(name) + "_";
  if (!file.starts_with(prefix) || !file.ends_with(".log")) {
    return 0;
  }
  const std::string_view number = file.substr(prefix.size(), file.size() - prefix.size() - sizeof(".log") + 1);
  return IsNumber(number) ? std::atoi(std::string(number).c_str()) : 0;
}

}  // namespace

std::filesystem::path NextLogFile(const std::filesystem::path& folder, std::string_view name) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (!std::filesystem::is_directory(folder, ec)) {
    return {};
  }
  int last = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    last = std::max(last, LogNumber(entry.path().filename().string(), name));
  }
  char number[16];
  std::snprintf(number, sizeof(number), "_%03d.log", last + 1);
  return folder / (std::string(name) + number);
}

std::filesystem::path BugReportLog(const std::filesystem::path& folder, const std::filesystem::path& active,
                                   std::string_view name) {
  std::filesystem::path newest;
  std::filesystem::path active_found;
  int newest_number = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    const int number = LogNumber(entry.path().filename().string(), name);
    if (number == 0) {
      continue;
    }
    if (entry.path().filename() == active.filename()) {
      active_found = entry.path();
    } else if (number > newest_number) {
      newest_number = number;
      newest = entry.path();
    }
  }
  return newest.empty() ? active_found : newest;
}

bool LogContainsCrashReport(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return false;
  }
  constexpr std::string_view kNeedle = "CRASH REPORT";
  std::string window;
  char buffer[4096];
  while (in) {
    in.read(buffer, sizeof(buffer));
    window.append(buffer, static_cast<size_t>(in.gcount()));
    if (window.find(kNeedle) != std::string::npos) {
      return true;
    }
    if (window.size() > kNeedle.size()) {
      window.erase(0, window.size() - kNeedle.size());
    }
  }
  return false;
}

std::filesystem::path LogsFolder(const std::filesystem::path& port_folder,
                                 const std::filesystem::path& user_data_root,
                                 const std::filesystem::path& active_log, bool appimage) {
  if (!active_log.empty()) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(active_log, ec);
    return (ec ? active_log : absolute).parent_path();
  }
  const std::filesystem::path portable = port_folder / "logs";
  if (!appimage) {
    return portable;
  }
  std::error_code ec;
  std::filesystem::create_directories(portable, ec);
  if (std::filesystem::is_directory(portable, ec)) {
    return portable;
  }
  return user_data_root / "logs";
}

}  // namespace eck
