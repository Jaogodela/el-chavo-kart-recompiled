#pragma once

// DEBUG_SAULO: temporary frame-pacing telemetry. Remove with debug_telemetry.cpp during final review.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace eck::debug {

inline constexpr double kEngineDeltaFloor = 0.0005;
inline constexpr double kSafeDeltaFloor = 0.0025;
inline constexpr double kHitchSeconds = 0.1;
// UWorld::Tick clamps the delta at 0.4 s, so longer gaps cannot be gameplay timing.
inline constexpr double kLoadingStallSeconds = 0.3;
inline constexpr double kGpuFencePct = 25.0;
inline constexpr int kWarmupFrames = 30;
inline constexpr int kMinValidFrames = 10;
inline constexpr int kMinFramesForVerdict = 60;
inline constexpr int kTickGroups = 5;

inline std::string Fmt(const char* format, double a) {
  char text[64];
  std::snprintf(text, sizeof text, format, a);
  return text;
}

struct Stat {
  uint64_t n = 0;
  double sum = 0;
  double lo = 1e300;
  double hi = 0;

  void Add(double v) {
    ++n;
    sum += v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  void Merge(const Stat& other) {
    n += other.n;
    sum += other.sum;
    lo = std::min(lo, other.lo);
    hi = std::max(hi, other.hi);
  }
  double avg() const { return n ? sum / double(n) : 0.0; }
  double min() const { return n ? lo : 0.0; }
  double max() const { return n ? hi : 0.0; }
};

// 0.05 ms bins up to 100 ms; the last bin holds everything slower.
class Histogram {
 public:
  static constexpr int kBins = 2000;
  static constexpr double kBinMs = 0.05;

  void Add(double ms) {
    stat_.Add(ms);
    const int bin = std::clamp(int(ms / kBinMs), 0, kBins);
    ++bins_[size_t(bin)];
  }
  double Percentile(double fraction) const {
    if (!stat_.n) {
      return 0.0;
    }
    const uint64_t target = std::max<uint64_t>(1, uint64_t(std::ceil(fraction * double(stat_.n))));
    uint64_t seen = 0;
    for (int i = 0; i <= kBins; ++i) {
      seen += bins_[size_t(i)];
      if (seen >= target) {
        return std::min((i + 1) * kBinMs, stat_.max());
      }
    }
    return stat_.max();
  }
  const Stat& stat() const { return stat_; }

 private:
  Stat stat_;
  std::array<uint32_t, kBins + 1> bins_{};
};

struct DeltaCounts {
  uint64_t below_engine_floor = 0;
  uint64_t below_safe_floor = 0;
  uint64_t over_20ms = 0;
  uint64_t over_33ms = 0;
  uint64_t over_50ms = 0;
  uint64_t over_100ms = 0;
  uint64_t console_30hz = 0;

  void Add(double seconds) {
    below_engine_floor += seconds < kEngineDeltaFloor;
    below_safe_floor += seconds < kSafeDeltaFloor;
    over_20ms += seconds > 0.020;
    over_33ms += seconds > 1.0 / 30.0 + 0.00005;
    over_50ms += seconds > 0.050;
    over_100ms += seconds > 0.100;
    console_30hz += std::fabs(seconds - 1.0 / 30.0) < 0.0005;
  }
  void Merge(const DeltaCounts& o) {
    below_engine_floor += o.below_engine_floor;
    below_safe_floor += o.below_safe_floor;
    over_20ms += o.over_20ms;
    over_33ms += o.over_33ms;
    over_50ms += o.over_50ms;
    over_100ms += o.over_100ms;
    console_30hz += o.console_30hz;
  }
};

struct Mode {
  int selected = 60;
  int cap = 60;
  bool unlimited = false;
  double target_ms() const { return 1000.0 / cap; }
};

struct Window {
  uint64_t loop_ticks = 0;
  uint64_t engine_ticks = 0;
  uint64_t world_ticks = 0;
  uint64_t flips = 0;
  uint64_t swaps = 0;
  uint64_t prepares = 0;
  uint64_t engine_presents = 0;
  uint64_t d3d_swaps = 0;
  std::array<uint64_t, kTickGroups> groups{};

  // Frames of continuous gameplay past the warm-up; loading, streaming and stalls never count here.
  uint64_t valid_frames = 0;
  uint64_t valid_swaps = 0;
  Stat delta_ms;
  Stat engine_delta_ms;
  DeltaCounts deltas;
  Histogram frame_ms;
  uint64_t slow_frames = 0;
  uint64_t stalls = 0;
  uint64_t loading_stalls = 0;
  double loading_stall_ms = 0;

