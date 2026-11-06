#include "main_menu_hooks.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "guest_region.h"
#include "hard_exit.h"
#include "language_setting.h"
#include "main_menu.h"
#include "main_menu_swf.h"
#include "online_menu_overlay.h"
#include "open_file_policy.h"
#include "sha256.h"

REXCVAR_DECLARE(std::string, eck_language);

// FString assignment and the engine's free.
REX_EXTERN(sub_823B5E08);
REX_EXTERN(sub_822E80C8);

namespace {

constexpr uint32_t kEngineAllocator = 0x83C1D568;
constexpr uint32_t kAllocatorMallocSlot = 40;
constexpr int kLoggedCandidates = 40;
constexpr int kLoggedOpens = 64;
constexpr uint32_t kMaxHashedBytes = 8u << 20;

void CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address) {
  rex::runtime::ResolveIndirectFunction(address)(ctx, base);
}

bool IsReadable(uint32_t address, uint32_t length) {
  if (address < 0x10000 || address + length < address) {
    return false;
  }
  auto* heap = rex::system::kernel_memory()->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (heap == nullptr || !heap->QueryRegionInfo(address, &info)) {
    return false;
  }
  return (info.state & rex::memory::kMemoryAllocationCommit) != 0 &&
         (info.protect & rex::memory::kMemoryProtectRead) != 0 &&
         eck::GuestRangeInRegion(address, length, heap->page_size(), info.region_size);
}

bool HasMovieSignature(const uint8_t* bytes) {
  static constexpr const char* kSignatures[] = {"FWS", "CWS", "GFX", "CFX"};
  for (const char* signature : kSignatures) {
    if (std::memcmp(bytes, signature, 3) == 0) {
      return true;
    }
  }
  return false;
}

std::string HashOf(const uint8_t* bytes, uint32_t size) {
  return eck::Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes), size));
}

