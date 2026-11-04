#include "online_game_hook.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"
#include "online/peer_table.h"
#include "online_game_guest.h"
#include "online_probe.h"
#include "online_race.h"
#include "online_runtime.h"
#include "settings.h"

namespace {

using namespace eck::online_guest;

constexpr size_t kGuestSpan = 0x90000000;

enum Group { kGroupIdentity, kGroupNet, kGroupSession, kGroupLive, kGroupEngine, kGroupSocket, kGroupRace, kGroupCount };

std::span<const CodeFingerprint> TableOf(Group group) {
  switch (group) {
    case kGroupIdentity: return kIdentityFingerprints;
    case kGroupNet: return kNetFingerprints;
    case kGroupSession: return kSessionFingerprints;
    case kGroupLive: return kLiveFingerprints;
    case kGroupSocket: return kSocketFingerprints;
    case kGroupRace: return kRaceFingerprints;
    default: return kEngineFingerprints;
  }
}

bool EnvFlag(const char* name) { return EnvOn(std::getenv(name)); }

// DEBUG_SAULO: one line per state transition, tagged for removal.
void Log(const std::string& key, uint64_t value, const std::string& text) {
  static std::mutex mutex;
  static TransitionGate gate;
  std::lock_guard lock(mutex);
  if (gate.ShouldLog(key, value, eck::OnlineNowSeconds())) {
    REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] {} calls={}", text, gate.calls(key));
  }
}

// DEBUG_SAULO: the passive session/XNet/socket trace. Socket traffic has its own, smaller budget.
void LogXbox(bool traffic, const std::string& key, uint64_t value, const char* api, const std::string& args,
             uint32_t result) {
  static std::mutex mutex;
  static TransitionGate calls_gate;
  static TransitionGate traffic_gate(0.5, 80);
  std::lock_guard lock(mutex);
  TransitionGate& gate = traffic ? traffic_gate : calls_gate;
  if (!gate.ShouldLog(key, value, eck::OnlineNowSeconds())) return;
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-XBOX] api={} {} args={} result=0x{:08X} calls={}", api,
              eck::online_probe::XboxContext(), args, result, gate.calls(key));
}

// Verified once per group against the image the game is running; a mismatch makes every hook of the group a pass-through.
bool GroupOk(Group group, const uint8_t* base) {
  static std::atomic<int> state[kGroupCount];
  int seen = state[group].load();
  if (seen == 0) {
    const char* bad = FirstMismatch(TableOf(group), base, kGuestSpan, 0);
    seen = bad == nullptr ? 1 : 2;
    state[group] = seen;
    REXLOG_INFO("[DEBUG_SAULO][ONLINE-XBOX] fingerprint group={} {}{}", int(group), seen == 1 ? "ok" : "MISMATCH at ",
                bad == nullptr ? "" : bad);
  }
  return seen == 1;
}

struct LiveView {
  bool active = false;
  uint64_t xuid = 0;
  std::string name;
};

// The title-local profile as the online profile, only when the developer opted in and the player is signed in.
LiveView CurrentLive() {
  static const bool opted_in = EnvFlag(kLiveProfileEnv);
  if (!opted_in) return {};
  const bool enabled = REXCVAR_GET(eck_online_enabled);
  const eck::online::OnlineProfile profile = enabled ? eck::Online().Profile() : eck::online::OnlineProfile{};
  LiveView view;
  view.active = ReportLiveProfile(opted_in, enabled, profile.signed_in) && !profile.friend_code.empty();
  if (view.active) {
    view.xuid = VirtualXuid(profile.friend_code);
    view.name = GuestName(profile.name.empty() ? profile.friend_code : profile.name);
  }
  Log("live_profile", view.active ? 1 : 0,
      std::string("live profile ") + (view.active ? "reported code=" + profile.friend_code : "off"));
  return view;
}

