// The player's settings file through the production GameSettings (src/settings.cpp), the launcher's TextFileSettings
// editor and the SDK's cvar loader, on real files in temporary folders. The same source builds on Windows and Linux.
// Run: eck_settings_tests

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "connection_test_gate.h"
#include "language_setting.h"
#include "settings.h"
#include "user_data.h"

#include "platform.h"

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
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool ParsesAsToml(const std::string& text) {
  try {
    (void)toml::parse(text);
    return true;
  } catch (const toml::parse_error&) {
    return false;
  }
}

std::string Cvar(const char* key) { return rex::cvar::GetFlagByName(key); }

// Every setting these tests touch, back to its registered default, as a fresh process would start.
void ResetCvars() {
  for (const char* key :
       {"eck_launcher", "eck_language", "eck_frame_rate", "eck_show_fps", "eck_skip_intro", "eck_graphics_adapter",
        "eck_player_1_name", "eck_player_2_name", "eck_online_enabled", "eck_online_name", "eck_online_ping_display",
        "eck_online_connection_test", "eck_launcher_x", "eck_launcher_width", "detailed_logging"}) {
    rex::cvar::ResetToDefault(key);
  }
}

void Reload(const fs::path& file) {
  ResetCvars();
  eck::LoadPlayerSettings(file);
}

struct Folders {
  fs::path root;
  fs::path data;
  fs::path player;
  fs::path shipped;
  Folders(const char* name, const fs::path& middle) {
    root = fs::temp_directory_path() / (std::string("eck-settings-tests-") + name);
    fs::remove_all(root);
    data = root / middle / "user_data";
    player = eck::PlayerSettingsPath(data);
    shipped = root / "shipped" / "el_chavo_kart.toml";
    Write(shipped, "eck_language = \"en-US\"\neck_show_fps = true\n");
  }
  ~Folders() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

const fs::path kAccented = fs::path(u8"João Português") / fs::path(u8"Ação");
const std::string kAccentedName = "Jo\xC3\xA3o A\xC3\xA7\xC3\xA3o";

void CaptureFresh(eck::GameSettings& settings, const Folders& f) {
  ResetCvars();
  eck::LoadPlayerSettings(f.player);
  settings.Capture(f.player, f.shipped);
}

void TestFreshSaveAndReload(const char* label, const fs::path& middle) {
  std::printf("a first Save creates a valid settings.toml that reloads exactly (%s)\n", label);
  Folders f("fresh", middle);
  eck::GameSettings settings;
  CaptureFresh(settings, f);
  CHECK(!fs::exists(f.player));
  settings.Set("eck_language", "pt-BR");
  settings.Set("eck_show_fps", "false");
  settings.Set("eck_frame_rate", "144");
  settings.Set("eck_online_name", "Chaves_8");
  CHECK(settings.Save());

  const std::string text = Read(f.player);
  CHECK(fs::is_regular_file(f.player));
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "eck_language = \"pt-BR\""));
  CHECK(Has(text, "eck_online_name = \"Chaves_8\""));
  CHECK(Has(text, "eck_show_fps = false"));
  CHECK(Has(text, "eck_frame_rate = 144"));
  CHECK(!Has(text, "eck_frame_rate = \"144\""));
  CHECK(!Has(text, "eck_show_fps = \"false\""));

  Reload(f.player);
  CHECK(Cvar("eck_language") == "pt-BR");
  CHECK(Cvar("eck_show_fps") == "false");
  CHECK(Cvar("eck_frame_rate") == "144");
  CHECK(Cvar("eck_online_name") == "Chaves_8");

  eck::GameSettings again;
  again.Capture(f.player, f.shipped);
  CHECK(again.Get("eck_language") == "pt-BR");
  CHECK(again.Get("eck_frame_rate") == "144");
  CHECK(again.Save());
  CHECK(Read(f.player) == text);
}

