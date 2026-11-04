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

#include "guest_memory.h"
#include "online_game_guest.h"
#include "online_probe_layout.h"
#include "online_start_race.h"
#include "online_race_url.h"
#include "online_runtime.h"

// DEBUG_SAULO: everything in this file is diagnostics for the online race work and is removed with it.

namespace eck::online_probe {

namespace {

namespace lay = layout;

constexpr uint32_t kGuestSpan = 0x90000000;

std::atomic<uint64_t> g_ue3[size_t(Ue3Api::kCount)];
std::atomic<uint32_t> g_phase{0xFFFFFFFFu};  // connection | room state << 8 | (seat + 1) << 16
std::atomic<int> g_url_humans{-1};
std::atomic<int> g_url_total{-1};

bool DebugOn() {
  static const bool on = online_guest::EnvOn(std::getenv(online_guest::kDebugEnv));
  return on;
}

void Race(const std::string& text) { REXLOG_INFO("{} {}", kRaceTag, text); }

// ---- Safe guest reads (poller runs outside the hooks) ----------------------------------------------------------------

bool Readable(uint32_t address, uint32_t length) {
  if (address < 0x10000 || address + length < address || address + length > kGuestSpan) return false;
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (heap == nullptr || !heap->QueryRegionInfo(address, &info)) return false;
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
         (info.protect & rex::memory::kMemoryProtectRead) != 0 &&
         uint64_t(address) + length <= uint64_t(info.base_address) + info.region_size;
}

struct Guest {
  bool Ok(uint32_t address, uint32_t length) const { return Readable(address, length); }
  std::optional<uint32_t> U32(uint32_t address) const {
    if (!Readable(address, 4)) return std::nullopt;
    return LoadBE32(rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(address), 0);
  }
  uint32_t P(uint32_t address) const { return U32(address).value_or(0); }  // pointer, 0 when unreadable
  int32_t I32(uint32_t address) const { return int32_t(U32(address).value_or(0)); }
  float F32(uint32_t address) const {
    const uint32_t bits = U32(address).value_or(0);
    float f;
    static_assert(sizeof f == sizeof bits);
    std::memcpy(&f, &bits, sizeof f);
    return f;
  }
  uint8_t U8(uint32_t address) const {
    return Readable(address, 1) ? rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(address)[0] : 0;
  }
};

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

void PollSlots(const Guest& g, std::vector<PawnView>* pawns, std::vector<uint32_t>* controllers) {
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
    char text[200];
    std::snprintf(text, sizeof text,
                  "slot=%u user=%d human=%s controller=%s kart=%s character=%d racing_ai=%s role=local", i, user,
                  pc != 0 ? "yes" : "no", pc != 0 ? Hex(pc).c_str() : "no", pawn != 0 ? Hex(pawn).c_str() : "none",
                  character, racing_ai ? "yes" : "no");
    OnChange("slot" + std::to_string(i), text);
    if (pc != 0) controllers->push_back(pc);
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

void Poll(double now) {
  const Guest g;
  std::vector<PawnView> pawns;
  std::vector<uint32_t> controllers;
  PollSlots(g, &pawns, &controllers);

  const PawnView* local = nullptr;
  const PawnView* remote = nullptr;
  for (const PawnView& view : pawns) {
    if (view.local_slot == 0) local = &view;
    if (view.local_slot < 0 && (remote == nullptr || g.I32(view.pawn + lay::kKartIdPawn) < g.I32(remote->pawn + lay::kKartIdPawn))) {
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
    const std::string who = std::string("kart=") + std::to_string(id) +
                            (view.local_slot >= 0 ? " seat=" + std::to_string(my_seat < 0 ? view.local_slot : my_seat) : "");
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
  if (base == nullptr || fstring < 0x10000 || fstring + 12 > kGuestSpan) return;
  const uint32_t data = LoadBE32(base, fstring);
  const uint32_t num = LoadBE32(base, fstring + 4);
  if (data < 0x10000 || num < 12 || num > 4096 || uint64_t(data) + 2ull * num > kGuestSpan) return;
  const uint16_t first = LoadBE<uint16_t>(base, data);
  if (first != 'o' && first != 'O' && first != 's' && first != 'S') return;  // every other console command ends here
  std::string text;
  text.reserve(num);
  for (uint32_t i = 0; i < num; ++i) {
    const uint16_t c = LoadBE<uint16_t>(base, data + 2 * i);
    if (c == 0) break;
    text.push_back(c < 0x80 ? char(c) : '?');
  }
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

namespace {

// Routes the room menu's Start Race through the probes so the dry-run and the UE3 summary come from one place.
void InstallStartRaceSeam() {
  online_start::SetStartRaceHook([](const online_start::Plan& plan) -> std::optional<online_start::Verdict> {
    if (!online_start::DebugEnabled()) return std::nullopt;
    RaceStartPlan probe_plan;
    probe_plan.host_seat = 0;
    probe_plan.local_seat = plan.local_seat;
    probe_plan.track = plan.config.track == online_start::kUnknownTrack ? 0 : plan.config.track;
    probe_plan.laps = plan.config.laps;
    probe_plan.total_karts = plan.config.total_karts;
    probe_plan.participants = plan.config.participants;
    OnOnlineRaceStartRequested(probe_plan);
    online_start::Verdict verdict;
    verdict.dry_run = true;
    verdict.reason = online_start::kUnprovenReason;
    return verdict;
  });
}

}  // namespace

void PumpGuestProbes(double now_seconds) {
  static const bool seam_installed = (InstallStartRaceSeam(), true);
  (void)seam_installed;
  if (!DebugOn()) return;
  static double next = 0;
  if (now_seconds < next) return;
  next = now_seconds + 0.5;
  Poll(now_seconds);
}

}  // namespace eck::online_probe