// Calls the original and logs the call: caller, the first arguments and the result.
void Trace(Group group, const char* label, uint32_t message, eck::online_probe::Ue3Api api, PPCContext& ctx,
           uint8_t* base, PPCFunc* original) {
  if (!GroupOk(group, base)) {
    original(ctx, base);
    return;
  }
  if (api != eck::online_probe::Ue3Api::kCount) eck::online_probe::NoteUe3Api(api);
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t args[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  original(ctx, base);
  const uint32_t result = ctx.r3.u32;
  char text[160];
  std::snprintf(text, sizeof text, "msg=0x%08X lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X", message, lr, args[0],
                args[1], args[2], args[3]);
  LogXbox(false, label, (uint64_t(result) << 32) | lr, label, text, result);
}

// socket / sendto / recvfrom of the game network: sizes, flags and the destination port only, never addresses or payload.
void TraceSocket(const char* label, eck::online_probe::Ue3Api api, PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (!GroupOk(kGroupSocket, base)) {
    original(ctx, base);
    return;
  }
  eck::online_probe::NoteUe3Api(api);
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t a0 = ctx.r3.u32, a1 = ctx.r4.u32, a2 = ctx.r5.u32, a3 = ctx.r6.u32, a4 = ctx.r7.u32;
  original(ctx, base);
  const uint32_t result = ctx.r3.u32;
  uint32_t port = 0;
  if (api != eck::online_probe::Ue3Api::kSocket && a4 >= 0x10000 && a4 + 4 <= kGuestSpan) {
    port = (uint32_t(base[a4 + 2]) << 8) | base[a4 + 3];
  }
  char text[160];
  if (api == eck::online_probe::Ue3Api::kSocket) {
    std::snprintf(text, sizeof text, "af=%u type=%u proto=%u lr=0x%08X", a0, a1, a2, lr);
  } else {
    std::snprintf(text, sizeof text, "sock=0x%X len=%u flags=0x%X port=%u lr=0x%08X", a0, a2, a3, port, lr);
  }
  const bool failed = result >= 0xFFFF0000u;
  LogXbox(true, label, (uint64_t(failed) << 32) | lr, label, text, result);
  (void)a1;
}

}  // namespace

using eck::online_probe::Ue3Api;