void TestUnicodeAndEscapes() {
  std::printf("Unicode, quotes and backslashes in string settings survive a save and a reload\n");
  Folders f("unicode", kAccented);
  eck::GameSettings settings;
  CaptureFresh(settings, f);
  const std::string tricky = "Placa \"Gr\xC3\xA1" "fica\" C:\\Jo\xC3\xA3o\\Pasta";
  settings.Set("eck_graphics_adapter", tricky);
  settings.Set("eck_player_1_name", kAccentedName);
  CHECK(settings.Save());
  const std::string text = Read(f.player);
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "\"Placa \\\"Gr\xC3\xA1" "fica\\\" C:\\\\Jo\xC3\xA3o\\\\Pasta\""));
  Reload(f.player);
  CHECK(Cvar("eck_graphics_adapter") == tricky);
  CHECK(Cvar("eck_player_1_name") == kAccentedName);
  CHECK(!fs::exists(eck::PlayerSettingsPath(f.data).string() + ".tmp"));
}

void TestUnrelatedContentSurvives() {
  std::printf("a save changes its own lines and leaves comments and unknown keys alone\n");
  Folders f("unrelated", kAccented);
  const std::string before =
      "# my notes\n"
      "some_future_setting = \"kept\"\n"
      "eck_show_fps = true\n"
      "eck_language = \"es-MX\"\n"
      "# end\n";
  Write(f.player, before);
  eck::GameSettings settings;
  CaptureFresh(settings, f);
  CHECK(Cvar("eck_language") == "es-MX");
  CHECK(settings.Save());
  CHECK(Read(f.player) == before);

  settings.Set("eck_show_fps", "false");
  CHECK(settings.Save());
  const std::string after = Read(f.player);
  CHECK(ParsesAsToml(after));
  CHECK(Has(after, "# my notes\nsome_future_setting = \"kept\"\neck_show_fps = false\neck_language = \"es-MX\"\n# end\n"));

  settings.Set("eck_skip_intro", "true");
  CHECK(settings.Save());
  const std::string appended = Read(f.player);
  CHECK(ParsesAsToml(appended));
  CHECK(Has(appended, "some_future_setting = \"kept\""));
  CHECK(Has(appended, "eck_skip_intro = true"));
  CHECK(Has(appended, "eck_language = \"es-MX\""));
}

void TestInvalidFileIsSetAsideAndRecovered(const char* label, const std::string& broken) {
  std::printf("an invalid settings.toml (%s) is kept as .invalid and the next save writes a valid file\n", label);
  Folders f("invalid", kAccented);
  Write(f.player, broken);
  CHECK(!ParsesAsToml(broken));
  const fs::path invalid = fs::path(f.player.string() + ".invalid");

  ResetCvars();
  eck::LoadPlayerSettings(f.player);
  CHECK(Cvar("eck_language") == eck::kDefaultLanguage);
  CHECK(!fs::exists(f.player));
  CHECK(Read(invalid) == broken);

  eck::GameSettings settings;
  settings.Capture(f.player, f.shipped);
  settings.Set("eck_language", "pt-BR");
  settings.Set("eck_frame_rate", "60");
  CHECK(settings.Save());
  const std::string text = Read(f.player);
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "eck_language = \"pt-BR\""));
  CHECK(Read(invalid) == broken);

  Reload(f.player);
  CHECK(Cvar("eck_language") == "pt-BR");
  CHECK(Cvar("eck_frame_rate") == "60");

  settings.Set("eck_show_fps", "false");
  CHECK(settings.Save());
  CHECK(ParsesAsToml(Read(f.player)));
  CHECK(Read(invalid) == broken);
}

void TestFileBrokenWhileRunning() {
  std::printf("a settings.toml broken after startup is set aside by the next save or SaveNow\n");
  for (const bool now : {false, true}) {
    Folders f("broken-while-running", kAccented);
    eck::GameSettings settings;
    CaptureFresh(settings, f);
    settings.Set("eck_show_fps", "false");
    CHECK(settings.Save());

    const std::string broken = "eck_show_fps = false\neck_language = \"pt-";
    Write(f.player, broken);
    if (now) {
      CHECK(settings.SaveNow({{"eck_frame_rate", "90"}}));
    } else {
      settings.Set("eck_frame_rate", "90");
      CHECK(settings.Save());
    }
    const std::string text = Read(f.player);
    CHECK(ParsesAsToml(text));
    CHECK(Has(text, "eck_frame_rate = 90"));
    CHECK(Read(fs::path(f.player.string() + ".invalid")) == broken);
    Reload(f.player);
    CHECK(Cvar("eck_frame_rate") == "90");
  }
}

