#pragma once

// The roster and the decision behind the Unlock race tracks option. Only the answers of
// KartScreen_TrackSelectMenu.IsTrackLocked and of its list's LockItem change, one call at a time: KartScoreDao.UnlockedTracks is
// what the save, achievement 35 and the Next Race rotation read, so it must stay untouched. Pure.

#include <array>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace eck::tracks {

inline constexpr int kTracks = 20;
inline constexpr int kAvailableAtStart = -1;

struct Track {
  const char* map;
  const char* name;
  int challenge;  // the KartDefaultData_Challenges definition whose win unlocks it
};

// KartProgressManager_Base.normalTracks order, which is EKartTracks and the index of KartScoreDao.UnlockedTracks.
inline constexpr std::array<Track, kTracks> kRoster = {{
    {"PistaBrasilM_Circuito0", "Brazil Maracana", kAvailableAtStart},
    {"PistaCastillo_Circuito0", "Castle of 71", kAvailableAtStart},
    {"PistaTanga_Circuito1", "Pueblito", kAvailableAtStart},
    {"PistaMuseo_Circuito0", "The Museum", kAvailableAtStart},
    {"PistaCiudad_Circuito0", "Chavoltepec", kAvailableAtStart},
    {"PistaBrasilC_Circuito0", "Brazilian Sky", kAvailableAtStart},
    {"PistaAcapulco_Circuito0", "Acapulco", kAvailableAtStart},
    {"PistaCastillo_Circuito1", "Haunted House", kAvailableAtStart},
    {"PistaElEspacio_Circuito0", "Space Route", kAvailableAtStart},
    {"PistaLaFeria_Circuito0", "The Funfair", kAvailableAtStart},
    {"PistaHeroes_Circuito0", "Heroic Wrestle", 3},
    {"PistaTanga_Circuito0", "Tangamandapio", 7},
    {"PistaCiudad_Circuito1", "The City", 11},
    {"PistaBrasilM_Circuito1", "Soccer Fever", 15},
    {"PistaMuseo_Circuito1", "The Gallery", 19},
    {"PistaBrasilC_Circuito1", "Over Rio", 23},
    {"PistaAcapulco_Circuito1", "The Coast", 27},
    {"PistaLaFeria_Circuito1", "Theme Park", 31},
    {"PistaHeroes_Circuito1", "Hero's Gym", 35},
    {"PistaElEspacio_Circuito1", "Milky Way", 39},
}};

constexpr bool IsPlayable(int track) { return track >= 0 && track < kTracks; }
constexpr bool IsLockedAtStart(int track) { return IsPlayable(track) && kRoster[size_t(track)].challenge != kAvailableAtStart; }

constexpr int LockedAtStartCount() {
  int count = 0;
  for (int track = 0; track < kTracks; ++track) count += IsLockedAtStart(track) ? 1 : 0;
  return count;
}

// An id outside the roster is not a track the game can run, whatever the option says.
constexpr bool LockedForTrack(bool game_locked, int track, bool option) { return game_locked && !(IsPlayable(track) && option); }

enum class Surface { kPreview, kList };

// Each state the track screen reaches is logged once.
class UnlockLog {
 public:
  std::vector<std::string> Note(Surface surface, std::optional<int> track, bool game_locked, bool locked) {
    std::vector<std::string> lines;
    if (!announced_) {
      announced_ = true;
      lines.push_back("roster total=" + std::to_string(kTracks) + " playable=" + std::to_string(kTracks) +
                      " default=" + std::to_string(kTracks - LockedAtStartCount()) + " locked=" + std::to_string(LockedAtStartCount()));
      lines.push_back("enabled=yes source=availability_hook");
    }
    if (seen_.insert({track.value_or(-1), int(surface), game_locked, locked}).second) {
      const char* name = track && IsPlayable(*track) ? kRoster[size_t(*track)].name : "unknown";
      lines.push_back("id=" + std::to_string(track.value_or(-1)) + " name=" + name + " original=" + (game_locked ? "locked" : "available") +
                      " override=" + (locked ? "locked" : "available") +
                      " context=" + (surface == Surface::kPreview ? "free_race" : "free_race_list"));
    }
    return lines;
  }

 private:
  bool announced_ = false;
  std::set<std::tuple<int, int, bool, bool>> seen_;
};

}  // namespace eck::tracks
