#include <cstdlib>

#include <rex/filesystem.h>

#include "platform.h"

namespace eck {

std::string Utf8(const std::filesystem::path& path) {
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

std::filesystem::path PathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::filesystem::path PortFolder() {
#if !defined(_WIN32)
  // An AppImage runs from a read-only mount that is gone once it exits.
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    return std::filesystem::path(appimage).parent_path();
  }
#endif
  return rex::filesystem::GetExecutableFolder();
}

}  // namespace eck
