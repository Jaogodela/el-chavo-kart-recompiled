#include "online_probe.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "online_game_guest.h"
#include "online_guest_reader.h"
#include "online_probe_layout.h"
#include "online_race_url.h"
#include "online_runtime.h"
#include "online_word_diff.h"

// Diagnostics for the online race work.

namespace eck::online_probe {

namespace {

namespace lay = layout;
using online_guest::Guest;

// The item probe watches memory the layout does not name yet, so it is opt-in and separate from the debug probes.
constexpr char kItemProbeEnv[] = "ECK_ITEM_PROBE";
// How much of the character info object the item probe watches. A guess, bounded by the reader's own checks.
constexpr uint32_t kCharInfoSpan = 0x200;

std::atomic<uint64_t> g_ue3[size_t(Ue3Api::kCount)];
std::atomic<uint32_t> g_phase{0xFFFFFFFFu};  // connection | room state << 8 | (seat + 1) << 16
std::atomic<int> g_url_humans{-1};
std::atomic<int> g_url_total{-1};
std::array<std::atomic<int>, 4> g_slot_seats{-1, -1, -1, -1};

bool DebugOn() {
  static const bool on = online_guest::EnvOn(std::getenv(online_guest::kDebugEnv));
  return on;
}

void Race(const std::string& text) { REXLOG_INFO("{} {}", kRaceTag, text); }

std::string Hex(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%08X", value);
  return text;
}

std::string Vec(const Guest& g, uint32_t at) {
  char text[96];
  std::snprintf(text, sizeof text, "(%.1f,%.1f,%.1f)", g.F32(at), g.F32(at + 4), g.F32(at + 8));
  return text;
}

std::string Rot(const Guest& g, uint32_t at) {
  char text[64];
  std::snprintf(text, sizeof text, "(%d,%d,%d)", g.I32(at), g.I32(at + 4), g.I32(at + 8));
  return text;
}

std::string Num(float value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.3f", std::isfinite(value) ? double(value) : 0.0);
  return text;
}

// Logs a line when the text under `key` changed.
void OnChange(const std::string& key, const std::string& text) {
  static std::map<std::string, std::string> last;
  static int lines = 0;
  auto& previous = last[key];
  if (previous == text || lines >= 800) return;
  previous = text;
  ++lines;
  Race(text);
}

struct PawnView {
  uint32_t pawn = 0;
  uint32_t controller = 0;
  int local_slot = -1;  // index in GamePlayers when a local player owns the controller
};

int OwnPad(const Guest& g) {
  const uint32_t world = g.P(lay::kGWorld);
  const uint32_t level = world != 0 ? g.P(world + lay::kWorldLevel) : 0;
  const uint32_t actors = level != 0 ? g.P(level + lay::kLevelActors) : 0;
  const uint32_t info = actors != 0 ? g.P(actors) : 0;
  const uint32_t game = info != 0 ? g.P(info + lay::kWorldGame) : 0;
  return game != 0 ? g.I32(game + lay::kGamePrimaryController) : -1;
}

void PollSlots(const Guest& g, std::vector<PawnView>* pawns, std::vector<uint32_t>* controllers, int* owned_slot) {
  const uint32_t engine = g.P(lay::kGEngine);
  if (engine == 0) {
    OnChange("engine", "engine=none");
    return;
  }
  const uint32_t data = g.P(engine + lay::kEngineGamePlayers);
  const uint32_t count = g.U32(engine + lay::kEngineGamePlayers + 4).value_or(0);
  std::string summary = "players=" + std::to_string(count) + " expected_humans=" + std::to_string(g_url_humans.load());
  OnChange("players", summary);
  for (uint32_t i = 0; i < count && i < 8 && data != 0; ++i) {
    const uint32_t lp = g.P(data + 4 * i);
    if (lp == 0) continue;
    const int user = g.I32(lp + lay::kLocalPlayerControllerId);
    const uint32_t pc = g.P(lp + lay::kPlayerActor);
    const uint32_t pawn = pc != 0 ? g.P(pc + lay::kControllerPawn) : 0;
    const uint32_t info = pawn != 0 ? g.P(pawn + lay::kKartCharInfo) : 0;
    const int character = info != 0 ? int(g.U8(info + lay::kCharInfoType)) : -1;
    const bool racing_ai = pawn != 0 && (g.P(pawn + lay::kKartFlags1) >> lay::kKartRacingAiBit & 1) != 0;
    const bool owned = user == OwnPad(g);
    char text[220];
    std::snprintf(text, sizeof text,
                  "slot=%u seat=%d user=%d human=%s controller=%s kart=%s character=%d racing_ai=%s role=%s", i,
                  i < g_slot_seats.size() ? g_slot_seats[i].load() : -1, user, pc != 0 ? "yes" : "no",
                  pc != 0 ? Hex(pc).c_str() : "no", pawn != 0 ? Hex(pawn).c_str() : "none", character,
                  racing_ai ? "yes" : "no", owned ? "local" : "remote_designated");
    OnChange("slot" + std::to_string(i), text);
    if (pc != 0) {
      if (owned) *owned_slot = int(controllers->size());
      controllers->push_back(pc);
    }
  }

  // Every pawn of the world.
  const uint32_t world = g.P(lay::kGWorld);
  const uint32_t level = world != 0 ? g.P(world + lay::kWorldLevel) : 0;
  const uint32_t actors = level != 0 ? g.P(level + lay::kLevelActors) : 0;
  const uint32_t info = actors != 0 ? g.P(actors) : 0;
  if (info == 0) {
    OnChange("worldinfo", "worldinfo=none");
    return;
  }
  uint32_t pawn = g.P(info + lay::kWorldPawnList);
  for (int guard = 0; pawn != 0 && guard < 16; ++guard) {
    PawnView view;
    view.pawn = pawn;
    view.controller = g.P(pawn + lay::kPawnController);
    for (size_t i = 0; i < controllers->size(); ++i) {
      if ((*controllers)[i] == view.controller && view.controller != 0) view.local_slot = int(i);
    }
    pawns->push_back(view);
    pawn = g.P(pawn + lay::kPawnNext);
  }
}

void SampleKart(const Guest& g, const PawnView& view, const char* role, double now) {
  static int lines = 0;
  if (lines >= 600) return;
  ++lines;
  const uint32_t p = view.pawn;
  char text[640];
  std::snprintf(text, sizeof text,
                "kart role=%s id=%d pos=%s rot=%s vel=%s angvel=%s steer=%s throttle=%s brake=%s boost=%s/%d/%s "
                "drift=%d/%s speed=%s lap=%d node=%s dist=%s controller=%s t=%.1f",
                role, g.I32(p + lay::kKartIdPawn), Vec(g, p + lay::kActorLocation).c_str(),
                Rot(g, p + lay::kActorRotation).c_str(), Vec(g, p + lay::kActorVelocity).c_str(),
                Vec(g, p + lay::kActorAngularVelocity).c_str(), Num(g.F32(p + lay::kKartTurnAxisInput)).c_str(),
                Num(g.F32(p + lay::kKartAccelInput)).c_str(), Num(g.F32(p + lay::kKartBrakeInput)).c_str(),
                Num(g.F32(p + lay::kKartBoostBar)).c_str(), g.I32(p + lay::kKartBoosterLevel),
                Num(g.F32(p + lay::kKartBoosterSpeed)).c_str(), g.I32(p + lay::kKartDriftDirection),
                Num(g.F32(p + lay::kKartDriftingElapsed)).c_str(), Num(g.F32(p + lay::kKartSpeed)).c_str(),
                g.I32(p + lay::kKartCompletedLaps), Hex(g.P(p + lay::kKartCurrentPathNode)).c_str(),
                Num(g.F32(g.P(p + lay::kKartCurrentPathNode) + lay::kPathNodeDistance)).c_str(),
                Hex(view.controller).c_str(), now);
  Race(text);
}

// What a kart carries after driving through an item box, watched instead of guessed: every word of the kart and of the
// character info it points at that changed since the last poll, minus the ones that always move. The offsets that only
// move when a box is taken, or when an item is used, name the item state. Every kart is watched, so the bots pick the
// boxes even when this PC's kart does not.
struct ItemWatch {
  race_sync::WordWatch kart;
  race_sync::WordWatch info;
  bool baselined = false;
};

std::map<uint32_t, ItemWatch>& ItemWatches() {
  static std::map<uint32_t, ItemWatch> watches;
  return watches;
}

void ScanItemCandidates(const Guest& g, const PawnView& view) {
  static int lines = 0;
  if (lines >= 400) return;
  // The words before `steering` are the actor's own (position, velocity, rotation): they always move.
  const uint32_t from = view.pawn + 0x400;
  const uint32_t end = view.pawn + lay::kKartSize;
  if (!g.Ok(from, 4)) return;
  const auto read = [&g](uint32_t at) { return g.P(at); };
  ItemWatch& watch = ItemWatches()[view.pawn];

  // The first sight of a kart prints the words that are already set, so a later change has something to change from.
  if (!watch.baselined) {
    watch.baselined = true;
    Race("item_base kart=" + Hex(view.pawn) + " id=" + std::to_string(g.I32(view.pawn + lay::kKartIdPawn)) +
         " nonzero=" + race_sync::DescribeNonZero(from, end, view.pawn, read));
  }

  // Room for every word of the range: physics moves dozens of floats per poll, and a short list (16) ends at the lowest
  // offsets that moved, so an item field further up the kart was never seen.
  static race_sync::WordWatch::Change changes[(lay::kKartSize - 0x400) / 4];
  bool truncated = false;
  const size_t count = watch.kart.Scan(from, end, read, changes, sizeof changes / sizeof changes[0], &truncated);
  // A held item may be an object rather than an id: a word that goes 0 -> anything or anything -> 0. Physics floats rarely
  // land on exactly zero, the driver inputs do (and are named in the layout, so they are left out).
  {
    std::vector<race_sync::WordWatch::Change> edges;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t at = changes[i].offset - view.pawn;
      if (at == lay::kKartAccelInput || at == lay::kKartBrakeInput || at == lay::kKartTurnAxisInput ||
          at == lay::kKartSteering) {
        continue;
      }
      if ((changes[i].before == 0) != (changes[i].after == 0)) edges.push_back(changes[i]);
    }
    static int edge_lines = 0;
    if (!edges.empty() && edge_lines < 3000) {
      ++edge_lines;
      Race(race_sync::DescribeChanges("item_edge kart=" + Hex(view.pawn) + " id=" + std::to_string(g.I32(view.pawn + lay::kKartIdPawn)),
                                      view.pawn, edges.data(), std::min<size_t>(edges.size(), 24), edges.size() > 24));
    }
  }
  // A pickup has a shape: a word that was zero becomes a small number (the item id). Nothing else on a kart does that,
  // which is what makes this readable where every float of the physics moves on its own.
  std::vector<race_sync::WordWatch::Change> kept;
  for (size_t i = 0; i < count; ++i) {
    if (changes[i].before != 0 || changes[i].after == 0 || changes[i].after > 64) continue;
    kept.push_back(changes[i]);
  }
  if (!kept.empty()) {
    ++lines;
    Race(race_sync::DescribeChanges("item_pick kart=" + Hex(view.pawn) + " id=" + std::to_string(g.I32(view.pawn + lay::kKartIdPawn)),
                                    view.pawn, kept.data(), kept.size(), false));
  }

