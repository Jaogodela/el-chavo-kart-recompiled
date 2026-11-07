// Updater tests: semantic versions, release parsing, SHA-256, ZIP path safety, staging validation, jobs and rollback.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "update_restart_arguments.h"
#include "updater/github_release.h"
#include "updater/install_transaction.h"
#include "updater/sha256.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_job.h"
#include "updater/update_paths.h"
#include "updater/update_zip.h"
#include "updater/url_policy.h"
#include "updater/version.h"

using namespace trg::update;

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

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("eck-update-tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(content.data(), std::streamsize(content.size()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& entries) {
  mz_zip_archive archive{};
  if (mz_zip_writer_init_heap(&archive, 0, 0) == MZ_FALSE) {
    return {};
  }
  for (const auto& [name, content] : entries) {
    if (mz_zip_writer_add_mem(&archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION) ==
        MZ_FALSE) {
      mz_zip_writer_end(&archive);
      return {};
    }
  }
  void* data = nullptr;
  size_t size = 0;
  if (mz_zip_writer_finalize_heap_archive(&archive, &data, &size) == MZ_FALSE) {
    mz_zip_writer_end(&archive);
    return {};
  }
  std::string result(static_cast<const char*>(data), size);
  mz_free(data);
  mz_zip_writer_end(&archive);
  return result;
}

void TestVersions() {
  std::printf("semantic versions\n");
  const auto v040 = ParseVersion("v0.4.0");
  const auto v040_plain = ParseVersion("0.4.0");
  const auto v0410 = ParseVersion("0.4.10");
  const auto v049 = ParseVersion("0.4.9");
  const auto v050 = ParseVersion("0.5.0");
  const auto v100 = ParseVersion("1.0.0");
  CHECK(v040 && v040_plain && v0410 && v049 && v050 && v100);
  CHECK(*v040 == *v040_plain);
  CHECK(*v040 < *v049);
  CHECK(*v049 < *v0410);
  CHECK(*v040 < *v0410);
  CHECK(*v0410 < *v050);
  CHECK(*v050 < *v100);
  CHECK(v040->CoreString() == "0.4.0");
  CHECK(v100->ToString() == "1.0.0");

  const auto rc = ParseVersion("0.3.2-rc1");
  const auto rc2 = ParseVersion("0.3.2-rc.2");
  const auto rc10 = ParseVersion("0.3.2-rc.10");
  const auto stable = ParseVersion("0.3.2");
  CHECK(rc && rc2 && rc10 && stable);
  CHECK(*rc < *stable);
  CHECK(*rc2 < *rc10);
  const auto with_build = ParseVersion("0.3.2+abc");
  CHECK(with_build && with_build->build == "abc");
  CHECK(with_build && SemanticVersion::Compare(*with_build, *stable) == 0);
  CHECK(ParseVersion("0.3.2-rc1+build.5")->prerelease.size() == 1);

  CHECK(!ParseVersion("1.0"));
  CHECK(!ParseVersion("1.0.0.0"));
  CHECK(!ParseVersion("01.0.0"));
  CHECK(!ParseVersion("1.0.0-"));
  CHECK(!ParseVersion("v"));
  CHECK(ParseVersion("V0.4.1").has_value());
}

