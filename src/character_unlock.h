#pragma once

// The roster and the decision behind the Unlock secret characters option. Only the answer of
// KartScreen_PlayerSelectManager.isCharacterLocked changes, one call at a time: KartScoreDao.UnlockedCharacters is what the
// save and achievement 34 read, so it must stay untouched. Pure.

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace eck::roster {

inline constexpr int kCharacters = 12;
inline constexpr int kAvailableAtStart = -1;

struct Character {
  const char* name;
  int cup;  // the KartDefaultData_Cups entry that unlocks it
};

inline constexpr std::array<Character, kCharacters> kRoster = {{
    {"Chaves", kAvailableAtStart},
    {"Quico", kAvailableAtStart},
    {"Seu Madruga", kAvailableAtStart},
    {"Dona Florinda", 1},
    {"Professor Girafales", 1},
    {"Popis", kAvailableAtStart},
    {"Nhonho", kAvailableAtStart},
    {"Senhor Barriga", kAvailableAtStart},
    {"Dona Clotilde", 3},
    {"Paty", 5},
    {"Rufino Malfeitor", 7},
    {"Godines", kAvailableAtStart},
}};

constexpr bool IsPlayable(int character) { return character >= 0 && character < kCharacters; }
constexpr bool IsSecret(int character) { return IsPlayable(character) && kRoster[size_t(character)].cup != kAvailableAtStart; }

constexpr int SecretCount() {
  int count = 0;
  for (int character = 0; character < kCharacters; ++character) count += IsSecret(character) ? 1 : 0;
  return count;
}

// A panel another PC drives is never locked here: its owner's choice has to reach the screen whatever this save holds.
constexpr bool LockedForPanel(bool game_locked, int character, bool option, bool remote_panel) {
  return game_locked && !(IsPlayable(character) && (option || remote_panel));
}

// UE3 layout, from UObject::CallFunction, execLocalVariable and FName::ToString.
namespace layout {
inline constexpr uint32_t kCallFunction = 0x82313048;
inline constexpr uint32_t kNameToString = 0x823392B0;
inline constexpr uint32_t kGNames = 0x83CF5EB8;  // pointer to the FNameEntry* array
inline constexpr uint32_t kObjectOuter = 40;
inline constexpr uint32_t kObjectName = 44;
inline constexpr uint32_t kStructScript = 76;
inline constexpr uint32_t kStructScriptSize = 80;
inline constexpr uint32_t kPropertyOffset = 84;
inline constexpr uint32_t kFrameObject = 20;
inline constexpr uint32_t kFrameCode = 24;
inline constexpr uint32_t kFrameLocals = 28;
inline constexpr uint32_t kNameFlags = 0;
inline constexpr uint32_t kNameText = 8;
inline constexpr uint32_t kNameWideBit = 1;
inline constexpr uint8_t kTokenLocalVariable = 0x00;
inline constexpr uint8_t kTokenInstanceVariable = 0x01;
inline constexpr uint32_t kScriptPointerLowWord = 4;  // script pointers take 8 bytes; execLocalVariable reads the low word
inline constexpr uint16_t kScriptReturnNothing = 0x040B;
}  // namespace layout

enum class Watched { kNone, kLockCheck, kPreviewLock };

struct WatchedFunction {
  Watched watched;
  std::string_view name;
  std::string_view owner;
  uint32_t script_size;
};

inline constexpr std::array<WatchedFunction, 2> kWatched = {{
    {Watched::kLockCheck, "isCharacterLocked", "KartScreen_PlayerSelectManager", 117},
    {Watched::kPreviewLock, "SetLockVisible", "KartSceneCapture2DActor", 279},
}};

constexpr bool MayBeWatched(uint32_t script_size) {
  for (const WatchedFunction& function : kWatched) {
    if (function.script_size == script_size) return true;
  }
  return false;
}

// `Memory` has Readable(address, length), U32 and U8, as in online_panel_bridge.h.
template <typename Memory>
std::string ObjectName(const Memory& memory, uint32_t object) {
  constexpr uint32_t kMaxName = 63;
  if (!memory.Readable(object + layout::kObjectName, 4) || !memory.Readable(layout::kGNames, 4)) return {};
  const uint32_t names = memory.U32(layout::kGNames);
  const uint32_t slot = names + memory.U32(object + layout::kObjectName) * 4;
  if (!memory.Readable(slot, 4)) return {};
  const uint32_t entry = memory.U32(slot);
  if (!memory.Readable(entry + layout::kNameFlags, 4) || (memory.U32(entry + layout::kNameFlags) & layout::kNameWideBit) != 0) return {};
  std::string name;
  for (uint32_t i = 0; i <= kMaxName; ++i) {
    if (!memory.Readable(entry + layout::kNameText + i, 1)) return {};
    const uint32_t c = memory.U8(entry + layout::kNameText + i);
    if (c == 0) return name;
    name.push_back(char(c));
  }
  return {};
}

template <typename Memory>
Watched Identify(const Memory& memory, uint32_t function, uint32_t script_size) {
  const std::string name = ObjectName(memory, function);
  for (const WatchedFunction& watched : kWatched) {
    if (watched.script_size != script_size || name != watched.name) continue;
    if (!memory.Readable(function + layout::kObjectOuter, 4)) return Watched::kNone;
    if (ObjectName(memory, memory.U32(function + layout::kObjectOuter)) == watched.owner) return watched.watched;
  }
  return Watched::kNone;
}

// The character a lock check was asked about, read before CallFunction consumes it. The game passes a local or a field.
template <typename Memory>
std::optional<int> CharacterArgument(const Memory& memory, uint32_t frame) {
  if (!memory.Readable(frame, layout::kFrameLocals + 4)) return std::nullopt;
  const uint32_t code = memory.U32(frame + layout::kFrameCode);
  if (!memory.Readable(code, 1 + layout::kScriptPointerLowWord + 4)) return std::nullopt;
  const uint32_t token = memory.U8(code);
  if (token != layout::kTokenLocalVariable && token != layout::kTokenInstanceVariable) return std::nullopt;
  const uint32_t property = memory.U32(code + 1 + layout::kScriptPointerLowWord);
  if (!memory.Readable(property + layout::kPropertyOffset, 4)) return std::nullopt;
  const uint32_t holder = memory.U32(frame + (token == layout::kTokenLocalVariable ? layout::kFrameLocals : layout::kFrameObject));
  const uint32_t at = holder + memory.U32(property + layout::kPropertyOffset);
  if (!memory.Readable(at, 4)) return std::nullopt;
  return int(memory.U32(at));
}

// DEBUG_SAULO: each state the character screen reaches is logged once.
class UnlockLog {
 public:
  std::vector<std::string> Note(bool option, bool remote_panel, std::optional<int> character, bool game_locked, bool locked) {
    std::vector<std::string> lines;
    if (!announced_) {
      announced_ = true;
      lines.push_back("roster total=" + std::to_string(kCharacters) + " playable=" + std::to_string(kCharacters) +
                      " default=" + std::to_string(kCharacters - SecretCount()) + " secret=" + std::to_string(SecretCount()));
    }
    if (option_ != option) {
      option_ = option;
      lines.push_back(std::string("enabled=") + (option ? "yes" : "no") + " source=call_function_hook");
    }
    if (seen_.insert({character.value_or(-1), game_locked, locked, remote_panel}).second) {
      const char* name = character && IsPlayable(*character) ? kRoster[size_t(*character)].name : "unknown";
      lines.push_back("id=" + std::to_string(character.value_or(-1)) + " name=" + name + " original=" + (game_locked ? "locked" : "available") +
                      " override=" + (locked ? "locked" : "available") + " panel=" + (remote_panel ? "remote" : "local"));
    }
    return lines;
  }

 private:
  bool announced_ = false;
  std::optional<bool> option_;
  std::set<std::tuple<int, bool, bool, bool>> seen_;
};

}  // namespace eck::roster
