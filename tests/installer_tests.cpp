#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "game_files.h"
#include "sha256.h"
#include "xiso.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

constexpr uint32_t kSector = 2048;

void PutLe16(std::vector<uint8_t>& out, size_t at, uint16_t v) {
  out[at] = uint8_t(v);
  out[at + 1] = uint8_t(v >> 8);
}

void PutLe32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[at + i] = uint8_t(v >> (8 * i));
  }
}

// One directory table entry; `right` is the byte offset of the next sibling divided by 4.
size_t PutEntry(std::vector<uint8_t>& table, size_t at, uint16_t right, uint32_t sector, uint32_t size, bool directory,
                const std::string& name) {
  PutLe16(table, at, 0);
  PutLe16(table, at + 2, right);
  PutLe32(table, at + 4, sector);
  PutLe32(table, at + 8, size);
  table[at + 12] = directory ? 0x10 : 0x20;
  table[at + 13] = uint8_t(name.size());
  std::memcpy(table.data() + at + 14, name.data(), name.size());
  return (at + 14 + name.size() + 3) & ~size_t(3);
}

// A bare XDVDFS partition: default.xex and <folder>/readme.txt.
std::filesystem::path MakeImage(const std::filesystem::path& dir, const std::string& xex,
                                const std::string& name = "test.iso", const std::string& folder = "ChavoKartGame") {
  std::vector<uint8_t> image(kSector * 40, 0);
  const char magic[] = "MICROSOFT*XBOX*MEDIA";
  std::memcpy(image.data() + 32 * kSector, magic, 20);
  std::memcpy(image.data() + 32 * kSector + 0x7EC, magic, 20);
  PutLe32(image, 32 * kSector + 20, 33);
  PutLe32(image, 32 * kSector + 24, kSector);

  std::vector<uint8_t> root(kSector, 0xFF);
  const size_t second = PutEntry(root, 0, 0, 35, uint32_t(xex.size()), false, "default.xex");
  PutLe16(root, 2, uint16_t(second / 4));
  PutEntry(root, second, 0, 34, kSector, true, folder);
  std::memcpy(image.data() + 33 * kSector, root.data(), root.size());

  std::vector<uint8_t> sub(kSector, 0xFF);
  const std::string readme = "hello";
  PutEntry(sub, 0, 0, 36, uint32_t(readme.size()), false, "readme.txt");
  std::memcpy(image.data() + 34 * kSector, sub.data(), sub.size());
  std::memcpy(image.data() + 35 * kSector, xex.data(), xex.size());
  std::memcpy(image.data() + 36 * kSector, readme.data(), readme.size());

  const std::filesystem::path path = dir / name;
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
  return path;
}

namespace fs = std::filesystem;

constexpr char kSyntheticXex[] = "the synthetic executable";
constexpr eck::RequiredFile kSyntheticFiles[] = {{"ChavoKartGame/readme.txt", 5}};
constexpr eck::RequiredFile kNeverOnDisc[] = {{"ChavoKartGame/never-on-disc.txt", 1}};

std::string SyntheticHash() {
  return eck::NormalizeSha256(eck::Sha256Hex(kSyntheticXex));
}

