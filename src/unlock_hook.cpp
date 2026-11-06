// Runtime answers of the character and track lock checks. Nothing here reaches the save: the only writes are a call's own
// result and a lock function's script for the length of one call.

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "character_unlock.h"
#include "guest_memory.h"
#include "online_game_guest.h"
#include "online_guest_reader.h"
#include "online_lobby.h"
#include "online_menu_overlay.h"
#include "track_unlock.h"

REXCVAR_DECLARE(bool, eck_unlock_secret_characters);
REXCVAR_DECLARE(bool, eck_unlock_race_tracks);
REXCVAR_DECLARE(bool, eck_online_enabled);

REX_EXTERN(__imp__sub_82313048);

namespace {

namespace layout = eck::roster::layout;
using eck::online_guest::CodeFingerprint;
using eck::roster::Watched;

constexpr size_t kGuestSpan = 0x90000000;

constexpr std::array kFingerprints = {
    CodeFingerprint{"CallFunction", layout::kCallFunction, {0x7D8802A6, 0x48DA7D59, 0x3BE1FAF0, 0x9421FAF0}},
    CodeFingerprint{"NameToString", layout::kNameToString, {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0}},
};

struct GuestView {
  bool Readable(uint32_t address, uint32_t length) const { return eck::online_guest::Guest{}.Ok(address, length); }
  uint32_t U32(uint32_t address) const { return eck::online_guest::Guest{}.P(address); }
  uint32_t U8(uint32_t address) const { return eck::online_guest::Guest{}.U8(address); }
};

std::array<std::atomic<uint32_t>, eck::roster::kWatched.size()> g_found{};

bool ImageOk(const uint8_t* base) {
  static const bool ok = [base] {
    const char* bad = eck::online_guest::FirstMismatch(kFingerprints, base, kGuestSpan, 0);
    if (bad != nullptr) REXLOG_WARN("[DEBUG_SAULO][CHAR-UNLOCK] fingerprint MISMATCH at {}", bad);
    return bad == nullptr;
  }();
  return ok;
}

Watched Classify(const uint8_t* base, uint32_t function) {
  const auto& table = eck::roster::kWatched;
  bool all_found = true;
  for (size_t i = 0; i < table.size(); ++i) {
    const uint32_t known = g_found[i].load(std::memory_order_relaxed);
    if (function == known) return table[i].watched;
    all_found = all_found && known != 0;
  }
  if (all_found) return Watched::kNone;
  const uint32_t script_size = eck::LoadBE32(base, function + layout::kStructScriptSize);
  if (!eck::roster::MayBeWatched(script_size)) return Watched::kNone;
  thread_local std::unordered_set<uint32_t> same_size;
  if (same_size.contains(function)) return Watched::kNone;
  const Watched found = eck::roster::Identify(GuestView{}, function, script_size);
  if (found == Watched::kNone) {
    same_size.insert(function);
    return found;
  }
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i].watched != found) continue;
    g_found[i].store(function, std::memory_order_relaxed);
    REXLOG_INFO("[DEBUG_SAULO][UNLOCK-HOOK] found {}.{} at 0x{:08X}", table[i].owner, table[i].name, function);
  }
  return found;
}

bool OnlineOn() { return REXCVAR_GET(eck_online_enabled); }

void ClearResult(uint8_t* base, uint32_t result) { eck::StoreBE32(base, result, 0); }

// The call still reads its arguments, so the caller's stream stays in step; only the body is cut.
void CallWithoutBody(PPCContext& ctx, uint8_t* base) {
  const uint32_t script = eck::LoadBE32(base, ctx.r6.u32 + layout::kStructScript);
  const uint16_t first = eck::LoadBE<uint16_t>(base, script);
  eck::StoreBE<uint16_t>(base, script, layout::kScriptReturnNothing);
  __imp__sub_82313048(ctx, base);
  eck::StoreBE<uint16_t>(base, script, first);
}

