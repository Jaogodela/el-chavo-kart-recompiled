#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eck::online {

inline constexpr int kProtocolVersion = 1;
inline constexpr char kGameId[] = "el-chavo-kart-pc";
inline constexpr char kImageId[] = "eck-usa-1db4e96b";
// Both sides of a room must report the same image and build. Bumped whenever the gameplay datagrams change shape, so a
// build that cannot talk to this one is refused with "Version mismatch" instead of freezing in silence.
inline constexpr int kBuildNumber = 2;
inline constexpr char kServiceEnvVar[] = "ECK_LOBBY";
inline constexpr char kDefaultServiceUrl[] = "wss://eck-lobby.saulobenigno.workers.dev";
inline constexpr char kTokenDomain[] = "eck-token:";
inline constexpr char kPlayerIdPrefix[] = "eck-player-id:";

inline constexpr int kMinSeats = 2;
inline constexpr int kMaxSeats = 4;
inline constexpr int kMaxRemotePeers = kMaxSeats - 1;

// Relay frame header ('E','K', version, type, token, seq, dest, src, length).
inline constexpr uint8_t kRelayMagic0 = 'E';
inline constexpr uint8_t kRelayMagic1 = 'K';
inline constexpr uint8_t kRelayVersion = 1;
inline constexpr uint8_t kRelayTypeData = 1;
inline constexpr size_t kRelayHeaderSize = 16;
inline constexpr size_t kRelayMaxPayload = 1472;
inline constexpr uint8_t kRelayDestAll = 0xFF;

// Gameplay UDP ports, tried in order. Disjoint from the other recompiled titles' 7001/7002.
inline constexpr uint16_t kFirstGamePort = 7101;
inline constexpr uint16_t kLastGamePort = 7110;

inline std::vector<uint16_t> GamePortRange() {
  std::vector<uint16_t> ports;
  for (uint32_t port = kFirstGamePort; port <= kLastGamePort; ++port) ports.push_back(uint16_t(port));
  return ports;
}

}  // namespace eck::online
