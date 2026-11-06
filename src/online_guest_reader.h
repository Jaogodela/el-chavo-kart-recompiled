#pragma once

// Bounds-checked reads of guest memory from outside the guest threads.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "guest_region.h"

namespace eck::online_guest {

inline constexpr uint32_t kGuestMemorySpan = 0x90000000;

inline bool Readable(uint32_t address, uint32_t length) {
  if (address < 0x10000 || address + length < address || address + length > kGuestMemorySpan) return false;
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (heap == nullptr || !heap->QueryRegionInfo(address, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 && (info.protect & rex::memory::kMemoryProtectRead) != 0 &&
         GuestRangeInRegion(address, length, heap->page_size(), info.region_size);
}

struct Guest {
  bool Ok(uint32_t address, uint32_t length) const { return Readable(address, length); }
  std::optional<uint32_t> U32(uint32_t address) const {
    if (!Readable(address, 4)) return std::nullopt;
    return LoadBE32(rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(address), 0);
  }
  uint32_t P(uint32_t address) const { return U32(address).value_or(0); }  // pointer, 0 when unreadable
  int32_t I32(uint32_t address) const { return int32_t(U32(address).value_or(0)); }
  float F32(uint32_t address) const {
    const uint32_t bits = U32(address).value_or(0);
    float f;
    static_assert(sizeof f == sizeof bits);
    std::memcpy(&f, &bits, sizeof f);
    return f;
  }
  uint8_t U8(uint32_t address) const {
    return Readable(address, 1) ? rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(address)[0] : 0;
  }
};

// The text of an open or start console command, empty for anything else. FString: data, num (with the NUL), max, UTF-16 BE.
inline std::string ConsoleCommandText(const uint8_t* base, uint32_t fstring) {
  if (base == nullptr || fstring < 0x10000 || fstring + 12 > kGuestMemorySpan) return {};
  const uint32_t data = LoadBE32(base, fstring);
  const uint32_t num = LoadBE32(base, fstring + 4);
  if (data < 0x10000 || num < 12 || num > 4096 || uint64_t(data) + 2ull * num > kGuestMemorySpan) return {};
  const uint16_t first = LoadBE<uint16_t>(base, data);
  if (first != 'o' && first != 'O' && first != 's' && first != 'S') return {};  // every other console command ends here
  std::string text;
  text.reserve(num);
  for (uint32_t i = 0; i < num; ++i) {
    const uint16_t c = LoadBE<uint16_t>(base, data + 2 * i);
    if (c == 0) break;
    text.push_back(c < 0x80 ? char(c) : '?');
  }
  return text;
}

}  // namespace eck::online_guest
