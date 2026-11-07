// The achievements as ReXGlue's AchievementManager keeps them, with the port's list and summary on top: 46 achievements
// and 1000 gamerscore, unlock, a second unlock of the same one, progress, the unlock file in a user-data folder with
// spaces and accents, reload by a second manager, and (on Linux) the AppImage user-data location. Real files in
// temporary folders. Run: eck_achievement_store_tests

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/system/achievement_manager.h>

#include "achievement_list.h"
#include "platform.h"
#include "user_data.h"

namespace fs = std::filesystem;
using rex::system::AchievementInfo;
using rex::system::AchievementManager;
using rex::system::AchievementUnlockResult;

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

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool ParsesAsToml(const std::string& text) {
  try {
    (void)toml::parse(text);
    return true;
  } catch (const toml::parse_error&) {
    return false;
  }
}

// The shipped catalog's shape: IDs 1 to 46, 44 of 20 and two of 60 for 1000 gamerscore.
std::vector<AchievementInfo> Catalog() {
  std::vector<AchievementInfo> catalog;
  for (uint32_t id = 46; id >= 1; --id) {
    AchievementInfo info;
    info.id = id;
    info.label = "Achievement " + std::to_string(id);
    info.description = "Earned " + std::to_string(id);
    info.unachieved_description = "Locked " + std::to_string(id);
    info.gamerscore = id <= 44 ? 20 : 60;
    catalog.push_back(std::move(info));
  }
  return catalog;
}

fs::path UnlockFile(const fs::path& user_data_root) { return user_data_root / "achievements" / "475807D7.toml"; }