  // An item may hang off the character info instead of the kart itself.
  const uint32_t info = g.P(view.pawn + lay::kKartCharInfo);
  if (info == 0 || !g.Ok(info, 4)) return;
  race_sync::WordWatch::Change info_changes[8];
  bool info_truncated = false;
  const size_t info_count = watch.info.Scan(info, info + kCharInfoSpan, read, info_changes,
                                            sizeof info_changes / sizeof info_changes[0], &info_truncated);
  std::vector<race_sync::WordWatch::Change> info_kept;
  for (size_t i = 0; i < info_count; ++i) {
    if (info_changes[i].before != 0 || info_changes[i].after == 0 || info_changes[i].after > 64) continue;
    info_kept.push_back(info_changes[i]);
  }
  if (info_kept.empty()) return;
  ++lines;
  Race(race_sync::DescribeChanges("item_pick char_info=" + Hex(info), info, info_kept.data(), info_kept.size(), false));
}

void Poll(double now) {
  const Guest g;
  std::vector<PawnView> pawns;
  std::vector<uint32_t> controllers;
  int owned_slot = -1;
  PollSlots(g, &pawns, &controllers, &owned_slot);

  const PawnView* local = nullptr;
  const PawnView* remote = nullptr;
  for (const PawnView& view : pawns) {
    if (view.local_slot >= 0 && view.local_slot == owned_slot) local = &view;
    if (view.local_slot >= 0 && view.local_slot != owned_slot && (remote == nullptr || g.I32(view.pawn + lay::kKartIdPawn) < g.I32(remote->pawn + lay::kKartIdPawn))) {
      remote = &view;
    }
  }

  // Race state and per-kart progress, changes only.
  OnChange("pawns", "pawns=" + std::to_string(pawns.size()));
  if (!pawns.empty()) {
    const uint32_t world = g.P(lay::kGWorld);
    const uint32_t actors = g.P(g.P(world + lay::kWorldLevel) + lay::kLevelActors);
    const uint32_t info = g.P(actors);
    const uint32_t game = info != 0 ? g.P(info + lay::kWorldGame) : 0;
    const unsigned state = game != 0 ? g.U8(game + lay::kMainGameRaceState) : 99;
    if (state <= 4) {
      const uint32_t first = pawns.front().pawn;
      OnChange("state", std::string("state=") + lay::RaceStateName(state) + " value=n/a clock=" +
                            Num(g.F32(first + lay::kKartRacingTime)));
    }
  }
  const int my_seat = PhaseSeat();
  for (const PawnView& view : pawns) {
    const int id = g.I32(view.pawn + lay::kKartIdPawn);
    const int slot_seat = view.local_slot >= 0 && size_t(view.local_slot) < g_slot_seats.size() ? g_slot_seats[size_t(view.local_slot)].load() : -1;
    const std::string who = std::string("kart=") + std::to_string(id) +
                            (view.local_slot >= 0 ? " seat=" + std::to_string(slot_seat >= 0 ? slot_seat : my_seat) : "");
    const uint32_t node = g.P(view.pawn + lay::kKartCurrentPathNode);
    const std::string progress = who + " lap=" + std::to_string(g.I32(view.pawn + lay::kKartCompletedLaps)) +
                                 " checkpoint=" + Hex(node) + " place=" +
                                 std::to_string(g.I32(view.pawn + lay::kKartRacePosition));
    OnChange("progress" + std::to_string(view.pawn), progress);
    const int finish = g.I32(view.pawn + lay::kKartRaceFinishPosition);
    if (finish > 0) OnChange("finish" + std::to_string(view.pawn), who + " finish_position=" + std::to_string(finish));
  }
  if (local != nullptr) SampleKart(g, *local, "local", now);
  if (remote != nullptr) SampleKart(g, *remote, "remote_designated", now);
  static const bool item_probe = online_guest::EnvOn(std::getenv(kItemProbeEnv));
  // Every kart, not only this PC's: a bot taking a box names the same fields.
  if (item_probe) {
    for (const PawnView& view : pawns) ScanItemCandidates(g, view);
  }
}

}  // namespace