std::string AsciiAt(const uint8_t* base, uint32_t address, uint32_t limit = 96) {
  std::string text;
  while (text.size() < limit && IsReadable(address + uint32_t(text.size()), 1)) {
    const char c = char(base[address + text.size()]);
    if (c == 0) {
      break;
    }
    text.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return text;
}

void LogOnce(const std::string& key, const std::string& message) {
  static std::mutex mutex;
  static std::set<std::string> logged;
  std::lock_guard lock(mutex);
  if (logged.insert(key).second) {
    REXLOG_INFO("[MAIN-MENU] {}", message);
  }
}

void LogCandidate(const uint8_t* base, uint32_t data, uint32_t size) {
  static std::atomic<int> logged{0};
  if (size < 1024 || size > (64u << 20) || !IsReadable(data, 4) || !HasMovieSignature(base + data) ||
      logged.fetch_add(1) >= kLoggedCandidates) {
    return;
  }
  const std::string hash =
      size <= kMaxHashedBytes && IsReadable(data, size) ? HashOf(base + data, size) : std::string("-");
  REXLOG_INFO("[MAIN-MENU] gfx candidate size={} hash={}", size, hash);
}

// The patched bytes for a recognized main-menu movie; every other buffer is ignored and every rejection is logged.
std::optional<std::vector<uint8_t>> PatchedMovieBytes(const uint8_t* base, uint32_t data, uint32_t size) {
  if (size != eck::kMainMenuSwfFingerprint.size || !IsReadable(data, size)) {
    return std::nullopt;
  }
  eck::SwfPatchResult patch = eck::PatchMainMenuSwf({base + data, size});
  switch (patch.status) {
    case eck::SwfPatchStatus::kApplied:
      return std::move(patch.bytes);
    case eck::SwfPatchStatus::kFingerprintMismatch:
      LogOnce("mismatch", "expected movie size matched but fingerprint mismatch actual=" + patch.detail);
      break;
    default:
      LogOnce("rejected:" + patch.detail, "patch rejected reason=" + patch.detail);
      break;
  }
  return std::nullopt;
}

void LogPatchApplied(size_t size) {
  REXLOG_INFO("[MAIN-MENU] scaleform patch applied size={} fingerprint={}", size,
              eck::kMainMenuSwfFingerprint.sha256);
}

struct CachedMovie {
  std::vector<uint8_t> bytes;
  uint32_t address = 0;
};

std::mutex g_cache_mutex;
CachedMovie g_cached_movie;

// The Scaleform memory file does not copy the buffer, so this copy stays allocated and is reused.
uint32_t SystemHeapMovie(const std::vector<uint8_t>& bytes) {
  std::lock_guard lock(g_cache_mutex);
  if (g_cached_movie.address != 0 && g_cached_movie.bytes == bytes) {
    return g_cached_movie.address;
  }
  auto* memory = rex::system::kernel_memory();
  const uint32_t address = memory->SystemHeapAlloc(uint32_t(bytes.size()));
  uint8_t* host = address != 0 ? memory->TranslateVirtual<uint8_t*>(address) : nullptr;
  if (host == nullptr) {
    LogOnce("alloc", "patch rejected reason=guest allocation failed");
    return 0;
  }
  std::memcpy(host, bytes.data(), bytes.size());
  g_cached_movie = {bytes, address};
  LogPatchApplied(bytes.size());
  return address;
}

// The engine owns the movie array it was read into, so the replacement comes from the engine's allocator.
uint32_t EngineHeapMovie(PPCContext& ctx, uint8_t* base, const std::vector<uint8_t>& bytes) {
  const uint32_t allocator = eck::LoadBE32(base, kEngineAllocator);
  if (allocator == 0 || !IsReadable(allocator, 4)) {
    LogOnce("alloc", "patch rejected reason=guest allocation failed");
    return 0;
  }
  ctx.r3.u32 = allocator;
  ctx.r4.u32 = uint32_t(bytes.size());
  ctx.r5.u32 = 0;
  CallGuest(ctx, base, eck::LoadBE32(base, eck::LoadBE32(base, allocator) + kAllocatorMallocSlot));
  const uint32_t address = ctx.r3.u32;
  if (address == 0 || !IsReadable(address, uint32_t(bytes.size()))) {
    LogOnce("alloc", "patch rejected reason=guest allocation failed");
    return 0;
  }
  std::memcpy(base + address, bytes.data(), bytes.size());
  LogPatchApplied(bytes.size());
  return address;
}

struct GuestView {
  const uint8_t* base;
  bool Readable(uint32_t address, uint32_t length) const { return IsReadable(address, length); }
  uint32_t U32(uint32_t address) const { return eck::LoadBE32(base, address); }
};

void InspectOpenedFile(PPCContext& ctx, uint8_t* base, uint32_t url, uint32_t file) {
  const std::string package = AsciiAt(base, url);
  const eck::open_file::Inspection inspection = eck::open_file::InspectionOf(package);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < kLoggedOpens) {
    REXLOG_INFO("[OPEN-FILE] package={} file=0x{:08X} operation={}", package, file,
                eck::open_file::InspectionName(inspection));
  }
  if (inspection == eck::open_file::Inspection::kPass) {
    return;
  }
  if (file == 0 || !IsReadable(file, 4)) {
    static std::once_flag once;
    std::call_once(once, [] { REXLOG_INFO("[OPEN-FILE] inspection skipped reason=invalid_pointer"); });
    return;
  }
  eck::NoteOnlineMovie(package);
  if (inspection != eck::open_file::Inspection::kPatchMainMenu) {
    return;
  }
  const auto fields =
      eck::open_file::FindMovieFieldsInTree(GuestView{base}, file, eck::kMainMenuSwfFingerprint.size);
  if (!fields) {
    return;
  }
  const uint32_t data = eck::LoadBE32(base, fields->pointer);
  const uint32_t size = eck::LoadBE32(base, fields->size);
  const auto bytes = PatchedMovieBytes(base, data, size);
  if (!bytes) {
    return;
  }
  const uint32_t patched = EngineHeapMovie(ctx, base, *bytes);
  if (patched == 0) {
    return;
  }
  eck::StoreBE32(base, fields->pointer, patched);
  eck::StoreBE32(base, fields->size, uint32_t(bytes->size()));
  if (IsReadable(fields->size + 4, 4) && eck::LoadBE32(base, fields->size + 4) == size) {
    eck::StoreBE32(base, fields->size + 4, uint32_t(bytes->size()));
  }
}

}  // namespace

// Scaleform's open-by-URL wrapper: (loader, url, flags) returns the opened file object.
REX_EXTERN(__imp__sub_82C22F28);
REX_HOOK_RAW(sub_82C22F28) {
  LogOnce("hook:open", "scaleform hook installed address=0x82C22F28 (open file)");
  const uint32_t url = ctx.r4.u32;
  __imp__sub_82C22F28(ctx, base);
  const uint32_t file = ctx.r3.u32;
  InspectOpenedFile(ctx, base, url, file);
  ctx.r3.u32 = file;
}

// Memory-file constructor Scaleform uses for a movie held in memory: (this, name, data, size).
REX_EXTERN(__imp__sub_82BD99F8);
REX_HOOK_RAW(sub_82BD99F8) {
  LogOnce("hook:memory", "scaleform hook installed address=0x82BD99F8 (memory file)");
  const uint32_t data = ctx.r5.u32;
  const uint32_t size = ctx.r6.u32;
  LogCandidate(base, data, size);
  if (const auto bytes = PatchedMovieBytes(base, data, size)) {
    if (const uint32_t patched = SystemHeapMovie(*bytes)) {
      ctx.r5.u32 = patched;
      ctx.r6.u32 = uint32_t(bytes->size());
    }
  }
  __imp__sub_82BD99F8(ctx, base);
}

void eck::LogMainMenuHooks() {
  REXLOG_INFO(
      "[MAIN-MENU] scaleform hooks linked memory_file=0x82BD99F8 open_file=0x82C22F28 text=0x82A46B68");
}

