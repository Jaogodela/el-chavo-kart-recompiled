#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace eck {

template <typename T>
inline T LoadBE(const uint8_t* base, uint32_t addr) {
  T v;
  std::memcpy(&v, base + addr, sizeof(v));
  return std::byteswap(v);
}

template <typename T>
inline void StoreBE(uint8_t* base, uint32_t addr, T v) {
  v = std::byteswap(v);
  std::memcpy(base + addr, &v, sizeof(v));
}

inline uint32_t LoadBE32(const uint8_t* base, uint32_t addr) { return LoadBE<uint32_t>(base, addr); }
inline uint64_t LoadBE64(const uint8_t* base, uint32_t addr) { return LoadBE<uint64_t>(base, addr); }
inline void StoreBE32(uint8_t* base, uint32_t addr, uint32_t v) { StoreBE<uint32_t>(base, addr, v); }
inline void StoreBE64(uint8_t* base, uint32_t addr, uint64_t v) { StoreBE<uint64_t>(base, addr, v); }

// Doubles and floats in guest memory are big-endian.
inline double LoadBEDouble(const uint8_t* base, uint32_t addr) {
  return std::bit_cast<double>(LoadBE64(base, addr));
}
inline float LoadBEFloat(const uint8_t* base, uint32_t addr) {
  return std::bit_cast<float>(LoadBE32(base, addr));
}

}  // namespace eck