eck::GameSpec SpecOf(const std::string& hash, std::span<const eck::RequiredFile> files = kSyntheticFiles) {
  return {sizeof(kSyntheticXex) - 1, hash, files};
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// A port folder as the player has it: everything that must survive a reinstall sits next to the game folder.
struct PortFolder {
  fs::path root;
  fs::path game = root / "game";
  fs::path image = root / "source" / "valid.iso";
  fs::path settings = root / "user_data" / "settings.toml";
  fs::path save = root / "user_data" / "profile" / "475807D7" / "00000001" / "save.bin";
  fs::path achievements = root / "user_data" / "achievements" / "475807D7.toml";
  fs::path identity = root / "user_data" / "online" / "identity.toml";
  fs::path log = root / "logs" / "el_chavo_kart_001.log";

  explicit PortFolder(const fs::path& path) : root(path) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "source");
    MakeImage(root / "source", kSyntheticXex, "valid.iso");
    WriteText(settings, "language = \"pt-BR\"\n");
    WriteText(save, "save bytes");
    WriteText(achievements, "unlocked = [1, 2]\n");
    WriteText(identity, "id = \"kept\"\n");
    WriteText(log, "log text");
  }

  bool UserDataIntact() const {
    return ReadText(settings) == "language = \"pt-BR\"\n" && ReadText(save) == "save bytes" &&
           ReadText(achievements) == "unlocked = [1, 2]\n" && ReadText(identity) == "id = \"kept\"\n" &&
           ReadText(log) == "log text";
  }

  bool NoWorkFolders() const {
    return !fs::exists(root / "game-install-temp") && !fs::exists(root / "game-install-backup");
  }
};

void TestPlayGate() {
  using eck::GameCanStart;
  using eck::GameFiles;
  Check(GameCanStart(GameFiles::kReady, false), "valid files can start");
  Check(!GameCanStart(GameFiles::kMissing, false), "missing files keep Play disabled");
  Check(!GameCanStart(GameFiles::kIncomplete, false), "an incomplete install keeps Play disabled");
  Check(!GameCanStart(GameFiles::kUnsupported, false), "an unsupported default.xex keeps Play disabled");
  Check(!GameCanStart(GameFiles::kReady, true), "a running reinstall keeps Play disabled");
}

void TestFingerprintUnchanged() {
  const eck::GameSpec& game = eck::SupportedGame();
  Check(game.xex_size == 30003200, "the supported size is unchanged");
  Check(game.xex_sha256 == "1db4e96b413d5629206ffe63e73c0e1cfaca9eea24bcf6fced6db9b135ce120f",
        "the supported SHA-256 is unchanged");
  Check(game.required_files.size() == 18, "the required file list is unchanged");
  Check(eck::kTitleId == 0x475807D7, "the title is unchanged");

  const fs::path dir = fs::temp_directory_path() / "eck_installer_fingerprint";
  std::error_code ec;
  fs::remove_all(dir, ec);
  WriteText(dir / "default.xex", kSyntheticXex);
  Check(eck::CheckGameFiles(dir).state == eck::GameFiles::kUnsupported, "the real check refuses another default.xex");
  fs::remove_all(dir, ec);
}

void TestFreshInstall(const fs::path& base) {
  PortFolder port(base / "fresh");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kMissing, "no game yet");

  const eck::InstallResult installed = eck::InstallFromDiscImage(port.image, port.game, {}, spec);
  Check(installed.ok(), "a valid source is accepted");
  Check(installed.files == 2, "both files were copied");
  Check(eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kReady, "the install is ready");
  Check(port.NoWorkFolders(), "no work folder is left behind");
  Check(port.UserDataIntact(), "a first install leaves user data alone");
}

void TestRejectedSources(const fs::path& base) {
  PortFolder port(base / "rejected");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "the working install");
  WriteText(port.game / "marker.txt", "old install");

  const fs::path wrong_game = MakeImage(port.root / "source", kSyntheticXex, "wrong_game.iso", "SomeOtherGame");
  Check(eck::InstallFromDiscImage(wrong_game, port.game, {}, spec).status == eck::InstallStatus::kWrongTitle,
        "another game's disc is rejected");

  const fs::path wrong_xex = MakeImage(port.root / "source", "a different executable", "wrong_xex.iso");
  Check(eck::InstallFromDiscImage(wrong_xex, port.game, {}, spec).status == eck::InstallStatus::kWrongVersion,
        "a default.xex with another fingerprint is rejected");

  std::string same_size(kSyntheticXex);
  same_size[0] ^= 1;
  const fs::path tampered = MakeImage(port.root / "source", same_size, "tampered.iso");
  Check(eck::InstallFromDiscImage(tampered, port.game, {}, spec).status == eck::InstallStatus::kWrongVersion,
        "the same size with another hash is rejected");

  Check(ReadText(port.game / "marker.txt") == "old install", "rejected sources left the old install alone");
  Check(port.NoWorkFolders(), "a rejected source leaves no work folder");
  Check(port.UserDataIntact(), "a rejected source leaves user data alone");
}