  Stat world_ms;
  Stat engine_tick_ms;
  Stat render_sync_ms;
  Stat swap_ms;

  Stat limiter_requested_ms;
  Stat limiter_actual_ms;
  double limiter_overshoot_max_ms = 0;
  double limiter_undershoot_max_ms = 0;
  uint64_t limiter_late = 0;
  uint64_t limiter_nowait = 0;
  uint64_t limiter_resets = 0;
  uint64_t rate_changes = 0;

  uint64_t physics_updates = 0;
  Stat physics_dt_ms;
  uint64_t timing_calls = 0;
  Stat substep_ms;
  Stat estimated_substeps;
  uint32_t max_iterations = 0;

  double game_seconds = 0;
  double wall_seconds = 0;
  uint64_t paused_ticks = 0;

  double actual_fps(double elapsed) const { return elapsed > 0 ? double(swaps) / elapsed : 0.0; }
  double gameplay_fps() const { return wall_seconds > 0 ? double(valid_frames) / wall_seconds : 0.0; }
};

// Elapsed game time over elapsed wall time while the world ran; below 1 is the game running slow.
inline double TimeRatio(double game, double wall) {
  return wall > 0 ? game / wall : 1.0;
}

inline bool HasEnoughFrames(const Window& w) {
  return w.valid_frames >= uint64_t(kMinValidFrames);
}

inline std::vector<std::string> DetectAnomalies(const Window& w, const Mode& mode, double elapsed, bool gameplay) {
  std::vector<std::string> found;
  if (!gameplay || !HasEnoughFrames(w)) {
    return found;
  }
  const std::string tag = " selected_fps=" + std::to_string(mode.selected);
  if (w.deltas.below_engine_floor) {
    found.push_back("delta_below_engine_floor count=" + std::to_string(w.deltas.below_engine_floor) + tag);
  } else if (w.deltas.below_safe_floor) {
    found.push_back("delta_below_safe_floor count=" + std::to_string(w.deltas.below_safe_floor) + tag);
  }
  if (mode.selected != 30 && w.deltas.console_30hz > 0 && w.deltas.console_30hz == w.delta_ms.n && w.delta_ms.n > 5) {
    found.push_back("delta_stuck_at_30hz count=" + std::to_string(w.deltas.console_30hz) + tag);
  }
  if (w.loading_stalls == 0 && w.valid_swaps > 4) {
    const int64_t world_vs_swaps = int64_t(w.valid_frames) - int64_t(w.valid_swaps);
    if (std::llabs(world_vs_swaps) > std::max<int64_t>(2, int64_t(w.valid_swaps / 20))) {
      found.push_back("world_ticks_vs_swaps world=" + std::to_string(w.valid_frames) + " swaps=" +
                      std::to_string(w.valid_swaps) + (world_vs_swaps > 0 ? " multiple_world_ticks_per_render"
                                                                          : " renders_without_world_tick") + tag);
    }
    const int64_t flips_vs_swaps = int64_t(w.flips) - int64_t(w.swaps);
    if (std::llabs(flips_vs_swaps) > std::max<int64_t>(2, int64_t(w.swaps / 20))) {
      found.push_back("flips_vs_swaps flips=" + std::to_string(w.flips) + " swaps=" + std::to_string(w.swaps) + tag);
    }
  }
  if (!mode.unlimited && w.swaps > 4) {
    const double actual = w.actual_fps(elapsed);
    if (actual < 0.9 * mode.cap) {
      found.push_back("performance_below_target actual=" + Fmt("%.1f", actual) + " cap=" + std::to_string(mode.cap) +
                      tag);
    }
    if (w.limiter_late * 2 > w.swaps && w.swaps > 10) {
      found.push_back("limiter_late_majority late=" + std::to_string(w.limiter_late) + " frames=" +
                      std::to_string(w.swaps) + tag);
    }
  }
  if (w.physics_updates > 0 && w.delta_ms.n > 4 && w.physics_dt_ms.avg() > 0 &&
      std::fabs(w.physics_dt_ms.avg() - w.delta_ms.avg()) > 0.2 * w.delta_ms.avg()) {
    found.push_back("physics_dt_differs avg_physics_ms=" + Fmt("%.2f", w.physics_dt_ms.avg()) + " avg_delta_ms=" +
                    Fmt("%.2f", w.delta_ms.avg()) + tag);
  }
  return found;
}

enum class Verdict { kPass, kWarn, kFail, kInfo, kInsufficient };

inline const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::kPass:
      return "PASS";
    case Verdict::kWarn:
      return "WARN";
    case Verdict::kFail:
      return "FAIL";
    case Verdict::kInfo:
      return "INFO";
    default:
      return "INSUFFICIENT";
  }
}

