#pragma once

// What cursor_input.h reads from the SDK header of the same name, so the cursor logic builds without the SDK.

#include <cstdint>

namespace rex {

template <typename T>
struct be {
  T value = 0;
  T get() const { return value; }
  operator T() const { return value; }
};

namespace input {

constexpr int32_t kThumbDeadzone = 7849;
constexpr uint8_t kTriggerThreshold = 30;

struct X_INPUT_GAMEPAD {
  be<uint16_t> buttons;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  be<int16_t> thumb_lx;
  be<int16_t> thumb_ly;
  be<int16_t> thumb_rx;
  be<int16_t> thumb_ry;
};

}  // namespace input
}  // namespace rex
