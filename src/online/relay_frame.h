#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "online/constants.h"
#include "online/udp_transport.h"

namespace eck::online {

// Where a seat that is reached through the service's relay appears to the game's socket: loopback, so a datagram
// that misses the relay is harmless. The port carries the seat number.
constexpr Endpoint RelayPeerEndpoint(int seat) { return Endpoint{0x7F000010u + uint32_t(seat), 1}; }
constexpr int RelaySeatOf(const Endpoint& endpoint) {
  return endpoint.port == 1 && endpoint.address >= 0x7F000010u && endpoint.address < 0x7F000010u + kMaxSeats
             ? int(endpoint.address - 0x7F000010u)
             : -1;
}

struct RelayFrame {
  uint32_t token = 0;
  uint32_t seq = 0;
  uint8_t dest = 0;  // 0..3: that seat only; kRelayDestAll: every other seat
  uint8_t src = 0;   // stamped by the service
  std::vector<uint8_t> payload;
};

enum class RelayFrameError { kNone, kTruncated, kBadMagic, kBadVersion, kBadType, kBadDest, kOversized, kBadLength };

const char* RelayFrameErrorName(RelayFrameError error);

// Empty for a payload that is empty or over kRelayMaxPayload, or a destination that is neither a seat nor
// kRelayDestAll. The sender's own `src` field is always written as 0.
std::vector<uint8_t> EncodeRelayFrame(uint32_t token, uint32_t seq, uint8_t dest, const uint8_t* payload, size_t size);
std::optional<RelayFrame> DecodeRelayFrame(const uint8_t* data, size_t size, RelayFrameError* error = nullptr);

}  // namespace eck::online
