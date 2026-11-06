#pragma once

// Turns a real race into exactly one race.start and one race.complete. Pure: the guest side feeds it what the game shows
// (online_race_telemetry_guest.cpp) and sends what it returns.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "online/protocol.h"
#include "online_race_url.h"

namespace eck::race_telemetry {

inline constexpr char kLogTag[] = "[DEBUG_SAULO][ANALYTICS]";
inline constexpr int kCharacterCount = 12;
inline constexpr int kTrackCount = 20;
inline constexpr int kMaxHumans = 4;
inline constexpr int kMaxPlace = 16;
inline constexpr double kLoadTimeoutSeconds = 180.0;
inline constexpr int kMinDurationMs = 1000;
inline constexpr int kMaxDurationMs = 3600 * 1000;

// EKartRaceState.
enum RaceState { kCinematic = 0, kCountdown = 1, kInProgress = 2, kMenu = 3, kFinished = 4 };

// nullptr for a map that is not a race (the cup podium, the tutorials, a menu).
const char* CompetitionOfPrefix(std::string_view prefix);
// The script's EKartTracks id of a map, with or without its mode prefix.
std::optional<int> TrackOfMap(std::string_view map);
// "KartPawnChavo", or nullptr out of range.
const char* CharacterClassName(int id);
// "PistaCiudad_Circuito0" and "track_name_city", or nullptr out of range.
const char* TrackMapName(int id);
const char* TrackLocaleId(int id);

struct RaceSetup {
  std::string map;
  std::string competition;
  int humans = 0;
  std::optional<int> track;
  std::vector<int> characters;   // one per human, or empty when the URL did not prove them
  std::vector<int> controllers;  // ControllerId per human, or empty
};

// nullopt when the URL is not a race worth counting. Humans come first in the URL arrays.
std::optional<RaceSetup> SetupOfUrl(const race_url::RaceUrl& url);

struct KartReading {
  int controller_id = -1;
  int character = -1;
  int finish_position = 0;
  float racing_time = 0;
};

// What the game shows now. `game` is the GameInfo object: a new one is a new world.
struct GameView {
  uint32_t game = 0;
  std::optional<int> race_state;
  std::vector<KartReading> humans;
};

struct ServiceView {
  bool ready = false;  // already signed in, and the service takes race telemetry
  std::string room;    // the started room this player is seated in
  int seat = -1;
};

struct Output {
  std::optional<online::RaceStartFields> start;
  std::optional<online::RaceCompleteFields> complete;
  std::vector<std::string> lines;  // DEBUG_SAULO, without the tag
};

class RaceTracker {
 public:
  // Both return the lines to log. A race still running ends unreported, and the service counts it abandoned.
  std::vector<std::string> Arm(RaceSetup setup);
  std::vector<std::string> Stop(std::string_view reason);
  bool Active() const { return phase_ != Phase::kIdle; }
  Output Step(const GameView& game, const ServiceView& service, double now);

 private:
  enum class Phase { kIdle, kArmed, kLoading, kRacing };

  void Begin(const ServiceView& service, double now, Output& out);
  void Finish(const GameView& game, const ServiceView& service, double now, Output& out);
  void Drop(std::string_view reason, Output& out);
  void Reset() { phase_ = Phase::kIdle; }

  Phase phase_ = Phase::kIdle;
  RaceSetup setup_;
  double armed_at_ = -1;
  double started_at_ = 0;
  double seen_at_ = 0;
  int last_state_ = kInProgress;
  int logged_state_ = -2;  // DEBUG_SAULO
  uint32_t game_ = 0;
  int race_ = 0;
  std::string room_;
  int next_local_ = 0;
  std::string numbered_room_;
  int next_room_ = 0;
};

// Each human's RaceFinishPosition in URL order, or empty unless every human checks out: found by ControllerId, on the
// character the URL gave it, with a place of its own. Capture the Flag has only team places, so it never gets here.
std::vector<int> PlacesOf(const RaceSetup& setup, const std::vector<KartReading>& humans, std::vector<std::string>* lines);

// The race's own clock when the game's agrees with the wall clock, which also counts pauses and misses nothing.
int DurationMs(const std::vector<KartReading>& humans, double wall_seconds);

// DEBUG_SAULO: confirms the static id tables against races as they run. In memory only, for the life of the process.
class Discovery {
 public:
  std::vector<std::string> Note(const RaceSetup& setup);

 private:
  std::set<int> characters_;
  std::set<int> tracks_;
};

}  // namespace eck::race_telemetry