struct Result {
  Verdict timing = Verdict::kInsufficient;
  Verdict performance = Verdict::kInsufficient;
  std::string timing_reason;
  std::string performance_reason;
};

inline void AddReason(std::string& reasons, const char* reason) {
  reasons += reasons.empty() ? reason : std::string("+") + reason;
}

// Timing answers whether game time followed real time during continuous gameplay: FAIL when it is off by more than 2%,
// a delta fell under the engine floor, the delta is stuck at 30 Hz, or world ticks do not follow renders by more than
// 10%. WARN above 0.5% drift, more than 1% of deltas under the safe floor, or an anomaly. Loading stalls never count.
// Performance answers whether the selected rate was sustained; Unlimited only reports the rate reached.
inline Result Judge(const Window& total, const Histogram& frames, const Mode& mode, uint64_t anomalies) {
  Result result;
  if (total.valid_frames < uint64_t(kMinFramesForVerdict)) {
    result.timing_reason = "not_enough_gameplay_frames";
    result.performance_reason = "not_enough_gameplay_frames";
    return result;
  }
  const double ratio = TimeRatio(total.game_seconds, total.wall_seconds);
  const double mismatch = total.valid_swaps
                              ? std::fabs(double(total.valid_frames) - double(total.valid_swaps)) /
                                    double(total.valid_swaps)
                              : 0.0;
  result.timing = Verdict::kPass;
  if (total.wall_seconds >= 2.0 && std::fabs(ratio - 1.0) > 0.02) {
    result.timing = Verdict::kFail;
    AddReason(result.timing_reason, "game_time_off_real_time");
  }
  if (total.deltas.below_engine_floor) {
    result.timing = Verdict::kFail;
    AddReason(result.timing_reason, "delta_below_engine_floor");
  }
  if (mode.selected != 30 && total.deltas.console_30hz == total.delta_ms.n && total.delta_ms.n > 30) {
    result.timing = Verdict::kFail;
    AddReason(result.timing_reason, "delta_stuck_at_30hz");
  }
  if (mismatch > 0.10) {
    result.timing = Verdict::kFail;
    AddReason(result.timing_reason, "world_ticks_not_following_renders");
  }
  if (result.timing == Verdict::kPass) {
    if (total.wall_seconds >= 2.0 && std::fabs(ratio - 1.0) > 0.005) {
      result.timing = Verdict::kWarn;
      AddReason(result.timing_reason, "small_drift");
    }
    if (total.deltas.below_safe_floor * 100 > total.valid_frames) {
      result.timing = Verdict::kWarn;
      AddReason(result.timing_reason, "deltas_under_safe_floor");
    }
    if (anomalies) {
      result.timing = Verdict::kWarn;
      AddReason(result.timing_reason, "anomalies_reported");
    }
  }

  if (mode.unlimited) {
    result.performance = Verdict::kInfo;
    result.performance_reason = "unlimited_reports_achieved_fps_only";
    return result;
  }
  result.performance = Verdict::kPass;
  if (total.gameplay_fps() < 0.95 * mode.cap) {
    result.performance = Verdict::kWarn;
    AddReason(result.performance_reason, "target_not_sustained");
  }
  if (frames.Percentile(0.99) > 2.0 * mode.target_ms()) {
    result.performance = Verdict::kWarn;
    AddReason(result.performance_reason, "frame_time_spikes");
  }
  if (total.stalls) {
    result.performance = Verdict::kWarn;
    AddReason(result.performance_reason, "hitches");
  }
  return result;
}

enum class Bottleneck { kLimiter, kGameCpu, kRenderCpu, kGpuOrD3d12, kSwap, kMixed, kUnknown };