void TestReleases() {
  std::printf("release JSON\n");
  constexpr const char* kJson = R"({
    "tag_name": "v0.5.0",
    "draft": false,
    "prerelease": false,
    "html_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/tag/v0.5.0",
    "assets": [
      {"name": "ElChavoKart-v0.5.0-windows-x64.zip",
       "browser_download_url": "https://github.com/example/release.zip",
       "size": 123456,
       "digest": "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
      {"name": "ElChavoKart-v0.5.0-windows-x64.sha256",
       "browser_download_url": "https://github.com/example/release.zip.sha256",
       "size": 107,
       "digest": "sha256:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"}
    ]
  })";

  std::string error;
  const std::optional<Release> release = ParseRelease(kJson, &error);
  CHECK(release.has_value());
  CHECK(error.empty());
  if (!release) {
    return;
  }
  CHECK(release->tag == "v0.5.0");
  CHECK(release->version.CoreString() == "0.5.0");
  CHECK(IsInstallableRelease(*release));
  CHECK(release->html_url.rfind("https://", 0) == 0);
  CHECK(release->assets.size() == 2);
  const ReleaseAsset* asset = FindAsset(*release, "ElChavoKart-v0.5.0-windows-x64.zip");
  CHECK(asset != nullptr);
  CHECK(asset && asset->size == 123456);
  CHECK(asset && asset->sha256() == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(FindAsset(*release, "ElChavoKart-v0.5.1-windows-x64.zip") == nullptr);
  CHECK(FindAsset(*release, "ElChavoKart-v0.5.0-x86_64.AppImage") == nullptr);

  const std::optional<Release> draft = ParseRelease(R"({
    "tag_name":"v0.5.1","draft":true,"prerelease":false,"assets":[]
  })");
  CHECK(draft && !IsInstallableRelease(*draft));
  const std::optional<Release> prerelease = ParseRelease(R"({
    "tag_name":"v0.5.1-rc1","draft":false,"prerelease":true,"assets":[]
  })");
  CHECK(prerelease && !IsInstallableRelease(*prerelease));
  const std::optional<Release> prerelease_tag = ParseRelease(R"({
    "tag_name":"v0.5.1-rc1","draft":false,"prerelease":false,"assets":[]
  })");
  CHECK(prerelease_tag && !IsInstallableRelease(*prerelease_tag));

  CHECK(!ParseRelease(R"({"tag_name": 42})").has_value());
  CHECK(!ParseRelease(R"({"tag_name": "not-a-version"})").has_value());
  CHECK(!ParseRelease("[]").has_value());
  CHECK(!ParseRelease("{").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.5.0","assets":{}})").has_value());
  CHECK(!ParseRelease(R"({"tag_name":"v0.5.0","assets":[{"name":"x"}]})").has_value());

  CHECK(ParseSha256Digest("sha256:abc") == std::nullopt);
  CHECK(ParseSha256Digest("md5:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == std::nullopt);
  CHECK(ParseSha256Digest("SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") ==
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(ParseSha256File("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789  "
                        "ElChavoKart-v0.5.0-windows-x64.zip\n") ==
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
  CHECK(ParseSha256File("not-a-hash  ElChavoKart-v0.5.0-windows-x64.zip") == std::nullopt);
}

void TestSha256() {
  std::printf("SHA-256\n");
  CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_text(1000000, 'a');
  CHECK(Sha256Hex(long_text) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  CHECK(Sha256Hex("The quick brown fox jumps over the lazy dog") ==
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
  CHECK(Sha256Hex("tampered") != Sha256Hex("original"));

  TempDir temp;
  const std::filesystem::path file = temp.path / "hash.bin";
  WriteFile(file, "abc");
  std::string error;
  CHECK(Sha256FileHex(file, error) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(error.empty());
}

void TestZipAndStaging() {
  std::printf("ZIP extraction and staging\n");
  TempDir temp;
  std::string error;

  const std::filesystem::path wrapped_archive = temp.path / "wrapped.zip";
  const std::string wrapped = MakeZip({
      {"ElChavoKart-v0.5.0-windows-x64/el_chavo_kart.exe", "launcher"},
      {"ElChavoKart-v0.5.0-windows-x64/assets/launcher/header-art.png", "art"},
      {"ElChavoKart-v0.5.0-windows-x64/licenses/THIRD-PARTY-LICENSES.txt", "license"},
  });
  CHECK(!wrapped.empty());
  WriteFile(wrapped_archive, wrapped);
  const std::filesystem::path wrapped_staging = temp.path / "wrapped-staging";
  CHECK(ExtractZip(wrapped_archive, wrapped_staging, {}, error));
  std::filesystem::path wrapped_root;
  CHECK(ResolveStagingRoot(wrapped_staging, {"el_chavo_kart.exe"}, wrapped_root, error));
  CHECK(wrapped_root == wrapped_staging / "ElChavoKart-v0.5.0-windows-x64");
  CHECK(ReadFile(wrapped_root / "el_chavo_kart.exe") == "launcher");
  CHECK(ValidateStaging(wrapped_root, {"el_chavo_kart.exe", "assets/launcher/header-art.png"}, error));
  CHECK(ListStagingFiles(wrapped_root, error).size() == 3);

  const std::filesystem::path flat_archive = temp.path / "flat.zip";
  WriteFile(flat_archive, MakeZip({{"el_chavo_kart.exe", "launcher"},
                                   {"assets/launcher/header-art.png", "art"},
                                   {"licenses/THIRD-PARTY-LICENSES.txt", "license"}}));
  const std::filesystem::path flat_staging = temp.path / "flat-staging";
  CHECK(ExtractZip(flat_archive, flat_staging, {}, error));
  std::filesystem::path flat_root;
  CHECK(ResolveStagingRoot(flat_staging, {"el_chavo_kart.exe"}, flat_root, error));
  CHECK(flat_root == flat_staging);

  WriteFile(flat_root / "game" / "default.xex", "game");
  CHECK(!ValidateStaging(flat_root, {"el_chavo_kart.exe"}, error));
  std::filesystem::remove_all(flat_root / "game");

  WriteFile(flat_root / "user_data" / "settings.toml", "settings");
  CHECK(!ValidateStaging(flat_root, {"el_chavo_kart.exe"}, error));
  std::filesystem::remove_all(flat_root / "user_data");

  WriteFile(flat_root / "logs" / "a.log", "log");
  CHECK(!ValidateStaging(flat_root, {"el_chavo_kart.exe"}, error));
  std::filesystem::remove_all(flat_root / "logs");

  CHECK(!ValidateStaging(flat_root, {"missing.exe"}, error));

  const std::vector<std::string> unsafe_names = {
      "../evil",        "C:/evil",          "C:\\evil",     "\\\\server\\share", "a/../../b",
      "a\\..\\b",       "a//b",             "game/default.xex", "game\\default.xex", "user_data/settings.toml",
      "logs/a.log",     "cache/a.bin",      "dlc/a.bin",     "settings.toml",     "a/b.",
      "con",            "C:evil",           "475807D7/a",    "default.xex",
  };
  int rejected = 0;
  for (const std::string& name : unsafe_names) {
    const std::filesystem::path unsafe_archive = temp.path / "unsafe.zip";
    const std::string bytes = MakeZip({{name, "payload"}});
    CHECK(!bytes.empty());
    WriteFile(unsafe_archive, bytes);
    const std::filesystem::path unsafe_staging = temp.path / "unsafe-staging";
    std::filesystem::remove_all(unsafe_staging);
    if (!ExtractZip(unsafe_archive, unsafe_staging, {}, error)) {
      ++rejected;
    }
    CHECK(!std::filesystem::exists(temp.path / "evil"));
  }
  CHECK(rejected == int(unsafe_names.size()));
  CHECK(IsSymlinkZipEntry(0120000u << 16));
  CHECK(!IsSymlinkZipEntry(0100644u << 16));
  CHECK(!IsSymlinkZipEntry(0));

  const std::filesystem::path two_folders = temp.path / "two-folders.zip";
  WriteFile(two_folders, MakeZip({{"one/el_chavo_kart.exe", "one"}, {"two/el_chavo_kart.exe", "two"}}));
  const std::filesystem::path two_staging = temp.path / "two-staging";
  CHECK(ExtractZip(two_folders, two_staging, {}, error));
  std::filesystem::path two_root;
  CHECK(!ResolveStagingRoot(two_staging, {"el_chavo_kart.exe"}, two_root, error));

  const std::filesystem::path dot_archive = temp.path / "dot.zip";
  WriteFile(dot_archive, MakeZip({{"./el_chavo_kart.exe", "launcher"}}));
  const std::filesystem::path dot_staging = temp.path / "dot-staging";
  CHECK(ExtractZip(dot_archive, dot_staging, {}, error));
  CHECK(std::filesystem::exists(dot_staging / "el_chavo_kart.exe"));
}

std::filesystem::path MakeJob(UpdateJob& job, const std::filesystem::path& temp, const std::string& manifest_text) {
  const std::filesystem::path install = temp / "install dir";
  const std::filesystem::path staging = temp / "staging dir";
  const std::filesystem::path backup = temp / "backup dir";
  const std::filesystem::path manifest = temp / "manifest.txt";
  std::filesystem::create_directories(install);
  std::filesystem::create_directories(staging);
  WriteFile(manifest, manifest_text);
  job = {};
  job.parent_pid = 1234;
  job.install_dir = install;
  job.staging_dir = staging;
  job.backup_dir = backup;
  job.manifest_file = manifest;
  job.restart_executable = install / "el_chavo_kart.exe";
  job.log_file = temp / "trg-updater.log";
  return install;
}

void TestJobs() {
  std::printf("update jobs\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "el_chavo_kart.exe\n");
  WriteFile(install / "el_chavo_kart.exe", "old launcher");
  WriteFile(job.staging_dir / "el_chavo_kart.exe", "launcher");
  job.restart_arguments = {"--user_data_root=with space", "--eck_launcher=true", ""};

  std::string error;
  const std::filesystem::path job_file = temp.path / "job.txt";
  CHECK(WriteUpdateJob(job_file, job, error));
  const std::optional<UpdateJob> read = ReadUpdateJob(job_file, error);
  CHECK(read.has_value());
  if (read) {
    CHECK(read->parent_pid == 1234);
    CHECK(read->install_dir == install);
    CHECK(read->restart_arguments == job.restart_arguments);
  }

  CHECK(ValidateInstallJob(job, error));

  UpdateJob inside = job;
  inside.staging_dir = install / "staging";
  std::filesystem::create_directories(inside.staging_dir);
  CHECK(!ValidateInstallJob(inside, error));

  UpdateJob relative = job;
  relative.manifest_file = "manifest.txt";
  CHECK(!ValidateInstallJob(relative, error));

  UpdateJob outside = job;
  outside.restart_executable = temp.path / "elsewhere.exe";
  CHECK(!ValidateInstallJob(outside, error));

  UpdateJob no_manifest = job;
  no_manifest.manifest_file = temp.path / "missing.txt";
  CHECK(!ValidateInstallJob(no_manifest, error));
}

void TestManifest() {
  std::printf("manifest validation\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "../evil\n");
  WriteFile(install / "el_chavo_kart.exe", "old launcher");
  WriteFile(job.staging_dir / "el_chavo_kart.exe", "new launcher");
  std::string error;
  InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "old launcher");

  WriteFile(job.manifest_file, "game/default.xex\n");
  WriteFile(job.staging_dir / "game" / "default.xex", "replacement");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "old launcher");

  WriteFile(job.manifest_file, "missing.dll\n");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kInvalidJob);

  WriteFile(job.manifest_file, "el_chavo_kart.exe\nrexruntime.dll\nel_chavo_kart.exe\n");
  WriteFile(job.staging_dir / "rexruntime.dll", "new runtime");
  outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "new launcher");
  CHECK(ReadFile(install / "rexruntime.dll") == "new runtime");
}

void TestSameVersionReplacement() {
  std::printf("same-version replacement\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "el_chavo_kart.exe\nrelease-version.txt\n");
  job.restart_executable = install / "el_chavo_kart.exe";
  WriteFile(install / "el_chavo_kart.exe", "DEVELOPMENT_BUILD");
  WriteFile(install / "release-version.txt", "0.4.0");
  WriteFile(install / "user_data" / "settings.toml", "player settings");
  WriteFile(install / "notes.txt", "keep me");
  WriteFile(job.staging_dir / "el_chavo_kart.exe", "PUBLIC_RELEASE");
  WriteFile(job.staging_dir / "release-version.txt", "0.4.0");

  const InstallOutcome outcome = InstallFiles(job, {}, false);
  CHECK(outcome.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "PUBLIC_RELEASE");
  CHECK(ReadFile(install / "release-version.txt") == "0.4.0");
  CHECK(ReadFile(install / "user_data" / "settings.toml") == "player settings");
  CHECK(ReadFile(install / "notes.txt") == "keep me");
}

void TestRollback() {
  std::printf("install and rollback\n");
  TempDir temp;
  UpdateJob job;
  const std::filesystem::path install = MakeJob(job, temp.path, "");
  const std::filesystem::path staging = job.staging_dir;
  const std::filesystem::path manifest = job.manifest_file;
  WriteFile(install / "el_chavo_kart.exe", "old launcher");
  WriteFile(install / "rexruntime.dll", "old runtime");
  WriteFile(install / "user_data" / "settings.toml", "player settings");
  WriteFile(install / "user_data" / "475807D7" / "profile.bin", "profile data");
  WriteFile(install / "game" / "default.xex", "game files");
  WriteFile(install / "game" / "extracted" / "gamedata.bin", "game assets");
  WriteFile(install / "logs" / "session.log", "log");
  WriteFile(install / "cache" / "475807D7" / "pipeline.bin", "shader cache");
  WriteFile(install / "custom-notes.txt", "keep me");

  auto stage = [&] {
    WriteFile(staging / "el_chavo_kart.exe", "new launcher");
    WriteFile(staging / "trg-updater.exe", "new helper");
    WriteFile(staging / "rexruntime.dll", "new runtime");
    WriteFile(staging / "assets" / "new" / "data.png", "new nested file");
    WriteFile(manifest, "el_chavo_kart.exe\ntrg-updater.exe\nrexruntime.dll\nassets/new/data.png\n");
  };
  stage();

  int replacements = 0;
  InstallOptions fail;
  fail.replace = [&replacements](const std::filesystem::path& from, const std::filesystem::path& to) {
    ++replacements;
    if (replacements == 3) {
      return false;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
  };
  InstallOutcome failed = InstallFiles(job, fail, false);
  CHECK(failed.status == InstallStatus::kInstallFailedRolledBack);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "old launcher");
  CHECK(!std::filesystem::exists(install / "trg-updater.exe"));
  CHECK(ReadFile(install / "rexruntime.dll") == "old runtime");
  CHECK(ReadFile(install / "user_data" / "settings.toml") == "player settings");
  CHECK(ReadFile(install / "user_data" / "475807D7" / "profile.bin") == "profile data");
  CHECK(ReadFile(install / "game" / "default.xex") == "game files");
  CHECK(ReadFile(install / "game" / "extracted" / "gamedata.bin") == "game assets");
  CHECK(ReadFile(install / "logs" / "session.log") == "log");
  CHECK(ReadFile(install / "cache" / "475807D7" / "pipeline.bin") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");

  stage();
  replacements = 0;
  const InstallOutcome success = InstallFiles(job, {}, false);
  CHECK(success.status == InstallStatus::kSuccess);
  CHECK(ReadFile(install / "el_chavo_kart.exe") == "new launcher");
  CHECK(ReadFile(install / "trg-updater.exe") == "new helper");
  CHECK(ReadFile(install / "rexruntime.dll") == "new runtime");
  CHECK(ReadFile(install / "assets" / "new" / "data.png") == "new nested file");
  CHECK(ReadFile(install / "user_data" / "settings.toml") == "player settings");
  CHECK(ReadFile(install / "user_data" / "475807D7" / "profile.bin") == "profile data");
  CHECK(ReadFile(install / "game" / "default.xex") == "game files");
  CHECK(ReadFile(install / "game" / "extracted" / "gamedata.bin") == "game assets");
  CHECK(ReadFile(install / "logs" / "session.log") == "log");
  CHECK(ReadFile(install / "cache" / "475807D7" / "pipeline.bin") == "shader cache");
  CHECK(ReadFile(install / "custom-notes.txt") == "keep me");
}

void TestPackagingNames() {
  std::printf("packaging names\n");
  const std::filesystem::path root = std::filesystem::path(ECK_SOURCE_DIR);
  const std::string config = ReadFile(root / "src" / "update_config.cpp");
  const std::string cmake = ReadFile(root / "CMakeLists.txt");
  const std::string windows_package = ReadFile(root / "tools" / "package-release.ps1");
  const std::string launcher = ReadFile(root / "src" / "launcher.cpp");

  CHECK(config.find("https://api.github.com/repos/saulob/el-chavo-kart-recompiled/releases/latest") !=
        std::string::npos);
  CHECK(config.find("ElChavoKart-v\" + version.CoreString() + \"-windows-x64.zip") != std::string::npos);
  CHECK(config.find("ElChavoKart-v\" + version.CoreString() + \"-windows-x64.sha256") != std::string::npos);
  CHECK(config.find("config.current_version = PortVersion();") != std::string::npos);
  CHECK(config.find("UpdateRestartArguments(trg::update::CurrentProcessArguments())") != std::string::npos);
  CHECK(config.find("Environment(\"ECK_UPDATE_TEST\")") != std::string::npos);
  CHECK(config.find("Environment(\"ECK_UPDATE_TEST_") == std::string::npos);
  CHECK(config.find("if (!TestMode()) {") != std::string::npos);
  CHECK(config.find("config.download_url_prefix = kDownloadUrlPrefix;") != std::string::npos);
  CHECK(config.find("\"https://github.com/saulob/el-chavo-kart-recompiled/releases/download/\"") !=
        std::string::npos);
  CHECK(config.find("*value == \"1\"") != std::string::npos);

  CHECK(cmake.find("project(el_chavo_kart VERSION") != std::string::npos);
  CHECK(cmake.find("trg-updater") != std::string::npos);
  CHECK(cmake.find("update_config.cpp") != std::string::npos);
  CHECK(windows_package.find("ElChavoKart-v$Version-windows-x64") != std::string::npos);
  CHECK(windows_package.find("trg-updater.exe") != std::string::npos);
  CHECK(windows_package.find("project\\(\\s*el_chavo_kart\\s+VERSION") != std::string::npos);

  CHECK(launcher.find("\"v\" + PortVersion()") != std::string::npos);
  CHECK(launcher.find("ui.Info(\"Port version\", PortVersion().c_str())") != std::string::npos);
  CHECK(launcher.find("\"0.1.0\"") == std::string::npos);
  CHECK(config.find("\"0.1.0\"") == std::string::npos);

  const std::string release_check = ReadFile(root / "tools" / "check-release-folder.ps1");
  CHECK(launcher.find("rex::filesystem::GetExecutableFolder() / \"assets\" / \"launcher\" / file") !=
        std::string::npos);
  CHECK(launcher.find("LoadLauncherImage(paths_.immediate_drawer, \"header-art.png\")") != std::string::npos);
  CHECK(launcher.find("LoadLauncherImage(paths_.immediate_drawer, \"sponsor-heart.png\", true)") !=
        std::string::npos);
  for (const char* asset : {"header-art.png", "sponsor-heart.png"}) {
    CHECK(cmake.find(std::string("$<TARGET_FILE_DIR:el_chavo_kart>/assets/launcher/") + asset) != std::string::npos);
    CHECK(windows_package.find(std::string("assets\\launcher\\") + asset) != std::string::npos);
    CHECK(release_check.find(std::string("assets\\launcher\\") + asset) != std::string::npos);
    CHECK(!ReadFile(root / "assets" / "launcher" / asset).empty());
  }

  for (const char* leaf : {"install_transaction.cpp", "update_job.cpp", "helper_main.cpp"}) {
    const std::string source = ReadFile(root / "src" / "updater" / leaf);
    CHECK(!source.empty());
    CHECK(source.find("SemanticVersion") == std::string::npos);
    CHECK(source.find("ParseVersion") == std::string::npos);
  }
}

void TestRestartArguments() {
  std::printf("restart arguments\n");
  const std::vector<std::string> cleaned = eck::UpdateRestartArguments(
      {"--user_data_root=C:/with space", "--eck_skip_launcher=true", "--eck_launcher=false", "--eck_launcher",
       "--eck_launcher_x=5", "--game_data_root=D:/game"});
  const std::vector<std::string> expected = {"--user_data_root=C:/with space", "--eck_launcher_x=5",
                                             "--game_data_root=D:/game", "--eck_launcher=true"};
  CHECK(cleaned == expected);
  CHECK(eck::UpdateRestartArguments({}) == std::vector<std::string>{"--eck_launcher=true"});
  CHECK(!eck::IsLauncherVisibilityArgument("--eck_launcher_width=1"));
  CHECK(eck::IsLauncherVisibilityArgument("--eck_skip_launcher"));
}

void TestUrlPolicy() {
  std::printf("url policy\n");
  const std::vector<std::string> hosts = GitHubReleaseHosts();
  const std::string prefix = "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/";
  CHECK(UrlHostAllowed("https://api.github.com/repos/saulob/el-chavo-kart-recompiled/releases/latest", hosts));
  CHECK(UrlHostAllowed("https://release-assets.githubusercontent.com/github-production-release-asset/1?sig=a", hosts));
  CHECK(UrlHostAllowed("https://objects.githubusercontent.com/x", hosts));
  CHECK(UrlHostAllowed("HTTPS://GitHub.com:443/saulob", hosts));
  CHECK(!UrlHostAllowed("http://github.com/saulob", hosts));
  CHECK(!UrlHostAllowed("https://example.com/github.com", hosts));
  CHECK(!UrlHostAllowed("https://github.com.example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com@example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com:8443/x", hosts));
  CHECK(!UrlHostAllowed("https://github.com\\@example.com/x", hosts));
  CHECK(!UrlHostAllowed("https://", hosts));
  CHECK(!UrlHostAllowed("https://github.com/x", {}));

  CHECK(UrlAllowed(prefix + "v0.5.0/ElChavoKart-v0.5.0-windows-x64.zip", hosts, prefix));
  CHECK(!UrlAllowed("https://github.com/someone/else/releases/download/v1/ElChavoKart-v1-windows-x64.zip", hosts,
                    prefix));
  CHECK(!UrlAllowed("https://objects.githubusercontent.com/ElChavoKart-v0.5.0-windows-x64.zip", hosts, prefix));

  // Every redirect hop goes back through the policy instead of being followed by WinHTTP or curl.
  const std::filesystem::path root = std::filesystem::path(ECK_SOURCE_DIR);
  const std::string patch = ReadFile(root / "patches" / "trg-launcher" / "0006-download-allowed-urls.patch");
  CHECK(patch.find("+  std::function<bool(const std::string& url)> allow_url;") != std::string::npos);
  CHECK(patch.find("WINHTTP_OPTION_REDIRECT_POLICY_NEVER") != std::string::npos);
  CHECK(patch.find("--max-redirs 0") != std::string::npos);
  CHECK(patch.find("if (!options.allow_url(current)) {") != std::string::npos);
  const std::string controller =
      ReadFile(root / "src" / "updater" / "update_controller.cpp");
  CHECK(controller.find("options.allow_url = ") != std::string::npos);
  CHECK(controller.find("IsHttpsUrl") == std::string::npos);
}

void TestPaths() {
  std::printf("path safety\n");
  CHECK(IsSafeRelativePath("el_chavo_kart.exe"));
  CHECK(IsSafeRelativePath("trg-updater.exe"));
  CHECK(IsSafeRelativePath("assets/launcher/header-art.png"));
  CHECK(IsSafeRelativePath("./el_chavo_kart.exe"));
  CHECK(IsSafeRelativePath("ElChavoKart-v0.5.0-windows-x64/el_chavo_kart.exe"));
  CHECK(!IsSafeRelativePath("../el_chavo_kart.exe"));
  CHECK(!IsSafeRelativePath("/absolute"));
  CHECK(!IsSafeRelativePath("C:/el_chavo_kart.exe"));
  CHECK(!IsSafeRelativePath("\\\\server\\share"));
  CHECK(!IsSafeRelativePath("a//b"));
  CHECK(!IsSafeRelativePath("a/./../b"));
  CHECK(IsProtectedRelativePath("game/default.xex"));
  CHECK(IsProtectedRelativePath("game/extracted/gamedata.bin"));
  CHECK(IsProtectedRelativePath("game/extracted/default.xex"));
  CHECK(IsProtectedRelativePath("game/default.xexp"));
  CHECK(IsProtectedRelativePath("default.xexp"));
  CHECK(IsProtectedRelativePath("user_data/475807D7/savegames.dat"));
  CHECK(IsProtectedRelativePath("game-install-temp/default.xex"));
  CHECK(IsProtectedRelativePath("game-install-backup/default.xex"));
  CHECK(IsProtectedRelativePath("user_data/settings.toml"));
  CHECK(IsProtectedRelativePath("user_data/475807D7/profile.bin"));
  CHECK(IsProtectedRelativePath("logs/a.log"));
  CHECK(IsProtectedRelativePath("cache/475807D7/a.bin"));
  CHECK(IsProtectedRelativePath("content/foo"));
  CHECK(IsProtectedRelativePath("screenshots/a.png"));
  CHECK(IsProtectedRelativePath("settings.toml"));
  CHECK(IsProtectedRelativePath("settings.toml.bak"));
  CHECK(IsProtectedRelativePath("settings.toml.invalid"));
  CHECK(IsProtectedRelativePath("475807D7/profile.bin"));
  CHECK(IsProtectedRelativePath("default.xex"));
  CHECK(IsProtectedRelativePath("game-folder.txt"));
  CHECK(IsProtectedRelativePath("disc.iso"));
  CHECK(IsProtectedRelativePath("Startup.xxx"));
  CHECK(IsProtectedRelativePath("Engine.upk"));
  CHECK(IsProtectedRelativePath("Textures.tfc"));
  CHECK(IsProtectedRelativePath("Sounds.fsbcache"));
  CHECK(IsProtectedRelativePath("Intro.bik"));
  CHECK(IsProtectedRelativePath("user_data.migrating/a"));
  CHECK(IsProtectedRelativePath("save/a.sav"));
  CHECK(!IsProtectedRelativePath("el_chavo_kart.exe"));
  CHECK(!IsProtectedRelativePath("trg-updater.exe"));
  CHECK(!IsProtectedRelativePath("rexruntime.dll"));
  CHECK(!IsProtectedRelativePath("rexgpu-xenos.dll"));
  CHECK(!IsProtectedRelativePath("el_chavo_kart.toml"));
  CHECK(!IsProtectedRelativePath("assets/launcher/header-art.png"));
  CHECK(!IsProtectedRelativePath("licenses/THIRD-PARTY-LICENSES.txt"));
  CHECK(!IsProtectedRelativePath("README.txt"));
  CHECK(!IsProtectedRelativePath("LICENSE"));
}

void TestAppImageTarget() {
  std::printf("the updater replaces only the AppImage the running program was started from\n");
  TempDir dir;
  const std::filesystem::path root = dir.path;
  const std::filesystem::path appimage = root / "ElChavoKart-v0.7.0-x86_64.AppImage";
  WriteFile(appimage, "appimage");
  const std::filesystem::path mount = root / "mount";
  const std::filesystem::path program = mount / "usr" / "bin" / "el_chavo_kart";
  std::filesystem::create_directories(program.parent_path());
  WriteFile(program, "program");
  CHECK(IsRunningAppImage(appimage, mount, program));

  const std::filesystem::path extracted = root / ".extract" / "squashfs-root";
  const std::filesystem::path extracted_program = extracted / "usr" / "bin" / "el_chavo_kart";
  std::filesystem::create_directories(extracted_program.parent_path());
  WriteFile(extracted_program, "program");
  CHECK(IsRunningAppImage(appimage, extracted, extracted_program));

  const std::filesystem::path build = root / "build" / "el_chavo_kart";
  std::filesystem::create_directories(build.parent_path());
  WriteFile(build, "program");
  CHECK(!IsRunningAppImage(appimage, mount, build));

  const std::filesystem::path other = root / "Other-v1.0.0-x86_64.AppImage";
  WriteFile(other, "other");
  CHECK(!IsRunningAppImage(other, mount, program));
  const std::filesystem::path empty_version = root / "ElChavoKart-v-x86_64.AppImage";
  WriteFile(empty_version, "other");
  CHECK(!IsRunningAppImage(empty_version, mount, program));
  const std::filesystem::path odd_version = root / "ElChavoKart-vx-x86_64.AppImage";
  WriteFile(odd_version, "other");
  CHECK(!IsRunningAppImage(odd_version, mount, program));

  const std::filesystem::path inside = mount / "ElChavoKart-v0.7.0-x86_64.AppImage";
  WriteFile(inside, "appimage");
  CHECK(!IsRunningAppImage(inside, mount, program));

  CHECK(!IsRunningAppImage({}, mount, program));
  CHECK(!IsRunningAppImage(appimage, {}, program));
  CHECK(!IsRunningAppImage(appimage, mount, {}));
  CHECK(!IsRunningAppImage(appimage, root / "not-a-mount", program));
}

void TestPlatformAssets() {
  std::printf("each platform downloads its own release asset, and only that\n");
  const std::string json = R"({
    "tag_name": "v0.5.0", "draft": false, "prerelease": false, "html_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/tag/v0.5.0",
    "body": "notes",
    "assets": [
      {"name": "ElChavoKart-v0.5.0-windows-x64.zip", "size": 10,
       "browser_download_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/v0.5.0/ElChavoKart-v0.5.0-windows-x64.zip"},
      {"name": "ElChavoKart-v0.5.0-windows-x64.sha256", "size": 20,
       "browser_download_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/v0.5.0/ElChavoKart-v0.5.0-windows-x64.sha256"},
      {"name": "ElChavoKart-v0.5.0-x86_64.AppImage", "size": 30,
       "browser_download_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/v0.5.0/ElChavoKart-v0.5.0-x86_64.AppImage"},
      {"name": "ElChavoKart-v0.5.0-x86_64.sha256", "size": 40,
       "browser_download_url": "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/v0.5.0/ElChavoKart-v0.5.0-x86_64.sha256"}
    ]})";
  const std::optional<Release> release = ParseRelease(json);
  CHECK(release.has_value());
  if (release) {
    const ReleaseAsset* zip = FindAsset(*release, "ElChavoKart-v0.5.0-windows-x64.zip");
    const ReleaseAsset* appimage = FindAsset(*release, "ElChavoKart-v0.5.0-x86_64.AppImage");
    CHECK(zip != nullptr && zip->name.ends_with(".zip") && zip->size == 10);
    CHECK(appimage != nullptr && appimage->name.ends_with(".AppImage") && appimage->size == 30);
    CHECK(FindAsset(*release, "ElChavoKart-v0.5.0-x86_64.sha256") != nullptr);
  }

  std::ifstream file(std::filesystem::path(ECK_SOURCE_DIR) / "src" / "update_config.cpp", std::ios::binary);
  std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::erase(source, '\r');
  const size_t begin = source.find("#if defined(_WIN32)\n  config.package_kind = UpdatePackageKind::kZip;");
  const size_t middle = source.find("#else\n  config.package_kind = UpdatePackageKind::kAppImage;", begin);
  const size_t end = source.find("#endif", middle);
  CHECK(begin != std::string::npos && middle != std::string::npos && end != std::string::npos);
  const std::string windows = source.substr(begin, middle - begin);
  const std::string linux_side = source.substr(middle, end - middle);
  CHECK(windows.find("-windows-x64.zip\"") != std::string::npos);
  CHECK(windows.find("-windows-x64.sha256\"") != std::string::npos);
  CHECK(windows.find("AppImage\"") == std::string::npos);
  CHECK(linux_side.find("-x86_64.AppImage\"") != std::string::npos);
  CHECK(linux_side.find("-x86_64.sha256\"") != std::string::npos);
  CHECK(linux_side.find(".zip") == std::string::npos);
  CHECK(linux_side.find("trg::update::HasOriginalAppImage(appimage)") != std::string::npos);
  CHECK(linux_side.find("config.restart_executable = appimage;") != std::string::npos);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestVersions();
  TestReleases();
  TestSha256();
  TestZipAndStaging();
  TestJobs();
  TestManifest();
  TestSameVersionReplacement();
  TestRollback();
  TestPackagingNames();
  TestRestartArguments();
  TestUrlPolicy();
  TestPaths();
  TestAppImageTarget();
  TestPlatformAssets();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
