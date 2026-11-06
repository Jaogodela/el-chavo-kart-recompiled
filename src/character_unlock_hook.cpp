// Runtime answer of the character lock check. Nothing here reaches the save: the only writes are a call's own result and
// the preview's script for the length of one call.

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

REXCVAR_DECLARE(bool, eck_unlock_secret_characters);
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

std::atomic<uint32_t> g_lock_check{0};
std::atomic<uint32_t> g_preview_lock{0};

bool ImageOk(const uint8_t* base) {
  static const bool ok = [base] {
    const char* bad = eck::online_guest::FirstMismatch(kFingerprints, base, kGuestSpan, 0);
    if (bad != nullptr) REXLOG_WARN("[DEBUG_SAULO][CHAR-UNLOCK] fingerprint MISMATCH at {}", bad);
    return bad == nullptr;
  }();
  return ok;
}

Watched Classify(const uint8_t* base, uint32_t function) {
  const uint32_t lock_check = g_lock_check.load(std::memory_order_relaxed);
  const uint32_t preview_lock = g_preview_lock.load(std::memory_order_relaxed);
  if (function == lock_check) return Watched::kLockCheck;
  if (function == preview_lock) return Watched::kPreviewLock;
  if (lock_check != 0 && preview_lock != 0) return Watched::kNone;
  const uint32_t script_size = eck::LoadBE32(base, function + layout::kStructScriptSize);
  if (!eck::roster::MayBeWatched(script_size)) return Watched::kNone;
  thread_local std::unordered_set<uint32_t> same_size;
  if (same_size.contains(function)) return Watched::kNone;
  const Watched found = eck::roster::Identify(GuestView{}, function, script_size);
  if (found == Watched::kNone) {
    same_size.insert(function);
    return found;
  }
  (found == Watched::kLockCheck ? g_lock_check : g_preview_lock).store(function, std::memory_order_relaxed);
  REXLOG_INFO("[DEBUG_SAULO][CHAR-UNLOCK] found {} at 0x{:08X}", found == Watched::kLockCheck ? "isCharacterLocked" : "SetLockVisible",
              function);
  return found;
}

bool OnlineOn() { return REXCVAR_GET(eck_online_enabled); }

void RunLockCheck(PPCContext& ctx, uint8_t* base) {
  const uint32_t frame = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32;
  const std::optional<int> character = eck::roster::CharacterArgument(GuestView{}, frame);
  const uint32_t panel = eck::LoadBE32(base, frame + layout::kFrameObject);
  __imp__sub_82313048(ctx, base);
  if (result == 0) return;
  const bool option = REXCVAR_GET(eck_unlock_secret_characters);
  const bool game_locked = eck::LoadBE32(base, result) != 0;
  const bool remote = game_locked && OnlineOn() && eck::lobby::IsRemotePanel(eck::OnlineLobby().plan(), panel);
  const bool locked = eck::roster::LockedForPanel(game_locked, character.value_or(-1), option, remote);
  if (game_locked && !locked) eck::StoreBE32(base, result, 0);
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
  const uint32_t script = eck::LoadBE32(base, ctx.r6.u32 + layout::kStructScript);
  const uint16_t first = eck::LoadBE<uint16_t>(base, script);
  eck::StoreBE<uint16_t>(base, script, layout::kScriptReturnNothing);
  __imp__sub_82313048(ctx, base);
  eck::StoreBE<uint16_t>(base, script, first);
  // DEBUG_SAULO
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) REXLOG_INFO("[DEBUG_SAULO][CHAR-UNLOCK] preview_paint=skipped reason=session");
}

}  // namespace

// UObject::CallFunction(this, FFrame*, result, UFunction*): every script call. FFrame.Object is the caller, here the panel.
REX_HOOK_RAW(sub_82313048) {
  const bool active = REXCVAR_GET(eck_unlock_secret_characters) || OnlineOn();
  const Watched watched = active && ImageOk(base) ? Classify(base, ctx.r6.u32) : Watched::kNone;
  switch (watched) {
    case Watched::kLockCheck: RunLockCheck(ctx, base); return;
    case Watched::kPreviewLock: RunPreviewLock(ctx, base); return;
    case Watched::kNone: __imp__sub_82313048(ctx, base); return;
  }
}
