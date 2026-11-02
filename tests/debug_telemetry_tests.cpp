// DEBUG_SAULO: tests of the temporary telemetry logic. Remove with debug_telemetry_core.h.

#include <cmath>
#include <cstdio>
#include <string>

#include "debug_telemetry_core.h"

using namespace eck::debug;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

bool HasAnomaly(const std::vector<std::string>& found, const char* kind) {
  for (const std::string& line : found) {
    if (line.find(kind) != std::string::npos) {
      return true;
    }
  }
  return false;
}

Mode ModeFor(int fps) {
  Mode mode;
  mode.selected = fps;
  mode.cap = fps ? fps : 300;
  mode.unlimited = fps == 0;
  return mode;
}

// Seconds of healthy gameplay at the given real frame rate.
Window Healthy(double fps, double seconds, Histogram* frames = nullptr) {
  Window w;
  const int count = int(fps * seconds);
  const double delta = 1.0 / fps;
  for (int i = 0; i < count; ++i) {
    w.loop_ticks++;
    w.engine_ticks++;
    w.world_ticks++;
    w.flips++;
    w.swaps++;
    w.valid_frames++;
    w.valid_swaps++;
    w.delta_ms.Add(delta * 1000.0);
    w.deltas.Add(delta);
    w.frame_ms.Add(delta * 1000.0);
    if (frames) {
      frames->Add(delta * 1000.0);
    }
    w.physics_updates++;
    w.physics_dt_ms.Add(delta * 1000.0);
    w.game_seconds += delta;
    w.wall_seconds += delta;
  }
  return w;
}

void TestHistogram() {
  std::printf("histogram\n");
  Histogram h;
  for (int i = 1; i <= 100; ++i) {
    h.Add(i * 0.1);
  }
  CHECK(h.stat().n == 100);
  CHECK(std::fabs(h.stat().avg() - 5.05) < 1e-9);
  CHECK(std::fabs(h.Percentile(0.5) - 5.0) < 0.1);
  CHECK(std::fabs(h.Percentile(0.95) - 9.5) < 0.1);
  CHECK(std::fabs(h.Percentile(0.99) - 9.9) < 0.1);
  Histogram empty;
  CHECK(empty.Percentile(0.99) == 0.0);
}

void TestDeltaCounts() {
  std::printf("delta counters\n");
  DeltaCounts c;
  for (const double d : {0.0004, 0.002, 0.0042, 0.0167, 0.025, 0.0334, 0.06, 0.12, 1.0 / 30.0}) {
    c.Add(d);
  }
  CHECK(c.below_engine_floor == 1);
  CHECK(c.below_safe_floor == 2);
  CHECK(c.over_20ms == 5);
  CHECK(c.over_33ms == 3);
  CHECK(c.over_50ms == 2);
  CHECK(c.over_100ms == 1);
  CHECK(c.console_30hz == 2);
}

void TestStableRatesPass() {
  std::printf("stable rates\n");
  for (const int fps : {30, 60, 90, 120, 144, 165, 240}) {
    Histogram frames;
    const Window w = Healthy(fps, 20.0, &frames);
    const Mode mode = ModeFor(fps);
    CHECK(DetectAnomalies(w, mode, 20.0, true).empty());
    const Result result = Judge(w, frames, mode, 0);
    CHECK(result.timing == Verdict::kPass);
    CHECK(result.performance == Verdict::kPass);
  }
}

void TestUnlimitedIsNotATarget() {
  std::printf("unlimited\n");
  const Mode mode = ModeFor(0);
  Histogram frames;
  const Window w = Healthy(132.0, 20.0, &frames);
  CHECK(!HasAnomaly(DetectAnomalies(w, mode, 20.0, true), "performance_below_target"));
  CHECK(DetectAnomalies(w, mode, 20.0, true).empty());
  const Result result = Judge(w, frames, mode, 0);
  CHECK(result.timing == Verdict::kPass);
  CHECK(result.performance == Verdict::kInfo);
  CHECK(result.performance_reason.find("unlimited") != std::string::npos);
  const Window slow = Healthy(55.0, 20.0, &frames);
  CHECK(Judge(slow, frames, mode, 0).performance == Verdict::kInfo);
  CHECK(Judge(slow, frames, mode, 0).timing == Verdict::kPass);

  Window fixed = Healthy(80.0, 20.0);
  CHECK(HasAnomaly(DetectAnomalies(fixed, ModeFor(120), 20.0, true), "performance_below_target"));
}

