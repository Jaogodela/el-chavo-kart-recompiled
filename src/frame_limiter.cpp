#include "frame_limiter.h"

#include <atomic>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "debug_telemetry.h"

namespace eck {
namespace {

FrameLimiter g_limiter;
std::mutex g_limiter_mutex;
std::atomic<bool> g_local_instance_pacing{false};

}  // namespace

void SetLocalInstancePacing(bool active) { g_local_instance_pacing.store(active, std::memory_order_release); }

// A change of rate re-arms the limiter, so time spent at another rate is never waited for or caught up.
void PaceGuestFrame() {
  static int last_rate = -1;
  const int selected = ConfiguredFrameRate();
  const int rate = g_local_instance_pacing.load(std::memory_order_acquire) ? LocalInstanceLimiterRate(selected) : LimiterRate(selected);
  std::optional<LimiterClock::time_point> deadline;
  const auto before = LimiterClock::now();
  {
    std::lock_guard lock(g_limiter_mutex);
    deadline = g_limiter.Next(rate, before);
  }
  const bool reset = rate != last_rate;
  last_rate = rate;
  if (deadline) {
    WaitUntil(*deadline);
    const auto after = LimiterClock::now();
    const auto ms = [](LimiterClock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    debug::OnLimiterWait(ms(*deadline - before), ms(after - before), ms(after - *deadline), true, false, false);
  } else {
    debug::OnLimiterWait(0, 0, 0, false, reset, !reset);
  }
}

}  // namespace eck

// GetMaxTickRate: the engine's own 62 FPS sleep is off, so the limiter above is the only frame limit.
REX_EXTERN(__imp__sub_825A4D70);
REX_HOOK_RAW(sub_825A4D70) {
  ctx.f1.f64 = 0.0;
}

// The flip scheduler: the present interval is in bits 8-11 of r3. Zero flips at once instead of at the title's default
// two vblanks, which the 60 Hz guest vblank turns into 30 FPS; the limiter paces every rate.
REX_EXTERN(__imp__sub_82B583B0);
REX_HOOK_RAW(sub_82B583B0) {
  const uint32_t requested = (ctx.r3.u32 & eck::kPresentIntervalMask) >> 8;
  ctx.r3.u32 &= ~eck::kPresentIntervalMask;
  eck::debug::OnFlip(requested, 0);
  __imp__sub_82B583B0(ctx, base);
}
