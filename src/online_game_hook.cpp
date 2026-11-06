#include "online_game_hook.h"

#include <algorithm>
#include <array>
#include <optional>
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
#include "online_guest_reader.h"
#include "online_menu_overlay.h"
#include "online_panel_bridge.h"
#include "online_probe.h"
#include "online_probe_layout.h"
#include "online_race.h"
#include "online_race_telemetry.h"
#include "online_race_sync_guest.h"
#include "online_race_telemetry_guest.h"
#include "online_race_url.h"
#include "online_runtime.h"
#include "online_setup_capture.h"
#include "settings.h"

// The engine's free.
REX_EXTERN(sub_822E80C8);

namespace {

using namespace eck::online_guest;

constexpr size_t kGuestSpan = 0x90000000;
constexpr uint8_t kScriptStringConst = 0x1F;  // EX_StringConst: the ANSI text follows in the bytecode
constexpr uint32_t kMaxScriptMethodChars = 32;
constexpr uint32_t kInputStateSize = 16;
constexpr uint32_t kInputStateButtons = 4;
constexpr uint32_t kInputStateLeftTrigger = 6;
constexpr uint32_t kInputStateRightTrigger = 7;
constexpr uint32_t kInputStateLeftX = 8;
constexpr uint32_t kInputStateLeftY = 10;
constexpr uint32_t kInputStateRightX = 12;
constexpr uint32_t kInputStateRightY = 14;
constexpr uint32_t kCommandFrame = 0x400;
constexpr uint32_t kCommandResult = 0x60;
constexpr uint32_t kCommandString = 0x70;
constexpr uint32_t kCommandChars = 0x80;
constexpr double kPanelScanRetrySeconds = 1.0;

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

void WriteInputAnalog(uint8_t* base, uint32_t state, const eck::race::PadState& pad) {
  base[state + kInputStateLeftTrigger] = pad.left_trigger;
  base[state + kInputStateRightTrigger] = pad.right_trigger;
  eck::StoreBE<int16_t>(base, state + kInputStateLeftX, pad.left_x);
  eck::StoreBE<int16_t>(base, state + kInputStateLeftY, pad.left_y);
  eck::StoreBE<int16_t>(base, state + kInputStateRightX, pad.right_x);
  eck::StoreBE<int16_t>(base, state + kInputStateRightY, pad.right_y);
}

eck::race::PadState ReadInputState(const uint8_t* base, uint32_t state) {
  eck::race::PadState pad;
  pad.buttons = eck::LoadBE<uint16_t>(base, state + kInputStateButtons);
  pad.left_trigger = base[state + kInputStateLeftTrigger];
  pad.right_trigger = base[state + kInputStateRightTrigger];
  pad.left_x = eck::LoadBE<int16_t>(base, state + kInputStateLeftX);
  pad.left_y = eck::LoadBE<int16_t>(base, state + kInputStateLeftY);
  pad.right_x = eck::LoadBE<int16_t>(base, state + kInputStateRightX);
  pad.right_y = eck::LoadBE<int16_t>(base, state + kInputStateRightY);
  return pad;
}

struct MovieView {
  bool Readable(uint32_t address, uint32_t length) const { return Guest{}.Ok(address, length); }
  uint32_t U32(uint32_t address) const { return Guest{}.P(address); }
  uint32_t U8(uint32_t address) const { return Guest{}.U8(address); }
};

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

namespace {

void TraceConsoleCommand(PPCContext& ctx, const uint8_t* base) {
  eck::race_telemetry::OnConsoleCommand(base, ctx.r5.u32);
  eck::online_probe::OnConsoleCommand(base, ctx.r5.u32, ctx.r4.u32, uint32_t(ctx.lr));
}

bool IsRaceLaunch(const std::string& command) {
  const std::optional<eck::race_url::RaceUrl> url = eck::race_url::ParseRaceUrl(command);
  return url && eck::race_telemetry::SetupOfUrl(*url).has_value();
}

// The script names the method with a literal right at FFrame.Code, so reading it consumes nothing.
std::string ScriptMethodName(const uint8_t* base, uint32_t stack) {
  const uint32_t code = eck::LoadBE32(base, stack + 24);
  if (base[code] != kScriptStringConst) return {};
  std::string name;
  for (uint32_t i = 1; i <= kMaxScriptMethodChars && base[code + i] != 0; ++i) name.push_back(char(base[code + i]));
  return name;
}

}  // namespace

// UGFxMoviePlayer::execActionScriptVoid(this, FFrame*, result): every screen shows, hides and takes its buttons here.
REX_EXTERN(__imp__sub_829B1990);
REX_HOOK_RAW(sub_829B1990) {
  if (REXCVAR_GET(eck_online_enabled) && GroupOk(kGroupRace, base)) {
    eck::NoteOnlineScreenCall(eck::LoadBE32(base, ctx.r3.u32 + eck::online_probe::layout::kObjectClass), ctx.r3.u32,
                              ScriptMethodName(base, ctx.r4.u32));
  }
  __imp__sub_829B1990(ctx, base);
}

// The game's GFxExternalInterface::Callback(this, view, method, args, count): every ExternalInterface.call of a movie.
REX_EXTERN(__imp__sub_829A1FA0);
REX_HOOK_RAW(sub_829A1FA0) {
  if (REXCVAR_GET(eck_online_enabled) && GroupOk(kGroupRace, base)) {
    const std::string method = eck::lobby::MovieText(MovieView{}, ctx.r5.u32);
    if (eck::lobby::IsSetupCall(method)) {
      const eck::lobby::MovieArgument argument = eck::lobby::FirstMovieArgument(MovieView{}, ctx.r6.u32, ctx.r7.u32);
      eck::NoteOnlineMovieCall(method, argument.number, argument.text);
    }
  }
  __imp__sub_829A1FA0(ctx, base);
}

// XamInputGetState(user, flags, *X_INPUT_STATE): every controller reaches the game through this stub, so a remote seat is
// a pad here and takes the same screens a local player does.
REX_EXTERN(__imp__sub_830D9E00);
REX_HOOK_RAW(sub_830D9E00) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  const bool online = REXCVAR_GET(eck_online_enabled) && GroupOk(kGroupRace, base);
  const eck::lobby::PadRule rule = online ? eck::OnlineLobby().RuleFor(user) : eck::lobby::PadRule{};
  if (rule.virtual_pad) {
    if (state != 0) {
      static std::array<std::atomic<uint32_t>, eck::lobby::kPanels> packets{};
      std::memset(base + state, 0, kInputStateSize);
      eck::StoreBE32(base, state, packets[user].fetch_add(1) + 1);
      const uint16_t own = rule.echo_buttons ? eck::OnlineLobby().own_pad().buttons : 0;
      eck::StoreBE<uint16_t>(base, state + kInputStateButtons, uint16_t(rule.buttons | (own & rule.echo_buttons)));
      WriteInputAnalog(base, state, rule.analog);
    }
    ctx.r3.u64 = 0;
    return;
  }
  if (rule.source_pad >= 0) ctx.r3.u32 = uint32_t(rule.source_pad);
  __imp__sub_830D9E00(ctx, base);
  if (state == 0) return;
  if (ctx.r3.u32 != 0) {
    if (rule.own_pad) eck::OnlineLobby().SetOwnPad({});
    if (rule.press == 0) return;
    static std::atomic<uint32_t> packet{};
    std::memset(base + state, 0, kInputStateSize);
    eck::StoreBE32(base, state, packet.fetch_add(1) + 1);
    ctx.r3.u64 = 0;
  }
  const uint16_t buttons = eck::LoadBE<uint16_t>(base, state + kInputStateButtons);
  if (rule.own_pad) {
    eck::OnlineLobby().NoteOwnBack((buttons & eck::lobby::kPadB) != 0, rule.back_leaves);
    eck::OnlineLobby().NoteOwnMenuButton((buttons & rule.menu_button) != 0);
    eck::OnlineLobby().SetOwnPad(ReadInputState(base, state));
  }
  const uint16_t reached = uint16_t((buttons & rule.allow) | rule.press);
  if (reached != buttons) eck::StoreBE<uint16_t>(base, state + kInputStateButtons, reached);
}

