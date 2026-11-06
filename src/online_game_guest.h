#pragma once

// Guest addresses of the Xbox 360 image (default.xex, base 0x82000000) that the online hooks touch, each proven in
// docs/online-analysis.md and re-read from game/analysis/default.image.bin. A hook acts only when the first four words
// of its function still match; on any mismatch the hooks call the original and do nothing else. Pure: no ReXGlue.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace eck::online_guest {

inline constexpr char kLiveProfileEnv[] = "ECK_ONLINE_LIVE_PROFILE";
inline constexpr char kDebugEnv[] = "ECK_ONLINE_DEBUG";

// Title-local wrappers of the Xam user imports (same arguments as the imports, no caller id).
inline constexpr uint32_t kSigninState = 0x830DAB48;  // (user)
inline constexpr uint32_t kCheckPrivilege = 0x830DAB58;  // (user, privilege, *result)
inline constexpr uint32_t kSigninInfo = 0x830DB0B8;  // (user, flags, *X_USER_SIGNIN_INFO)
inline constexpr uint32_t kGetName = 0x830DAB40;  // (user, buffer, size)
inline constexpr uint32_t kGetXuidA = 0x830DA2C0;  // arguments not proven: fingerprinted, never hooked
inline constexpr uint32_t kGetXuidB = 0x830DA408;

inline constexpr uint32_t kTitleXnAddr = 0x82B74D10;
inline constexpr uint32_t kXnAddrToInAddr = 0x82B74BF0;
inline constexpr uint32_t kQosListen = 0x82B74C80;

// XGI session wrappers (XMsgStartIORequest, app 0xFB).
inline constexpr uint32_t kSessionCreate = 0x82B76128;
inline constexpr uint32_t kSessionDelete = 0x82B76398;
inline constexpr uint32_t kSessionJoinLocal = 0x82B76448;
inline constexpr uint32_t kSessionJoinRemote = 0x82B764F8;
inline constexpr uint32_t kSessionLeaveLocal = 0x82B765A8;
inline constexpr uint32_t kSessionLeaveRemote = 0x82B76650;
inline constexpr uint32_t kSessionStart = 0x82B767D8;
inline constexpr uint32_t kSessionEnd = 0x82B76878;
inline constexpr uint32_t kSessionSearch = 0x82B76928;

// UOnlineSubsystemLive virtuals (this in r3).
inline constexpr uint32_t kLiveCreateOnlineGame = 0x82A0E268;
inline constexpr uint32_t kLiveFindOnlineGames = 0x82A0F5F0;
inline constexpr uint32_t kLiveJoinOnlineGame = 0x82A10660;
inline constexpr uint32_t kLiveStartOnlineGame = 0x82A158B0;
inline constexpr uint32_t kLiveEndOnlineGame = 0x82A15B08;
inline constexpr uint32_t kLiveShowInviteUI = 0x82A151D0;

// Script natives (this, FFrame*, result).
inline constexpr uint32_t kCreateNamedNetDriver = 0x8258E2F8;
inline constexpr uint32_t kClientTravel = 0x82574920;

// NetDll wrappers the game network goes through (title wrappers, caller id dropped): socket(af, type, proto),
// sendto(s, buf, len, flags, to, tolen), recvfrom(s, buf, len, flags, from, fromlen).
inline constexpr uint32_t kNetSocket = 0x82B74930;
inline constexpr uint32_t kNetSendTo = 0x82B74AA0;
inline constexpr uint32_t kNetRecvFrom = 0x82B74A68;

// Race start: AActor::ConsoleCommand (vtable slot 75), (FString* result, this, const FString* command, bWriteToLog).
// KartProgressManager_Base.LoadRace calls TheGame.ConsoleCommand("open|start <map>?...").
inline constexpr uint32_t kActorConsoleCommand = 0x826A1F30;
// CRT srand(seed): stores the seed in the per-thread data that rand() (0x830B53E0) steps.
inline constexpr uint32_t kCrtSrand = 0x830B53B0;
inline constexpr uint32_t kMovieActionScriptVoid = 0x829B1990;
inline constexpr uint32_t kInputGetState = 0x830D9E00;  // title stub of XamInputGetState(user, flags, *state)
inline constexpr uint32_t kEngineTick = 0x825A4D98;
// The ExternalInterface handler, slot 1 of the vtable at 0x8222E700: (this, movie view, method, GFxValue args, count).
inline constexpr uint32_t kMovieExternalCall = 0x829A1FA0;
inline constexpr uint32_t kFarMoveActor = 0x825D4E10;

// XGI message ids the session wrappers send.
inline constexpr uint32_t kMsgSessionCreate = 0x000B0010;
inline constexpr uint32_t kMsgSessionDelete = 0x000B0011;
inline constexpr uint32_t kMsgSessionJoin = 0x000B0012;
inline constexpr uint32_t kMsgSessionLeave = 0x000B0013;
inline constexpr uint32_t kMsgSessionStart = 0x000B0014;
inline constexpr uint32_t kMsgSessionEnd = 0x000B0015;
inline constexpr uint32_t kMsgSessionSearch = 0x000B0016;

