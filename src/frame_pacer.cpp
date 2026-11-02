#include "frame_pacer.h"

#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(eck_frame_pacing, true, "El Chavo Kart",
                    "Hold the game's swaps to 60 per second; the title presents without VSync and paces itself to 62");

namespace eck {
namespace {

using namespace std::chrono_literals;

constexpr auto kSpinWindow = 500us;

FramePacer g_pacer;
std::mutex g_pacer_mutex;

}  // namespace

#if defined(_WIN32)

void WaitUntil(PacerClock::time_point deadline) {
  thread_local const HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto coarse = deadline - PacerClock::now() - kSpinWindow;
  if (coarse > PacerClock::duration::zero()) {
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -std::chrono::duration_cast<std::chrono::nanoseconds>(coarse).count() / 100;
      if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
      }
    } else {
      std::this_thread::sleep_for(coarse);
    }
  }
  while (PacerClock::now() < deadline) {
    std::this_thread::yield();
  }
}

#else

void WaitUntil(PacerClock::time_point deadline) {
  std::this_thread::sleep_until(deadline - kSpinWindow);
  while (PacerClock::now() < deadline) {
    std::this_thread::yield();
  }
}

#endif

void PaceGuestSwap() {
  if (!REXCVAR_GET(eck_frame_pacing)) {
    return;
  }
  std::optional<PacerClock::time_point> deadline;
  {
    std::lock_guard lock(g_pacer_mutex);
    deadline = g_pacer.Next(kFrameRate, PacerClock::now());
  }
  if (deadline) {
    WaitUntil(*deadline);
  }
}

}  // namespace eck
