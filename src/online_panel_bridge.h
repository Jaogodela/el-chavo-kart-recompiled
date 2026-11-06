#pragma once

// Finds the character panels in the objects the screen hook saw, by what their fields must hold, so a layout mistake costs
// remote panels and nothing else. Pure: `Memory` has Readable(address, length), U32 and U8.

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "online/protocol.h"
#include "online_character_select.h"
#include "online_lobby.h"
#include "online_probe_layout.h"

namespace eck::lobby {

namespace layout = online_probe::layout;

inline constexpr uint32_t kPanelScanFirst = 0x300;
inline constexpr uint32_t kPanelScanLast = 0x700;
inline constexpr size_t kMaxScanHits = 6;

struct PanelFields {
  uint32_t pad = 0;  // ControlledById
  uint32_t state = 0;
  uint32_t character = 0;
};

// DEBUG_SAULO
struct ScanCandidate {
  uint32_t pad_at = 0;
  uint32_t game_at = 0;
  uint32_t parent_at = 0;
  uint32_t parent = 0;
  uint32_t state_at = 0;
  uint32_t state = 0;
  uint32_t character_at = 0;
  uint32_t character = 0;
  const char* failed = "no_candidate";
};

struct PanelScan {
  std::optional<PanelFields> fields;
  uint32_t manager = 0;  // ScreenParent
  uint32_t shift = 0;    // of the real KartScreen fields from the layout header
  bool readable = false;
  const char* failure = "none";
  std::vector<uint32_t> pad_hits;   // DEBUG_SAULO: where panel 1 holds 1
  std::vector<uint32_t> game_hits;  // DEBUG_SAULO: where panel 0 points at the game
  ScanCandidate candidate;          // DEBUG_SAULO: the last pad field tried
};

template <typename Memory>
std::optional<int> PadAt(const Memory& memory, uint32_t panel, uint32_t at) {
  if (!memory.Readable(panel + at, 4)) return std::nullopt;
  const uint32_t value = memory.U32(panel + at);
  if (value >= kPanels) return std::nullopt;
  return int(value);
}

// ControlledById and TheGame are neighbours and every panel has its own pad, which no other pair of words in a screen repeats.
template <typename Memory>
bool IsPadField(const Memory& memory, const menu::PanelObjects& objects, uint32_t at, uint32_t game) {
  std::set<int> pads;
  for (const uint32_t panel : objects) {
    const std::optional<int> pad = PadAt(memory, panel, at);
    if (!pad || !memory.Readable(panel + at + 4, 4) || memory.U32(panel + at + 4) != game) return false;
    pads.insert(*pad);
  }
  return pads.size() == kPanels;
}

template <typename Memory>
std::optional<uint32_t> SharedParent(const Memory& memory, const menu::PanelObjects& objects, uint32_t shift) {
  const uint32_t at = layout::kPanelParent + shift;
  uint32_t parent = 0;
  for (const uint32_t panel : objects) {
    if (!memory.Readable(panel + at, 4)) return std::nullopt;
    const uint32_t value = memory.U32(panel + at);
    if (value == 0 || (parent != 0 && value != parent)) return std::nullopt;
    parent = value;
  }
  if (!memory.Readable(parent + layout::kObjectClass, 4) ||
      memory.U32(parent + layout::kObjectClass) == memory.U32(objects[0] + layout::kObjectClass)) {
    return std::nullopt;
  }
  return parent;
}

template <typename Memory>
std::optional<Panels> ReadPanels(const Memory& memory, const menu::PanelObjects& objects, const PanelFields& fields) {
  Panels panels;
  std::array<bool, kPanels> seen{};
  for (const uint32_t panel : objects) {
    const std::optional<int> pad = PadAt(memory, panel, fields.pad);
    if (!pad || seen[size_t(*pad)] || !memory.Readable(panel + fields.state, 1) || !memory.Readable(panel + fields.character, 4)) {
      return std::nullopt;
    }
    const uint32_t state = memory.U8(panel + fields.state);
    const uint32_t character = memory.U32(panel + fields.character);
    if (state > uint32_t(online::PickState::kConfirmed) || character >= uint32_t(online::kCharacters)) return std::nullopt;
    seen[size_t(*pad)] = true;
    panels[size_t(*pad)] = {online::PickState(state), int(character)};
  }
  return panels;
}

template <typename Memory>
void NoteScanHits(const Memory& memory, const menu::PanelObjects& objects, uint32_t game, uint32_t at, PanelScan& scan) {
  if (memory.Readable(objects[1] + at, 4) && memory.U32(objects[1] + at) == 1 && scan.pad_hits.size() < kMaxScanHits) {
    scan.pad_hits.push_back(at);
  }
  if (memory.Readable(objects[0] + at, 4) && memory.U32(objects[0] + at) == game && scan.game_hits.size() < kMaxScanHits) {
    scan.game_hits.push_back(at);
  }
}

// DEBUG_SAULO: what panel 0 holds at a candidate, with the first check it did not pass.
template <typename Memory>
ScanCandidate DescribeCandidate(const Memory& memory, const menu::PanelObjects& objects, uint32_t at, const PanelFields& fields,
                                bool parent_ok, bool panels_ok) {
  ScanCandidate seen;
  seen.pad_at = at;
  seen.game_at = at + 4;
  seen.parent_at = layout::kPanelParent + (at - layout::kPanelPad);
  seen.state_at = fields.state;
  seen.character_at = fields.character;
  if (memory.Readable(objects[0] + seen.parent_at, 4)) seen.parent = memory.U32(objects[0] + seen.parent_at);
  if (memory.Readable(objects[0] + fields.state, 1)) seen.state = memory.U8(objects[0] + fields.state);
  if (memory.Readable(objects[0] + fields.character, 4)) seen.character = memory.U32(objects[0] + fields.character);
  seen.failed = !parent_ok ? "screen_parent" : !panels_ok ? "state_or_character" : "none";
  return seen;
}

template <typename Memory>
PanelScan ScanPanels(const Memory& memory, const menu::PanelObjects& objects, uint32_t game) {
  PanelScan scan;
  for (const uint32_t panel : objects) {
    if (panel == 0) {
      scan.failure = "panels_not_seen";
      return scan;
    }
  }
  scan.readable = true;
  for (const uint32_t panel : objects) {
    scan.readable = scan.readable && memory.Readable(panel + layout::kObjectClass, 4) &&
                    memory.U32(panel + layout::kObjectClass) == memory.U32(objects[0] + layout::kObjectClass);
  }
  if (!scan.readable) {
    scan.failure = "panel_unreadable";
    return scan;
  }
  if (game == 0) {
    scan.failure = "game_unreadable";
    return scan;
  }
  for (uint32_t at = kPanelScanFirst; at < kPanelScanLast; at += 4) {
    NoteScanHits(memory, objects, game, at, scan);
    if (!IsPadField(memory, objects, at, game)) continue;
    const uint32_t shift = at - layout::kPanelPad;
    const std::optional<uint32_t> parent = SharedParent(memory, objects, shift);
    const PanelFields fields{at, layout::kPanelState + shift, layout::kPanelCharacter + shift};
    const bool panels_ok = ReadPanels(memory, objects, fields).has_value();
    scan.candidate = DescribeCandidate(memory, objects, at, fields, parent.has_value(), panels_ok);
    if (!parent || !panels_ok) continue;
    scan.fields = fields;
    scan.manager = *parent;
    scan.shift = shift;
    return scan;
  }
  scan.failure = "layout_not_found";
  return scan;
}

}  // namespace eck::lobby
