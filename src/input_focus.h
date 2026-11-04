#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace eck {

// Every game process on the PC reads the same controllers, so local input belongs to the window with keyboard focus.
// It starts without focus: a window that opens behind another one never reports losing it.
class InputFocus {
 public:
  bool accepted() const { return focused_.load(std::memory_order_acquire); }

  // Returns whether the focus changed.
  bool Update(bool focused) { return focused_.exchange(focused, std::memory_order_acq_rel) != focused; }

 private:
  std::atomic<bool> focused_{false};
};

inline bool GameReadsInput(bool focused, bool overlay_open) { return focused && !overlay_open; }

enum class MouseAction { kMove, kDown, kUp, kWheel };

// A release always passes, so a press that began with focus cannot stick.
inline bool MouseReachesUi(bool focused, MouseAction action) { return focused || action == MouseAction::kUp; }

template <size_t Buttons, size_t Sticks>
struct NavInput {
  std::array<bool, Buttons> down{};
  std::array<float, Sticks> amount{};
};

// The union of every connected pad, so any of them navigates. Neutral without focus, and with no pad connected, which
// is what releases a key held when the focus left or its pad was unplugged.
template <size_t Buttons, size_t Sticks, typename Pads, typename ReadButton, typename ReadStick>
NavInput<Buttons, Sticks> CombinePads(bool focused, const Pads& pads, ReadButton read_button, ReadStick read_stick) {
  NavInput<Buttons, Sticks> input;
  if (!focused) {
    return input;
  }
  for (const auto& pad : pads) {
    for (size_t i = 0; i < Buttons; ++i) {
      input.down[i] = input.down[i] || read_button(pad, i);
    }
    for (size_t i = 0; i < Sticks; ++i) {
      input.amount[i] = std::max(input.amount[i], read_stick(pad, i));
    }
  }
  return input;
}

// DEBUG_SAULO
inline std::string InputFocusLogLine(bool focused, std::string_view instance, uintptr_t window) {
  char handle[32];
  std::snprintf(handle, sizeof(handle), "0x%llX", static_cast<unsigned long long>(window));
  std::string line = focused ? "[DEBUG_SAULO][INPUT] focus=active controller_input=enabled input_state_reset=yes"
                             : "[DEBUG_SAULO][INPUT] focus=inactive controller_input=suppressed";
  line += " instance=";
  line += instance.empty() ? std::string_view("none") : instance;
  line += " window=";
  line += handle;
  return line;
}

}  // namespace eck
