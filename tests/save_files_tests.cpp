// How the game's saves reach the disk: the ReXGlue virtual file system and the host path device the content manager mounts
// for a save package (allow_share_delete on, symbolic link "save:"), on real temporary folders, run against the patched
// runtime. A save is created, written, reopened by a second file system, overwritten in place, found under another letter
// case and deleted. Run: eck_save_files_tests

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>

namespace fs = std::filesystem;
using rex::filesystem::File;
using rex::filesystem::FileAction;
using rex::filesystem::FileDisposition;
using rex::filesystem::HostPathDevice;
using rex::filesystem::VirtualFileSystem;

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

constexpr uint32_t kRead = 0x80000000;
constexpr uint32_t kWrite = 0x40000000;
constexpr char kDevice[] = "\\Device\\Content\\7\\";

std::string HostRead(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// What the content manager does for a save package.
struct SaveVolume {
  VirtualFileSystem vfs;
  bool mounted = false;
  explicit SaveVolume(const fs::path& host) {
    auto device = std::make_unique<HostPathDevice>(kDevice, host, false, true);
    mounted = device->Initialize() && vfs.RegisterDevice(std::move(device)) && vfs.RegisterSymbolicLink("save:", kDevice);
  }

  bool Open(const std::string& path, FileDisposition disposition, uint32_t access, FileAction* action,
            File** file) {
    *file = nullptr;
    return vfs.OpenFile(nullptr, path, disposition, access, false, true, file, action) == 0 && *file != nullptr;
  }

  bool Write(const std::string& path, FileDisposition disposition, const std::string& text, FileAction* action) {
    File* file = nullptr;
    if (!Open(path, disposition, kWrite, action, &file)) return false;
    size_t written = 0;
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    const bool ok = file->WriteSync(bytes, 0, &written) == 0 && written == text.size();
    file->Destroy();
    return ok;
  }

  bool Read(const std::string& path, std::string* text) {
    File* file = nullptr;
    FileAction action{};
    if (!Open(path, FileDisposition::kOpen, kRead, &action, &file)) return false;
    std::vector<uint8_t> buffer(4096);
    size_t read = 0;
    const bool ok = file->ReadSync(std::span<uint8_t>(buffer), 0, &read) == 0;
    file->Destroy();
    text->assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(read));
    return ok;
  }
};

