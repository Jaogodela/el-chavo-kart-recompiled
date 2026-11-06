#pragma once

// DEBUG_SAULO: probes and the menu-facing API for online races. Everything declared here is stable for the menu code;
// the first half is pure (no ReXGlue), the second half is implemented in online_probe_guest.cpp.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "online_race.h"

namespace eck::online_probe {

inline constexpr char kRaceTag[] = "[DEBUG_SAULO][ONLINE-RACE]";
inline constexpr char kXboxTag[] = "[DEBUG_SAULO][ONLINE-XBOX]";

// ---- The six missing runtime pieces (docs/online-analysis.md, "Gameplay sync status") -------------------------------

struct ProbeInfo {
  int number;              // item number in the doc
  const char* name;
  const char* unknown;
  const char* probe;       // file and function
  const char* log_prefix;  // what the expected line starts with after the tag
  const char* unlocks;
};

inline constexpr std::array<ProbeInfo, 6> kProbes = {{
    {1, "race_url", "What LoadRace hands to ConsoleCommand: NumHumanKarts limit and the fields a remote human needs",
     "online_probe_guest.cpp OnConsoleCommand via hook sub_826A1F30 (AActor::ConsoleCommand)", "url=",
     "Builds the online race URL (remote humans as virtual LocalPlayers) without guessing"},
    {2, "kart_layout", "AKartPawn field offsets (position, velocity, inputs, lap) in guest memory",
     "online_probe_guest.cpp PumpGuestProbes, kart= line at 2 Hz (ECK_ONLINE_DEBUG=1)", "kart=",
     "State encoder/decoder for KartState; confirms or corrects online_probe_layout.h"},
    {3, "slot_control", "Whether a remote kart can be a human PlayerController without a pad and never a CPU",
     "online_probe_guest.cpp PumpGuestProbes, slot= line (GamePlayers, Controller kind, bIsRacingAI)", "slot=",
     "Decides virtual LocalPlayer per remote seat versus controllerless pawn"},
    {4, "race_seed", "Where item and AI randomness is seeded (CRT srand) and by whom",
     "online_probe_guest.cpp hook sub_830B53B0 (srand)", "seed=",
     "Carry RaceConfig.seed into every PC at race start"},
    {5, "finish", "Which state reports a kart finishing and its final place",
     "online_probe_guest.cpp PumpGuestProbes, finish_position= and state= lines", "finish_position=",
     "Fill Finish/RaceResult from the game, end the room race"},
    {6, "lap_checkpoint", "Whether lap and path-node progress are plain fields that can be snapped on a remote kart",
     "online_probe_guest.cpp PumpGuestProbes, lap= line with field addresses", "lap=",
     "Resync a kart that desynchronised; lap/checkpoint authority"},
}};

inline const std::array<ProbeInfo, 6>& RegisteredProbes() { return kProbes; }

// ---- Race start plan and dry-run mapping ---------------------------------------------------------------------------

// What the menu knows when "Start Race" is pressed in a room.
struct RaceStartPlan {
  int host_seat = 0;
  int local_seat = 0;
  int track = 0;
  int laps = 3;
  int total_karts = race::kMaxKarts;
  int bot_difficulty = 0;
  std::string map_prefix = "TA";  // game mode prefix, see DefaultMapPrefixes
  std::vector<race::Participant> participants;  // every human seat with its character and kart
};

struct SeatLine {
  int seat = -1;
  int slot = -1;
  bool local = false;
  int local_player = -1;      // index in GamePlayers on this PC (remote seats: -1 unless they are virtual players)
  int controller_id = -1;     // ControllerId in PlayersArray
  int character = 0;
  bool cpu = false;
};

struct DryRun {
  bool valid = false;
  int host_seat = 0;
  int local_seat = 0;
  std::vector<SeatLine> humans;  // slot order
  int bots = 0;
  int num_human_karts = 0;
  int total_karts = 0;
  int track = 0;
  int bot_difficulty = 0;
  std::vector<int> players_array;  // per kart
  std::vector<int> char_array;
  std::vector<int> bot_array;
};

// Expected game URL fields for the plan. Remote humans are expected to be extra local players on virtual controller ids
// (1 to 3 skipping the local pad 0): this is the layout `SetupLocalPlayers` can build, not something observed.
DryRun PlanDryRun(const RaceStartPlan& plan);
// The lines OnOnlineRaceStartRequested prints, without the tag.
std::vector<std::string> DryRunLines(const DryRun& dry);
// "start blocked reason=<reason>", without the tag.
std::string BlockedLine(std::string_view reason);

// ---- UE3 net activity summary -------------------------------------------------------------------------------------

enum class Ue3Api : int {
  kSocket,
  kSendTo,
  kRecvFrom,
  kCreateNamedNetDriver,
  kClientTravel,
  kLiveCreateOnlineGame,
  kLiveJoinOnlineGame,
  kXSessionCreate,
  kXSessionJoinRemote,
  kCount
};

const char* Ue3ApiName(Ue3Api api);
// "ue3_net_summary socket=0 ... verdict=none_active", without the tag.
std::string Ue3NetSummaryLine(const std::array<uint64_t, size_t(Ue3Api::kCount)>& counts, std::string_view reason);

// ---- Menu-facing API (online_probe_guest.cpp) ----------------------------------------------------------------------

// Call from Start Race. Logs the dry-run mapping and the UE3 summary. Never starts anything.
void OnOnlineRaceStartRequested(const RaceStartPlan& plan);
// Call instead when Start Race is refused; logs `start blocked reason=<reason>` and the UE3 summary.
void OnOnlineRaceStartBlocked(std::string_view reason);

// Internal hooks of the probes.
// AActor::ConsoleCommand entry: `fstring` is the guest address of the FString (data ptr, num incl. NUL, max), UTF-16 BE.
// Reads it without consuming anything and logs it only when it is a race start URL.
void OnConsoleCommand(const uint8_t* base, uint32_t fstring, uint32_t self, uint32_t caller);
// The session seat of each human kart slot of the race about to load, -1 for none.
void SetRaceSeats(const std::array<int, 4>& seats);
void OnSeed(uint32_t seed, uint32_t caller);
std::string XboxContext();  // "t_ms=.. phase=conn:../room:.. seat=.."
void NoteUe3Api(Ue3Api api);
void SetOnlinePhase(int connection, int room_state, int seat);  // from the 1 Hz pump
uint32_t OnlinePhasePacked();
std::string PhaseText();  // "conn:3/room:1"
int PhaseSeat();
// Pumped from PumpOnlineGameDebug once per frame; samples at 2 Hz and only when ECK_ONLINE_DEBUG=1.
void PumpGuestProbes(double now_seconds);

}  // namespace eck::online_probe
