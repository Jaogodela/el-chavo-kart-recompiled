#include "game_files.h"

#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sha256.h"
#include "xiso.h"

namespace eck {
namespace {

constexpr const char* kStagingFolder = "game-install-temp";
constexpr const char* kBackupFolder = "game-install-backup";
constexpr uint64_t kSpaceMargin = 256ull << 20;
constexpr int kRenameAttempts = 10;

struct RequiredFile {
  const char* path;
  uint64_t size;
};

// Files the game cannot start, load its front end or play its intro without, with their size on the disc.
constexpr RequiredFile kRequiredFiles[] = {
    {"ChavoKartGame/Xbox360TOC.txt", 25370},
    {"ChavoKartGame/Xbox360TOC_PTB.txt", 462},
    {"ChavoKartGame/CookedXbox360/Core.xxx", 32768},
    {"ChavoKartGame/CookedXbox360/Engine.xxx", 1146880},
    {"ChavoKartGame/CookedXbox360/Startup.xxx", 98304},
    {"ChavoKartGame/CookedXbox360/ChavoKartGame.xxx", 3899392},
    {"ChavoKartGame/CookedXbox360/FMODAudio.xxx", 32768},
    {"ChavoKartGame/CookedXbox360/GlobalShaderCache-Xbox360.bin", 939069},
    {"ChavoKartGame/CookedXbox360/Coalesced_INT.bin", 367604},
    {"ChavoKartGame/CookedXbox360/Coalesced_PTB.bin", 371206},
    {"ChavoKartGame/CookedXbox360/Coalesced_ESM.bin", 371408},
    {"ChavoKartGame/CookedXbox360/Coalesced_ESN.bin", 371762},
    {"ChavoKartGame/CookedXbox360/TitleMap.xxx", 17760256},
    {"ChavoKartGame/CookedXbox360/StartMenu.xxx", 15925248},
    {"ChavoKartGame/CookedXbox360/Textures.tfc", 33239975},
    {"ChavoKartGame/CookedXbox360/Lighting.tfc", 108650780},
    {"ChavoKartGame/Movies/INTRO.BIK", 53437548},
    {"ChavoKartGame/Movies/DISCLAIMER01_PTB.BIK", 8551000},
};

// The console dashboard update on the disc is not game data.
const std::vector<std::string> kSkippedFolders = {"$SystemUpdate"};

// The antivirus scanner can hold a freshly written folder for a moment.
bool RenameWithRetry(const std::filesystem::path& from, const std::filesystem::path& to) {
  for (int attempt = 0; attempt < kRenameAttempts; ++attempt) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  return false;
}

// An existing folder is only replaced when it is empty or already holds game files.
bool IsReplaceable(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return false;
  }
  return std::filesystem::is_empty(dir, ec) || std::filesystem::exists(dir / "default.xex", ec) ||
         std::filesystem::exists(dir / "ChavoKartGame", ec);
}

class StagingGuard {
 public:
  explicit StagingGuard(std::filesystem::path dir) : dir_(std::move(dir)) {}
  ~StagingGuard() {
    if (armed_) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
    }
  }
  void Release() { armed_ = false; }

 private:
  std::filesystem::path dir_;
  bool armed_ = true;
};

InstallResult Failure(InstallStatus status, std::string detail = {}) {
  InstallResult result;
  result.status = status;
  result.detail = std::move(detail);
  return result;
}

InstallStatus FromImage(xiso::Status status) {
  switch (status) {
    case xiso::Status::kOpenFailed:
      return InstallStatus::kOpenFailed;
    case xiso::Status::kNotImage:
      return InstallStatus::kNotImage;
    case xiso::Status::kCorrupt:
    case xiso::Status::kUnsafePath:
      return InstallStatus::kDamagedImage;
    case xiso::Status::kCancelled:
      return InstallStatus::kCancelled;
    case xiso::Status::kWriteFailed:
    case xiso::Status::kOk:
      break;
  }
  return InstallStatus::kWriteFailed;
}

}  // namespace