#define ECK_TRACE_HOOK(addr, group, label, message, api) \
  REX_EXTERN(__imp__sub_##addr); \
  REX_HOOK_RAW(sub_##addr) { Trace(group, label, message, api, ctx, base, __imp__sub_##addr); }

// XGI session wrappers.
ECK_TRACE_HOOK(82B76128, kGroupSession, "XSessionCreate", kMsgSessionCreate, Ue3Api::kXSessionCreate)
ECK_TRACE_HOOK(82B76398, kGroupSession, "XSessionDelete", kMsgSessionDelete, Ue3Api::kCount)
ECK_TRACE_HOOK(82B76448, kGroupSession, "XSessionJoinLocal", kMsgSessionJoin, Ue3Api::kCount)
ECK_TRACE_HOOK(82B764F8, kGroupSession, "XSessionJoinRemote", kMsgSessionJoin, Ue3Api::kXSessionJoinRemote)
ECK_TRACE_HOOK(82B765A8, kGroupSession, "XSessionLeaveLocal", kMsgSessionLeave, Ue3Api::kCount)
ECK_TRACE_HOOK(82B76650, kGroupSession, "XSessionLeaveRemote", kMsgSessionLeave, Ue3Api::kCount)
ECK_TRACE_HOOK(82B767D8, kGroupSession, "XSessionStart", kMsgSessionStart, Ue3Api::kCount)
ECK_TRACE_HOOK(82B76878, kGroupSession, "XSessionEnd", kMsgSessionEnd, Ue3Api::kCount)
ECK_TRACE_HOOK(82B76928, kGroupSession, "XSessionSearch", kMsgSessionSearch, Ue3Api::kCount)

// XNet wrappers.
ECK_TRACE_HOOK(82B74D10, kGroupNet, "XNetGetTitleXnAddr", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(82B74BF0, kGroupNet, "XNetXnAddrToInAddr", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(82B74C80, kGroupNet, "XNetQosListen", 0, Ue3Api::kCount)

// UOnlineSubsystemLive virtuals.
ECK_TRACE_HOOK(82A0E268, kGroupLive, "Live.CreateOnlineGame", 0, Ue3Api::kLiveCreateOnlineGame)
ECK_TRACE_HOOK(82A0F5F0, kGroupLive, "Live.FindOnlineGames", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(82A10660, kGroupLive, "Live.JoinOnlineGame", 0, Ue3Api::kLiveJoinOnlineGame)
ECK_TRACE_HOOK(82A158B0, kGroupLive, "Live.StartOnlineGame", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(82A15B08, kGroupLive, "Live.EndOnlineGame", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(82A151D0, kGroupLive, "Live.ShowInviteUI", 0, Ue3Api::kCount)

// Engine natives.
ECK_TRACE_HOOK(8258E2F8, kGroupEngine, "CreateNamedNetDriver", 0, Ue3Api::kCreateNamedNetDriver)
ECK_TRACE_HOOK(82574920, kGroupEngine, "ClientTravel", 0, Ue3Api::kClientTravel)

// NetDll wrappers of the game network (trace only).
REX_EXTERN(__imp__sub_82B74930);
REX_HOOK_RAW(sub_82B74930) { TraceSocket("NetDll_socket", Ue3Api::kSocket, ctx, base, __imp__sub_82B74930); }
REX_EXTERN(__imp__sub_82B74AA0);
REX_HOOK_RAW(sub_82B74AA0) { TraceSocket("NetDll_sendto", Ue3Api::kSendTo, ctx, base, __imp__sub_82B74AA0); }
REX_EXTERN(__imp__sub_82B74A68);
REX_HOOK_RAW(sub_82B74A68) { TraceSocket("NetDll_recvfrom", Ue3Api::kRecvFrom, ctx, base, __imp__sub_82B74A68); }

// AActor::ConsoleCommand(FString* result, this, const FString* command, bWriteToLog): the string is already evaluated here,
// so reading it consumes nothing. Only race start URLs are logged.
REX_EXTERN(__imp__sub_826A1F30);
REX_HOOK_RAW(sub_826A1F30) {
  if (GroupOk(kGroupRace, base)) {
    eck::online_probe::OnConsoleCommand(base, ctx.r5.u32, ctx.r4.u32, uint32_t(ctx.lr));
  }
  __imp__sub_826A1F30(ctx, base);
}

// CRT srand(seed).
REX_EXTERN(__imp__sub_830B53B0);
REX_HOOK_RAW(sub_830B53B0) {
  if (GroupOk(kGroupRace, base)) eck::online_probe::OnSeed(ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_830B53B0(ctx, base);
}

// Title-local profile. User 0 only: pads 1 to 3 stay local guests.
// DEBUG_SAULO: sign-in queries, state changes per caller only.
void TraceSignin(const char* api, uint32_t lr, uint32_t user, uint32_t result, bool live_reported) {
  char text[96];
  std::snprintf(text, sizeof text, "user=%u lr=0x%08X live_reported=%s", user, lr, live_reported ? "yes" : "no");
  LogXbox(false, api, (uint64_t(result) << 32) | (uint64_t(live_reported) << 31) | lr, api, text, result);
}

REX_EXTERN(__imp__sub_830DAB48);
REX_HOOK_RAW(sub_830DAB48) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const bool checked = GroupOk(kGroupIdentity, base);
  if (user == 0 && checked && CurrentLive().active) {
    ctx.r3.u64 = kSignedInToLive;
    TraceSignin("XamUserGetSigninState", lr, user, kSignedInToLive, true);
    return;
  }
  __imp__sub_830DAB48(ctx, base);
  if (checked) TraceSignin("XamUserGetSigninState", lr, user, ctx.r3.u32, false);
}

// XamUserGetXUID wrappers: arguments not proven, so the trace is the caller, the first words and the result.
ECK_TRACE_HOOK(830DA2C0, kGroupIdentity, "XamUserGetXUID_A", 0, Ue3Api::kCount)
ECK_TRACE_HOOK(830DA408, kGroupIdentity, "XamUserGetXUID_B", 0, Ue3Api::kCount)

REX_EXTERN(__imp__sub_830DAB58);
REX_HOOK_RAW(sub_830DAB58) {
  const uint32_t result = ctx.r5.u32;
  if (ctx.r3.u32 == 0 && result != 0 && GroupOk(kGroupIdentity, base) && CurrentLive().active) {
    eck::StoreBE32(base, result, 1);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_830DAB58(ctx, base);
}

// X_USER_SIGNIN_INFO: xuid u64, flags u32, signin state u32, two unknown words, name[16].
REX_EXTERN(__imp__sub_830DB0B8);
REX_HOOK_RAW(sub_830DB0B8) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t info = ctx.r5.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_830DB0B8(ctx, base);
  if (!GroupOk(kGroupIdentity, base)) return;
  const LiveView live = user == 0 && info != 0 ? CurrentLive() : LiveView{};
  if (live.active) {
    eck::StoreBE64(base, info, live.xuid);
    eck::StoreBE32(base, info + 8, kSigninInfoLiveEnabled);
    eck::StoreBE32(base, info + 12, kSignedInToLive);
    std::memset(base + info + 24, 0, 16);
    std::memcpy(base + info + 24, live.name.data(), std::min<size_t>(live.name.size(), 15));
    ctx.r3.u64 = 0;
  }
  TraceSignin("XamUserGetSigninInfo", lr, user, ctx.r3.u32, live.active);
}

// XamUserGetName(user, buffer, size)
REX_EXTERN(__imp__sub_830DAB40);
REX_HOOK_RAW(sub_830DAB40) {
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  if (ctx.r3.u32 == 0 && buffer != 0 && size >= 2 && size <= 256 && GroupOk(kGroupIdentity, base)) {
    if (const LiveView live = CurrentLive(); live.active) {
      const size_t length = std::min<size_t>(live.name.size(), size - 1);
      std::memcpy(base + buffer, live.name.data(), length);
      base[buffer + length] = 0;
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_830DAB40(ctx, base);
}

namespace eck {

void LogOnlineGameHooks() {
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-XBOX] hooks linked: identity=4 xuid=2 xgi_session=9 xnet=3 live_virtuals=6 engine=2 "
              "net_socket=3 race_url=1 crt_srand=1 live_profile_env={} debug_env={}",
              EnvFlag(kLiveProfileEnv), EnvFlag(kDebugEnv));
}

namespace {

std::vector<race::KartAssignment> DryRunKarts(const online::Snapshot& state) {
  if (!state.room) return {};
  race::RaceConfig config;
  config.total_karts = race::kMaxKarts;
  for (const online::Seat& seat : state.room->seats) {
    if (seat.state != online::SeatState::kEmpty && seat.seat >= 0 && seat.seat < race::kMaxHumans) {
      config.participants.push_back({uint8_t(seat.seat), 0, 0});
    }
  }
  return race::MapSeatsToKarts(config, state.seat);
}

std::string DescribeRoom(const online::Snapshot& state) {
  std::string line = "connection=" + std::to_string(int(state.connection));
  if (!state.room) return line + " room=none";
  const online::RoomView& room = *state.room;
  line += " room=" + std::to_string(room.players) + "/" + std::to_string(room.capacity) +
          " state=" + std::to_string(int(room.state)) + " seat=" + std::to_string(state.seat) +
          " paths_ready=" + (state.paths_ready ? "1" : "0") + " peers=";
  for (const online::PeerSnapshot& peer : state.peers) {
    line += std::to_string(peer.seat) + ":" + online::PeerPathName(peer.path) + (peer.connected ? "" : "(lost)") + ",";
  }
  const auto karts = DryRunKarts(state);
  if (karts.empty()) return line + " karts=n/a";
  line += " dry_run_karts=";
  for (const race::KartAssignment& kart : karts) {
    line += std::to_string(kart.slot) + (kart.local ? "L" : kart.remote_human ? "R" : "B") +
            (kart.human ? std::to_string(kart.seat) : "") + ",";
  }
  return line;
}

// "seat=N role=local|remote": a remote seat is never a CPU.
std::string DescribeSeatRoles(const online::Snapshot& state) {
  std::string text;
  for (const race::KartAssignment& kart : DryRunKarts(state)) {
    if (!kart.human) continue;
    text += (text.empty() ? "" : " | ") + std::string("seat=") + std::to_string(kart.seat) +
            " role=" + (kart.local ? "local" : "remote") + " kart_slot=" + std::to_string(kart.slot) + " cpu=no";
  }
  return text;
}

}  // namespace

void PumpOnlineGameDebug() {
  static const bool on = EnvFlag(kDebugEnv);
  const double now = OnlineNowSeconds();

  // Phase for the [ONLINE-XBOX] lines, refreshed at most once per second.
  static double next_phase = 0;
  static std::string last;
  static std::string last_roles;
  if (now >= next_phase) {
    next_phase = now + 1.0;
    if (REXCVAR_GET(eck_online_enabled)) {
      const online::Snapshot state = Online().snapshot();
      online_probe::SetOnlinePhase(int(state.connection), state.room ? int(state.room->state) : 0xFF, state.seat);
      if (on) {
        const std::string line = DescribeRoom(state);
        if (line != last) {
          last = line;
          REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] {}", line);
        }
        const std::string roles = DescribeSeatRoles(state);
        if (roles != last_roles) {
          last_roles = roles;
          if (!roles.empty()) REXLOG_INFO("{} {}", online_probe::kRaceTag, roles);
        }
      }
    }
  }
  if (on) online_probe::PumpGuestProbes(now);
}

}  // namespace eck
