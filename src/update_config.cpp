#include "update_config.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging.h>

#include "platform.h"
#include "port_version.h"
#include "update_restart_arguments.h"
#include "updater/update_platform.h"

namespace eck {
namespace {

constexpr const char* kReleasesUrl =
    "https://api.github.com/repos/saulob/el-chavo-kart-recompiled/releases/latest";
constexpr const char* kDownloadUrlPrefix = "https://github.com/saulob/el-chavo-kart-recompiled/releases/download/";

std::optional<std::string> Environment(const char* name) {
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);
  if (result.empty()) {
    return std::nullopt;
  }
  return result;
}

bool TestMode() {
  const std::optional<std::string> value = Environment("ECK_UPDATE_TEST");
  return value && *value == "1";
}

std::optional<std::string> TestVariable(const char* name) {
  if (!TestMode()) {
    return std::nullopt;
  }
  return Environment(name);
}

}  // namespace

trg::update::UpdateConfig MakeUpdateConfig(const std::filesystem::path& install_dir) {
  using trg::update::SemanticVersion;
  using trg::update::UpdatePackageKind;

  trg::update::UpdateConfig config;
  config.current_version = PortVersion();
  config.releases_url = kReleasesUrl;
  config.download_url_prefix = kDownloadUrlPrefix;
  config.user_agent = "ElChavoKart-Updater/" + PortVersion();
  config.install_dir = install_dir;
  config.restart_arguments = UpdateRestartArguments(trg::update::CurrentProcessArguments());

  config.package_kind = UpdatePackageKind::kZip;
  config.asset_name = [](const SemanticVersion& version) {
    return "ElChavoKart-v" + version.CoreString() + "-windows-x64.zip";
  };
  config.checksum_asset_name = [](const SemanticVersion& version) {
    return "ElChavoKart-v" + version.CoreString() + "-windows-x64.sha256";
  };
  config.restart_executable = install_dir / "el_chavo_kart.exe";
  config.required_staged_files = {"el_chavo_kart.exe"};
  config.install_supported = std::filesystem::exists(install_dir / "trg-updater.exe");
  if (!config.install_supported) {
    config.install_unsupported_reason =
        "The update helper is missing from this installation. Reinstall this release to enable automatic updates.";
  }

  config.log = [](std::string_view text) { REXLOG_INFO("Updater: {}", text); };

  if (const std::optional<std::string> version = TestVariable("ECK_UPDATE_TEST_CURRENT_VERSION"); version) {
    config.current_version = *version;
    REXLOG_WARN("Updater test mode: reporting version {}", config.current_version);
  }
  if (const std::optional<std::string> json = TestVariable("ECK_UPDATE_TEST_RELEASE_JSON"); json) {
    config.release_json_override = PathFromUtf8(*json);
  }
  if (const std::optional<std::string> hash = TestVariable("ECK_UPDATE_TEST_ASSET_SHA256"); hash) {
    config.sha256_override = *hash;
  }
  return config;
}

}  // namespace eck