void TestFailedReinstallKeepsTheGame(const fs::path& base) {
  PortFolder port(base / "failed");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "the working install");
  WriteText(port.game / "marker.txt", "old install");
  const auto old_install_intact = [&] {
    return ReadText(port.game / "marker.txt") == "old install" &&
           eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kReady;
  };

  const eck::GameSpec incomplete = SpecOf(hash, kNeverOnDisc);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, incomplete).status == eck::InstallStatus::kIncomplete,
        "a disc without the required files fails verification");
  Check(old_install_intact(), "failed verification keeps the old install");

  eck::InstallHooks cancel;
  cancel.progress = [](uint64_t, uint64_t) { return false; };
  Check(eck::InstallFromDiscImage(port.image, port.game, cancel, spec).status == eck::InstallStatus::kCancelled,
        "cancelling stops the install");
  Check(old_install_intact(), "cancelling keeps the old install");
  Check(port.NoWorkFolders(), "cancelling leaves no work folder");

  eck::InstallHooks activation_fails;
  activation_fails.rename = [&](const fs::path& from, const fs::path& to) {
    if (from.filename() == "game-install-temp") {
      return false;
    }
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
  };
  Check(eck::InstallFromDiscImage(port.image, port.game, activation_fails, spec).status ==
            eck::InstallStatus::kWriteFailed,
        "a failed swap is reported");
  Check(old_install_intact(), "a failed swap rolls the old install back");
  Check(port.NoWorkFolders(), "a rollback leaves no work folder");
  Check(port.UserDataIntact(), "failed reinstalls leave user data alone");
}

void TestSuccessfulReinstallKeepsUserData(const fs::path& base) {
  PortFolder port(base / "reinstall");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "the first install");
  WriteText(port.game / "marker.txt", "old install");
  WriteText(port.game / "default.xex", "damaged");
  Check(eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kUnsupported, "the install is damaged");

  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "a reinstall repairs it");
  Check(eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kReady, "the repaired game is ready");
  Check(!fs::exists(port.game / "marker.txt"), "the game folder was replaced");
  Check(ReadText(port.settings) == "language = \"pt-BR\"\n", "settings survive a reinstall");
  Check(ReadText(port.save) == "save bytes", "saves survive a reinstall");
  Check(ReadText(port.achievements) == "unlocked = [1, 2]\n", "achievements survive a reinstall");
  Check(ReadText(port.identity) == "id = \"kept\"\n", "the online identity survives a reinstall");
  Check(ReadText(port.log) == "log text", "logs survive a reinstall");
  Check(port.NoWorkFolders(), "no work folder is left behind");
}

void TestInterruptedInstallRecovers(const fs::path& base) {
  PortFolder port(base / "killed");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "the working install");
  WriteText(port.game / "marker.txt", "old install");
  Check(!eck::RecoverInterruptedInstall(port.game), "a healthy install has nothing to recover");

  eck::InstallHooks killed_mid_swap;
  killed_mid_swap.rename = [](const fs::path& from, const fs::path& to) {
    if (from.filename() != "game") {
      return false;
    }
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
  };
  eck::InstallFromDiscImage(port.image, port.game, killed_mid_swap, spec);
  Check(!fs::exists(port.game) && fs::exists(port.root / "game-install-backup"), "the game is stranded in the backup");

  Check(eck::RecoverInterruptedInstall(port.game), "the next start restores it");
  Check(ReadText(port.game / "marker.txt") == "old install", "the old install is back");
  Check(eck::CheckGameFiles(port.game, spec).state == eck::GameFiles::kReady, "and ready");
  Check(!eck::RecoverInterruptedInstall(port.game), "recovery runs once");

  eck::InstallFromDiscImage(port.image, port.game, killed_mid_swap, spec);
  Check(!fs::exists(port.game), "stranded again");
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "a new install recovers and replaces it");
  Check(!fs::exists(port.game / "marker.txt") && port.NoWorkFolders(), "nothing of the old install is left over");

  fs::remove_all(port.game);
  fs::create_directories(port.root / "game-install-backup");
  Check(!eck::RecoverInterruptedInstall(port.game), "a backup without default.xex is not restored");
}

