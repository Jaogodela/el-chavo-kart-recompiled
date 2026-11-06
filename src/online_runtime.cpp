#include "online_runtime.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/identity.h"
#include "online/instance.h"
#include "online/local_addresses.h"
#include "frame_limiter.h"
#include "online_game_hook.h"
#include "online_race_telemetry_guest.h"
#include "online_setting.h"
#include "port_version.h"
#include "settings.h"

namespace eck {
namespace {

// Declared before the service, which taps it, so it is destroyed after.
online::OnlineGameSocket g_game_socket;
double g_next_bind_try = 0;
bool g_bind_failure_logged = false;

std::mutex g_mutex;
std::filesystem::path g_user_dir;
bool g_configured = false;
std::unique_ptr<online::OnlineService> g_service;
std::unique_ptr<online::InstanceLock> g_lock;
bool g_runtime_signed_in = false;

// After Play restarts the process, the launcher that holds the lock may still be exiting.
constexpr int kLockAttempts = 12;
constexpr auto kLockRetry = std::chrono::milliseconds(250);
constexpr double kBindRetrySeconds = 5.0;

online::Identity ResolveIdentity() {
  for (int attempt = 0; attempt < kLockAttempts && !g_lock; ++attempt) {
    if (attempt) std::this_thread::sleep_for(kLockRetry);
    g_lock = online::InstanceLock::TryAcquire(g_user_dir);
  }
  if (!g_lock) {
    REXLOG_INFO("online second instance on this user folder: temporary identity");
    return online::MakeEphemeralIdentity();
  }
  return online::LoadOrCreateIdentity(g_user_dir);
}

const char* PlatformName() {
#if defined(_WIN32)
  return "Windows";
#else
  return "Linux";
#endif
}

std::vector<uint16_t> GamePorts() {
  if (const char* value = std::getenv(kGamePortEnv); value && *value) {
    const long port = std::strtol(value, nullptr, 10);
    if (port >= 1024 && port <= 65535) return {uint16_t(port)};
  }
  return online::GamePortRange();
}

online::ServiceConfig MakeConfig() {
  online::ServiceConfig config;
  if (const char* url = std::getenv(kOnlineServiceEnv); url && *url) config.url = url;
  if (const char* stun = std::getenv(kStunServersEnv); stun && *stun) config.stun = stun;
  if (const char* upnp = std::getenv(kUpnpEnv); upnp && *upnp) config.upnp = std::string_view(upnp) != "0";
  config.version = PortVersion();
  config.platform = PlatformName();
  config.lan = online::LocalCandidates();
  config.game_socket = &g_game_socket;
  config.game_ports = GamePorts();
  config.identity = ResolveIdentity;
  config.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
  return config;
}

online::UserSettings SavedSettings() {
  online::UserSettings settings;
  settings.enabled = REXCVAR_GET(eck_online_enabled);
  settings.name = REXCVAR_GET(eck_online_name);
  return settings;
}

}  // namespace

void ConfigureOnline(const std::filesystem::path& user_dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_user_dir = user_dir;
  g_configured = true;
}

online::OnlineService& Online() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_service) g_service = std::make_unique<online::OnlineService>(MakeConfig());
  return *g_service;
}

bool LeaveOnlineForExit() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_service && g_service->LeaveSession("window_close");
}

void TickOnline(const online::UserSettings& settings) {
  online::OnlineService& service = Online();
  service.Apply(settings);
  service.Update(OnlineNowSeconds());
}

static void UpdateLocalInstancePacing() {
  static const char* const instance = std::getenv("ECK_INSTANCE");
  static bool active = false;
  const bool wanted = LocalInstancePacing(instance, int(Online().GamePeers().size()));
  if (wanted == active) return;
  active = wanted;
  SetLocalInstancePacing(active);
  REXLOG_INFO("{}", LocalInstancePacingLine(active, ConfiguredFrameRate()));
}

void PumpOnlineService() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_configured) return;
  }
  const online::UserSettings settings = SavedSettings();
  if (settings.enabled) EnsureOnlineGameSocket();
  TickOnline(settings);
  UpdateLocalInstancePacing();
  const bool signed_in = Online().SignedIn();
  if (signed_in != g_runtime_signed_in) {
    g_runtime_signed_in = signed_in;
    REXLOG_INFO("online runtime signed in={}", signed_in);
  }
  race_telemetry::PumpRaceTelemetry(Online(), OnlineNowSeconds());
  PumpOnlineGameDebug();
}

double OnlineNowSeconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool EnsureOnlineGameSocket() {
  if (g_game_socket.is_open()) return true;
  const double now = OnlineNowSeconds();
  if (now < g_next_bind_try) return false;
  // A second instance on this PC finds the first port taken and takes the next one; the advertised endpoint says which.
  const std::vector<uint16_t> ports = GamePorts();
  if (g_game_socket.Open(ports)) {
    REXLOG_INFO("udp socket bound port={}", g_game_socket.bound_port());
    return true;
  }
  g_next_bind_try = now + kBindRetrySeconds;
  if (!g_bind_failure_logged) {
    g_bind_failure_logged = true;
    REXLOG_INFO("udp socket bind failed ports={}-{}", ports.front(), ports.back());
  }
  return false;
}

online::OnlineGameSocket& GameSocket() { return g_game_socket; }

}  // namespace eck
