#include "main_menu_swf.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>

#include "main_menu.h"
#include "sha256.h"

namespace eck {
namespace {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

constexpr std::uint16_t kTagEnd = 0;
constexpr std::uint16_t kTagPlaceObject2 = 26;
constexpr std::uint16_t kTagDefineSprite = 39;
constexpr std::uint16_t kTagDoInitAction = 59;
constexpr std::uint16_t kTagPlaceObject3 = 70;

constexpr std::uint16_t kMenuSpriteId = 99;
constexpr std::uint16_t kMenuClassSpriteId = 130;
constexpr std::uint16_t kTabItemSymbolId = 22;
constexpr std::size_t kSwfLengthOffset = 4;
constexpr std::uint32_t kClipEventConstruct = 0x00040000;

namespace op {
constexpr std::uint8_t kPop = 0x17;
constexpr std::uint8_t kNot = 0x12;
constexpr std::uint8_t kGetVariable = 0x1C;
constexpr std::uint8_t kSetVariable = 0x1D;
constexpr std::uint8_t kCastOp = 0x2B;
constexpr std::uint8_t kEquals2 = 0x49;
constexpr std::uint8_t kGetMember = 0x4E;
constexpr std::uint8_t kSetMember = 0x4F;
constexpr std::uint8_t kCallMethod = 0x52;
constexpr std::uint8_t kStoreRegister = 0x87;
constexpr std::uint8_t kDefineFunction2 = 0x8E;
constexpr std::uint8_t kPush = 0x96;
constexpr std::uint8_t kJump = 0x99;
constexpr std::uint8_t kIf = 0x9D;
}  // namespace op

std::uint16_t Le16(ByteView d, std::size_t p) { return std::uint16_t(d[p] | (d[p + 1] << 8)); }
std::uint32_t Le32(ByteView d, std::size_t p) {
  return std::uint32_t(d[p]) | (std::uint32_t(d[p + 1]) << 8) | (std::uint32_t(d[p + 2]) << 16) |
         (std::uint32_t(d[p + 3]) << 24);
}

void PutLe16(Bytes& out, std::uint16_t v) {
  out.push_back(std::uint8_t(v));
  out.push_back(std::uint8_t(v >> 8));
}
void PutLe32(Bytes& out, std::uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(v >> shift));
  }
}
void PutCString(Bytes& out, std::string_view s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}
void SetLe32(Bytes& out, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out[at + i] = std::uint8_t(v >> (8 * i));
  }
}

struct Tag {
  std::uint16_t code;
  std::size_t header;
  std::size_t header_size;
  std::size_t body;
  std::size_t size;
  std::size_t end() const { return body + size; }
};

// A tag list is well formed only if it ends exactly at `end` with an End tag.
std::optional<std::vector<Tag>> WalkTags(ByteView d, std::size_t begin, std::size_t end) {
  std::vector<Tag> tags;
  std::size_t pos = begin;
  while (true) {
    if (pos + 2 > end) {
      return std::nullopt;
    }
    const std::uint16_t head = Le16(d, pos);
    std::size_t length = head & 63;
    std::size_t header_size = 2;
    if (length == 63) {
      if (pos + 6 > end) {
        return std::nullopt;
      }
      length = Le32(d, pos + 2);
      header_size = 6;
    }
    if (length > end - pos - header_size) {
      return std::nullopt;
    }
    const Tag tag{std::uint16_t(head >> 6), pos, header_size, pos + header_size, length};
    tags.push_back(tag);
    pos = tag.end();
    if (tag.code == kTagEnd) {
      return pos == end ? std::optional(std::move(tags)) : std::nullopt;
    }
  }
}

Bytes MakeTag(std::uint16_t code, const Bytes& body) {
  Bytes tag;
  if (body.size() < 63) {
    PutLe16(tag, std::uint16_t((code << 6) | body.size()));
  } else {
    PutLe16(tag, std::uint16_t((code << 6) | 63));
    PutLe32(tag, std::uint32_t(body.size()));
  }
  tag.insert(tag.end(), body.begin(), body.end());
  return tag;
}

class BitReader {
 public:
  BitReader(ByteView data, std::size_t start) : data_(data), start_(start) {}

