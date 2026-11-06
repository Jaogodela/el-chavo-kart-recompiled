#pragma once

// What the Scaleform open-file hook may do with a movie. Only the main menu is searched: everything else is identified by
// its URL, or left to the game. Pure: the guest memory comes in as a template parameter.

#include <cstdint>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "online_character_select.h"

namespace eck::open_file {

enum class Inspection { kPass, kTrackScreen, kPatchMainMenu };

constexpr Inspection InspectionOf(std::string_view url) {
  const menu::GameScreen screen = menu::ScreenOfMovie(url);
  if (screen == menu::GameScreen::kMainMenu) return Inspection::kPatchMainMenu;
  return screen == menu::GameScreen::kOther ? Inspection::kPass : Inspection::kTrackScreen;
}

constexpr const char* InspectionName(Inspection inspection) {
  switch (inspection) {
    case Inspection::kPatchMainMenu: return "patch_main_menu";
    case Inspection::kTrackScreen: return "track_screen";
    default: return "pass";
  }
}

struct MovieFields {
  uint32_t pointer;
  uint32_t size;
};

inline constexpr uint32_t kObjectScanBytes = 0x80;
inline constexpr uint32_t kChildScanBytes = 0x100;
inline constexpr int kMaxObjectDepth = 4;
inline constexpr size_t kMaxScannedObjects = 2048;
inline constexpr uint32_t kGfxMagic = 0x474658;  // "GFX", the first three bytes of a movie

constexpr bool LooksLikeHeapObject(uint32_t address) {
  return address >= 0x40000000 && address < 0x80000000 && address % 4 == 0;
}

// `Memory` has Readable(address, length) and U32(address); U32 is only called on a range Readable accepted.
template <typename Memory>
std::optional<MovieFields> FindMovieFields(const Memory& memory, uint32_t object, uint32_t span, uint32_t movie_size) {
  if (!memory.Readable(object, span)) return std::nullopt;
  for (uint32_t at = 0; at + 4 <= span; at += 4) {
    if (memory.U32(object + at) != movie_size) continue;
    for (const int64_t neighbor : {int64_t(-4), int64_t(4)}) {
      const int64_t pointer_at = int64_t(at) + neighbor;
      if (pointer_at < 0 || pointer_at + 4 > span) continue;
      const uint32_t data = memory.U32(object + uint32_t(pointer_at));
      if (memory.Readable(data, 4) && (memory.U32(data) >> 8) == kGfxMagic) {
        return MovieFields{object + uint32_t(pointer_at), object + at};
      }
    }
  }
  return std::nullopt;
}

// Breadth-first through the objects the opened file points at, so a movie owned by a wrapper is still found.
template <typename Memory>
std::optional<MovieFields> FindMovieFieldsInTree(const Memory& memory, uint32_t root, uint32_t movie_size) {
  std::vector<uint32_t> level{root};
  std::set<uint32_t> visited{root};
  for (int depth = 0; depth < kMaxObjectDepth && !level.empty(); ++depth) {
    std::vector<uint32_t> next;
    for (const uint32_t object : level) {
      const uint32_t span = depth == 0 ? kObjectScanBytes : kChildScanBytes;
      if (!memory.Readable(object, span)) continue;
      if (const auto fields = FindMovieFields(memory, object, span, movie_size)) return fields;
      for (uint32_t at = 0; at + 4 <= span && visited.size() < kMaxScannedObjects; at += 4) {
        const uint32_t child = memory.U32(object + at);
        if (LooksLikeHeapObject(child) && visited.insert(child).second) next.push_back(child);
      }
    }
    level = std::move(next);
  }
  return std::nullopt;
}

}  // namespace eck::open_file