inline constexpr uint32_t kSignedInLocal = 1;
inline constexpr uint32_t kSignedInToLive = 2;
inline constexpr uint32_t kSigninInfoLiveEnabled = 1;

struct CodeFingerprint {
  const char* name;
  uint32_t address;
  std::array<uint32_t, 4> words;  // big-endian PowerPC instruction words, as in the image
};

inline constexpr std::array kIdentityFingerprints = {
    CodeFingerprint{"SigninState", kSigninState, {0x483FFA2C, 0x00000000, 0x483FFC44, 0x00000000}},
    CodeFingerprint{"CheckPrivilege", kCheckPrivilege, {0x7D8802A6, 0x4BFE027D, 0x9421FF80, 0x7C7E1B78}},
    CodeFingerprint{"SigninInfo", kSigninInfo, {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x483FF761}},
    CodeFingerprint{"GetName", kGetName, {0x483FFC44, 0x00000000, 0x483FFA2C, 0x00000000}},
    CodeFingerprint{"GetXuidA", kGetXuidA, {0x7D8802A6, 0x4BFE0B01, 0x9421FD50, 0x39600000}},
    CodeFingerprint{"GetXuidB", kGetXuidB, {0x7D8802A6, 0x4BFE09C5, 0x9421FF70, 0x3960FFFF}},
};

inline constexpr std::array kNetFingerprints = {
    CodeFingerprint{"TitleXnAddr", kTitleXnAddr, {0x7C641B78, 0x38600001, 0x4896575C, 0x00000000}},
    CodeFingerprint{"XnAddrToInAddr", kXnAddrToInAddr, {0x7CA62B78, 0x7C852378, 0x7C641B78, 0x38600001}},
    CodeFingerprint{"QosListen", kQosListen, {0x7CE83B78, 0x7CC73378, 0x7CA62B78, 0x7C852378}},
};

inline constexpr std::array kSessionFingerprints = {
    CodeFingerprint{"SessionCreate", kSessionCreate, {0x7D8802A6, 0x48544C95, 0x9421FF20, 0x7C7F1B78}},
    CodeFingerprint{"SessionDelete", kSessionDelete, {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0}},
    CodeFingerprint{"SessionJoinLocal", kSessionJoinLocal, {0x7D8802A6, 0x48544989, 0x9421FF50, 0x7C9D2378}},
    CodeFingerprint{"SessionJoinRemote", kSessionJoinRemote, {0x7D8802A6, 0x485448D9, 0x9421FF50, 0x7C9D2378}},
    CodeFingerprint{"SessionLeaveLocal", kSessionLeaveLocal, {0x7D8802A6, 0x4854482D, 0x9421FF60, 0x7C9D2378}},
    CodeFingerprint{"SessionLeaveRemote", kSessionLeaveRemote, {0x7D8802A6, 0x48544785, 0x9421FF60, 0x7C9D2378}},
    CodeFingerprint{"SessionStart", kSessionStart, {0x7D8802A6, 0x48544601, 0x9421FF70, 0x7C9D2378}},
    CodeFingerprint{"SessionEnd", kSessionEnd, {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0}},
    CodeFingerprint{"SessionSearch", kSessionSearch, {0x7D8802A6, 0x485444A9, 0x9421FF60, 0x1D65052E}},
};

inline constexpr std::array kLiveFingerprints = {
    CodeFingerprint{"CreateOnlineGame", kLiveCreateOnlineGame, {0x7D8802A6, 0x486ACB61, 0x9421FF30, 0x7C9C2378}},
    CodeFingerprint{"FindOnlineGames", kLiveFindOnlineGames, {0x7D8802A6, 0x486AB7E9, 0x9421FF90, 0x7C7E1B78}},
    CodeFingerprint{"JoinOnlineGame", kLiveJoinOnlineGame, {0x7D8802A6, 0x486AA769, 0x9421FF50, 0x7C9A2378}},
    CodeFingerprint{"StartOnlineGame", kLiveStartOnlineGame, {0x7D8802A6, 0x486A5519, 0x9421FF40, 0x3FA08015}},
    CodeFingerprint{"EndOnlineGame", kLiveEndOnlineGame, {0x7D8802A6, 0x486A52C1, 0x9421FF40, 0x3FC08015}},
    CodeFingerprint{"ShowInviteUI", kLiveShowInviteUI, {0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x5483063E}},
};

inline constexpr std::array kEngineFingerprints = {
    CodeFingerprint{"CreateNamedNetDriver", kCreateNamedNetDriver, {0x7D8802A6, 0x48B2CAD9, 0x9421FF70, 0x81640018}},
    CodeFingerprint{"ClientTravel", kClientTravel, {0x7D8802A6, 0x48B464B1, 0x9421FF40, 0x81640018}},
};

