#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace eck {

inline constexpr uint32_t kTitleId = 0x475807D7;
inline constexpr uint32_t kMediaId = 0x5C2ECDD6;
inline constexpr char kGameVersion[] = "0.0.0.3";

// The default.xex the code was recompiled from (El Chavo Kart (USA) disc, see tools\codegen.ps1).
inline constexpr uint64_t kXexSize = 30003200;
inline constexpr char kXexSha256[] = "1db4e96b413d5629206ffe63e73c0e1cfaca9eea24bcf6fced6db9b135ce120f";

enum class GameFiles { kReady, kMissing, kIncomplete, kUnsupported };

struct GameCheck {
  GameFiles state = GameFiles::kMissing;
  std::string xex_sha256;    // set when default.xex was read
  std::string missing_file;  // the first required file that is absent or has the wrong size
};

// <port folder>/game, or the folder named by the first line of <port folder>/game-folder.txt (tools\build.ps1 writes it so a
// development build runs from game\extracted).
std::filesystem::path DefaultGameFolder(const std::filesystem::path& port_folder);

// The one readiness check: launcher state, install verification and the clean-start decision all use it.
GameCheck CheckGameFiles(const std::filesystem::path& game_dir);

// A support log line about the game files that carries no folder: state, size and SHA-256.
std::string GameFilesLogText(const GameCheck& check);

enum class InstallStatus {
  kInstalled,
  kCancelled,
  kNoGameFolder,
  kForeignFolder,
  kOpenFailed,
  kNotImage,
  kWrongTitle,
  kDamagedImage,
  kNoSpace,
  kWriteFailed,
  kWrongVersion,
  kIncomplete,
};

struct InstallResult {
  InstallStatus status = InstallStatus::kInstalled;
  std::string detail;  // for the log, not the UI
  uint64_t bytes = 0;
  size_t files = 0;
  bool ok() const { return status == InstallStatus::kInstalled; }
};

struct InstallHooks {
  std::function<void(const char* phase)> phase;
  // Returns false to cancel. Cancelling is possible until the new files are activated.
  std::function<bool(uint64_t done, uint64_t total)> progress;
};

// Checks the image's default.xex first, then extracts the disc into a staging folder next to `game_dir`, checks it
// with CheckGameFiles and only then swaps it in. On any failure or cancellation `game_dir` is left as it was.
InstallResult InstallFromDiscImage(const std::filesystem::path& image, const std::filesystem::path& game_dir,
                                   const InstallHooks& hooks = {});

struct InstallMessage {
  const char* title;
  const char* text;
};
InstallMessage DescribeInstall(InstallStatus status);

}  // namespace eck