// AActor::ConsoleCommand(FString* result, this, const FString* command, bWriteToLog): the string is already evaluated here,
// so reading it consumes nothing. Only race start URLs are logged.
REX_EXTERN(__imp__sub_826A1F30);
REX_HOOK_RAW(sub_826A1F30) {
  if (GroupOk(kGroupRace, base)) {
    const std::string command = ConsoleCommandText(base, ctx.r5.u32);
    if (REXCVAR_GET(eck_online_enabled) && eck::lobby::IsMenuCommand(command)) eck::OnlineLobby().NoteHostReturn(command);
    if (REXCVAR_GET(eck_online_enabled) && IsRaceLaunch(command)) {
      // The script only ever discards this result, but it frees what it is given.
      if (eck::OnlineLobby().HoldRace(command)) {
        REXLOG_INFO("[DEBUG_SAULO][ONLINE-RACE] race held command={}", command);
        for (uint32_t offset = 0; offset < 12; offset += 4) eck::StoreBE32(base, ctx.r3.u32 + offset, 0);
        eck::NoteOnlineTravel();
        return;
      }
      eck::NoteOnlineRaceLaunch();
    }
    TraceConsoleCommand(ctx, base);
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

namespace {

namespace layout = eck::online_probe::layout;

// WorldInfo.Game: the actor the game's own travel goes through.
uint32_t CurrentGame() {
  const Guest g;
  const uint32_t world = g.P(layout::kGWorld);
  const uint32_t level = world ? g.P(world + layout::kWorldLevel) : 0;
  const uint32_t actors = level ? g.P(level + layout::kLevelActors) : 0;
  const uint32_t info = actors ? g.P(actors) : 0;
  return info ? g.P(info + layout::kWorldGame) : 0;
}

// AActor::ConsoleCommand with the string in a stack frame of its own. The result is the engine's to free.
void RunConsoleCommand(PPCContext& ctx, uint8_t* base, uint32_t actor, const std::string& command) {
  const uint32_t saved_sp = ctx.r1.u32;
  const uint32_t frame = saved_sp - kCommandFrame;
  ctx.r1.u32 = frame;
  const uint32_t result = frame + kCommandResult;
  const uint32_t text = frame + kCommandString;
  const uint32_t chars = frame + kCommandChars;
  const uint32_t count = uint32_t(command.size()) + 1;
  for (size_t i = 0; i < command.size(); ++i) eck::StoreBE<uint16_t>(base, chars + 2 * uint32_t(i), uint16_t(command[i]));
  eck::StoreBE<uint16_t>(base, chars + 2 * uint32_t(command.size()), 0);
  for (uint32_t offset = 0; offset < 12; offset += 4) eck::StoreBE32(base, result + offset, 0);
  eck::StoreBE32(base, text, chars);
  eck::StoreBE32(base, text + 4, count);
  eck::StoreBE32(base, text + 8, count);
  ctx.r3.u32 = result;
  ctx.r4.u32 = actor;
  ctx.r5.u32 = text;
  ctx.r6.u32 = 0;
  TraceConsoleCommand(ctx, base);
  __imp__sub_826A1F30(ctx, base);
  if (const uint32_t data = eck::LoadBE32(base, result)) {
    ctx.r3.u32 = data;
    sub_822E80C8(ctx, base);
  }
  ctx.r1.u32 = saved_sp;
}

// DEBUG_SAULO: only when the outcome changes.
void LogPanelScan(const eck::lobby::PanelScan& scan, const eck::menu::PanelObjects& objects) {
  const auto hits = [](const std::vector<uint32_t>& at) {
    std::string text;
    for (const uint32_t offset : at) text += (text.empty() ? "" : ",") + std::to_string(offset);
    return text.empty() ? std::string("none") : text;
  };
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] panel_scan manager=0x{:08X} base=0x{:08X} panel0=0x{:08X} readable={} shift={} pad_hits={} "
              "game_hits={}",
              scan.manager, scan.fields ? scan.fields->pad : 0, objects[0], scan.readable ? "yes" : "no", scan.shift,
              hits(scan.pad_hits), hits(scan.game_hits));
  const eck::lobby::ScanCandidate& seen = scan.candidate;
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] panel_candidate pad_at=0x{:X} game_at=0x{:X} parent_at=0x{:X} parent=0x{:08X} state_at=0x{:X} "
              "state={} character_at=0x{:X} character={} failed={}",
              seen.pad_at, seen.game_at, seen.parent_at, seen.parent, seen.state_at, seen.state, seen.character_at, seen.character,
              seen.failed);
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] remote_panel_bridge={} reason={}", scan.fields ? "available" : "unavailable", scan.failure);
}