void TestUnlimitedCeilingJitter() {
  std::printf("unlimited ceiling jitter\n");
  const Mode mode = ModeFor(0);
  CHECK(mode.cap == 300 && std::fabs(mode.target_ms() - 3.3333) < 0.001);
  Histogram frames;
  Window w;
  for (int i = 0; i < 3000; ++i) {
    const double delta = (3.3333 + ((i % 7) - 3) * 0.1) / 1000.0;
    w.swaps++;
    w.valid_swaps++;
    w.world_ticks++;
    w.valid_frames++;
    w.flips++;
    w.delta_ms.Add(delta * 1000.0);
    w.deltas.Add(delta);
    w.frame_ms.Add(delta * 1000.0);
    frames.Add(delta * 1000.0);
    w.game_seconds += delta;
    w.wall_seconds += delta;
  }
  CHECK(w.deltas.below_safe_floor == 0);
  CHECK(!HasAnomaly(DetectAnomalies(w, mode, 10.0, true), "delta_below_safe_floor"));
  const Result result = Judge(w, frames, mode, 0);
  CHECK(result.timing == Verdict::kPass);
  CHECK(result.performance == Verdict::kInfo);
  w.deltas.Add(0.0022);
  CHECK(HasAnomaly(DetectAnomalies(w, mode, 10.0, true), "delta_below_safe_floor"));
}

void TestFixedTargetPerformanceWarn() {
  std::printf("fixed target performance\n");
  const Mode mode = ModeFor(120);
  Histogram frames;
  Window w = Healthy(80.0, 20.0, &frames);
  const Result result = Judge(w, frames, mode, 0);
  CHECK(result.timing == Verdict::kPass);
  CHECK(result.performance == Verdict::kWarn);
  CHECK(result.performance_reason.find("target_not_sustained") != std::string::npos);
}

void TestStable120TimingPassesWithSlowerInterval() {
  std::printf("stable 120 timing with a slow interval\n");
  const Mode mode = ModeFor(120);
  Histogram frames;
  Window total = Healthy(120.0, 20.0, &frames);
  Merge(total, Healthy(70.0, 5.0, &frames));
  const Result result = Judge(total, frames, mode, 0);
  CHECK(result.timing == Verdict::kPass);
  CHECK(result.performance != Verdict::kPass);
}

void TestLoadingStallDoesNotFailTiming() {
  std::printf("loading stalls\n");
  const Mode mode = ModeFor(60);
  Histogram frames;
  Window total = Healthy(60.0, 30.0, &frames);
  total.loading_stalls = 2;
  total.loading_stall_ms = 2300;
  const Result result = Judge(total, frames, mode, 0);
  CHECK(result.timing == Verdict::kPass);

  Window window = Healthy(60.0, 1.0);
  window.loading_stalls = 1;
  window.loading_stall_ms = 1000;
  window.valid_swaps = 40;
  CHECK(!HasAnomaly(DetectAnomalies(window, mode, 1.0, true), "world_ticks_vs_swaps"));

  Window drifting = total;
  drifting.game_seconds *= 0.95;
  CHECK(Judge(drifting, frames, mode, 0).timing == Verdict::kFail);
}

void TestWarmupGate() {
  std::printf("warm-up gate\n");
  Window w = Healthy(60.0, 0.1);
  w.deltas.Add(0.0003);
  w.delta_ms.Add(0.3);
  CHECK(w.valid_frames < uint64_t(kMinValidFrames));
  CHECK(DetectAnomalies(w, ModeFor(60), 1.0, true).empty());
  Histogram frames;
  const Result result = Judge(w, frames, ModeFor(60), 0);
  CHECK(result.timing == Verdict::kInsufficient);
  CHECK(result.performance == Verdict::kInsufficient);
}