  std::optional<std::uint32_t> Read(unsigned count) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
      const std::size_t byte = start_ + (bit_ >> 3);
      if (byte >= data_.size()) {
        return std::nullopt;
      }
      value = (value << 1) | ((data_[byte] >> (7 - (bit_ & 7))) & 1);
      ++bit_;
    }
    return value;
  }

  std::optional<std::int32_t> ReadSigned(unsigned count) {
    const std::optional<std::uint32_t> raw = Read(count);
    if (!raw || count == 0) {
      return raw ? std::optional<std::int32_t>(0) : std::nullopt;
    }
    const std::uint32_t sign = 1u << (count - 1);
    return std::int32_t((*raw ^ sign)) - std::int32_t(sign);
  }

  std::size_t BytesUsed() const { return (bit_ + 7) >> 3; }

 private:
  ByteView data_;
  std::size_t start_;
  std::size_t bit_ = 0;
};

class BitWriter {
 public:
  void Write(std::uint32_t value, unsigned count) {
    for (unsigned i = count; i-- > 0;) {
      if ((bit_ & 7) == 0) {
        bytes_.push_back(0);
      }
      bytes_.back() |= std::uint8_t(((value >> i) & 1) << (7 - (bit_ & 7)));
      ++bit_;
    }
  }
  const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
  std::size_t bit_ = 0;
};

struct Placement {
  std::string name;
  std::uint16_t depth = 0;
  std::uint16_t character = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
};

std::optional<std::string> ReadCString(ByteView d, std::size_t& p) {
  const auto end = std::find(d.begin() + p, d.end(), std::uint8_t(0));
  if (end == d.end()) {
    return std::nullopt;
  }
  std::string s(d.begin() + p, end);
  p = std::size_t(end - d.begin()) + 1;
  return s;
}

bool SkipTransformTerms(BitReader& bits) {
  const auto has_add = bits.Read(1);
  const auto has_mult = bits.Read(1);
  const auto width = bits.Read(4);
  if (!has_add || !has_mult || !width) {
    return false;
  }
  const unsigned terms = (*has_add ? 4 : 0) + (*has_mult ? 4 : 0);
  for (unsigned i = 0; i < terms; ++i) {
    if (!bits.Read(*width)) {
      return false;
    }
  }
  return true;
}

// Parses the fields up to the instance name; the translation matrix must be present.
std::optional<Placement> ParsePlacement(ByteView d, const Tag& tag) {
  const ByteView body = d.subspan(tag.body, tag.size);
  std::size_t p = 0;
  if (body.size() < 3) {
    return std::nullopt;
  }
  const std::uint8_t flags = body[p++];
  std::uint8_t flags3 = 0;
  if (tag.code == kTagPlaceObject3) {
    flags3 = body[p++];
  }
  Placement placement;
  if (p + 2 > body.size()) {
    return std::nullopt;
  }
  placement.depth = Le16(body, p);
  p += 2;
  if (flags3 & 0x08) {
    if (!ReadCString(body, p)) {
      return std::nullopt;
    }
  }
  if (flags & 0x02) {
    if (p + 2 > body.size()) {
      return std::nullopt;
    }
    placement.character = Le16(body, p);
    p += 2;
  }
  if (!(flags & 0x04)) {
    return std::nullopt;
  }
  BitReader matrix(body, p);
  const auto has_scale = matrix.Read(1);
  if (!has_scale) {
    return std::nullopt;
  }
  if (*has_scale) {
    const auto width = matrix.Read(5);
    if (!width || !matrix.Read(*width) || !matrix.Read(*width)) {
      return std::nullopt;
    }
  }
  const auto has_rotate = matrix.Read(1);
  if (!has_rotate) {
    return std::nullopt;
  }
  if (*has_rotate) {
    const auto width = matrix.Read(5);
    if (!width || !matrix.Read(*width) || !matrix.Read(*width)) {
      return std::nullopt;
    }
  }
  const auto width = matrix.Read(5);
  if (!width) {
    return std::nullopt;
  }
  const auto x = matrix.ReadSigned(*width);
  const auto y = matrix.ReadSigned(*width);
  if (!x || !y) {
    return std::nullopt;
  }
  placement.x = *x;
  placement.y = *y;
  p += matrix.BytesUsed();
  if (flags & 0x08) {
    BitReader transform(body, p);
    if (!SkipTransformTerms(transform)) {
      return std::nullopt;
    }
    p += transform.BytesUsed();
  }
  if (flags & 0x10) {
    p += 2;
  }
  if (flags & 0x20) {
    const std::optional<std::string> name = ReadCString(body, p);
    if (!name) {
      return std::nullopt;
    }
    placement.name = *name;
  }
  return placement;
}