std::filesystem::path DefaultGameFolder(const std::filesystem::path& port_folder) {
  std::ifstream in(port_folder / "game-folder.txt", std::ios::binary);
  std::string line;
  if (in && std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      return std::filesystem::path(std::u8string(line.begin(), line.end()));
    }
  }
  return port_folder / "game";
}

GameCheck CheckGameFiles(const std::filesystem::path& game_dir) {
  GameCheck check;
  const auto xex = game_dir / "default.xex";
  std::error_code ec;
  if (game_dir.empty() || !std::filesystem::is_regular_file(xex, ec)) {
    return check;
  }
  check.state = GameFiles::kUnsupported;
  if (std::filesystem::file_size(xex, ec) != kXexSize || ec) {
    return check;
  }
  std::string error;
  check.xex_sha256 = NormalizeSha256(Sha256FileHex(xex, error));
  if (check.xex_sha256 != kXexSha256) {
    return check;
  }
  for (const RequiredFile& required : kRequiredFiles) {
    const std::filesystem::path path = game_dir / std::filesystem::path(required.path).make_preferred();
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size != required.size) {
      check.state = GameFiles::kIncomplete;
      check.missing_file = required.path;
      return check;
    }
  }
  check.state = GameFiles::kReady;
  return check;
}

std::string GameFilesLogText(const GameCheck& check) {
  const char* state = "missing";
  switch (check.state) {
    case GameFiles::kReady:
      state = "valid";
      break;
    case GameFiles::kUnsupported:
      state = "unsupported version";
      break;
    case GameFiles::kIncomplete:
      state = "incomplete";
      break;
    case GameFiles::kMissing:
      break;
  }
  std::string text = std::string("Game files: ") + state + " (default.xex";
  if (!check.xex_sha256.empty()) {
    text += ", size " + std::to_string(kXexSize) + ", SHA-256 " + check.xex_sha256;
  }
  if (!check.missing_file.empty()) {
    text += "; missing or damaged: " + check.missing_file;
  }
  return text + ")";
}

