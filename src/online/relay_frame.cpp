#include "online/relay_frame.h"

#include <cstring>

namespace eck::online {

const char* RelayFrameErrorName(RelayFrameError error) {
  switch (error) {
    case RelayFrameError::kNone: return "none";
    case RelayFrameError::kTruncated: return "truncated";
    case RelayFrameError::kBadMagic: return "bad_magic";
    case RelayFrameError::kBadVersion: return "bad_version";
    case RelayFrameError::kBadType: return "bad_type";
    case RelayFrameError::kBadDest: return "bad_dest";
    case RelayFrameError::kOversized: return "oversized";
    case RelayFrameError::kBadLength: return "bad_length";
  }
  return "unknown";
}

namespace {

void PutU32(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = uint8_t(value >> (24 - 8 * i));
}

uint32_t GetU32(const uint8_t* in) {
  return uint32_t(in[0]) << 24 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 8 | uint32_t(in[3]);
}

}  // namespace

std::vector<uint8_t> EncodeRelayFrame(uint32_t token, uint32_t seq, uint8_t dest, const uint8_t* payload, size_t size) {
  if (size == 0 || size > kRelayMaxPayload || (dest >= kMaxSeats && dest != kRelayDestAll)) return {};
  std::vector<uint8_t> frame(kRelayHeaderSize + size);
  frame[0] = kRelayMagic0;
  frame[1] = kRelayMagic1;
  frame[2] = kRelayVersion;
  frame[3] = kRelayTypeData;
  PutU32(&frame[4], token);
  PutU32(&frame[8], seq);
  frame[12] = dest;
  frame[13] = 0;
  frame[14] = uint8_t(size >> 8);
  frame[15] = uint8_t(size);
  std::memcpy(&frame[kRelayHeaderSize], payload, size);
  return frame;
}

std::optional<RelayFrame> DecodeRelayFrame(const uint8_t* data, size_t size, RelayFrameError* error) {
  const auto fail = [error](RelayFrameError why) -> std::optional<RelayFrame> {
    if (error) *error = why;
    return std::nullopt;
  };
  if (size < kRelayHeaderSize) return fail(RelayFrameError::kTruncated);
  if (data[0] != kRelayMagic0 || data[1] != kRelayMagic1) return fail(RelayFrameError::kBadMagic);
  if (data[2] != kRelayVersion) return fail(RelayFrameError::kBadVersion);
  if (data[3] != kRelayTypeData) return fail(RelayFrameError::kBadType);
  if (data[12] >= kMaxSeats && data[12] != kRelayDestAll) return fail(RelayFrameError::kBadDest);
  const size_t length = size_t(data[14]) << 8 | data[15];
  if (length == 0 || length > kRelayMaxPayload) return fail(RelayFrameError::kOversized);
  if (size != kRelayHeaderSize + length) return fail(RelayFrameError::kBadLength);
  RelayFrame frame;
  frame.token = GetU32(data + 4);
  frame.seq = GetU32(data + 8);
  frame.dest = data[12];
  frame.src = data[13];
  frame.payload.assign(data + kRelayHeaderSize, data + size);
  if (error) *error = RelayFrameError::kNone;
  return frame;
}

}  // namespace eck::online