// The layout is searched once per set of panels. A miss is retried at this pace, as the game may just not be readable yet.
std::optional<eck::lobby::Panels> CurrentPanels(const eck::menu::PanelObjects& objects) {
  static eck::menu::PanelObjects scanned{};
  static eck::lobby::PanelScan scan;
  static std::string reported;
  static double next_scan = 0;
  const double now = eck::OnlineNowSeconds();
  if (objects != scanned || (!scan.fields && now >= next_scan)) {
    scanned = objects;
    next_scan = now + kPanelScanRetrySeconds;
    scan = eck::lobby::ScanPanels(MovieView{}, objects, CurrentGame());
    const std::string outcome = std::string(scan.failure) + std::to_string(scan.fields ? scan.fields->pad : 0);
    if (outcome != reported) {
      reported = outcome;
      LogPanelScan(scan, objects);
    }
  }
  if (!scan.fields) return std::nullopt;
  return eck::lobby::ReadPanels(MovieView{}, objects, *scan.fields);
}

std::optional<int> OwnPanel() {
  const uint32_t game = CurrentGame();
  const int primary = game ? Guest{}.I32(game + layout::kGamePrimaryController) : -1;
  if (primary < 0 || primary >= eck::lobby::kPanels) return std::nullopt;
  return primary;
}