// ---- Phase and counters ----------------------------------------------------------------------------------------------

void NoteUe3Api(Ue3Api api) {
  if (size_t(api) < size_t(Ue3Api::kCount)) g_ue3[size_t(api)].fetch_add(1, std::memory_order_relaxed);
}

void SetOnlinePhase(int connection, int room_state, int seat) {
  g_phase.store(uint32_t(connection & 0xFF) | uint32_t(room_state & 0xFF) << 8 | uint32_t((seat + 1) & 0xFF) << 16);
}

uint32_t OnlinePhasePacked() { return g_phase.load(); }

std::string PhaseText() {
  const uint32_t packed = g_phase.load();
  if (packed == 0xFFFFFFFFu) return "unknown";
  return "conn:" + std::to_string(packed & 0xFF) + "/room:" + std::to_string(packed >> 8 & 0xFF);
}

int PhaseSeat() {
  const uint32_t packed = g_phase.load();
  return packed == 0xFFFFFFFFu ? -1 : int(packed >> 16 & 0xFF) - 1;
}

std::string XboxContext() {
  return "t_ms=" + std::to_string(uint64_t(OnlineNowSeconds() * 1000.0)) + " phase=" + PhaseText() +
         " seat=" + std::to_string(PhaseSeat());
}

namespace {

void PrintUe3Summary(std::string_view reason) {
  std::array<uint64_t, size_t(Ue3Api::kCount)> counts{};
  for (size_t i = 0; i < counts.size(); ++i) counts[i] = g_ue3[i].load();
  REXLOG_INFO("{} {} {}", kXboxTag, XboxContext(), Ue3NetSummaryLine(counts, reason));
}

}  // namespace