void TestOverlapsAreRefused(const fs::path& base) {
  PortFolder port(base / "overlap");
  const std::string hash = SyntheticHash();
  const eck::GameSpec spec = SpecOf(hash);
  Check(eck::InstallFromDiscImage(port.image, port.game, {}, spec).ok(), "the working install");

  const fs::path inside = MakeImage(port.game, kSyntheticXex, "inside.iso");
  Check(eck::InstallFromDiscImage(inside, port.game, {}, spec).status == eck::InstallStatus::kImageInGameFolder,
        "an image inside the game folder would delete itself");
  Check(fs::exists(inside), "it is left where it was");

  eck::InstallHooks user_data_inside;
  user_data_inside.user_data = port.game / "user_data";
  Check(eck::InstallFromDiscImage(port.image, port.game, user_data_inside, spec).status ==
            eck::InstallStatus::kForeignFolder,
        "user data inside the game folder would be replaced with it");

  eck::InstallHooks user_data_beside;
  user_data_beside.user_data = port.root / "user_data";
  Check(eck::InstallFromDiscImage(port.image, port.game, user_data_beside, spec).ok(),
        "user data beside the game folder is the normal case");
}

}  // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "eck_installer_tests";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);

  const std::filesystem::path image = MakeImage(dir, "not the real executable");
  {
    // The image stays open while `disc` lives.
    eck::xiso::Image disc;
    Check(disc.Open(image).ok(), "a synthetic image opens");
    Check(disc.file_count() == 2, "both files are listed");
    Check(disc.Find("DEFAULT.XEX") != nullptr, "lookup ignores case");
    Check(disc.Find("chavokartgame/README.TXT") != nullptr, "files below folders are listed");

    const std::filesystem::path out = dir / "out";
    Check(disc.ExtractTo(out, {}, {}).ok(), "extraction succeeds");
    std::ifstream readme(out / "ChavoKartGame" / "readme.txt");
    std::string text;
    readme >> text;
    Check(text == "hello", "extracted content matches");
  }

  const std::filesystem::path game = dir / "game";
  const eck::InstallResult wrong = eck::InstallFromDiscImage(image, game);
  Check(wrong.status == eck::InstallStatus::kWrongVersion, "another default.xex is rejected before copying");
  Check(!std::filesystem::exists(game), "a rejected install leaves no game folder");

  std::ofstream(dir / "plain.bin") << "not an image";
  Check(eck::InstallFromDiscImage(dir / "plain.bin", game).status == eck::InstallStatus::kNotImage,
        "a file that is not a disc image is rejected");
  Check(eck::CheckGameFiles(dir / "missing").state == eck::GameFiles::kMissing, "a missing folder reads as missing");
  Check(eck::DefaultGameFolder(dir) == dir / "game", "without game-folder.txt the game is next to the program");
  std::ofstream(dir / "game-folder.txt") << "C:\\games\\chavo\r\n";
  Check(eck::DefaultGameFolder(dir) == std::filesystem::path("C:\\games\\chavo"), "game-folder.txt names the folder");

  TestPlayGate();
  TestFingerprintUnchanged();
  TestFreshInstall(dir);
  TestRejectedSources(dir);
  TestFailedReinstallKeepsTheGame(dir);
  TestSuccessfulReinstallKeepsUserData(dir);
  TestInterruptedInstallRecovers(dir);
  TestOverlapsAreRefused(dir);

  std::filesystem::remove_all(dir, ec);
  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
