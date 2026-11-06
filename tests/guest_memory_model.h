#pragma once

// A sparse model of the guest memory the lock hooks read: the name table, objects and an FFrame.
#include <cstdint>
#include <map>
#include <string>

#include "character_unlock.h"

namespace guest_model {

class Bytes {
 public:
  void Put(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes_[address + uint32_t(i)] = uint8_t(value >> (24 - 8 * i));
  }
  void PutByte(uint32_t address, uint8_t value) { bytes_[address] = value; }
  void PutText(uint32_t address, const std::string& text) {
    for (size_t i = 0; i <= text.size(); ++i) bytes_[address + uint32_t(i)] = i < text.size() ? uint8_t(text[i]) : 0;
  }
  bool Readable(uint32_t address, uint32_t length) const {
    for (uint32_t i = 0; i < length; ++i) {
      if (bytes_.count(address + i) == 0) return false;
    }
    return true;
  }
  uint32_t U8(uint32_t address) const { return bytes_.at(address); }
  uint32_t U32(uint32_t address) const {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) value = (value << 8) | bytes_.at(address + i);
    return value;
  }

 private:
  std::map<uint32_t, uint8_t> bytes_;
};

inline constexpr uint32_t kNames = 0x40000000;

inline void PutName(Bytes& memory, uint32_t object, uint32_t index, const std::string& name) {
  namespace layout = eck::roster::layout;
  const uint32_t entry = 0x41000000 + index * 0x100;
  memory.Put(kNames + index * 4, entry);
  memory.Put(entry + layout::kNameFlags, 0);
  memory.PutText(entry + layout::kNameText, name);
  memory.Put(object + layout::kObjectName, index);
}

// A function object named `name` owned by a class object named `owner`.
inline void PutFunction(Bytes& memory, uint32_t function, uint32_t name_index, const std::string& name, uint32_t owner,
                        uint32_t owner_index, const std::string& owner_name) {
  PutName(memory, function, name_index, name);
  memory.Put(function + eck::roster::layout::kObjectOuter, owner);
  PutName(memory, owner, owner_index, owner_name);
}

}  // namespace guest_model
