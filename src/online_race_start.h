#pragma once

// The race every PC loads. The host's final URL becomes validated numbers in the session's rules, so a guest never runs text
// it received. Kart slots are the same everywhere; only which pad holds each slot differs. Pure.

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "online/online_service.h"
#include "online/protocol.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"

namespace eck::lobby {

inline constexpr int kRaceMaxKarts = 8;
inline constexpr int kRacePads = online::kMaxSeats;
inline constexpr int kBotController = 99;
inline constexpr double kRaceStartSeconds = 15.0;  // SSF2T's establishment budget
inline constexpr size_t kMaxCommandChars = 400;
inline constexpr std::array<std::string_view, 7> kRacePrefixes = {"", "TA", "SU", "EN", "CO", "FO", "CT"};
inline constexpr char kExhibitionOption[] = "GameType0";  // the game writes it without '='

using PadSeats = std::array<int, kRacePads>;  // index = pad, value = seat, -1 for none

struct CanonicalRace {
  int generation = 0;
  int mode = 0;
  int track = 0;
  int difficulty = 0;
  int humans = 0;
  int total = 0;
  std::array<int, kRaceMaxKarts> characters{};
  uint32_t bots = 0;   // bit per kart slot
  uint32_t teams = 0;  // bit per kart slot
  std::array<int, kRacePads> seats{};  // kart slot of a human -> session seat
};

struct RaceParse {
  std::optional<CanonicalRace> race;
  const char* failed = "none";
};

inline int RaceModeOfPrefix(std::string_view prefix) {
  for (size_t i = 0; i < kRacePrefixes.size(); ++i) {
    if (race_url::GameModeOfPrefix(prefix) == race_url::GameModeOfPrefix(kRacePrefixes[i])) return int(i);
  }
  return -1;
}

constexpr bool ValidRaceCounts(int humans, int total) { return humans >= 2 && humans <= kRacePads && total >= humans && total <= kRaceMaxKarts; }

// `seat_of_pad` is who holds each panel on the host. Humans come first in the game's URL, so slot and seat line up.
inline RaceParse CanonicalOfUrl(const race_url::RaceUrl& url, const PadSeats& seat_of_pad, int generation) {
  RaceParse parse;
  const auto fail = [&](const char* field) {
    parse.failed = field;
    return parse;
  };
  CanonicalRace race;
  race.generation = generation;
  race.mode = RaceModeOfPrefix(url.map_prefix);
  if (race.mode < 0) return fail("mode");
  const std::optional<int> track = race_telemetry::TrackOfMap(url.map);
  if (!track) return fail("track");
  race.track = *track;
  race.difficulty = url.ai_difficulty.value_or(-1);
  if (race.difficulty < 0 || race.difficulty > 2) return fail("difficulty");
  race.humans = url.num_human_karts.value_or(0);
  race.total = url.total_karts.value_or(0);
  if (!ValidRaceCounts(race.humans, race.total)) return fail("karts");
  const size_t total = size_t(race.total);
  if (url.players.size() != total || url.characters.size() != total || url.bots.size() != total || url.teams.size() != total) {
    return fail("arrays");
  }
  for (const auto& [key, value] : url.other) {
    if (key != kExhibitionOption) return fail("unknown_option");
  }
  std::array<bool, kRacePads> seated{};
  for (int slot = 0; slot < race.total; ++slot) {
    const size_t index = size_t(slot);
    const int character = url.characters[index];
    if (character < 0 || character >= online::kCharacters) return fail("characters");
    race.characters[index] = character;
    if (url.teams[index] < 0 || url.teams[index] > 1) return fail("teams");
    if (url.teams[index] == 1) race.teams |= 1u << slot;
    const bool human = slot < race.humans;
    if (url.bots[index] != (human ? 0 : 1)) return fail("bots");
    if (!human) {
      race.bots |= 1u << slot;
      if (url.players[index] != kBotController) return fail("players");
      continue;
    }
    const int pad = url.players[index];
    if (pad < 0 || pad >= kRacePads) return fail("players");
    const int seat = seat_of_pad[size_t(pad)];
    if (seat < 0 || seat >= kRacePads || seated[size_t(seat)]) return fail("seats");
    seated[size_t(seat)] = true;
    race.seats[index] = seat;
  }
  parse.race = race;
  return parse;
}

inline online::RoomRules RaceRules(online::RoomRules rules, const CanonicalRace& race) {
  const auto nibbles = [&](int first) {
    int value = 0;
    for (int i = 0; i < 4; ++i) value |= race.characters[size_t(first + i)] << (4 * i);
    return value;
  };
  int seats = 0;
  for (int slot = 0; slot < race.humans; ++slot) seats |= race.seats[size_t(slot)] << (3 * slot);
  rules.emplace_back("race", race.generation);
  rules.emplace_back("race_mode", race.mode);
  rules.emplace_back("race_track", race.track);
  rules.emplace_back("race_ai", race.difficulty);
  rules.emplace_back("race_humans", race.humans);
  rules.emplace_back("race_karts", race.total);
  rules.emplace_back("race_chars_low", nibbles(0));
  rules.emplace_back("race_chars_high", nibbles(4));
  rules.emplace_back("race_bots", int(race.bots));
  rules.emplace_back("race_teams", int(race.teams));
  rules.emplace_back("race_seats", seats);
  return rules;
}

// A guest trusts nothing here: every field is checked as if the host had not.
inline std::optional<CanonicalRace> RaceOfRules(const online::RoomRules& rules) {
  const auto value = [&](std::string_view key) -> std::optional<int> {
    for (const auto& [name, number] : rules) {
      if (name == key) return number;
    }
    return std::nullopt;
  };
  const auto generation = value("race"), mode = value("race_mode"), track = value("race_track"), difficulty = value("race_ai"),
             humans = value("race_humans"), total = value("race_karts"), low = value("race_chars_low"),
             high = value("race_chars_high"), bots = value("race_bots"), teams = value("race_teams"), seats = value("race_seats");
  if (!generation || !mode || !track || !difficulty || !humans || !total || !low || !high || !bots || !teams || !seats) {
    return std::nullopt;
  }
  if (*generation < 1 || *mode < 0 || *mode >= int(kRacePrefixes.size()) || !race_telemetry::TrackMapName(*track) || *difficulty < 0 ||
      *difficulty > 2 || !ValidRaceCounts(*humans, *total)) {
    return std::nullopt;
  }
  CanonicalRace race;
  race.generation = *generation;
  race.mode = *mode;
  race.track = *track;
  race.difficulty = *difficulty;
  race.humans = *humans;
  race.total = *total;
  race.teams = uint32_t(*teams);
  race.bots = uint32_t(*bots);
  const uint32_t human_mask = (1u << race.humans) - 1;
  const uint32_t slot_mask = (1u << race.total) - 1;
  if ((race.teams & ~slot_mask) != 0 || race.bots != (slot_mask & ~human_mask)) return std::nullopt;
  for (int slot = 0; slot < race.total; ++slot) {
    const int packed = slot < 4 ? *low : *high;
    const int character = (packed >> (4 * (slot % 4))) & 0xF;
    if (character >= online::kCharacters) return std::nullopt;
    race.characters[size_t(slot)] = character;
  }
  std::array<bool, kRacePads> seated{};
  for (int slot = 0; slot < race.humans; ++slot) {
    const int seat = (*seats >> (3 * slot)) & 7;
    if (seat >= kRacePads || seated[size_t(seat)]) return std::nullopt;
    seated[size_t(seat)] = true;
    race.seats[size_t(slot)] = seat;
  }
  return race;
}

inline std::optional<int> SlotOfSeat(const CanonicalRace& race, int seat) {
  for (int slot = 0; slot < race.humans; ++slot) {
    if (race.seats[size_t(slot)] == seat) return slot;
  }
  return std::nullopt;
}

// The pad a seat's kart answers to on this PC. Unlike the character screen, a race reads the physical pad as itself: the local
// player keeps their own pad and the others take the remaining pads in seat order. SetupLocalPlayers gives GamePlayers[slot]
// this id, so the pad decides which kart a local player drives and the slots stay the same on every PC.
inline int PadOfSeat(const CanonicalRace& race, int seat, int local_seat, int own_pad) {
  if (seat == local_seat) return own_pad;
  std::array<int, kRacePads> others{};
  int count = 0;
  for (int slot = 0; slot < race.humans; ++slot) {
    if (race.seats[size_t(slot)] != local_seat) others[size_t(count++)] = race.seats[size_t(slot)];
  }
  std::sort(others.begin(), others.begin() + count);
  int pad = 0;
  for (int i = 0; i < count; ++i, ++pad) {
    if (pad == own_pad) ++pad;
    if (others[size_t(i)] == seat) return pad;
  }
  return -1;
}

// The game's URL names each kart by the panel it was picked on. It stands only while that is also the pad RaceSync drives,
// which an empty seat or a host on another pad breaks.
inline bool GameCommandMatchesPads(const CanonicalRace& race, int own_pad) {
  for (int slot = 0; slot < race.humans; ++slot) {
    const int seat = race.seats[size_t(slot)];
    if (PadOfSeat(race, seat, 0, own_pad) != seat) return false;
  }
  return true;
}

// The same race on every PC: only PlayersArray and PrimaryControllerID belong to this PC.
inline std::string RaceCommand(const CanonicalRace& race, int local_seat, int own_pad) {
  const auto list = [&](auto value) {
    std::string text;
    for (int slot = 0; slot < race.total; ++slot) text += (slot ? "-" : "") + std::to_string(value(slot));
    return text;
  };
  const std::string prefix(kRacePrefixes[size_t(race.mode)]);
  std::string command = "start " + (prefix.empty() ? std::string() : prefix + "-") + race_telemetry::TrackMapName(race.track);
  command += "?CharArray=" + list([&](int slot) { return race.characters[size_t(slot)]; });
  command += "?IsBotArray=" + list([&](int slot) { return (race.bots >> slot) & 1; });
  command += "?PlayersArray=" + list([&](int slot) {
    return slot < race.humans ? PadOfSeat(race, race.seats[size_t(slot)], local_seat, own_pad) : kBotController;
  });
  command += "?TeamData=" + list([&](int slot) { return (race.teams >> slot) & 1; }) + "-";
  command += "?PrimaryControllerID=" + std::to_string(own_pad);
  command += "?NumSelectedTrack=" + std::to_string(race.track);
  command += "?NumHumanKarts=" + std::to_string(race.humans);
  command += "?TotalKarts=" + std::to_string(race.total);
  command += "?KartAIDiffculty=" + std::to_string(race.difficulty);
  return command + "?" + kExhibitionOption;
}

struct RaceStep {
  bool publish = false;
  bool commit = false;
  const char* abort = nullptr;
};

// The host side of a start. The room is the shared clock: every PC travels on `racing`, which the service sets once each
// seat's ready arrived, so a guest needs no timer of its own.
class HostRaceStart {
 public:
  void Begin(double now) {
    phase_ = Phase::kWaitingForPaths;
    deadline_ = now + kRaceStartSeconds;
  }
  void NotePublished() {
    if (phase_ == Phase::kWaitingForPaths) phase_ = Phase::kWaitingForRacing;
  }
  bool active() const { return phase_ != Phase::kIdle; }

  RaceStep Update(const online::Snapshot& state, double now) {
    RaceStep step;
    if (phase_ == Phase::kIdle) return step;
    if (!state.room || state.seat != 0 || state.room->players < 2) {
      step.abort = "peer_lost";
    } else if (phase_ == Phase::kWaitingForRacing && state.room->state == online::RoomState::kRacing) {
      step.commit = true;
    } else if (now >= deadline_) {
      step.abort = "race_transition_timeout";
    } else if (phase_ == Phase::kWaitingForPaths && state.paths_ready && state.room->state == online::RoomState::kOpen) {
      step.publish = true;
    }
    if (step.commit || step.abort) phase_ = Phase::kIdle;
    return step;
  }

 private:
  enum class Phase { kIdle, kWaitingForPaths, kWaitingForRacing };

  Phase phase_ = Phase::kIdle;
  double deadline_ = 0;
};

}  // namespace eck::lobby