struct Folders {
  fs::path root;
  fs::path save;
  Folders() {
    root = fs::temp_directory_path() / "eck-save-files-tests";
    fs::remove_all(root);
    save = root / fs::path(u8"João Português") / fs::path(u8"Ação") / "user_data" / "profile" / "475807D7" / "00000001";
  }
  ~Folders() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void TestCreateWriteReopen() {
  std::printf("a save is created, written and found by a second file system that mounts the same folder\n");
  Folders f;
  FileAction action{};
  {
    SaveVolume volume(f.save);
    CHECK(volume.mounted);
    CHECK(fs::is_directory(f.save));
    CHECK(volume.Write("save:\\savegame.dat", FileDisposition::kCreate, "lap times 01:23.456", &action));
    CHECK(action == FileAction::kCreated);
    CHECK(HostRead(f.save / "savegame.dat") == "lap times 01:23.456");
  }
  SaveVolume reopened(f.save);
  CHECK(reopened.mounted);
  std::string text;
  CHECK(reopened.Read("save:\\savegame.dat", &text) && text == "lap times 01:23.456");

  std::printf("creating a save that exists fails and keeps it; opening one that does not exist fails\n");
  CHECK(!reopened.Write("save:\\savegame.dat", FileDisposition::kCreate, "replacement", &action));
  CHECK(HostRead(f.save / "savegame.dat") == "lap times 01:23.456");
  File* file = nullptr;
  CHECK(!reopened.Open("save:\\absent.dat", FileDisposition::kOpen, kRead, &action, &file));
  CHECK(!fs::exists(f.save / "absent.dat"));
}

void TestOverwriteInPlace() {
  std::printf("a shorter save written over a longer one leaves none of the old bytes\n");
  Folders f;
  SaveVolume volume(f.save);
  FileAction action{};
  CHECK(volume.Write("save:\\profile.dat", FileDisposition::kCreate, "0123456789ABCDEFGHIJ", &action));
  CHECK(volume.Write("save:\\profile.dat", FileDisposition::kSuperscede, "short", &action));
  CHECK(action == FileAction::kSuperseded);
  CHECK(HostRead(f.save / "profile.dat") == "short");
  CHECK(volume.Write("save:\\profile.dat", FileDisposition::kOverwriteIf, "tiny", &action));
  CHECK(action == FileAction::kOverwritten);
  CHECK(HostRead(f.save / "profile.dat") == "tiny");
  CHECK(volume.Write("save:\\second.dat", FileDisposition::kOverwriteIf, "created by overwrite-if", &action));
  CHECK(action == FileAction::kCreated);

  SaveVolume reopened(f.save);
  std::string text;
  CHECK(reopened.Read("save:\\profile.dat", &text) && text == "tiny");
  CHECK(reopened.Read("save:\\second.dat", &text) && text == "created by overwrite-if");
}

void TestOverwriteWhileOpenForReading() {
  std::printf("a save slot preview that is still open does not stop the save from being replaced\n");
  Folders f;
  SaveVolume volume(f.save);
  FileAction action{};
  CHECK(volume.Write("save:\\slot.dat", FileDisposition::kCreate, "first", &action));
  File* preview = nullptr;
  CHECK(volume.Open("save:\\slot.dat", FileDisposition::kOpen, kRead, &action, &preview));
  CHECK(volume.Write("save:\\slot.dat", FileDisposition::kSuperscede, "second", &action));
  if (preview) preview->Destroy();
  CHECK(HostRead(f.save / "slot.dat") == "second");
}

void TestLetterCase() {
  std::printf("a save is found under another letter case and no second file appears\n");
  Folders f;
  SaveVolume volume(f.save);
  FileAction action{};
  CHECK(volume.Write("save:\\SaveGame.Dat", FileDisposition::kCreate, "kept", &action));
  std::string text;
  CHECK(volume.Read("save:\\savegame.dat", &text) && text == "kept");
  CHECK(volume.Read("save:\\SAVEGAME.DAT", &text) && text == "kept");
  CHECK(volume.Write("save:\\savegame.dat", FileDisposition::kOverwriteIf, "updated", &action));
  size_t files = 0;
  for (const auto& entry : fs::directory_iterator(f.save)) {
    (void)entry;
    ++files;
  }
  CHECK(files == 1);
  SaveVolume reopened(f.save);
  CHECK(reopened.Read("save:\\SaveGame.Dat", &text) && text == "updated");
  CHECK(reopened.Read("save:\\SAVEGAME.DAT", &text) && text == "updated");
}

void TestFoldersAndDelete() {
  std::printf("a save in a folder is written, found after a remount and deleted\n");
  Folders f;
  SaveVolume volume(f.save);
  FileAction action{};
  CHECK(volume.vfs.CreatePath("save:\\Slot1", 0x10) != nullptr);
  CHECK(fs::is_directory(f.save / "Slot1"));
  CHECK(volume.Write("save:\\Slot1\\data.sav", FileDisposition::kCreate, "slot one", &action));
  SaveVolume reopened(f.save);
  std::string text;
  CHECK(reopened.Read("save:\\Slot1\\data.sav", &text) && text == "slot one");
  CHECK(reopened.Read("save:\\slot1\\DATA.SAV", &text) && text == "slot one");
  CHECK(reopened.vfs.DeletePath("save:\\Slot1\\data.sav"));
  CHECK(!fs::exists(f.save / "Slot1" / "data.sav"));
  File* file = nullptr;
  CHECK(!reopened.Open("save:\\Slot1\\data.sav", FileDisposition::kOpen, kRead, &action, &file));
}

}  // namespace

int main() {
  TestCreateWriteReopen();
  TestOverwriteInPlace();
  TestOverwriteWhileOpenForReading();
  TestLetterCase();
  TestFoldersAndDelete();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
