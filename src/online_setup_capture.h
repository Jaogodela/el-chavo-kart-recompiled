#pragma once

// The host's game, taken from the values the menu movies hand to the script at the moment they are chosen. Those calls come
// through ExternalInterface, the only way into SelectMenu, SelectCompetitionType and SetAIDifficulty on these screens, and
// nothing about the character screen is needed to know them. Pure.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "online_character_select.h"
#include "online_lobby.h"

namespace eck::lobby {

inline constexpr char kSelectMenu[] = "SelectMenu";
inline constexpr char kSelectCompetition[] = "SelectCompetitionType";
inline constexpr char kSetDifficulty[] = "SetAIDifficulty";
inline constexpr char kFreeRaceMenu[] = "Exhibition";

constexpr bool IsSetupCall(std::string_view method) {
  return method == kSelectMenu || method == kSelectCompetition || method == kSetDifficulty;
}

class SetupCapture {
 public:
  // Each returns a line for the log, empty when nothing changed.
  std::string NoteMovieCall(std::string_view method, std::optional<int> number, std::string_view text) {
    if (method == kSelectMenu) return NoteMenu(text);
    if (!free_race_) return {};
    if (method == kSelectCompetition) return NoteCompetition(number);
    if (method == kSetDifficulty) return NoteDifficulty(number);
    return {};
  }

  std::string NoteScreen(menu::GameScreen screen) {
    switch (screen) {
      case menu::GameScreen::kMainMenu: return free_race_ ? Reset("main_menu") : std::string();
      case menu::GameScreen::kCompetition: return reached_character_ ? BackToCompetition() : std::string();
      case menu::GameScreen::kCharacter: return AtCharacterScreen();
      default: return {};
    }
  }

  std::optional<GameSetup> setup() const {
    if (!free_race_ || !mode_ || !difficulty_) return std::nullopt;
    return GameSetup{kGameTypeStartMenu, *mode_, *difficulty_};
  }

  // The first capture event that has not happened.
  const char* missing() const {
    if (!free_race_) return "free_race";
    if (!mode_) return "mode";
    return difficulty_ ? "none" : "difficulty";
  }

  int generation() const { return generation_; }

 private:
  std::string NoteMenu(std::string_view menu) {
    if (menu != kFreeRaceMenu) return free_race_ ? Reset("menu=" + std::string(menu)) : std::string();
    Clear();
    free_race_ = true;
    ++generation_;
    return "free_race generation=" + std::to_string(generation_);
  }

  std::string NoteCompetition(std::optional<int> number) {
    difficulty_.reset();
    reached_character_ = false;
    mode_.reset();
    if (!number || !ValidSetup({kGameTypeStartMenu, *number, 0})) return "mode rejected";
    mode_ = *number;
    return "mode committed value=" + std::to_string(*number);
  }

  std::string NoteDifficulty(std::optional<int> number) {
    difficulty_.reset();
    if (!number || !ValidSetup({kGameTypeStartMenu, 0, *number})) return "difficulty rejected";
    difficulty_ = *number;
    return "difficulty committed value=" + std::to_string(*number);
  }

  // CompetitionSelect also shows again when the difficulty popup closes, so only the way back from the character screen
  // takes the choices away.
  std::string BackToCompetition() {
    mode_.reset();
    difficulty_.reset();
    reached_character_ = false;
    return "reset reason=back_to_competition";
  }

  std::string AtCharacterScreen() {
    reached_character_ = true;
    if (const std::optional<GameSetup> game = setup()) {
      return "ready generation=" + std::to_string(generation_) + " game_type=" + std::to_string(game->game_type) +
             " mode=" + std::to_string(game->mode) + " difficulty=" + std::to_string(game->difficulty);
    }
    return std::string("incomplete generation=") + std::to_string(generation_) + " missing=" + missing();
  }

  std::string Reset(const std::string& reason) {
    Clear();
    return "reset reason=" + reason;
  }

  void Clear() {
    free_race_ = false;
    reached_character_ = false;
    mode_.reset();
    difficulty_.reset();
  }

  int generation_ = 0;
  bool free_race_ = false;
  bool reached_character_ = false;
  std::optional<int> mode_;
  std::optional<int> difficulty_;
};

// GFxValue, as Scaleform's ExternalInterface.call lays out its arguments.
inline constexpr uint32_t kGfxValueType = 12;
inline constexpr uint32_t kGfxValueData = 16;
inline constexpr uint32_t kGfxValueSize = 16 + 16;
inline constexpr uint32_t kGfxTypeMask = 0x0F;
inline constexpr uint32_t kGfxManagedBit = 0x40;
inline constexpr uint32_t kGfxInt = 3;
inline constexpr uint32_t kGfxUInt = 4;
inline constexpr uint32_t kGfxNumber = 5;
inline constexpr uint32_t kGfxString = 6;
inline constexpr uint32_t kMaxMovieChars = 31;

struct MovieArgument {
  std::optional<int> number;
  std::string text;
};

// `Memory` has Readable(address, length), U32 and U8. Nothing is read that was not checked, and the method name needs its
// terminator inside the limit, so a method that is not one of ours comes back empty.
template <typename Memory>
std::string MovieText(const Memory& memory, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; i <= kMaxMovieChars; ++i) {
    if (!memory.Readable(address + i, 1)) return {};
    const uint32_t c = memory.U8(address + i);
    if (c == 0) return text;
    if (c < 0x20 || c >= 0x7F) return {};
    text.push_back(char(c));
  }
  return {};
}

template <typename Memory>
MovieArgument FirstMovieArgument(const Memory& memory, uint32_t args, uint32_t count) {
  MovieArgument argument;
  if (count == 0 || args == 0 || !memory.Readable(args, kGfxValueSize)) return argument;
  const uint32_t raw_type = memory.U32(args + kGfxValueType);
  const uint32_t value = memory.U32(args + kGfxValueData);
  switch (raw_type & kGfxTypeMask) {
    case kGfxInt:
    case kGfxUInt:
      argument.number = int(value);
      break;
    case kGfxNumber: {
      const uint64_t bits = (uint64_t(value) << 32) | memory.U32(args + kGfxValueData + 4);
      double number;
      static_assert(sizeof number == sizeof bits);
      std::memcpy(&number, &bits, sizeof number);
      if (number >= -1.0 && number <= 0xFFFF && number == double(int(number))) argument.number = int(number);
      break;
    }
    case kGfxString: {
      const bool managed = (raw_type & kGfxManagedBit) != 0;
      const uint32_t text = managed && memory.Readable(value, 4) ? memory.U32(value) : (managed ? 0 : value);
      if (text != 0) argument.text = MovieText(memory, text);
      break;
    }
    default: break;
  }
  return argument;
}

}  // namespace eck::lobby