struct PushValue {
  enum class Kind { kString, kRegister, kInteger } kind;
  std::string text;
  std::int32_t number = 0;
};

PushValue Str(std::string_view text) { return {PushValue::Kind::kString, std::string(text), 0}; }
PushValue Reg(std::int32_t index) { return {PushValue::Kind::kRegister, {}, index}; }
PushValue Num(std::int32_t value) { return {PushValue::Kind::kInteger, {}, value}; }

struct FunctionParam {
  std::uint8_t reg;
  std::string_view name;
};

class ActionWriter {
 public:
  void Op(std::uint8_t code) { bytes_.push_back(code); }

  void Push(std::initializer_list<PushValue> values) {
    Bytes payload;
    for (const PushValue& value : values) {
      switch (value.kind) {
        case PushValue::Kind::kString:
          payload.push_back(0);
          PutCString(payload, value.text);
          break;
        case PushValue::Kind::kRegister:
          payload.push_back(4);
          payload.push_back(std::uint8_t(value.number));
          break;
        case PushValue::Kind::kInteger:
          payload.push_back(7);
          PutLe32(payload, std::uint32_t(value.number));
          break;
      }
    }
    Op(op::kPush);
    PutLe16(bytes_, std::uint16_t(payload.size()));
    bytes_.insert(bytes_.end(), payload.begin(), payload.end());
  }

  void StoreRegister(std::uint8_t index) {
    Op(op::kStoreRegister);
    PutLe16(bytes_, 1);
    bytes_.push_back(index);
  }

  std::size_t Branch(std::uint8_t code) {
    Op(code);
    PutLe16(bytes_, 2);
    PutLe16(bytes_, 0);
    return bytes_.size() - 2;
  }

  void Resolve(std::size_t branch) {
    const std::int16_t offset = std::int16_t(bytes_.size() - (branch + 2));
    bytes_[branch] = std::uint8_t(offset);
    bytes_[branch + 1] = std::uint8_t(offset >> 8);
  }

  void DefineFunction2(std::uint8_t registers, std::initializer_list<FunctionParam> params, const ActionWriter& body) {
    Op(op::kDefineFunction2);
    Bytes header;
    header.push_back(0);
    PutLe16(header, std::uint16_t(params.size()));
    header.push_back(registers);
    PutLe16(header, kPreloadThisNoArgumentsNoSuper);
    for (const FunctionParam& param : params) {
      header.push_back(param.reg);
      PutCString(header, param.name);
    }
    PutLe16(header, std::uint16_t(body.bytes_.size()));
    PutLe16(bytes_, std::uint16_t(header.size()));
    bytes_.insert(bytes_.end(), header.begin(), header.end());
    bytes_.insert(bytes_.end(), body.bytes_.begin(), body.bytes_.end());
  }

  const Bytes& bytes() const { return bytes_; }

 private:
  static constexpr std::uint16_t kPreloadThisNoArgumentsNoSuper = 0x29;
  Bytes bytes_;
};

void PushLocalizationClass(ActionWriter& w) {
  w.Push({Str("com")});
  w.Op(op::kGetVariable);
  w.Push({Str("efectoLib")});
  w.Op(op::kGetMember);
  w.Push({Str("LocalizationTextSet")});
  w.Op(op::kGetMember);
}

void CallLocalizationInstance(ActionWriter& w, std::string_view method) {
  PushLocalizationClass(w);
  w.Push({Str("getInstance")});
  w.Op(op::kCallMethod);
  w.Push({Str(method)});
  w.Op(op::kCallMethod);
  w.Op(op::kPop);
}

void CallThis(ActionWriter& w, std::string_view method) {
  w.Push({Num(0), Reg(1), Str(method)});
  w.Op(op::kCallMethod);
  w.Op(op::kPop);
}

void EmitExitDescriptionBinding(ActionWriter& w) {
  w.Push({Str(kExitDescriptionKey), Num(1), Num(0)});
  CallLocalizationInstance(w, "CreateSyncText");
  w.Push({Str("TextField")});
  w.Op(op::kGetVariable);
  w.Push({Reg(1), Str("txtDescription")});
  w.Op(op::kGetMember);
  w.Op(op::kCastOp);
  w.Push({Str(kExitDescriptionKey), Num(2), Num(0)});
  CallLocalizationInstance(w, "AddTextFieldToSyncText");
}

