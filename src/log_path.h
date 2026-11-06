#pragma once

#include <filesystem>
#include <string_view>

namespace eck {

inline constexpr char kLogName[] = "el_chavo_kart";

// The next free <name>_NNN.log in `folder`, which is created. Empty when it cannot be created.
std::filesystem::path NextLogFile(const std::filesystem::path& folder, std::string_view name);

// The log a bug report should carry: the highest numbered session log that is not `active`, this run's own, which
// holds nothing yet when the launcher is the program that is running. `active` itself when it is the only one; empty
// when there are none.
std::filesystem::path BugReportLog(const std::filesystem::path& folder, const std::filesystem::path& active,
                                   std::string_view name);

// Whether a crash handler wrote a CRASH REPORT block into this log.
bool LogContainsCrashReport(const std::filesystem::path& file);

// The folder holding the logs users send: the active log's folder, or <port folder>/logs with the
// AppImage fallback to <user data>/logs.
std::filesystem::path LogsFolder(const std::filesystem::path& port_folder,
                                 const std::filesystem::path& user_data_root,
                                 const std::filesystem::path& active_log, bool appimage);

}  // namespace eck