// The saved panel of a run is not the one of the game that travels, so the primary pad is read at the moment of leaving.
// False when there is no game to travel from: a map is loading.
bool RunTravel(PPCContext& ctx, uint8_t* base, const eck::lobby::TravelRequest& travel) {
  const Guest g;
  const uint32_t game = CurrentGame();
  const int primary = game ? g.I32(game + layout::kGamePrimaryController) : -1;
  if (primary < 0 || primary >= eck::lobby::kPanels) {
    Log("travel_skipped", game, "travel skipped reason=no_game primary=" + std::to_string(primary));
    return false;
  }
  const std::string command = eck::lobby::TravelCommand(travel, primary);
  if (command.size() > eck::lobby::kMaxCommandChars) {
    REXLOG_INFO("[DEBUG_SAULO][ONLINE] travel skipped reason=command_too_long chars={}", command.size());
    return true;
  }
  REXLOG_INFO("[DEBUG_SAULO][ONLINE] travel command={}", command);
  eck::NoteOnlineTravel();
  RunConsoleCommand(ctx, base, game, command);
  return true;
}

}  // namespace

namespace eck {

void OnlineEngineTick(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(eck_online_enabled) || !GroupOk(kGroupRace, base)) return;
  lobby::Bridge& bridge = OnlineLobby();
  if (const lobby::TravelRequest travel = bridge.TakeTravel(); travel.kind != lobby::TravelKind::kNone) {
    // Nothing else takes a player out of a race that is over, however late the game can act on it.
    if (!RunTravel(ctx, base, travel) && travel.kind != lobby::TravelKind::kRace) bridge.RetryTravel(travel);
  }
  if (const std::string race = bridge.TakeReleasedRace(); !race.empty() && race.size() <= lobby::kMaxCommandChars) {
    REXLOG_INFO("[DEBUG_SAULO][ONLINE-RACE] race released");
    RunConsoleCommand(ctx, base, CurrentGame(), race);
  }
  static lobby::LobbyPads pads;
  static int session = 0;
  const lobby::Plan plan = bridge.plan();
  if (plan.session != session) {
    session = plan.session;
    pads.NewSession();
    bridge.ClearOwn();
    if (plan.local_seat >= 0) REXLOG_INFO("[DEBUG_SAULO][ONLINE-GAME] {}", lobby::CanonicalPanelsLine(plan.local_seat));
  }
  const std::optional<lobby::Panels> panels = plan.on_screen ? CurrentPanels(plan.panels) : std::nullopt;
  const std::optional<int> physical = panels ? OwnPanel() : std::nullopt;
  const std::optional<int> race_pad = plan.in_race ? OwnPanel() : std::nullopt;
  const bool racing = TickRaceSync(ctx, base, plan, race_pad, bridge);
  if (!physical) {
    pads.Reset();
    const std::optional<int> pad = plan.locked ? OwnPanel() : std::nullopt;
    if (pad) bridge.SetRules(lobby::LockedPadRules(*pad));
    else if (!racing) bridge.SetRules({});
    bridge.ClearOwn();
    return;
  }
  bridge.SetRules(pads.Step(OnlineNowSeconds(), plan, *panels, *physical));
  bridge.ReportOwn((*panels)[size_t(lobby::LocalPanel(plan, *physical))], plan.session);
  bridge.SetPadSeats(lobby::CanonicalPadSeats(plan), *physical);
  // DEBUG_SAULO: one line per panel change, all four when the room changes.
  if (plan.local_seat < 0) return;
  const uint64_t room = uint64_t(plan.session) << 16 | uint64_t(plan.local_seat + 1) << 8;
  for (int i = 0; i < lobby::kPanels; ++i) {
    const lobby::PanelView& panel = (*panels)[size_t(i)];
    const uint64_t occupied = plan.remote[size_t(i)] ? 0x80 : 0;
    Log("lobby_panel" + std::to_string(i), room | occupied | uint64_t(panel.state) << 5 | uint64_t(panel.character),
        lobby::PanelLine(plan, *panels, i, *physical));
  }
}

void LogOnlineGameHooks() {
  REXLOG_INFO("[DEBUG_SAULO][ONLINE-XBOX] hooks linked: identity=4 xuid=2 xgi_session=9 xnet=3 live_virtuals=6 engine=2 "
              "net_socket=3 race_url=1 crt_srand=1 screen_calls=1 input_state=1 live_profile_env={} debug_env={}",
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