// flash.external.ExternalInterface.call("RequestTextFieldUpdate", key): the game's own text request.
void EmitTextRequest(ActionWriter& w, std::string_view key) {
  w.Push({Str(key), Str("RequestTextFieldUpdate"), Num(2), Str("flash")});
  w.Op(op::kGetVariable);
  w.Push({Str("external")});
  w.Op(op::kGetMember);
  w.Push({Str("ExternalInterface")});
  w.Op(op::kGetMember);
  w.Push({Str("call")});
  w.Op(op::kCallMethod);
  w.Op(op::kPop);
}

ActionWriter OnLoadBody() {
  ActionWriter w;
  CallThis(w, "ECK_BaseOnLoad");
  EmitTextRequest(w, kOnlineMenuCommand);
  EmitExitDescriptionBinding(w);
  return w;
}

ActionWriter OnTabItemSelectedBody() {
  ActionWriter w;
  w.Push({Reg(2), Num(1), Reg(1), Str("ECK_BaseOnTabItemSelected")});
  w.Op(op::kCallMethod);
  w.Op(op::kPop);
  w.Push({Reg(2), Str(kExitCommand)});
  w.Op(op::kEquals2);
  w.Op(op::kNot);
  const std::size_t skip = w.Branch(op::kIf);
  w.Push({Str(kExitDescriptionKey), Num(1), Num(0)});
  CallLocalizationInstance(w, "NotifyTextFieldExternalCall");
  w.Resolve(skip);
  return w;
}

// The reserved key goes through the game's own text-request path; the lookup hook turns it into the exit.
ActionWriter OnGamepadABody() {
  ActionWriter w;
  w.Push({Num(0), Num(0), Reg(1), Str("tabGroup")});
  w.Op(op::kGetMember);
  w.Push({Str("__get__selectedItem")});
  w.Op(op::kCallMethod);
  w.Push({Str("__get__actionCommand")});
  w.Op(op::kCallMethod);
  w.StoreRegister(2);
  w.Op(op::kPop);
  w.Push({Reg(2), Str(kExitCommand)});
  w.Op(op::kEquals2);
  w.Op(op::kNot);
  const std::size_t other = w.Branch(op::kIf);
  EmitTextRequest(w, kExitCommand);
  const std::size_t done = w.Branch(op::kJump);
  w.Resolve(other);
  CallThis(w, "ECK_BaseOnGamepadA");
  w.Resolve(done);
  return w;
}

// Keeps the original method under `base_name` and installs the wrapper in its place.
void WrapPrototypeMethod(ActionWriter& w, std::string_view name, std::string_view base_name, std::uint8_t registers,
                         std::initializer_list<FunctionParam> params, const ActionWriter& body) {
  w.Push({Reg(1), Str(base_name), Reg(1), Str(name)});
  w.Op(op::kGetMember);
  w.Op(op::kSetMember);
  w.Push({Reg(1), Str(name)});
  w.DefineFunction2(registers, params, body);
  w.Op(op::kSetMember);
}

Bytes BuildMenuClassPatch() {
  ActionWriter w;
  w.Push({Str("chavoKart")});
  w.Op(op::kGetVariable);
  w.Push({Str("mainMenu")});
  w.Op(op::kGetMember);
  w.Push({Str("CKMainMenuRoot")});
  w.Op(op::kGetMember);
  w.Push({Str("prototype")});
  w.Op(op::kGetMember);
  w.StoreRegister(1);
  w.Op(op::kPop);
  WrapPrototypeMethod(w, "onLoad", "ECK_BaseOnLoad", 3, {}, OnLoadBody());
  WrapPrototypeMethod(w, "OnTabItemSelected", "ECK_BaseOnTabItemSelected", 3, {{2, "actionCommand"}},
                      OnTabItemSelectedBody());
  WrapPrototypeMethod(w, "OnGamepadA", "ECK_BaseOnGamepadA", 3, {}, OnGamepadABody());
  w.Op(0);

  Bytes body;
  PutLe16(body, kMenuClassSpriteId);
  body.insert(body.end(), w.bytes().begin(), w.bytes().end());
  return MakeTag(kTagDoInitAction, body);
}

unsigned SignedWidth(std::int32_t a, std::int32_t b) {
  const std::int32_t magnitude = std::max(std::abs(a), std::abs(b));
  unsigned width = 1;
  while ((1 << (width - 1)) <= magnitude) {
    ++width;
  }
  return width;
}

