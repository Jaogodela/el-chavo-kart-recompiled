#include "online_race_sync_guest.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"
#include "online_guest_reader.h"
#include "online_kart_write.h"
#include "online_probe_layout.h"
#include "online_runtime.h"

// UWorld::FarMoveActor(this, actor, const FVector*, test, noCheck, attachedMove): what SetLocation runs, and how UE3 places a
// replicated actor. noCheck leaves the host's position alone: this PC has no say in where a kart may stand.
REX_EXTERN(sub_825D4E10);

namespace eck {
namespace {

namespace layout = online_probe::layout;
using online_guest::Guest;

constexpr uint32_t kMoveFrame = 0x100;
constexpr uint32_t kMoveVector = 0x60;
constexpr double kSummarySeconds = 1.0;

std::atomic<bool> g_hold_guest_results{false};

void Copy3(const Guest& g, uint32_t address, float (&out)[3]) {
  for (uint32_t i = 0; i < 3; ++i) out[i] = g.F32(address + 4 * i);
}

void StoreFloat(uint8_t* base, uint32_t address, float value) { StoreBE32(base, address, std::bit_cast<uint32_t>(value)); }

bool Finite(const race::KartSample& kart) {
  bool ok = std::isfinite(kart.speed) && std::isfinite(kart.distance) && std::isfinite(kart.racing_time);
  for (const float v : kart.position) ok = ok && std::isfinite(v);
  for (const float v : kart.velocity) ok = ok && std::isfinite(v);
  for (const float v : kart.quat) ok = ok && std::isfinite(v);
  return ok;
}

// The game's counter, as the byte the datagram carries. A field that does not hold what this layout expects is clamped,
// never trusted: the host decides the race, and one wrong offset must not put a kart on lap 3 billion.
uint8_t Counter(const Guest& g, uint32_t address) {
  const int32_t value = g.I32(address);
  return uint8_t(std::clamp(value, 0, 255));
}

// Reads the race the kart is having, as the host's snapshot carries it onward.
void ReadProgress(const Guest& g, uint32_t pawn, race::KartSample& kart) {
  kart.lap = Counter(g, pawn + layout::kKartCompletedLaps);
  kart.distance = g.F32(pawn + layout::kKartDistanceInRace);
  kart.place = Counter(g, pawn + layout::kKartRacePosition);
  kart.finish_place = Counter(g, pawn + layout::kKartRaceFinishPosition);
  kart.racing_time = g.F32(pawn + layout::kKartRacingTime);
  kart.item = uint8_t(std::min<int32_t>(std::max<int32_t>(g.I32(pawn + layout::kKartHeldItem), 0), race::kMaxItemId));
  kart.finished = (g.P(pawn + layout::kKartFlags2) >> layout::kKartRaceFinishedBit) & 1;
}

using Pawns = std::array<uint32_t, race::kMaxKarts>;
using WorldAddrs = std::array<uint32_t, race::kMaxWorldPawns>;

bool KnownKart(const Pawns& karts, int total, uint32_t pawn) {
  for (int slot = 0; slot < total; ++slot) {
    if (karts[size_t(slot)] == pawn) return true;
  }
  return false;
}

// Kart slots are the URL's on every PC, so a pawn says which it is. Nothing here until the race's karts are all up.
bool ReadKarts(const Guest& g, int total, Pawns& pawns, race_sync::KartFrames& karts) {
  const uint32_t world = g.P(layout::kGWorld);
  const uint32_t level = world ? g.P(world + layout::kWorldLevel) : 0;
  const uint32_t actors = level ? g.P(level + layout::kLevelActors) : 0;
  const uint32_t info = actors ? g.P(actors) : 0;
  uint32_t pawn = info ? g.P(info + layout::kWorldPawnList) : 0;
  int found = 0;
  for (int guard = 0; pawn != 0 && guard < 32; ++guard) {
    if (!g.Ok(pawn, layout::kKartSize)) break;
    const int slot = g.I32(pawn + layout::kKartIdPawn);
    const bool ready = (g.P(pawn + layout::kKartFlags1) >> layout::kKartInitializedBit) & 1;
    if (ready && slot >= 0 && slot < total && !karts[size_t(slot)]) {
      race::KartSample kart;
      kart.slot = uint8_t(slot);
      Copy3(g, pawn + layout::kActorLocation, kart.position);
      for (uint32_t i = 0; i < 4; ++i) kart.quat[i] = g.F32(pawn + layout::kKartMovementQuat + 4 * i);
      Copy3(g, pawn + layout::kActorVelocity, kart.velocity);
      kart.speed = g.F32(pawn + layout::kKartSpeed);
      ReadProgress(g, pawn, kart);
      if (!Finite(kart)) break;
      karts[size_t(slot)] = kart;
      pawns[size_t(slot)] = pawn;
      ++found;
    }
    pawn = g.P(pawn + layout::kPawnNext);
  }
  return found == total;
}

void MoveKart(PPCContext& ctx, uint8_t* base, uint32_t pawn, const float (&position)[3]) {
  const uint32_t saved_sp = ctx.r1.u32;
  const uint32_t frame = saved_sp - kMoveFrame;
  ctx.r1.u32 = frame;
  for (uint32_t i = 0; i < 3; ++i) StoreFloat(base, frame + kMoveVector + 4 * i, position[i]);
  ctx.r3.u32 = LoadBE32(base, layout::kGWorld);
  ctx.r4.u32 = pawn;
  ctx.r5.u32 = frame + kMoveVector;
  ctx.r6.u32 = 0;
  ctx.r7.u32 = 1;
  ctx.r8.u32 = 0;
  sub_825D4E10(ctx, base);
  ctx.r1.u32 = saved_sp;
}

// Walkers stay on the track. The host's list order is the id every PC uses, so a bump is against the same body.
void ReadWorldPawns(const Guest& g, const Pawns& karts, int total, std::vector<race::WorldPawnSample>& out,
                    WorldAddrs& addrs) {
  const uint32_t world = g.P(layout::kGWorld);
  const uint32_t level = world ? g.P(world + layout::kWorldLevel) : 0;
  const uint32_t actors = level ? g.P(level + layout::kLevelActors) : 0;
  const uint32_t info = actors ? g.P(actors) : 0;
  uint32_t pawn = info ? g.P(info + layout::kWorldPawnList) : 0;
  int id = 0;
  for (int guard = 0; pawn != 0 && guard < 64 && id < race::kMaxWorldPawns; ++guard) {
    const uint32_t next = g.Ok(pawn, layout::kPawnNext + 4) ? g.P(pawn + layout::kPawnNext) : 0;
    if (!KnownKart(karts, total, pawn) && g.Ok(pawn, layout::kActorLocation + 12)) {
      race::WorldPawnSample sample;
      sample.id = uint8_t(id);
      Copy3(g, pawn + layout::kActorLocation, sample.position);
      if (g.Ok(pawn, layout::kActorVelocity + 12)) Copy3(g, pawn + layout::kActorVelocity, sample.velocity);
      sample.yaw = uint16_t(g.I32(pawn + layout::kActorRotation + 4));
      bool finite = true;
      for (float v : sample.position) finite = finite && std::isfinite(v);
      for (float v : sample.velocity) finite = finite && std::isfinite(v);
      if (finite) {
        out.push_back(sample);
        addrs[size_t(id)] = pawn;
        ++id;
      }
    }
    pawn = next;
  }
}

void ApplyWorldPawn(PPCContext& ctx, uint8_t* base, uint32_t pawn, const race::WorldPawnSample& sample, bool guest) {
  MoveKart(ctx, base, pawn, sample.position);
  for (uint32_t i = 0; i < 3; ++i) StoreFloat(base, pawn + layout::kActorVelocity + 4 * i, sample.velocity[i]);
  StoreBE32(base, pawn + layout::kActorRotation + 4, sample.yaw);
  // Guests do not walk them locally: the next tick would put a body where the host does not have one. Collision stays.
  if (guest) base[pawn + layout::kActorPhysics] = 0;
}

// Heading and speed go in as well: the game turns them into the kart's mesh, wheels and engine sound on its next tick.
void ApplyWrite(PPCContext& ctx, uint8_t* base, uint32_t pawn, const race_sync::KartWrite& write) {
  // The host marking a guest's reported finish on its copy: the kart stays where the host's game has it.
  if (write.progress_only) {
    race_sync::WriteProgress(base, pawn, write.state);
    return;
  }
  if (write.item_only) {
    race_sync::WriteHeldItem(base, pawn, write.state.item);
    return;
  }
  MoveKart(ctx, base, pawn, write.state.position);
  if (write.position_only) return;
  if (race_sync::HasHeading(write.state)) {
    for (uint32_t i = 0; i < 4; ++i) StoreFloat(base, pawn + layout::kKartMovementQuat + 4 * i, write.state.quat[i]);
  }
  for (uint32_t i = 0; i < 3; ++i) StoreFloat(base, pawn + layout::kActorVelocity + 4 * i, write.state.velocity[i]);
  StoreFloat(base, pawn + layout::kKartSpeed, write.state.speed);
  // Laps, place, distance and the finish flag: the host owns the race, so this PC never counts them on a remote kart.
  race_sync::WriteProgress(base, pawn, write.state);
}

race_sync::SyncConfig ConfigOf(const lobby::CanonicalRace& race, int seat, int own_pad, int start_in_ms) {
  race_sync::SyncConfig config;
  config.start_in_ms = start_in_ms;
  config.generation = uint32_t(race.generation);
  config.seat = seat;
  config.humans = race.humans;
  config.total = race.total;
  config.seats = race.seats;
  for (int slot = 0; slot < race.humans; ++slot) {
    const int who = race.seats[size_t(slot)];
    config.pad_of_seat[size_t(who)] = lobby::PadOfSeat(race, who, seat, own_pad);
  }
  return config;
}

// Proof of traffic, once per second.
void LogSummary(const race_sync::RaceSync& sync, const race_sync::PeerRoutes& routes, int race_state, double now) {
  const race_sync::Traffic& net = sync.traffic();
  REXLOG_INFO("[ONLINE-NET] gameplay tx_packets={} rx_packets={} tx_bytes={} rx_bytes={} dropped_old={} rejected={} "
              "own_snaps={} own_items={} remote_jumps={} remote_teleports={} finish_refused={} race_state={} begun={}",
              net.tx_packets, net.rx_packets, net.tx_bytes, net.rx_bytes, net.dropped_old, net.rejected, net.own_snaps,
              net.own_item_writes,
              net.remote_jumps, net.remote_teleports, net.finish_refused, race_state, sync.begun() ? "yes" : "no");
  for (const race_sync::PeerRoute& route : routes) {
    if (route.seat < 0) continue;
    REXLOG_INFO("[ONLINE-NET] peer seat={} transport={} last_input_seq={} last_snapshot_seq={} age_ms={} rtt_ms={}",
                route.seat, online::RelaySeatOf(route.endpoint) >= 0 ? "relay" : "direct", sync.last_input_seq(route.seat),
                sync.last_snapshot_seq(), int(sync.LastHeardAge(route.seat, now) * 1000.0),
                sync.RttMs(route.seat, now).value_or(-1));
  }
}

struct Running {
  explicit Running(const race_sync::SyncConfig& config, const race_sync::PeerRoutes& routes)
      : sync(config), link(GameSocket(), routes), started(OnlineNowSeconds()) {}
  race_sync::RaceSync sync;
  race_sync::SocketLink link;
  double started;
  int own_pad = 0;
  double last_tick = 0;
  double next_summary = 0;
  bool held_results = false;
  bool said_world = false;
};

}  // namespace

bool TickRaceSync(PPCContext& ctx, uint8_t* base, const lobby::Plan& plan, std::optional<int> own_pad, lobby::Bridge& bridge) {
  static std::unique_ptr<Running> running;
  if (!plan.in_race || !plan.race || plan.local_seat < 0) {
    if (running) {
      running.reset();
      bridge.SetRaceStatus({});
      REXLOG_INFO("[ONLINE-RACE] gameplay stopped");
    }
    g_hold_guest_results.store(false);
    return false;
  }
  // What the game thread was given, once per generation: a start that never fires is either not in the plan or not in the
  // sync, and this says which.
  static int said_generation = 0;
  if (said_generation != plan.race->generation) {
    said_generation = plan.race->generation;
    REXLOG_INFO("[ONLINE-RACE] plan in_race={} seat={} start_in={} now={:.3f}", plan.in_race, plan.local_seat,
                plan.start_in_ms, OnlineNowSeconds());
  }

  // The game is gone for the length of the map load. The session is not, so the streams carry on from where they were.
  if (!running && !own_pad) return false;
  if (!running || running->sync.config().generation != uint32_t(plan.race->generation)) {
    running = std::make_unique<Running>(ConfigOf(*plan.race, plan.local_seat, *own_pad, plan.start_in_ms), plan.routes);
    running->own_pad = *own_pad;
    REXLOG_INFO("[ONLINE-RACE] gameplay started generation={} seat={} own_pad={}", plan.race->generation, plan.local_seat,
                *own_pad);
  }
  running->link.SetRoutes(plan.routes);
  running->sync.SetStartIn(plan.start_in_ms);

  const double now = OnlineNowSeconds();
  const Guest g;
  race_sync::FrameIn in;
  in.now = now;
  in.dt = running->last_tick > 0 ? now - running->last_tick : 0;
  running->last_tick = now;
  in.own_pad = bridge.own_pad();
  Pawns pawns{};
  WorldAddrs world_addrs{};
  if (ReadKarts(g, plan.race->total, pawns, in.karts)) {
    const uint32_t world = g.P(layout::kGWorld);
    const uint32_t info = g.P(g.P(g.P(world + layout::kWorldLevel) + layout::kLevelActors));
    const uint32_t game = info ? g.P(info + layout::kWorldGame) : 0;
    if (game) in.race_state = g.U8(game + layout::kMainGameRaceState);
    ReadWorldPawns(g, pawns, plan.race->total, in.world_pawns, world_addrs);
  }

  const race_sync::FrameOut out = running->sync.Tick(in, running->link);
  for (int i = 0; i < out.write_count; ++i) {
    const race_sync::KartWrite& write = out.writes[size_t(i)];
    if (pawns[size_t(write.slot)]) ApplyWrite(ctx, base, pawns[size_t(write.slot)], write);
  }
  const bool guest = plan.local_seat > 0;
  if (guest) {
    for (int i = 0; i < out.world_write_count; ++i) {
      const race::WorldPawnSample& sample = out.world_writes[size_t(i)];
      const uint32_t pawn = sample.id < race::kMaxWorldPawns ? world_addrs[sample.id] : 0;
      if (pawn) ApplyWorldPawn(ctx, base, pawn, sample, true);
    }
    if (!running->said_world && out.world_write_count > 0) {
      running->said_world = true;
      REXLOG_INFO("[ONLINE-RACE] world pawns from host applied={}", out.world_write_count);
    }
  } else if (!running->said_world && !in.world_pawns.empty()) {
    running->said_world = true;
    REXLOG_INFO("[ONLINE-RACE] world pawns host_count={}", in.world_pawns.size());
  }
  const uint32_t world = g.P(layout::kGWorld);
  const uint32_t info = world ? g.P(g.P(g.P(world + layout::kWorldLevel) + layout::kLevelActors)) : 0;
  const uint32_t game = info ? g.P(info + layout::kWorldGame) : 0;
  const bool hold = race_sync::ShouldHoldGuestResults(guest, out.post_race, in.race_state,
                                                     running->held_results || out.own_finished_by_host);
  if (hold != running->held_results) {
    running->held_results = hold;
    REXLOG_INFO("[ONLINE-RACE] guest results {} held_for={:.1f}s", hold ? "held" : "released", now - running->started);
  }
  g_hold_guest_results.store(hold);
  if (game && g.Ok(game + layout::kMainGameRaceState, 1)) {
    if (hold) {
      base[game + layout::kMainGameRaceState] = uint8_t(race_sync::kRaceInProgress);
    } else if (guest && out.post_race) {
      base[game + layout::kMainGameRaceState] = uint8_t(race_sync::kRacePostRace);
    }
  }
  bridge.SetRules(lobby::RacePadRules(running->own_pad, out, guest, hold ? race_sync::kRaceInProgress : in.race_state,
                                      plan.results_open));

  lobby::Bridge::RaceStatus status;
  status.waiting_for_host = out.waiting_for_host;
  for (int seat = 0; seat < lobby::kPanels; ++seat) status.rtt_ms[size_t(seat)] = running->sync.RttMs(seat, now);
  if (guest) {
    const double heard = running->sync.LastHeardAge(0, now);
    status.host_silence = heard >= 0 ? heard : now - running->started;
  }
  bridge.SetRaceStatus(status);

  for (const std::string& note : running->sync.TakeNotes()) REXLOG_INFO("[ONLINE-RACE] {}", note);
  if (now >= running->next_summary) {
    running->next_summary = now + kSummarySeconds;
    LogSummary(running->sync, plan.routes, in.race_state, now);
  }
  return true;
}

bool GuestResultsHeld() { return g_hold_guest_results.load(std::memory_order_relaxed); }

}  // namespace eck
