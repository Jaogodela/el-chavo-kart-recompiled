#pragma once

#include <chrono>
#include <optional>

namespace eck {

inline constexpr int kFrameRate = 60;

using PacerClock = std::chrono::steady_clock;

inline PacerClock::duration FramePeriod(int fps) {
  return std::chrono::duration_cast<PacerClock::duration>(std::chrono::duration<double>(1.0 / fps));
}

// The schedule of the frame pacer. Next() gives the time the caller has to wait for, if any; a frame that ran a whole
// period late restarts the schedule instead of being caught up.
class FramePacer {
 public:
  std::optional<PacerClock::time_point> Next(int fps, PacerClock::time_point now) {
    const auto period = FramePeriod(fps);
    if (!armed_) {
      armed_ = true;
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
  PacerClock::time_point target_;
};

// Blocks the calling thread until the deadline: a high-resolution timer for most of it, then a short spin.
void WaitUntil(PacerClock::time_point deadline);

// Called on every guest VdSwap: waits until the next 60 Hz deadline unless eck_frame_pacing is off.
void PaceGuestSwap();

}  // namespace eck
