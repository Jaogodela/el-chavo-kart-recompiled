#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eck {

struct SwfFingerprint {
  std::size_t size;
  std::string_view sha256;
};

inline constexpr SwfFingerprint kMainMenuSwfFingerprint{
    176575, "6ac9fb05fdac97a9070878a99000ac6bd217b1678c68cf8ae5784e92b4ae8d7e"};

enum class SwfPatchStatus {
  kApplied,
  kSizeMismatch,
  kFingerprintMismatch,
  kUnexpectedStructure,
};

struct SwfPatchResult {
  SwfPatchStatus status = SwfPatchStatus::kUnexpectedStructure;
  std::vector<std::uint8_t> bytes;
  std::string detail;
};

// Adds the Exit Game row to the main-menu movie. The input is never modified, and a result other than kApplied
// carries no bytes.
SwfPatchResult PatchMainMenuSwf(std::span<const std::uint8_t> original,
                                const SwfFingerprint& expected = kMainMenuSwfFingerprint);

struct MenuRow {
  std::string name;
  std::int32_t y;
  std::string command;
};

// The tab rows of the main-menu sprite ordered top to bottom, or nothing if the movie is not recognizable.
std::optional<std::vector<MenuRow>> ListMainMenuRows(std::span<const std::uint8_t> movie);

}  // namespace eck