inline constexpr std::array kSocketFingerprints = {
    CodeFingerprint{"NetSocket", kNetSocket, {0x7CA62B78, 0x7C852378, 0x7C641B78, 0x38600001}},
    CodeFingerprint{"NetSendTo", kNetSendTo, {0x7D094378, 0x7CE83B78, 0x7CC73378, 0x7CA62B78}},
    CodeFingerprint{"NetRecvFrom", kNetRecvFrom, {0x7D094378, 0x7CE83B78, 0x7CC73378, 0x7CA62B78}},
};

inline constexpr std::array kRaceFingerprints = {
    CodeFingerprint{"ActorConsoleCommand", kActorConsoleCommand, {0x7D8802A6, 0x48A18E91, 0x9421FF20, 0x7C771B78}},
    CodeFingerprint{"CrtSrand", kCrtSrand, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0}},
    CodeFingerprint{"MovieActionScriptVoid", kMovieActionScriptVoid, {0x7D8802A6, 0x48709445, 0x9421FF50, 0x7C7F1B78}},
    CodeFingerprint{"InputGetState", kInputGetState, {0x48400924, 0x00000000, 0x7D8802A6, 0x9181FFF8}},
    CodeFingerprint{"EngineTick", kEngineTick, {0x7D8802A6, 0x48B16009, 0xDBC1FF60, 0xDBE1FF68}},
    CodeFingerprint{"MovieExternalCall", kMovieExternalCall, {0x7D8802A6, 0x48718E1D, 0x3BE1FF10, 0x9421FF10}},
    CodeFingerprint{"FarMoveActor", kFarMoveActor, {0x7D8802A6, 0x48AE5FB5, 0xDBA1FFA0, 0xDBC1FFA8}},
};

// `memory` holds the guest image starting at guest address `memory_base`. Returns nullptr when every function matches,
// otherwise the name of the first one that does not (or that lies outside the buffer).
inline const char* FirstMismatch(std::span<const CodeFingerprint> table, const uint8_t* memory, size_t memory_size,
                                 uint32_t memory_base) {
  for (const CodeFingerprint& entry : table) {
    if (memory == nullptr || entry.address < memory_base) return entry.name;
    const uint64_t offset = uint64_t(entry.address) - memory_base;
    if (offset + 16 > memory_size) return entry.name;
    for (size_t i = 0; i < 4; ++i) {
      const uint8_t* p = memory + offset + 4 * i;
      const uint32_t word = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      if (word != entry.words[i]) return entry.name;
    }
  }
  return nullptr;
}

// ---- Title-local LIVE profile ---------------------------------------------------------------------------------------

constexpr bool EnvOn(const char* value) { return value != nullptr && value[0] == '1' && value[1] == 0; }

// Reporting the title-local profile as signed in to LIVE changes ELoginStatus (1 to 2) for the game's login-change
// handling and gives the profile another XUID, so it is a developer opt-in, never the default.
constexpr bool ReportLiveProfile(bool env_enabled, bool online_enabled, bool signed_in) {
  return env_enabled && online_enabled && signed_in;
}

// 0x0009 prefix as for online profiles, 48 bits derived only from the public friend code (never the private token).
inline uint64_t VirtualXuid(std::string_view friend_code) {
  uint64_t hash = 1469598103934665603ull;
  for (char c : std::string_view("eck-xuid:")) hash = (hash ^ uint8_t(c)) * 1099511628211ull;
  for (char c : friend_code) hash = (hash ^ uint8_t(c)) * 1099511628211ull;
  const uint64_t bits = hash & 0x0000FFFFFFFFFFFFull;
  return 0x0009000000000000ull | (bits ? bits : 1);
}

// Up to 15 characters of `name` as the NUL-terminated ASCII the Xam user calls hand out.
inline std::string GuestName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (out.size() >= 15) break;
    out.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return out;
}

// ---- DEBUG_SAULO: transition-only, rate-limited logging -------------------------------------------------------------

// A key logs when its value changed since the last line it logged, at most once per `min_gap` seconds, and at most
// `max_lines` lines in total.
class TransitionGate {
 public:
  explicit TransitionGate(double min_gap = 0.25, int max_lines = 400) : min_gap_(min_gap), max_lines_(max_lines) {}

  bool ShouldLog(const std::string& key, uint64_t value, double now) {
    Entry& e = entries_[key];
    ++e.calls;
    if (lines_ >= max_lines_) return false;
    if (e.logged && (e.value == value || now - e.at < min_gap_)) return false;
    e.logged = true;
    e.value = value;
    e.at = now;
    ++lines_;
    return true;
  }
  uint64_t calls(const std::string& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? 0 : it->second.calls;
  }
  int lines() const { return lines_; }

 private:
  struct Entry {
    uint64_t value = 0;
    double at = 0;
    uint64_t calls = 0;
    bool logged = false;
  };
  std::map<std::string, Entry> entries_;
  double min_gap_;
  int max_lines_;
  int lines_ = 0;
};

}  // namespace eck::online_guest
