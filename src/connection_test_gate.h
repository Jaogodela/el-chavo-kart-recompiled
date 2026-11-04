#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "online/constants.h"
#include "online_setting.h"

namespace eck {

inline constexpr char kConnectionTestRequired[] = "Run the Connection test on the Online page before playing online.";

// What a finished test counts for: the gameplay ports the online socket tries. Other ports ask for a new test.
inline std::string OnlineUdpConfiguration(const std::string& ports = std::string("udp ") +
                                                                     std::to_string(online::kFirstGamePort) + "-" +
                                                                     std::to_string(online::kLastGamePort)) {
  return ports;
}

// With Online play on, PLAY waits for a Connection test finished on this UDP configuration, whatever its result.
// `Store` is trg::SettingsStore (Get, GetBool, Set).
template <class Store>
bool ConnectionTestNeeded(const Store& settings, bool testing, const std::string& configuration = OnlineUdpConfiguration()) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled)) return false;
  return testing || settings.Get(kConnectionTestSetting) != configuration;
}

using SaveSettingNow = std::function<bool(std::string_view key, const std::string& value)>;

// Written to the player file at once as well, so closing the launcher without Save keeps it. Nothing with Online play
// off: a test then proves nothing for the next time it is turned on.
template <class Store>
bool RecordConnectionTest(Store& settings, const SaveSettingNow& save_now,
                          const std::string& configuration = OnlineUdpConfiguration()) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled)) return false;
  settings.Set(kConnectionTestSetting, configuration);
  return !save_now || save_now(kConnectionTestSetting, configuration);
}

// Online play off forgets the test, pending until Save like the switch itself, so turning it on again asks for one.
template <class Store>
void ForgetConnectionTestWhenOff(Store& settings) {
  if (!settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled) && !settings.Get(kConnectionTestSetting).empty()) {
    settings.Set(kConnectionTestSetting, "");
  }
}

#if defined(_WIN32)
inline constexpr bool kOnWindows = true;
#else
inline constexpr bool kOnWindows = false;
#endif

// Under a running Connection test, whose socket may be the first use of the multiplayer port Windows Firewall asks about.
inline const char* ConnectionTestRunningNote(bool windows) {
  return windows ? "If Windows asks whether to allow network access, choose Allow access so other players can "
                   "connect to you."
                 : nullptr;
}

}  // namespace eck