Bytes BuildExitItemPlacement(std::uint16_t depth, std::int32_t x, std::int32_t y) {
  ActionWriter actions;
  actions.Push({Str("_actionCommand"), Str(kExitCommand)});
  actions.Op(op::kSetVariable);
  actions.Op(0);

  const unsigned width = SignedWidth(x, y);
  BitWriter matrix;
  matrix.Write(0, 1);
  matrix.Write(0, 1);
  matrix.Write(width, 5);
  matrix.Write(std::uint32_t(x) & ((1u << width) - 1), width);
  matrix.Write(std::uint32_t(y) & ((1u << width) - 1), width);

  Bytes body;
  body.push_back(0xA6);  // clip actions, name, matrix, character
  PutLe16(body, depth);
  PutLe16(body, kTabItemSymbolId);
  body.insert(body.end(), matrix.bytes().begin(), matrix.bytes().end());
  PutCString(body, kExitItemName);
  PutLe16(body, 0);
  PutLe32(body, kClipEventConstruct);
  PutLe32(body, kClipEventConstruct);
  PutLe32(body, std::uint32_t(actions.bytes().size()));
  body.insert(body.end(), actions.bytes().begin(), actions.bytes().end());
  PutLe32(body, 0);
  return MakeTag(kTagPlaceObject2, body);
}

bool Contains(ByteView haystack, std::string_view needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                     [](std::uint8_t a, char b) { return a == std::uint8_t(b); }) != haystack.end();
}

SwfPatchResult Fail(SwfPatchStatus status, std::string detail) {
  SwfPatchResult result;
  result.status = status;
  result.detail = std::move(detail);
  return result;
}

}  // namespace

SwfPatchResult PatchMainMenuSwf(ByteView original, const SwfFingerprint& expected) {
  if (original.size() != expected.size) {
    return Fail(SwfPatchStatus::kSizeMismatch, "size " + std::to_string(original.size()));
  }
  const std::string actual =
      Sha256Hex(std::string_view(reinterpret_cast<const char*>(original.data()), original.size()));
  if (actual != expected.sha256) {
    return Fail(SwfPatchStatus::kFingerprintMismatch, "sha256 " + actual);
  }

  constexpr std::size_t kSignatureAndLength = 8;
  if (original.size() < 16 || std::memcmp(original.data(), "GFX", 3) != 0 || original[3] != 9 ||
      Le32(original, kSwfLengthOffset) != original.size()) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "header");
  }
  const unsigned rect_bits = original[kSignatureAndLength] >> 3;
  const std::size_t tags_begin = kSignatureAndLength + (5 + 4 * rect_bits + 7) / 8 + 4;
  const auto top = WalkTags(original, tags_begin, original.size());
  if (!top) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "top-level tags");
  }

  const Tag* sprite = nullptr;
  const Tag* class_init = nullptr;
  for (const Tag& tag : *top) {
    if (tag.code == kTagDefineSprite && tag.size >= 4 && Le16(original, tag.body) == kMenuSpriteId) {
      if (sprite) {
        return Fail(SwfPatchStatus::kUnexpectedStructure, "duplicate menu sprite");
      }
      sprite = &tag;
    }
    if (tag.code == kTagDoInitAction && tag.size > 2 && Le16(original, tag.body) == kMenuClassSpriteId &&
        Contains(original.subspan(tag.body, tag.size), "COMMAND_GOTO_ACHIEVEMENTS")) {
      if (class_init) {
        return Fail(SwfPatchStatus::kUnexpectedStructure, "duplicate menu class script");
      }
      class_init = &tag;
    }
  }
  if (!sprite || sprite->header_size != 6 || !class_init) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "menu sprite or class script not found");
  }

  const auto inner = WalkTags(original, sprite->body + 4, sprite->end());
  if (!inner) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "menu sprite tags");
  }
  std::map<std::string, std::pair<const Tag*, Placement>> items;
  std::uint16_t top_depth = 0;
  for (const Tag& tag : *inner) {
    if (tag.code != kTagPlaceObject2 && tag.code != kTagPlaceObject3) {
      continue;
    }
    const std::optional<Placement> placement = ParsePlacement(original, tag);
    if (!placement) {
      continue;
    }
    top_depth = std::max(top_depth, placement->depth);
    if (!placement->name.empty() && !items.emplace(placement->name, std::make_pair(&tag, *placement)).second) {
      return Fail(SwfPatchStatus::kUnexpectedStructure, "duplicate instance " + placement->name);
    }
  }
  const std::size_t original_item_count = kMainMenuItems.size() - 1;
  std::vector<const std::pair<const Tag*, Placement>*> rows;
  for (std::size_t i = 0; i < original_item_count; ++i) {
    const auto found = items.find(std::string(kMainMenuItems[i].name));
    if (found == items.end() || found->second.second.character != kTabItemSymbolId) {
      return Fail(SwfPatchStatus::kUnexpectedStructure, "missing row " + std::string(kMainMenuItems[i].name));
    }
    rows.push_back(&found->second);
  }
  if (items.contains(std::string(kExitItemName))) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "exit row already present");
  }
  const Placement& first = rows.front()->second;
  const Placement& last = rows.back()->second;
  const std::int32_t step = (last.y - first.y) / std::int32_t(original_item_count - 1);
  if (step <= 0 || last.x != first.x) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "row spacing");
  }

  const Bytes item = BuildExitItemPlacement(std::uint16_t(top_depth + 1), last.x, last.y + step);
  const Bytes script = BuildMenuClassPatch();
  const std::size_t item_at = rows.back()->first->end();
  const std::size_t script_at = class_init->end();
  if (item_at > sprite->end() || script_at < sprite->end() || item.size() > 0xFFFF) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "insertion points");
  }

  Bytes patched;
  patched.reserve(original.size() + item.size() + script.size());
  patched.insert(patched.end(), original.begin(), original.begin() + item_at);
  patched.insert(patched.end(), item.begin(), item.end());
  patched.insert(patched.end(), original.begin() + item_at, original.begin() + script_at);
  patched.insert(patched.end(), script.begin(), script.end());
  patched.insert(patched.end(), original.begin() + script_at, original.end());
  SetLe32(patched, sprite->header + 2, std::uint32_t(sprite->size + item.size()));
  SetLe32(patched, kSwfLengthOffset, std::uint32_t(patched.size()));

  const auto check = WalkTags(patched, tags_begin, patched.size());
  const auto check_inner = WalkTags(patched, sprite->body + 4, sprite->end() + item.size());
  if (!check || !check_inner || check->size() != top->size() + 1 || check_inner->size() != inner->size() + 1) {
    return Fail(SwfPatchStatus::kUnexpectedStructure, "patched tags");
  }

  SwfPatchResult result;
  result.status = SwfPatchStatus::kApplied;
  result.bytes = std::move(patched);
  return result;
}