InstallResult InstallFromDiscImage(const std::filesystem::path& image, const std::filesystem::path& game_dir,
                                   const InstallHooks& hooks) {
  const auto phase = [&](const char* text) {
    if (hooks.phase) {
      hooks.phase(text);
    }
  };

  std::error_code ec;
  std::filesystem::path target = std::filesystem::absolute(game_dir, ec).lexically_normal();
  if (!target.has_filename()) {
    target = target.parent_path();
  }
  const std::filesystem::path parent = target.parent_path();
  if (game_dir.empty() || ec || parent.empty() || parent == target) {
    return Failure(InstallStatus::kNoGameFolder);
  }

  phase("Reading disc image...");
  xiso::Image disc;
  if (const xiso::Result read = disc.Open(image); !read.ok()) {
    return Failure(FromImage(read.status), read.detail);
  }
  const xiso::Entry* xex = disc.Find("default.xex");
  if (!xex || xex->directory || !disc.Find("ChavoKartGame")) {
    return Failure(InstallStatus::kWrongTitle, "no default.xex or ChavoKartGame folder");
  }

  phase("Checking the game version...");
  std::vector<uint8_t> xex_bytes;
  if (const xiso::Result read = disc.Read(*xex, xex_bytes); !read.ok()) {
    return Failure(FromImage(read.status), read.detail);
  }
  const std::string xex_sha256 =
      Sha256Hex(std::string_view(reinterpret_cast<const char*>(xex_bytes.data()), xex_bytes.size()));
  if (xex_bytes.size() != kXexSize || NormalizeSha256(xex_sha256) != kXexSha256) {
    return Failure(InstallStatus::kWrongVersion, "default.xex SHA-256 " + xex_sha256);
  }
  xex_bytes = {};

  std::filesystem::create_directories(parent, ec);
  if (ec) {
    return Failure(InstallStatus::kWriteFailed, "cannot create the parent folder: " + ec.message());
  }
  if (std::filesystem::exists(target, ec) && !IsReplaceable(target)) {
    return Failure(InstallStatus::kForeignFolder, "the target folder holds other files");
  }

  const std::filesystem::path staging = parent / kStagingFolder;
  const std::filesystem::path backup = parent / kBackupFolder;
  std::filesystem::remove_all(staging, ec);
  if (ec || std::filesystem::exists(staging, ec)) {
    return Failure(InstallStatus::kWriteFailed, "cannot clear the staging folder");
  }
  const auto space = std::filesystem::space(parent, ec);
  if (!ec && space.available < disc.total_bytes() + kSpaceMargin) {
    return Failure(InstallStatus::kNoSpace, "needs " + std::to_string(disc.total_bytes()) + " bytes, " +
                                                std::to_string(space.available) + " available");
  }
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    return Failure(InstallStatus::kWriteFailed, "cannot create the staging folder: " + ec.message());
  }
  StagingGuard guard(staging);

  phase("Copying game files...");
  if (const xiso::Result extracted = disc.ExtractTo(staging, kSkippedFolders, hooks.progress); !extracted.ok()) {
    return Failure(FromImage(extracted.status), extracted.detail);
  }

  phase("Verifying installation...");
  const GameCheck check = CheckGameFiles(staging);
  switch (check.state) {
    case GameFiles::kReady:
      break;
    case GameFiles::kUnsupported:
      return Failure(InstallStatus::kWrongVersion);
    case GameFiles::kIncomplete:
    case GameFiles::kMissing:
      return Failure(InstallStatus::kIncomplete, check.missing_file);
  }
  if (hooks.progress && !hooks.progress(disc.total_bytes(), disc.total_bytes())) {
    return Failure(InstallStatus::kCancelled);
  }

  phase("Finishing...");
  const bool replacing = std::filesystem::exists(target, ec);
  if (replacing) {
    std::filesystem::remove_all(backup, ec);
    if (!RenameWithRetry(target, backup)) {
      return Failure(InstallStatus::kWriteFailed, "cannot move the old game folder aside");
    }
  }
  if (!RenameWithRetry(staging, target)) {
    if (replacing) {
      RenameWithRetry(backup, target);
    }
    return Failure(InstallStatus::kWriteFailed, "cannot activate the new game folder");
  }
  guard.Release();
  if (replacing) {
    std::filesystem::remove_all(backup, ec);
  }

  InstallResult result;
  result.bytes = disc.total_bytes();
  result.files = disc.file_count();
  return result;
}

InstallMessage DescribeInstall(InstallStatus status) {
  switch (status) {
    case InstallStatus::kInstalled:
      return {"Game installed", "Press Play to start the game."};
    case InstallStatus::kCancelled:
      return {"Install cancelled", "The game files were not changed."};
    case InstallStatus::kNoGameFolder:
      return {"No game folder", "Start the game with --game_data_root to choose where the game files go."};
    case InstallStatus::kForeignFolder:
      return {"Game folder not empty", "The game folder holds other files, so it was not replaced."};
    case InstallStatus::kOpenFailed:
      return {"Could not open the file", "Check that the file exists and that it is not in use."};
    case InstallStatus::kNotImage:
      return {"Not an Xbox 360 disc image", "Select the .iso image of your El Chavo Kart Xbox 360 disc."};
    case InstallStatus::kWrongTitle:
      return {"Wrong disc image", "This is not an El Chavo Kart disc image."};
    case InstallStatus::kDamagedImage:
      return {"Damaged disc image", "The image is damaged or incomplete. Check the file and try again."};
    case InstallStatus::kNoSpace:
      return {"Not enough disk space", "About 2 GB is needed next to the game files. Free up space and try again."};
    case InstallStatus::kWriteFailed:
      return {"Could not write the game files", "Check that the game folder is writable and not in use."};
    case InstallStatus::kWrongVersion:
      return {"Different version of the game",
              "This port supports the El Chavo Kart (USA) Xbox 360 disc, game version 0.0.0.3."};
    case InstallStatus::kIncomplete:
      return {"Game files missing", "The disc image does not contain all of the game files this port needs."};
  }
  return {"Install failed", ""};
}

}  // namespace eck