void TestEmptyAndMissingFiles() {
  std::printf("a missing or empty settings.toml loads nothing and is not an error\n");
  Folders f("empty", kAccented);
  ResetCvars();
  eck::LoadPlayerSettings(f.player);
  CHECK(!fs::exists(f.player));
  CHECK(!fs::exists(fs::path(f.player.string() + ".invalid")));

  Write(f.player, "");
  eck::LoadPlayerSettings(f.player);
  CHECK(fs::exists(f.player));
  CHECK(!fs::exists(fs::path(f.player.string() + ".invalid")));

  eck::GameSettings settings;
  settings.Capture(f.player, f.shipped);
  settings.Set("eck_skip_intro", "true");
  CHECK(settings.Save());
  CHECK(ParsesAsToml(Read(f.player)));
}

void TestConnectionTestSetting() {
  std::printf("the Connection test result is saved at once, quoted, without touching the rest of the file\n");
  Folders f("connection", kAccented);
  const std::string before =
      "# El Chavo Kart player settings, written by the launcher.\n"
      "eck_language = \"pt-BR\"\n"
      "eck_online_name = \"Quico\"\n"
      "eck_frame_rate = 120\n"
      "eck_launcher_width = 1280\n"
      "unrelated_key = \"stays\"\n";
  Write(f.player, before);

  eck::GameSettings settings;
  CaptureFresh(settings, f);
  CHECK(Cvar("eck_online_name") == "Quico");
  settings.Set(eck::kOnlineEnabledSetting, "true");
  CHECK(eck::ConnectionTestNeeded(settings, false));
  const bool recorded = eck::RecordConnectionTest(settings, [&](std::string_view key, const std::string& value) {
    return settings.SaveNow({{std::string(key), value}});
  });
  CHECK(recorded);

  std::string text = Read(f.player);
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "eck_online_connection_test = \"" + eck::OnlineUdpConfiguration() + "\""));
  CHECK(Has(text, "eck_language = \"pt-BR\""));
  CHECK(Has(text, "eck_online_name = \"Quico\""));
  CHECK(Has(text, "eck_frame_rate = 120"));
  CHECK(Has(text, "eck_launcher_width = 1280"));
  CHECK(Has(text, "unrelated_key = \"stays\""));
  CHECK(text.compare(0, before.size(), before) == 0);

  std::printf("closing the launcher without Play keeps the test, and a later Save adds to it\n");
  Reload(f.player);
  CHECK(Cvar("eck_online_connection_test") == eck::OnlineUdpConfiguration());
  CHECK(Cvar("eck_language") == "pt-BR");
  CHECK(Cvar("eck_frame_rate") == "120");
  CHECK(Cvar("eck_online_enabled") == "false");

  CHECK(settings.Save());
  text = Read(f.player);
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "eck_online_enabled = true"));
  CHECK(Has(text, "eck_online_connection_test = \"" + eck::OnlineUdpConfiguration() + "\""));
  CHECK(Has(text, "unrelated_key = \"stays\""));

  Reload(f.player);
  CHECK(Cvar("eck_online_enabled") == "true");
  CHECK(Cvar("eck_online_connection_test") == eck::OnlineUdpConfiguration());
  eck::GameSettings reopened;
  reopened.Capture(f.player, f.shipped);
  CHECK(!eck::ConnectionTestNeeded(reopened, false));

  std::printf("turning Online play off forgets the test in the file too\n");
  reopened.Set(eck::kOnlineEnabledSetting, "false");
  eck::ForgetConnectionTestWhenOff(reopened);
  CHECK(reopened.Save());
  text = Read(f.player);
  CHECK(ParsesAsToml(text));
  CHECK(Has(text, "eck_online_enabled = false"));
  CHECK(Has(text, "eck_online_connection_test = \"\""));
  Reload(f.player);
  CHECK(Cvar("eck_online_connection_test").empty());
  CHECK(Cvar("eck_language") == "pt-BR");
}

void CountedFiles(const fs::path& root, std::vector<std::string>& names) {
  std::error_code ec;
  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    names.push_back(entry.path().lexically_relative(root).generic_string());
  }
}

