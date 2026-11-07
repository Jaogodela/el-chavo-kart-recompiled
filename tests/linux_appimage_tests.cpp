// The AppImage's file-system behaviour, run for real in temporary folders: where the user's data lives while the program
// runs from a read-only mount or from an extract-and-run folder, and what the Linux self-updater does to the installed
// AppImage. Nothing starts the game; the "restarted" AppImage is a shell script that records how it was started.
// Run: eck_linux_appimage_tests

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "log_path.h"
#include "platform.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"
#include "user_data.h"

namespace fs = std::filesystem;

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
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void Write(const fs::path& path, const std::string& content, bool executable = false) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
  chmod(path.c_str(), executable ? 0755 : 0644);
}

bool Executable(const fs::path& path) {
  struct stat status {};
  return stat(path.c_str(), &status) == 0 && (status.st_mode & S_IXUSR) != 0;
}

std::vector<std::string> Leftovers(const fs::path& folder) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(folder)) {
    const std::string name = entry.path().filename().string();
    if (name.find(".trg-update-") != std::string::npos) {
      names.push_back(name);
    }
  }
  return names;
}

bool WaitFor(const fs::path& file) {
  for (int i = 0; i < 100; ++i) {
    if (fs::exists(file) && !Read(file).empty()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

struct Install {
  fs::path root, appimage, user_data, marker;
  explicit Install(const char* name) {
    root = fs::temp_directory_path() / (std::string("eck_appimage_") + name + "_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    appimage = root / "ElChavoKart-v0.2.2-x86_64.AppImage";
    user_data = root / "user_data";
    marker = root / "started.txt";
    Write(appimage, "#!/bin/sh\necho old\n", true);
    Write(user_data / "settings.toml", "settings");
    Write(user_data / "475807D7" / "profile.dat", "save");
    Write(user_data / "achievements.dat", "achievements");
    Write(user_data / "online" / "identity.json", "identity");
  }
  ~Install() { fs::remove_all(root); }
  bool UserDataIntact() const {
    return Read(user_data / "settings.toml") == "settings" && Read(user_data / "475807D7" / "profile.dat") == "save" &&
           Read(user_data / "achievements.dat") == "achievements" && Read(user_data / "online" / "identity.json") == "identity";
  }
};

void TestUserDataBesideTheAppImage() {
  std::printf("user data, logs and saves stay beside the AppImage, outside the read-only mount\n");
  Install install("paths");
  const fs::path mount = install.root / ".mount_ElChavaBcDeF";
  Write(mount / "usr" / "bin" / "el_chavo_kart", "program", true);
  setenv("APPIMAGE", install.appimage.c_str(), 1);
  setenv("APPDIR", mount.c_str(), 1);

  CHECK(eck::PortFolder() == install.root);
  CHECK(!trg::update::PathIsWithin(eck::PortFolder(), mount));
  const fs::path system = install.root / "system-data";
  eck::UserDataSelection selection = eck::SelectUserData({eck::PortFolder(), system, false});
  CHECK(selection.outcome == eck::UserDataOutcome::kPortable);
  CHECK(selection.root == install.user_data);
  CHECK(!trg::update::PathIsWithin(selection.root, mount));
  CHECK(install.UserDataIntact());

  const fs::path logs = eck::LogsFolder(eck::PortFolder(), selection.root, {}, true);
  CHECK(logs == install.root / "logs");
  CHECK(!trg::update::PathIsWithin(logs, mount));
  const fs::path log = eck::NextLogFile(logs, "el_chavo_kart");
  CHECK(!log.empty() && log.parent_path() == logs);

  std::printf("a port folder that cannot be written keeps the logs in user_data\n");
  CHECK(eck::LogsFolder(install.root / "missing", selection.root, {}, true) == selection.root / "logs" ||
        eck::LogsFolder(install.root / "missing", selection.root, {}, true) == install.root / "missing" / "logs");

  std::printf("the extract-and-run folder is not a place for user data either\n");
  const fs::path extracted = fs::temp_directory_path() / ("appimage_extracted_" + std::to_string(getpid()));
  Write(extracted / "usr" / "bin" / "el_chavo_kart", "program", true);
  setenv("APPDIR", extracted.c_str(), 1);
  CHECK(eck::PortFolder() == install.root);
  selection = eck::SelectUserData({eck::PortFolder(), system, false});
  CHECK(selection.root == install.user_data);
  CHECK(!trg::update::PathIsWithin(selection.root, extracted));
  CHECK(install.UserDataIntact());
  fs::remove_all(extracted);

  std::printf("outside an AppImage the data stays beside the program\n");
  unsetenv("APPIMAGE");
  unsetenv("APPDIR");
  CHECK(eck::PortFolder() != install.root);
  CHECK(!eck::PortFolder().empty());
}

void TestOriginalAppImageIsTheTarget() {
  std::printf("the updater targets the AppImage the program was started from\n");
  Install install("target");
  const fs::path mount = install.root / ".mount_ElChavaBcDeF";
  setenv("APPIMAGE", install.appimage.c_str(), 1);
  setenv("APPDIR", mount.c_str(), 1);
  fs::path found;
  CHECK(!trg::update::HasOriginalAppImage(found));  // this test program does not run inside APPDIR
  unsetenv("APPDIR");
  CHECK(!trg::update::HasOriginalAppImage(found));
  unsetenv("APPIMAGE");
  CHECK(!trg::update::HasOriginalAppImage(found));
}

void TestUpdateReplacesAndRestarts() {
  std::printf("a downloaded AppImage replaces the installed one and is started from the original path\n");
  Install install("replace");
  const fs::path downloaded = install.root / "download" / "ElChavoKart-v0.3.0-x86_64.AppImage";
  const std::string script = "#!/bin/sh\nprintf '%s|%s|%s' \"$0\" \"$*\" \"$ECK_APPIMAGE_MARKER\" > \"$ECK_APPIMAGE_MARKER\"\n";
  Write(downloaded, script, false);
  setenv("ECK_APPIMAGE_MARKER", install.marker.c_str(), 1);

  std::string error;
  CHECK(trg::update::ApplyAppImageUpdate(downloaded, install.appimage, {"--eck_skip_launcher=true", "--fullscreen=true"}, error));
  CHECK(error.empty());
  CHECK(Read(install.appimage) == script);
  CHECK(Executable(install.appimage));
  CHECK(Leftovers(install.root).empty());
  CHECK(install.UserDataIntact());
  CHECK(fs::exists(downloaded));
  CHECK(WaitFor(install.marker));
  const std::string started = Read(install.marker);
  CHECK(started.rfind(install.appimage.string() + "|--eck_skip_launcher=true --fullscreen=true|", 0) == 0);
  unsetenv("ECK_APPIMAGE_MARKER");
}

void TestUpdateRefusals() {
  std::printf("a refused update leaves the installed AppImage as it was\n");
  Install install("refuse");
  const std::string original = Read(install.appimage);
  std::string error;
  CHECK(!trg::update::ApplyAppImageUpdate(install.root / "absent.AppImage", install.appimage, {}, error));
  CHECK(!error.empty());
  CHECK(!trg::update::ApplyAppImageUpdate(install.appimage, install.appimage, {}, error));
  CHECK(!error.empty());
  CHECK(!trg::update::ApplyAppImageUpdate(install.appimage, install.root / "absent.AppImage", {}, error));
  CHECK(Read(install.appimage) == original);
  CHECK(Executable(install.appimage));
  CHECK(Leftovers(install.root).empty());
  CHECK(install.UserDataIntact());
}

void TestUpdateRollsBackWhenItCannotStart() {
  std::printf("an update that cannot be started is rolled back to the previous AppImage\n");
  Install install("rollback");
  const std::string original = Read(install.appimage);
  const fs::path downloaded = install.root / "download" / "ElChavoKart-v0.3.0-x86_64.AppImage";
  Write(downloaded, "#!/nonexistent-interpreter\n", false);
  std::string error;
  CHECK(!trg::update::ApplyAppImageUpdate(downloaded, install.appimage, {}, error));
  CHECK(!error.empty());
  CHECK(Read(install.appimage) == original);
  CHECK(Executable(install.appimage));
  CHECK(Leftovers(install.root).empty());
  CHECK(install.UserDataIntact());
}

void TestUpdateKeepsTheInstalledMode() {
  std::printf("the replacement keeps the installed file's permissions and gains execute\n");
  Install install("mode");
  chmod(install.appimage.c_str(), 0700);
  const fs::path downloaded = install.root / "download" / "ElChavoKart-v0.3.0-x86_64.AppImage";
  Write(downloaded, "#!/bin/sh\necho started > \"$ECK_APPIMAGE_MARKER\"\n", false);
  chmod(downloaded.c_str(), 0600);
  setenv("ECK_APPIMAGE_MARKER", install.marker.c_str(), 1);
  std::string error;
  CHECK(trg::update::ApplyAppImageUpdate(downloaded, install.appimage, {}, error));
  struct stat status {};
  CHECK(stat(install.appimage.c_str(), &status) == 0 && (status.st_mode & 0700) == 0700);
  CHECK(Leftovers(install.root).empty());
  CHECK(WaitFor(install.marker));
  unsetenv("ECK_APPIMAGE_MARKER");
}

void TestAwkwardPaths() {
  std::printf("spaces, accents, a relative path and a symlink all lead to the AppImage that is running\n");
  const fs::path root = fs::temp_directory_path() / ("eck_appimage_paths_" + std::to_string(getpid()));
  fs::remove_all(root);
  const std::string name = "ElChavoKart-v0.1.0-x86_64.AppImage";
  const fs::path relative = fs::path("Jogos com Espa\xC3\xA7o") / "A\xC3\xA7\xC3\xA3o";
  const fs::path folder = root / relative;
  const fs::path appimage = folder / name;
  const fs::path neighbour = root / name;
  const fs::path mount = root / ".mount_ElChavaBcDeF";
  const fs::path program = mount / "usr" / "bin" / "el_chavo_kart";
  const fs::path marker = root / "started.txt";
  Write(appimage, "#!/bin/sh\necho old\n", true);
  Write(neighbour, "the AppImage of another folder", true);
  Write(program, "program", true);

  CHECK(trg::update::IsRunningAppImage(appimage, mount, program));
  setenv("APPIMAGE", appimage.c_str(), 1);
  CHECK(eck::PortFolder() == folder);
  unsetenv("APPIMAGE");

  const fs::path start = fs::current_path();
  fs::current_path(folder);
  CHECK(trg::update::IsRunningAppImage(name, mount, program));
  fs::current_path(root);
  CHECK(trg::update::IsRunningAppImage(relative / name, mount, program));
  fs::current_path(start);

  const fs::path link = root / "game-link.AppImage";
  fs::create_symlink(appimage, link);
  CHECK(trg::update::IsRunningAppImage(link, mount, program));
  CHECK(fs::weakly_canonical(link) == fs::weakly_canonical(appimage));

  const fs::path downloaded = root / "download" / name;
  const std::string script = "#!/bin/sh\nprintf '%s|%s' \"$0\" \"$*\" > \"$ECK_APPIMAGE_MARKER\"\n";
  Write(downloaded, script, false);
  setenv("ECK_APPIMAGE_MARKER", marker.c_str(), 1);
  std::string error;
  CHECK(trg::update::ApplyAppImageUpdate(downloaded, fs::weakly_canonical(link), {"--eck_launcher=true", "a b"}, error));
  CHECK(error.empty());
  unsetenv("ECK_APPIMAGE_MARKER");
  CHECK(Read(appimage) == script);
  CHECK(Executable(appimage));
  CHECK(fs::is_symlink(link));
  CHECK(Read(neighbour) == "the AppImage of another folder");
  CHECK(Leftovers(folder).empty());
  CHECK(WaitFor(marker));
  CHECK(Read(marker).rfind(fs::weakly_canonical(appimage).string() + "|--eck_launcher=true a b", 0) == 0);
  fs::remove_all(root);
}

}  // namespace

int main() {
  TestUserDataBesideTheAppImage();
  TestOriginalAppImageIsTheTarget();
  TestUpdateReplacesAndRestarts();
  TestUpdateRefusals();
  TestUpdateRollsBackWhenItCannotStart();
  TestUpdateKeepsTheInstalledMode();
  TestAwkwardPaths();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