namespace {

constexpr uint32_t kNativeFunctionTable = 0x83C77308;
constexpr uint32_t kGetFieldValueSlot = 424;
constexpr uint32_t kFrameSize = 0x200;
constexpr uint32_t kNameSlot = 80;
constexpr uint32_t kValueSlot = 96;
constexpr uint32_t kTextSlot = 112;
constexpr uint32_t kTextChars = 128;
constexpr uint32_t kMaxKeyChars = 64;
constexpr uint32_t kMaxTextChars = 120;

// FString is {data, count including the terminator, capacity} with 16-bit big-endian characters.
std::string AsciiKey(const uint8_t* base, uint32_t string) {
  const uint32_t data = eck::LoadBE32(base, string);
  const uint32_t count = eck::LoadBE32(base, string + 4);
  if (data == 0 || count < 2 || count > kMaxKeyChars + 1) {
    return {};
  }
  std::string key;
  for (uint32_t i = 0; i + 1 < count; ++i) {
    const uint16_t c = eck::LoadBE<uint16_t>(base, data + 2 * i);
    if (c == 0 || c > 0x7F) {
      return {};
    }
    key.push_back(char(c));
  }
  return key;
}

void ReleaseString(PPCContext& ctx, uint8_t* base, uint32_t string) {
  const uint32_t data = eck::LoadBE32(base, string);
  eck::StoreBE32(base, string + 4, 0);
  eck::StoreBE32(base, string + 8, 0);
  if (data != 0) {
    ctx.r3.u32 = data;
    sub_822E80C8(ctx, base);
    eck::StoreBE32(base, string, 0);
  }
}

// Builds the replacement text in the caller's stack frame and returns its FString, or 0 for no override.
uint32_t OverrideText(uint8_t* base, uint32_t frame, const std::string& key) {
  const std::string_view package = eck::LanguageFor(REXCVAR_GET(eck_language)).package;
  const auto text = eck::MainMenuTextOverride(package, key);
  if (!text || text->size() > kMaxTextChars) {
    return 0;
  }
  const uint32_t chars = frame + kTextChars;
  for (size_t i = 0; i < text->size(); ++i) {
    eck::StoreBE<uint16_t>(base, chars + 2 * uint32_t(i), uint16_t((*text)[i]));
  }
  eck::StoreBE<uint16_t>(base, chars + 2 * uint32_t(text->size()), 0);
  const uint32_t count = uint32_t(text->size()) + 1;
  const uint32_t string = frame + kTextSlot;
  eck::StoreBE32(base, string, chars);
  eck::StoreBE32(base, string + 4, count);
  eck::StoreBE32(base, string + 8, count);
  LogOnce(std::string(package) + "/" + key, "localization language=" + std::string(package) + " key=" + key);
  return string;
}

}  // namespace

// Script exec KartScreen_Base.GetFieldValueByName(FieldName): (this, FFrame*, result FString*). It returns a class
// field by name, which is where every localized screen text comes from. Written as the original's own steps so the
// field name can be read before the result is copied out.
REX_HOOK_RAW(sub_82A46B68) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t stack = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32;
  const uint32_t saved_sp = ctx.r1.u32;
  const uint32_t frame = saved_sp - kFrameSize;
  ctx.r1.u32 = frame;

  const uint32_t name = frame + kNameSlot;
  const uint32_t value = frame + kValueSlot;
  for (uint32_t offset = 0; offset < 12; offset += 4) {
    eck::StoreBE32(base, name + offset, 0);
  }

  const uint32_t code = eck::LoadBE32(base, stack + 24);
  eck::StoreBE32(base, stack + 24, code + 1);
  ctx.r3.u32 = eck::LoadBE32(base, stack + 20);
  ctx.r4.u32 = stack;
  ctx.r5.u32 = name;
  CallGuest(ctx, base, eck::LoadBE32(base, kNativeFunctionTable + 4u * base[code]));
  eck::StoreBE32(base, stack + 24, eck::LoadBE32(base, stack + 24) + 1);

  ctx.r3.u32 = value;
  ctx.r4.u32 = self;
  ctx.r5.u32 = name;
  CallGuest(ctx, base, eck::LoadBE32(base, eck::LoadBE32(base, self) + kGetFieldValueSlot));
  uint32_t source = ctx.r3.u32;

  const std::string key = AsciiKey(base, name);
  if (!key.empty()) {
    switch (eck::ClassifyReservedKey(key)) {
      case eck::ReservedKey::kExit:
        REXLOG_INFO("[MAIN-MENU] action={}", key);
        eck::HardExit("Exit Game selected, hard-exiting process.");
        break;
      case eck::ReservedKey::kOnlineMenu:
        eck::NoteMainMenuShown();
        break;
      case eck::ReservedKey::kNone: break;
    }
    if (const uint32_t replacement = OverrideText(base, frame, key)) {
      source = replacement;
    }
  }

  ctx.r3.u32 = result;
  ctx.r4.u32 = source;
  sub_823B5E08(ctx, base);
  ReleaseString(ctx, base, value);
  ReleaseString(ctx, base, name);
  ctx.r1.u32 = saved_sp;
}