struct Folders {
  fs::path root;
  Folders() {
    root = fs::temp_directory_path() / "eck-achievement-store-tests";
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~Folders() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

void LoadInto(AchievementManager& manager, const fs::path& file) {
  manager.ReplaceAchievements(Catalog());
  manager.SetUnlockSavePath(file);
  manager.LoadUnlockState();
}

void TestCatalogAndSummary() {
  std::printf("46 achievements, 1000 gamerscore, all locked on a new profile\n");
  Folders f;
  AchievementManager manager;
  LoadInto(manager, UnlockFile(f.root));
  const auto entries = eck::ListAchievements(manager);
  CHECK(entries.size() == 46);
  CHECK(entries.front().info.id == 1 && entries.back().info.id == 46);
  const eck::AchievementSummary summary = eck::Summarize(entries);
  CHECK(summary.total == 46 && summary.total_gamerscore == 1000);
  CHECK(summary.unlocked == 0 && summary.earned_gamerscore == 0);
  CHECK(!fs::exists(UnlockFile(f.root)));
}

void TestUnlockDuplicateAndProgress() {
  std::printf("an unlock is saved at once; the same one again changes nothing; progress adds up\n");
  Folders f;
  const fs::path file = UnlockFile(f.root / fs::path(u8"João Português") / fs::path(u8"Ação") / "user_data");
  AchievementManager manager;
  LoadInto(manager, file);
  int callbacks = 0;
  manager.RegisterUnlockCallback([&](const rex::system::AchievementEvent&) { ++callbacks; });

  CHECK(manager.UnlockAchievement(1) == AchievementUnlockResult::kUnlocked);
  CHECK(fs::is_regular_file(file));
  CHECK(ParsesAsToml(Read(file)));
  CHECK(manager.IsUnlocked(1) && !manager.IsUnlocked(2));
  const uint64_t first_time = manager.GetUnlockTime(1);
  CHECK(first_time != 0);

  CHECK(manager.UnlockAchievement(1) == AchievementUnlockResult::kAlreadyUnlocked);
  CHECK(manager.GetUnlockTime(1) == first_time);
  CHECK(callbacks == 1);
  CHECK(manager.UnlockAchievement(9999) == AchievementUnlockResult::kUnknownAchievement);
  CHECK(!manager.IsUnlocked(9999));

  CHECK(manager.UnlockAchievement(46) == AchievementUnlockResult::kUnlocked);
  CHECK(manager.UnlockAchievement(44) == AchievementUnlockResult::kUnlocked);
  eck::AchievementSummary summary = eck::Summarize(eck::ListAchievements(manager));
  CHECK(summary.unlocked == 3 && summary.earned_gamerscore == 20 + 60 + 20);
  for (uint32_t id = 1; id <= 46; ++id) manager.UnlockAchievement(id);
  summary = eck::Summarize(eck::ListAchievements(manager));
  CHECK(summary.unlocked == 46 && summary.earned_gamerscore == 1000);
  CHECK(callbacks == 46);
  CHECK(!fs::exists(fs::path(file.string() + ".tmp")));
}

void TestReloadAfterReopen() {
  std::printf("a second manager on the same folder sees the same unlocks and times\n");
  Folders f;
  const fs::path file = UnlockFile(f.root / fs::path(u8"João Português") / fs::path(u8"Ação") / "user_data");
  uint64_t time_5 = 0;
  {
    AchievementManager manager;
    LoadInto(manager, file);
    manager.UnlockAchievement(5);
    manager.UnlockAchievement(46);
    time_5 = manager.GetUnlockTime(5);
  }
  AchievementManager reopened;
  LoadInto(reopened, file);
  CHECK(reopened.IsUnlocked(5) && reopened.IsUnlocked(46) && !reopened.IsUnlocked(6));
  CHECK(reopened.GetUnlockTime(5) == time_5);
  const eck::AchievementSummary summary = eck::Summarize(eck::ListAchievements(reopened));
  CHECK(summary.unlocked == 2 && summary.earned_gamerscore == 80);
  CHECK(reopened.UnlockAchievement(5) == AchievementUnlockResult::kAlreadyUnlocked);
  CHECK(reopened.UnlockAchievement(6) == AchievementUnlockResult::kUnlocked);

  AchievementManager third;
  LoadInto(third, file);
  CHECK(third.IsUnlocked(5) && third.IsUnlocked(6) && third.IsUnlocked(46));
}

void TestUnreadableUnlockFile() {
  std::printf("an unlock file that is not valid TOML unlocks nothing and the next unlock rewrites it\n");
  Folders f;
  const fs::path file = UnlockFile(f.root / "user_data");
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << "[unlocked.3\nfiletime = ";
  AchievementManager manager;
  LoadInto(manager, file);
  CHECK(!manager.IsUnlocked(3));
  CHECK(manager.UnlockAchievement(7) == AchievementUnlockResult::kUnlocked);
  CHECK(ParsesAsToml(Read(file)));
  AchievementManager reopened;
  LoadInto(reopened, file);
  CHECK(reopened.IsUnlocked(7) && !reopened.IsUnlocked(3));
}

void TestUserDataLocation() {
  std::printf("unlocks live in <user data>/achievements, beside the program or the AppImage\n");
  Folders f;
  const fs::path port = f.root / fs::path(u8"Meu Jogo é") / "Port Folder";
  fs::create_directories(port);
  const eck::UserDataSelection selection = eck::SelectUserData({port, f.root / "system", false});
  CHECK(selection.root == port / "user_data");
  AchievementManager manager;
  LoadInto(manager, UnlockFile(selection.root));
  CHECK(manager.UnlockAchievement(12) == AchievementUnlockResult::kUnlocked);
  CHECK(fs::is_regular_file(port / "user_data" / "achievements" / "475807D7.toml"));
  CHECK(!fs::exists(f.root / "system"));

#if !defined(_WIN32)
  const fs::path home = f.root / fs::path(u8"João Português") / fs::path(u8"Ação");
  const fs::path appimage = home / "El Chavo Kart.AppImage";
  const fs::path mount = f.root / "tmp" / ".mount_ElChavoXyz";
  fs::create_directories(mount / "usr" / "bin");
  std::ofstream(appimage) << "image";
  setenv("APPIMAGE", appimage.c_str(), 1);
  setenv("APPDIR", mount.c_str(), 1);
  const eck::UserDataSelection packed = eck::SelectUserData({eck::PortFolder(), f.root / "system", false});
  CHECK(packed.root == home / "user_data");
  AchievementManager in_image;
  LoadInto(in_image, UnlockFile(packed.root));
  CHECK(in_image.UnlockAchievement(30) == AchievementUnlockResult::kUnlocked);
  CHECK(fs::is_regular_file(home / "user_data" / "achievements" / "475807D7.toml"));
  size_t mounted_files = 0;
  for (const auto& entry : fs::recursive_directory_iterator(mount)) {
    (void)entry;
    ++mounted_files;
  }
  CHECK(mounted_files == 2);
  CHECK(!fs::exists(mount / "user_data") && !fs::exists(mount / "achievements"));
  AchievementManager again;
  LoadInto(again, UnlockFile(packed.root));
  CHECK(again.IsUnlocked(30));
  unsetenv("APPIMAGE");
  unsetenv("APPDIR");
#endif
}

}  // namespace

int main() {
  TestCatalogAndSummary();
  TestUnlockDuplicateAndProgress();
  TestReloadAfterReopen();
  TestUnreadableUnlockFile();
  TestUserDataLocation();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
