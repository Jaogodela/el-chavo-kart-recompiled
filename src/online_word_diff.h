#pragma once

// Finds the 4-byte words of a memory range that changed since the previous scan. The caller supplies the reader, so this
// is pure: a test points it at a buffer, and the game points it at guest memory through the bounds-checked `Guest`.
//
// Used to locate fields the port does not know yet — what a kart holds after driving through an item box, for instance.
// Watching what changes when a player picks an item up and uses it names the field, without guessing an offset.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace eck::race_sync {

class WordWatch {
 public:
  struct Change {
    uint32_t offset = 0;
    uint32_t before = 0;
    uint32_t after = 0;
  };

  // Words that differ from the previous scan, in ascending offset order, at most `capacity`. The first scan of a range
  // only records it and reports nothing, because everything would otherwise be "changed". `truncated` is set when more
  // words changed than fit, and those stay recorded for the next scan.
  template <typename Read>
  size_t Scan(uint32_t from, uint32_t to, Read read, Change* out, size_t capacity, bool* truncated) {
    if (truncated != nullptr) *truncated = false;
    if (out == nullptr || capacity == 0 || to < from + 4) return 0;
    const size_t count = (to - from) / 4;
    const bool recorded = !before_.empty() && range_from_ == from && before_.size() == count;
    if (!recorded) {
      before_.resize(count);
      range_from_ = from;
      for (size_t i = 0; i < count; ++i) before_[i] = read(from + uint32_t(4 * i));
      return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t value = read(from + uint32_t(4 * i));
      if (value == before_[i]) continue;
      if (written == capacity) {
        if (truncated != nullptr) *truncated = true;
        return written;
      }
      out[written++] = Change{from + uint32_t(4 * i), before_[i], value};
      before_[i] = value;
    }
    return written;
  }

  void Reset() {
    before_.clear();
    range_from_ = 0;
  }

 private:
  std::vector<uint32_t> before_;
  uint32_t range_from_ = 0;
};

// The words of a range that are not zero, as " +0x868=0x0000000F" entries: the first sight of an object, which a later
// change is read against.
template <typename Read>
std::string DescribeNonZero(uint32_t from, uint32_t to, uint32_t base, Read read) {
  std::string text;
  for (uint32_t at = from; at + 4 <= to; at += 4) {
    const uint32_t value = read(at);
    if (value == 0) continue;
    char word[40];
    std::snprintf(word, sizeof word, " +0x%X=0x%08X", at - base, value);
    text += word;
  }
  return text;
}

// One probe line: the offsets that moved, against what they held. Offsets are relative to `base`, which is what a header
// constant takes, so the number in the log is the number to write down.
inline std::string DescribeChanges(const std::string& prefix, uint32_t base, const WordWatch::Change* changes,
                                   size_t count, bool truncated) {
  std::string text = prefix + " changed=";
  for (size_t i = 0; i < count; ++i) {
    char delta[56];
    std::snprintf(delta, sizeof delta, "%s+0x%X 0x%08X>0x%08X", i ? "," : "", changes[i].offset - base,
                  changes[i].before, changes[i].after);
    text += delta;
  }
  if (truncated) text += ",...";
  return text;
}

}  // namespace eck::race_sync