// ---- Menu API ----------------------------------------------------------------------------------------------------------

void OnOnlineRaceStartRequested(const RaceStartPlan& plan) {
  for (const std::string& line : DryRunLines(PlanDryRun(plan))) Race(line);
  PrintUe3Summary("race_start_requested");
}

void OnOnlineRaceStartBlocked(std::string_view reason) {
  Race(BlockedLine(reason));
  PrintUe3Summary("race_start_blocked");
}

// ---- Hook bodies ----------------------------------------------------------------------------------------------------------

void OnConsoleCommand(const uint8_t* base, uint32_t fstring, uint32_t self, uint32_t caller) {
  const std::string text = online_guest::ConsoleCommandText(base, fstring);
  if (text.empty()) return;
  const std::optional<race_url::RaceUrl> url = race_url::ParseRaceUrl(text);
  if (!url) return;

  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 32) return;
  Race("url=" + text);
  Race("url_origin caller=" + Hex(caller) + " this=" + Hex(self) + " phase=" + PhaseText() +
       " seat=" + std::to_string(PhaseSeat()));
  for (const std::string& field : race_url::DescribeRaceUrl(*url)) Race("url." + field);
  for (const std::string& problem : race_url::RaceUrlProblems(*url)) Race("url.problem " + problem);
  g_url_humans.store(url->num_human_karts.value_or(-1));
  g_url_total.store(url->total_karts.value_or(-1));
  if (PhaseSeat() >= 0) PrintUe3Summary("race_url_while_in_room");
}

void OnSeed(uint32_t seed, uint32_t caller) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 40) return;
  Race("seed=" + Hex(seed) + " srand_caller=" + Hex(caller));
}

void SetRaceSeats(const std::array<int, 4>& seats) {
  for (size_t slot = 0; slot < seats.size(); ++slot) g_slot_seats[slot].store(seats[slot]);
}

void PumpGuestProbes(double now_seconds) {
  // The item probe is its own switch: it needs the kart discovery of Poll, but not the debug lines that come with it.
  static const bool item = online_guest::EnvOn(std::getenv(kItemProbeEnv));
  if (!DebugOn() && !item) return;
  static double next = 0;
  if (now_seconds < next) return;
  next = now_seconds + 0.5;
  Poll(now_seconds);
}

}  // namespace eck::online_probe
