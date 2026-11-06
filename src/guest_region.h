#pragma once

#include <cstdint>

namespace eck {

// HeapAllocationInfo reports base_address as the queried address but counts region_size from the start of that address's
// page, so their sum overshoots the region by the address's offset into the page.
constexpr uint64_t GuestRegionEnd(uint32_t address, uint32_t page_size, uint32_t region_size) {
  return uint64_t(address & ~(page_size - 1)) + region_size;
}

constexpr bool GuestRangeInRegion(uint32_t address, uint32_t length, uint32_t page_size, uint32_t region_size) {
  return uint64_t(address) + length <= GuestRegionEnd(address, page_size, region_size);
}

}  // namespace eck
