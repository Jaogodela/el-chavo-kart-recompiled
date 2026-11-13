#pragma once

// The authoritative race progression of one kart, written into guest memory from outside the guest thread. Pure: no
// ReXGlue, so a test can point it at a buffer and read the fields back.
//
// Widths: the layout stores CompletedLaps, RacePosition and RaceFinishPosition as the 32-bit ints UnrealScript gives an
// `int`, and this header refuses a kart whose own values do not look like that. A layout that turns out to be a narrower
// field therefore writes nothing, instead of overwriting the fields that follow it.
//
// Written only when the host says so: a guest never decides its own laps or place, and the kart is a network kart there.

#include <bit>
#include <cmath>
#include <cstdint>

#include "guest_memory.h"
#include "online_probe_layout.h"
#include "online_race.h"

namespace eck::race_sync {

// std::byteswap takes integers only, so a float crosses as its bits.
inline void StoreFloatBE(uint8_t* base, uint32_t address, float value) {
  StoreBE32(base, address, std::bit_cast<uint32_t>(value));
}

inline float LoadFloatBE(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(LoadBE32(base, address));
}

// -1 is what an unfinished kart may hold; anything past the race's own range is not this layout.
inline bool PlausibleCount(uint32_t raw) { return raw <= 4096 || raw == 0xFFFFFFFFu; }

// False when the kart does not look like this layout, or when the values are not sane: nothing is written then.
inline bool WriteProgress(uint8_t* base, uint32_t kart, const race::KartSample& state) {
  if (base == nullptr || kart == 0) return false;
  // The grid sits behind the start line: DistanceInRace and racingTime are finite negatives until GO.
  if (!std::isfinite(state.distance) || !std::isfinite(state.racing_time)) return false;
  if (state.item > race::kMaxItemId) return false;
  namespace layout = online_probe::layout;
  if (!PlausibleCount(LoadBE32(base, kart + layout::kKartCompletedLaps))) return false;
  if (!PlausibleCount(LoadBE32(base, kart + layout::kKartRacePosition))) return false;
  if (!PlausibleCount(LoadBE32(base, kart + layout::kKartRaceFinishPosition))) return false;
  if (!std::isfinite(LoadFloatBE(base, kart + layout::kKartDistanceInRace))) return false;
  if (!std::isfinite(LoadFloatBE(base, kart + layout::kKartRacingTime))) return false;

  StoreBE32(base, kart + layout::kKartCompletedLaps, state.lap);
  StoreBE32(base, kart + layout::kKartRacePosition, state.place);
  StoreBE32(base, kart + layout::kKartRaceFinishPosition, state.finish_place);
  StoreFloatBE(base, kart + layout::kKartDistanceInRace, state.distance);
  StoreFloatBE(base, kart + layout::kKartRacingTime, state.racing_time);
  // The item in hand: the guest draws what the host gave it. The card in the HUD reads its own kart, so this is the only
  // thing a guest has to be told for items to appear; using one already travels as the pad's X button.
  StoreBE32(base, kart + layout::kKartHeldItem, state.item);
  uint32_t flags = LoadBE32(base, kart + layout::kKartFlags2);
  if (state.finished) {
    flags |= 1u << layout::kKartRaceFinishedBit;
  } else {
    flags &= ~(1u << layout::kKartRaceFinishedBit);
  }
  StoreBE32(base, kart + layout::kKartFlags2, flags);
  return true;
}

// Only the item in hand, for a guest's own kart: its position and race are its own game's.
inline bool WriteHeldItem(uint8_t* base, uint32_t kart, uint8_t item) {
  if (base == nullptr || kart == 0 || item > race::kMaxItemId) return false;
  namespace layout = online_probe::layout;
  const uint32_t current = LoadBE32(base, kart + layout::kKartHeldItem);
  if (current > race::kMaxItemId && current != 0xFFFFFFFFu) return false;  // not the field this layout expects
  StoreBE32(base, kart + layout::kKartHeldItem, item);
  return true;
}

}  // namespace eck::race_sync
