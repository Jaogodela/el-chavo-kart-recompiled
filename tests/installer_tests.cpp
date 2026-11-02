#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_files.h"
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

// A bare XDVDFS partition: default.xex and ChavoKartGame/readme.txt.
std::filesystem::path MakeImage(const std::filesystem::path& dir, const std::string& xex) {
  std::vector<uint8_t> image(kSector * 40, 0);
  const char magic[] = "MICROSOFT*XBOX*MEDIA";
  std::memcpy(image.data() + 32 * kSector, magic, 20);
  std::memcpy(image.data() + 32 * kSector + 0x7EC, magic, 20);
  PutLe32(image, 32 * kSector + 20, 33);
  PutLe32(image, 32 * kSector + 24, kSector);

  std::vector<uint8_t> root(kSector, 0xFF);
  const size_t second = PutEntry(root, 0, 0, 35, uint32_t(xex.size()), false, "default.xex");
  PutLe16(root, 2, uint16_t(second / 4));
  PutEntry(root, second, 0, 34, kSector, true, "ChavoKartGame");
  std::memcpy(image.data() + 33 * kSector, root.data(), root.size());

  std::vector<uint8_t> sub(kSector, 0xFF);
  const std::string readme = "hello";
  PutEntry(sub, 0, 0, 36, uint32_t(readme.size()), false, "readme.txt");
  std::memcpy(image.data() + 34 * kSector, sub.data(), sub.size());
  std::memcpy(image.data() + 35 * kSector, xex.data(), xex.size());
  std::memcpy(image.data() + 36 * kSector, readme.data(), readme.size());

  const std::filesystem::path path = dir / "test.iso";
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
  return path;
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

  std::filesystem::remove_all(dir, ec);
  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