void RunLockCheck(PPCContext& ctx, uint8_t* base) {
  const uint32_t frame = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32;
  const std::optional<int> character = eck::roster::IntArgument(GuestView{}, frame);
  const uint32_t panel = eck::LoadBE32(base, frame + layout::kFrameObject);
  __imp__sub_82313048(ctx, base);
  if (result == 0) return;
  const bool option = REXCVAR_GET(eck_unlock_secret_characters);
  const bool game_locked = eck::LoadBE32(base, result) != 0;
  const bool remote = game_locked && OnlineOn() && eck::lobby::IsRemotePanel(eck::OnlineLobby().plan(), panel);
  const bool locked = eck::roster::LockedForPanel(game_locked, character.value_or(-1), option, remote);
  if (game_locked && !locked) ClearResult(base, result);
  // DEBUG_SAULO
  static eck::roster::UnlockLog log;
  for (const std::string& line : log.Note(option, remote, character, game_locked, locked)) {
    REXLOG_INFO("[DEBUG_SAULO][CHAR-UNLOCK] {}", line);
  }
}

// The four panels share twelve preview actors and the game never takes the black material off again, so in a session one
// panel passing over a character must not paint the model another player picked.
void RunPreviewLock(PPCContext& ctx, uint8_t* base) {
  if (!OnlineOn() || !eck::lobby::HasRemotePanels(eck::OnlineLobby().plan())) {
    __imp__sub_82313048(ctx, base);
    return;
  }
  CallWithoutBody(ctx, base);
  // DEBUG_SAULO
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("[DEBUG_SAULO][CHAR-UNLOCK] preview_paint=skipped reason=session");
}

// DEBUG_SAULO
void NoteTrack(eck::tracks::Surface surface, std::optional<int> track, bool game_locked, bool locked) {
  static eck::tracks::UnlockLog log;
  for (const std::string& line : log.Note(surface, track, game_locked, locked)) {
    REXLOG_INFO("[DEBUG_SAULO][TRACK-UNLOCK] {}", line);
  }
}

// Only the free race's track screen has these two functions, so the campaign's screens and progression never reach them.
void RunTrackLockCheck(PPCContext& ctx, uint8_t* base) {
  const uint32_t result = ctx.r5.u32;
  const std::optional<int> track = eck::roster::IntArgument(GuestView{}, ctx.r4.u32);
  __imp__sub_82313048(ctx, base);
  if (result == 0) return;
  const bool game_locked = eck::LoadBE32(base, result) != 0;
  const bool locked = eck::tracks::LockedForTrack(game_locked, track.value_or(-1), REXCVAR_GET(eck_unlock_race_tracks));
  if (game_locked && !locked) ClearResult(base, result);
  NoteTrack(eck::tracks::Surface::kPreview, track, game_locked, locked);
}

// The screen locks a locked track's list item when it opens, from the save's array and not from the check above.
void RunTrackItemLock(PPCContext& ctx, uint8_t* base) {
  const std::optional<int> track = eck::roster::IntArgument(GuestView{}, ctx.r4.u32);
  const bool locked = eck::tracks::LockedForTrack(true, track.value_or(-1), REXCVAR_GET(eck_unlock_race_tracks));
  NoteTrack(eck::tracks::Surface::kList, track, true, locked);
  if (locked) {
    __imp__sub_82313048(ctx, base);
  } else {
    CallWithoutBody(ctx, base);
  }
}

}  // namespace

// UObject::CallFunction(this, FFrame*, result, UFunction*): every script call. FFrame.Object is the caller, here the panel.
REX_HOOK_RAW(sub_82313048) {
  const bool tracks = REXCVAR_GET(eck_unlock_race_tracks);
  const bool active = REXCVAR_GET(eck_unlock_secret_characters) || tracks || OnlineOn();
  const Watched watched = active && ImageOk(base) ? Classify(base, ctx.r6.u32) : Watched::kNone;
  switch (watched) {
    case Watched::kLockCheck: RunLockCheck(ctx, base); return;
    case Watched::kPreviewLock: RunPreviewLock(ctx, base); return;
    case Watched::kTrackLockCheck:
      if (!tracks) break;
      RunTrackLockCheck(ctx, base);
      return;
    case Watched::kTrackItemLock:
      if (!tracks) break;
      RunTrackItemLock(ctx, base);
      return;
    case Watched::kNone: break;
  }
  __imp__sub_82313048(ctx, base);
}
