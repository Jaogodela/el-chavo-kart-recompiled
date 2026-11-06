#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eck {

inline constexpr int kFrameRateOriginal = 30;
inline constexpr int kFrameRateDefault = 60;
inline constexpr int kFrameRateUnlimited = 0;
// Unlimited still holds frames to this rate, keeping the delta clear of the 2.5 ms physics sub-step floor.
inline constexpr int kUnlimitedFrameRate = 300;
inline constexpr uint32_t kPresentIntervalMask = 0xF00;
inline constexpr int kFrameRates[] = {30, 60, 90, 120, 144, 165, 240, kFrameRateUnlimited};

inline bool IsFrameRateChoice(int fps) {
  return std::find(std::begin(kFrameRates), std::end(kFrameRates), fps) != std::end(kFrameRates);
}

// The setting's value, or the default when it holds anything else.
inline int FrameRateOrDefault(int fps) {
  return IsFrameRateChoice(fps) ? fps : kFrameRateDefault;
}

// The stored text as a rate; anything that is not one of the choices, an empty value included, reads as 60.
inline int FrameRateFromText(const std::string& text) {
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  return !text.empty() && end != nullptr && *end == '\0' && value >= 0 && value <= 100000
             ? FrameRateOrDefault(static_cast<int>(value))
             : kFrameRateDefault;
}

inline std::string FrameRateLabel(int fps) {
  if (fps == kFrameRateUnlimited) {
    return "Unlimited";
  }
  return fps == kFrameRateOriginal ? "30 Original" : std::to_string(fps);
}

// The launcher's choices: the stored value and its label.
inline std::vector<std::pair<std::string, std::string>> FrameRateChoices() {
  std::vector<std::pair<std::string, std::string>> choices;
  for (const int fps : kFrameRates) {
    choices.emplace_back(std::to_string(fps), FrameRateLabel(fps));
  }
  return choices;
}

// The rate the limiter holds frames to.
inline int LimiterRate(int fps) {
  return fps == kFrameRateUnlimited ? kUnlimitedFrameRate : fps;
}

// Two instances on one GPU, set by tools/online-instance.ps1 only: past this rate the focused one leaves the other a few
// frames per second, and that one's race stops with them. Separate PCs never come here.
inline constexpr int kLocalInstanceRate = 60;

inline bool LocalInstancePacing(const char* instance_label, int connected_peers) {
  return instance_label != nullptr && *instance_label != '\0' && connected_peers > 0;
}

inline int LocalInstanceLimiterRate(int fps) {
  return std::min(LimiterRate(fps), kLocalInstanceRate);
}


inline std::string LocalInstancePacingLine(bool active, int fps) {
  const std::string requested = fps == kFrameRateUnlimited ? "unlimited" : std::to_string(fps);
  const std::string effective = active ? std::to_string(LocalInstanceLimiterRate(fps)) : requested;
  return std::string("[ONLINE-PACING] local_multi_instance=") + (active ? "yes" : "no") + " requested=" + requested +
         " effective=" + effective;
}

using LimiterClock = std::chrono::steady_clock;

inline LimiterClock::duration FramePeriod(int fps) {
  if (fps <= 0) {
    return LimiterClock::duration::zero();
  }
  return std::chrono::duration_cast<LimiterClock::duration>(std::chrono::duration<double>(1.0 / fps));
}

// The schedule of the frame limiter. Next() gives the time the caller has to wait for, if any; a frame that ran a whole
// period late restarts the schedule instead of being caught up.
class FrameLimiter {
 public:
  std::optional<LimiterClock::time_point> Next(int fps, LimiterClock::time_point now) {
    const auto period = FramePeriod(fps);
    if (period == LimiterClock::duration::zero()) {
      armed_ = false;
      return std::nullopt;
    }
    if (!armed_ || fps != fps_) {
      armed_ = true;
      fps_ = fps;
      target_ = now + period;
      return std::nullopt;
    }
    const auto deadline = target_;
    target_ = now - deadline > period ? now + period : deadline + period;
    if (deadline <= now) {
      return std::nullopt;
    }
    return deadline;
  }

 private:
  bool armed_ = false;
  int fps_ = 0;
  LimiterClock::time_point target_;
};

// Blocks the calling thread until the deadline: a high-resolution timer for most of it, then a short spin.
void WaitUntil(LimiterClock::time_point deadline);

// The selected frame rate, or the default when the cvar holds something else.
int ConfiguredFrameRate();

// Called on every guest VdSwap: waits for the next frame time of the selected rate.
void PaceGuestFrame();
void SetLocalInstancePacing(bool active);

}  // namespace eck
