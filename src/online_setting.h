#pragma once

namespace eck {

inline constexpr char kOnlineEnabledSetting[] = "eck_online_enabled";
inline constexpr bool kDefaultOnlineEnabled = false;
inline constexpr char kOnlineEnabledLabel[] = "Online play";
inline constexpr char kOnlineEnabledHelp[] =
    "Lets you race with up to 3 invited friends. Players meet through the online service; "
    "the race itself runs directly between your PCs.";

inline constexpr char kOnlineNameSetting[] = "eck_online_name";
inline constexpr char kDefaultOnlineNameValue[] = "Player";
inline constexpr char kOnlineNameLabel[] = "Online name";
inline constexpr char kOnlineNameHelp[] = "What other players see: up to 16 letters, digits, - or _.";

// The UDP configuration of the last connection test finished with Online play on; empty: none since Online play was
// turned on.
inline constexpr char kConnectionTestSetting[] = "eck_online_connection_test";

// Developer only: another online service than the default one.
inline constexpr char kOnlineServiceEnv[] = "ECK_LOBBY";
// Developer only: STUN servers ("host:port,host:port" or "off") and UPnP ("0" or "1") of the connection test.
inline constexpr char kStunServersEnv[] = "ECK_STUN";
inline constexpr char kUpnpEnv[] = "ECK_UPNP";
// Developer only: the first UDP port of the gameplay socket's range, so several instances on one PC do not collide.
inline constexpr char kGamePortEnv[] = "ECK_ONLINE_PORT";

}  // namespace eck