std::optional<std::vector<MenuRow>> ListMainMenuRows(std::span<const std::uint8_t> movie) {
  if (movie.size() < 16 || std::memcmp(movie.data(), "GFX", 3) != 0) {
    return std::nullopt;
  }
  const unsigned rect_bits = movie[8] >> 3;
  const std::size_t tags_begin = 8 + (5 + 4 * rect_bits + 7) / 8 + 4;
  const auto top = WalkTags(movie, tags_begin, movie.size());
  if (!top) {
    return std::nullopt;
  }
  for (const Tag& sprite : *top) {
    if (sprite.code != kTagDefineSprite || sprite.size < 4 || Le16(movie, sprite.body) != kMenuSpriteId) {
      continue;
    }
    const auto inner = WalkTags(movie, sprite.body + 4, sprite.end());
    if (!inner) {
      return std::nullopt;
    }
    std::vector<MenuRow> rows;
    for (const Tag& tag : *inner) {
      if (tag.code != kTagPlaceObject2 && tag.code != kTagPlaceObject3) {
        continue;
      }
      const std::optional<Placement> placement = ParsePlacement(movie, tag);
      if (placement && placement->character == kTabItemSymbolId) {
        const ByteView body = movie.subspan(tag.body, tag.size);
        const std::string command = "_actionCommand";
        const auto at = std::search(body.begin(), body.end(), command.begin(), command.end(),
                                    [](std::uint8_t a, char b) { return a == std::uint8_t(b); });
        std::string action;
        if (at != body.end()) {
          std::size_t p = std::size_t(at - body.begin()) + command.size() + 2;
          action = ReadCString(body, p).value_or("");
        }
        rows.push_back({placement->name, placement->y, action});
      }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const MenuRow& a, const MenuRow& b) { return a.y < b.y; });
    return rows;
  }
  return std::nullopt;
}

}  // namespace eck