void TestRealTimingFailures() {
  std::printf("real timing failures\n");
  const Mode mode = ModeFor(0);
  Histogram frames;
  Window w = Healthy(240.0, 10.0, &frames);
  for (int i = 0; i < 50; ++i) {
    w.deltas.Add(0.0003);
    w.delta_ms.Add(0.3);
  }
  CHECK(HasAnomaly(DetectAnomalies(w, mode, 10.0, true), "delta_below_engine_floor"));
  CHECK(Judge(w, frames, mode, 1).timing == Verdict::kFail);

  Window stuck;
  for (int i = 0; i < 120; ++i) {
    stuck.swaps++;
    stuck.valid_swaps++;
    stuck.world_ticks++;
    stuck.valid_frames++;
    stuck.flips++;
    stuck.delta_ms.Add(1000.0 / 30.0);
    stuck.deltas.Add(1.0 / 30.0);
    stuck.game_seconds += 1.0 / 60.0;
    stuck.wall_seconds += 1.0 / 60.0;
  }
  CHECK(HasAnomaly(DetectAnomalies(stuck, ModeFor(60), 2.0, true), "delta_stuck_at_30hz"));
  CHECK(!HasAnomaly(DetectAnomalies(stuck, ModeFor(30), 2.0, true), "delta_stuck_at_30hz"));
  CHECK(Judge(stuck, frames, ModeFor(60), 0).timing == Verdict::kFail);
  CHECK(Judge(stuck, frames, ModeFor(30), 0).timing != Verdict::kFail);

  Window mismatch = Healthy(120.0, 10.0, &frames);
  mismatch.world_ticks = 2400;
  mismatch.valid_frames = 2400;
  CHECK(HasAnomaly(DetectAnomalies(mismatch, ModeFor(120), 10.0, true), "multiple_world_ticks_per_render"));
  CHECK(Judge(mismatch, frames, ModeFor(120), 0).timing == Verdict::kFail);

  Window flips = Healthy(120.0, 1.0);
  flips.flips = 60;
  CHECK(HasAnomaly(DetectAnomalies(flips, ModeFor(120), 1.0, true), "flips_vs_swaps"));
}

void TestWarnLevels() {
  std::printf("warn levels\n");
  Histogram frames;
  Window total = Healthy(60.0, 20.0, &frames);
  Window drifting = total;
  drifting.game_seconds *= 0.99;
  CHECK(Judge(drifting, frames, ModeFor(60), 0).timing == Verdict::kWarn);
  CHECK(Judge(total, frames, ModeFor(60), 3).timing == Verdict::kWarn);
  Window hitched = total;
  hitched.stalls = 2;
  const Result result = Judge(hitched, frames, ModeFor(60), 0);
  CHECK(result.timing == Verdict::kPass);
  CHECK(result.performance == Verdict::kWarn);
  CHECK(result.performance_reason.find("hitches") != std::string::npos);
  CHECK(std::string(VerdictName(Verdict::kPass)) == "PASS" && std::string(VerdictName(Verdict::kFail)) == "FAIL" &&
        std::string(VerdictName(Verdict::kInfo)) == "INFO");
}

void TestBottleneckClassification() {
  std::printf("bottleneck\n");
  PerfInputs in;
  in.cap = 120;
  in.frame_ms = 12.5;
  in.fps = 80;
  CHECK(Classify(in) == Bottleneck::kUnknown);

  PerfInputs limiter = in;
  limiter.fps = 119;
  CHECK(Classify(limiter) == Bottleneck::kLimiter);

  PerfInputs game = in;
  game.world_ms = 9.0;
  game.engine_ms = 10.0;
  CHECK(Classify(game) == Bottleneck::kGameCpu);

  PerfInputs render = in;
  render.world_ms = 2.0;
  render.engine_ms = 10.0;
  CHECK(Classify(render) == Bottleneck::kRenderCpu);

  PerfInputs gpu = in;
  gpu.cp_available = true;
  gpu.cp_fence_pct = 40.0;
  CHECK(Classify(gpu) == Bottleneck::kGpuOrD3d12);

  PerfInputs sync = in;
  sync.cp_available = true;
  sync.cp_idle_pct = 5.0;
  sync.sync_ms = 8.0;
  CHECK(Classify(sync) == Bottleneck::kGpuOrD3d12);

  PerfInputs no_cp = in;
  no_cp.sync_ms = 8.0;
  CHECK(Classify(no_cp) == Bottleneck::kUnknown);

  PerfInputs swap = in;
  swap.swap_ms = 8.0;
  CHECK(Classify(swap) == Bottleneck::kSwap);

  PerfInputs mixed = game;
  mixed.cp_available = true;
  mixed.cp_fence_pct = 60.0;
  CHECK(Classify(mixed) == Bottleneck::kMixed);
  CHECK(std::string(BottleneckName(Bottleneck::kGpuOrD3d12)) == "GPU_OR_D3D12");
}

void TestTimeRatio() {
  std::printf("time ratio\n");
  CHECK(std::fabs(TimeRatio(29.998, 30.0) - 0.99993) < 1e-4);
  CHECK(TimeRatio(1.0, 0.0) == 1.0);
}

}  // namespace

int main() {
  TestHistogram();
  TestDeltaCounts();
  TestStableRatesPass();
  TestUnlimitedIsNotATarget();
  TestUnlimitedCeilingJitter();
  TestFixedTargetPerformanceWarn();
  TestStable120TimingPassesWithSlowerInterval();
  TestLoadingStallDoesNotFailTiming();
  TestWarmupGate();
  TestRealTimingFailures();
  TestWarnLevels();
  TestBottleneckClassification();
  TestTimeRatio();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