void TestUserDataLocation() {
  std::printf("settings live in <port folder>/user_data, in a folder with spaces and accents\n");
  Folders f("location", kAccented);
  const fs::path port = f.root / fs::path(u8"Meu Jogo é") / "Port Folder";
  const fs::path system = f.root / "system" / "el_chavo_kart";
  fs::create_directories(port);
  const eck::UserDataSelection selection = eck::SelectUserData({port, system, false});
  CHECK(selection.outcome == eck::UserDataOutcome::kPortable);
  CHECK(selection.root == port / "user_data");

  eck::GameSettings settings;
  settings.Capture(eck::PlayerSettingsPath(selection.root), f.shipped);
  settings.Set("eck_language", "es-MX");
  CHECK(settings.Save());
  CHECK(fs::is_regular_file(port / "user_data" / "settings.toml"));
  CHECK(!fs::exists(system));
  Reload(port / "user_data" / "settings.toml");
  CHECK(Cvar("eck_language") == "es-MX");

#if !defined(_WIN32)
  std::printf("an AppImage keeps settings beside the .AppImage file and writes nothing into its mount\n");
  const fs::path home = f.root / fs::path(u8"João Português") / fs::path(u8"Ação");
  const fs::path appimage = home / "El Chavo Kart.AppImage";
  const fs::path mount = f.root / "tmp" / ".mount_ElChavoXyz";
  fs::create_directories(mount / "usr" / "bin");
  Write(mount / "usr" / "bin" / "el_chavo_kart", "binary");
  Write(appimage, "image");
  std::vector<std::string> mounted_before;
  CountedFiles(mount, mounted_before);

  setenv("APPIMAGE", appimage.c_str(), 1);
  setenv("APPDIR", mount.c_str(), 1);
  CHECK(eck::PortFolder() == home);
  const eck::UserDataSelection appimage_data = eck::SelectUserData({eck::PortFolder(), system, false});
  CHECK(appimage_data.outcome == eck::UserDataOutcome::kPortable);
  CHECK(appimage_data.root == home / "user_data");

  eck::GameSettings packed;
  ResetCvars();
  packed.Capture(eck::PlayerSettingsPath(appimage_data.root), f.shipped);
  packed.Set("eck_language", "pt-BR");
  packed.Set("eck_graphics_adapter", "0");
  CHECK(packed.Save());
  CHECK(packed.SaveNow({{"eck_launcher_width", "1280"}}));
  CHECK(fs::is_regular_file(home / "user_data" / "settings.toml"));
  CHECK(ParsesAsToml(Read(home / "user_data" / "settings.toml")));
  Reload(home / "user_data" / "settings.toml");
  CHECK(Cvar("eck_language") == "pt-BR");
  CHECK(Cvar("eck_launcher_width") == "1280");

  std::vector<std::string> mounted_after;
  CountedFiles(mount, mounted_after);
  CHECK(mounted_after == mounted_before);
  CHECK(!fs::exists(mount / "user_data") && !fs::exists(mount / "settings.toml"));
  CHECK(!fs::exists(mount / "usr" / "bin" / "settings.toml"));
  unsetenv("APPIMAGE");
  unsetenv("APPDIR");
#endif
}

}  // namespace

int main() {
  TestFreshSaveAndReload("plain folder", fs::path("plain"));
  TestFreshSaveAndReload("Unicode and spaces", kAccented);
  TestFreshSaveAndReload("spaces only", fs::path("Meus Jogos") / "Chaves Kart");
  TestUnicodeAndEscapes();
  TestUnrelatedContentSurvives();
  TestInvalidFileIsSetAsideAndRecovered("garbage", "this is not toml at all\n[[[\n");
  TestInvalidFileIsSetAsideAndRecovered("truncated string", "eck_show_fps = false\neck_language = \"pt-");
  TestInvalidFileIsSetAsideAndRecovered("duplicate key", "eck_show_fps = true\neck_show_fps = false\n");
  TestInvalidFileIsSetAsideAndRecovered("binary noise", std::string("\x01\x02\xFF\xFE = \x00 = ", 12));
  TestFileBrokenWhileRunning();
  TestEmptyAndMissingFiles();
  TestConnectionTestSetting();
  TestUserDataLocation();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