inline const char* BottleneckName(Bottleneck b) {
  switch (b) {
    case Bottleneck::kLimiter:
      return "LIMITER";
    case Bottleneck::kGameCpu:
      return "GAME_CPU";
    case Bottleneck::kRenderCpu:
      return "RENDER_CPU";
    case Bottleneck::kGpuOrD3d12:
      return "GPU_OR_D3D12";
    case Bottleneck::kSwap:
      return "SWAP";
    case Bottleneck::kMixed:
      return "MIXED";
    default:
      return "UNKNOWN";
  }
}

// Per-frame averages of one reporting interval. The command processor figures come from the graphics layer; without
// them the graphics side is never named.
struct PerfInputs {
  double fps = 0;
  int cap = 0;
  double frame_ms = 0;
  double world_ms = 0;
  double engine_ms = 0;
  double sync_ms = 0;
  double swap_ms = 0;
  bool cp_available = false;
  double cp_idle_pct = 0;
  double cp_fence_pct = 0;
};

// A long gap with the command processor parked on the GPU fence is GPU time this process did not get, not a load.
inline const char* StallKind(bool cp_available, double cp_fence_pct) {
  return cp_available && cp_fence_pct >= kGpuFencePct ? "gpu_fence_wait" : "loading_or_streaming";
}

inline Bottleneck Classify(const PerfInputs& in) {
  if (in.frame_ms <= 0 || in.fps <= 0) {
    return Bottleneck::kUnknown;
  }
  if (in.cap > 0 && in.fps >= 0.95 * in.cap) {
    return Bottleneck::kLimiter;
  }
  const double render_cpu_ms = std::max(0.0, in.engine_ms - in.world_ms);
  const bool game = in.world_ms > 0.5 * in.frame_ms;
  const bool render = render_cpu_ms > 0.5 * in.frame_ms;
  const bool gpu =
      in.cp_available && (in.cp_fence_pct >= kGpuFencePct || (in.sync_ms > 0.4 * in.frame_ms && in.cp_idle_pct < 20.0));
  const bool swap = in.swap_ms > 0.4 * in.frame_ms;
  const int candidates = int(game) + int(render) + int(gpu) + int(swap);
  if (candidates == 0) {
    return Bottleneck::kUnknown;
  }
  if (candidates > 1) {
    return Bottleneck::kMixed;
  }
  return game ? Bottleneck::kGameCpu : render ? Bottleneck::kRenderCpu : gpu ? Bottleneck::kGpuOrD3d12
                                                                               : Bottleneck::kSwap;
}

inline void Merge(Window& into, const Window& w) {
  into.loop_ticks += w.loop_ticks;
  into.engine_ticks += w.engine_ticks;
  into.world_ticks += w.world_ticks;
  into.flips += w.flips;
  into.swaps += w.swaps;
  into.prepares += w.prepares;
  into.engine_presents += w.engine_presents;
  into.d3d_swaps += w.d3d_swaps;
  for (int i = 0; i < kTickGroups; ++i) {
    into.groups[size_t(i)] += w.groups[size_t(i)];
  }
  into.valid_frames += w.valid_frames;
  into.valid_swaps += w.valid_swaps;
  into.delta_ms.Merge(w.delta_ms);
  into.engine_delta_ms.Merge(w.engine_delta_ms);
  into.deltas.Merge(w.deltas);
  into.slow_frames += w.slow_frames;
  into.stalls += w.stalls;
  into.loading_stalls += w.loading_stalls;
  into.loading_stall_ms += w.loading_stall_ms;
  into.world_ms.Merge(w.world_ms);
  into.engine_tick_ms.Merge(w.engine_tick_ms);
  into.render_sync_ms.Merge(w.render_sync_ms);
  into.swap_ms.Merge(w.swap_ms);
  into.limiter_requested_ms.Merge(w.limiter_requested_ms);
  into.limiter_actual_ms.Merge(w.limiter_actual_ms);
  into.limiter_overshoot_max_ms = std::max(into.limiter_overshoot_max_ms, w.limiter_overshoot_max_ms);
  into.limiter_late += w.limiter_late;
  into.limiter_nowait += w.limiter_nowait;
  into.limiter_resets += w.limiter_resets;
  into.rate_changes += w.rate_changes;
  into.physics_updates += w.physics_updates;
  into.physics_dt_ms.Merge(w.physics_dt_ms);
  into.timing_calls += w.timing_calls;
  into.game_seconds += w.game_seconds;
  into.wall_seconds += w.wall_seconds;
  into.paused_ticks += w.paused_ticks;
}

}  // namespace eck::debug
