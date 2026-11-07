#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "log_path.h"

namespace eck {

inline constexpr char kRepositoryUrl[] = "https://github.com/saulob/el-chavo-kart-recompiled";

inline std::string PercentEncoded(std::string_view text) {
  std::string encoded;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
    if (unreserved) {
      encoded += c;
    } else {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", byte);
      encoded += hex;
    }
  }
  return encoded;
}

// GitHub's new issue form, filled in with what the port can state. The player reads it and submits it themselves.
inline std::string BugReportUrl(std::string_view repository, std::string_view version, std::string_view os) {
  std::string body = "**Port version:** ";
  body += version;
  body += "\n**Operating system:** ";
  body += os;
  body += "\n\n**What happened?**\n\n\n**Steps to reproduce**\n\n\n";
  body += "Please attach the log file the launcher just showed you.\n";
  return std::string(repository) + "/issues/new?body=" + PercentEncoded(body);
}

// What Report a bug does, in this order: shows the newest session log (the logs folder when there is none), then opens
// the issue form. Nothing is uploaded.
struct BugReportPlan {
  std::filesystem::path reveal;
  std::string url;
};

inline BugReportPlan PlanBugReport(const std::filesystem::path& logs_folder, const std::filesystem::path& active_log,
                                   std::string_view version, std::string_view os) {
  const std::filesystem::path log = BugReportLog(logs_folder, active_log, kLogName);
  return {log.empty() ? logs_folder : log, BugReportUrl(kRepositoryUrl, version, os)};
}

}  // namespace eck
