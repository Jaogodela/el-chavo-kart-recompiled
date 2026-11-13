// Frame-pacing telemetry written to the session log.

#include "debug_telemetry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "debug_telemetry_core.h"
#include "frame_limiter.h"
#include "guest_memory.h"
#include "internal_render_scale.h"
#include "online_game_hook.h"
#include "settings.h"
#include "window_host.h"

namespace eck::debug {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kGDeltaTime = 0x83C04F98;
constexpr uint32_t kGWorld = 0x83CE2DD0;
constexpr uint32_t kWorldInfoTimeSeconds = 964;
constexpr double kReportSeconds = 1.0;
constexpr double kIdleSeconds = 3.0;
constexpr int kLongWindowReports = 10;

struct State {
  std::mutex mutex;
  Window window;
  Window session;
  Histogram session_frames;
  Mode mode;
  bool mode_known = false;
  bool session_open = false;
  Clock::time_point window_start = Clock::now();
  Clock::time_point session_start;
  Clock::time_point last_world;
  Clock::time_point last_swap;
  bool have_world = false;
  bool have_swap = false;
  uint64_t session_anomalies = 0;
  uint32_t last_requested_interval = 0xFF;
  double last_physics_dt_ms = 0;
  double long_game = 0;
  double long_wall = 0;
  int long_reports = 0;
  int warm_count = 0;
  std::array<uint32_t, 7> bottlenecks{};
  Stat session_cp_idle_pct;
  Stat session_cp_fence_pct;
  Stat session_unaccounted_ms;
};

State g;

bool Warm() {
  return g.warm_count >= kWarmupFrames;
}

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

std::string Fixed(double v, int digits) {
  char text[48];
  std::snprintf(text, sizeof text, "%.*f", digits, v);
  return text;
}

std::string Selected(int fps) {
  return fps == kFrameRateUnlimited ? "unlimited" : std::to_string(fps);
}

const char* IntervalName(uint32_t interval) {
  switch (interval) {
    case 0:
      return "IMMEDIATE";
    case 1:
      return "ONE";
    case 2:
      return "TWO";
    case 3:
      return "THREE";
    default:
      return "OTHER";
  }
}

Mode CurrentMode() {
  Mode mode;
  mode.selected = ConfiguredFrameRate();
  mode.cap = LimiterRate(mode.selected);
  mode.unlimited = mode.selected == kFrameRateUnlimited;
  return mode;
}

std::string Cvar(const char* name) {
  return rex::cvar::GetFlagByName(name);
}

void Log(const std::string& line) {
  REXLOG_INFO("{}", line);
}

int InternalPercent() {
  return NormalizeInternalRenderScale(std::atoi(Cvar("eck_internal_render_scale").c_str()));
}

std::string ModeMarker(const Mode& mode) {
  const int scale = std::max(1, std::atoi(Cvar("resolution_scale").c_str()));
  const int percent = InternalPercent();
  std::string line = "[FPS-MODE] selected=" + Selected(mode.selected) +
                     " original_console_rate=" + (mode.selected == kFrameRateOriginal ? "yes" : "no") +
                     " limiter=" + (mode.unlimited ? "off" : "on") + " effective_cap=" + std::to_string(mode.cap);
  if (mode.unlimited) {
    line += " safety_cap=" + std::to_string(kUnlimitedFrameRate) + " reason=UE3 delta safety margin";
  }
  line += " present_interval=IMMEDIATE(requested TWO) vsync=" +
          std::string(Cvar(kTearingSetting) == "true" ? "off(tearing allowed)" : "on") +
          " render=" + std::to_string(kGuestWidth * scale) + "x" + std::to_string(kGuestHeight * scale) +
          " internal=" + std::to_string(percent) + "% msaa=" +
          (Cvar("eck_msaa_compatibility") == "true" ? "4x(compat)" : "2x");
  return line;
}

std::string RenderLine() {
  const int scale = std::max(1, std::atoi(Cvar("resolution_scale").c_str()));
  const int percent = InternalPercent();
  return "[RENDER-CONFIG] output=" + std::to_string(kGuestWidth * scale) + "x" +
         std::to_string(kGuestHeight * scale) + " guest=" + std::to_string(kGuestWidth) + "x" +
         std::to_string(kGuestHeight) + " internal=" +
         std::to_string(InternalRenderWidth(scale, percent)) + "x" +
         std::to_string(InternalRenderHeight(scale, percent)) + " scale=" + std::to_string(percent) + "%";
}

void CloseSessionLocked(std::vector<std::string>& out) {
  if (!g.session_open) {
    return;
  }
  g.session_open = false;
  const double duration = std::chrono::duration<double>(Clock::now() - g.session_start).count();
  const Window& s = g.session;
  const Result result = Judge(s, g.session_frames, g.mode, g.session_anomalies);
  const double ratio = TimeRatio(s.game_seconds, s.wall_seconds);
  out.push_back(
      "[FPS-SUMMARY] selected=" + Selected(g.mode.selected) + " duration=" + Fixed(duration, 1) +
      "s frames=" + std::to_string(s.swaps) + " avg_fps=" + Fixed(s.actual_fps(duration), 2) +
      " gameplay_frames=" + std::to_string(s.valid_frames) + " gameplay_fps=" + Fixed(s.gameplay_fps(), 2) +
      " gameplay_ratio=" + Fixed(ratio, 5) + " gameplay_drift_ms=" +
      Fixed((s.game_seconds - s.wall_seconds) * 1000.0, 1) + " frame_p95_ms=" +
      Fixed(g.session_frames.Percentile(0.95), 2) + " frame_p99_ms=" + Fixed(g.session_frames.Percentile(0.99), 2) +
      " stalls=" + std::to_string(s.stalls) + " loading_stalls=" + std::to_string(s.loading_stalls) +
      " loading_stall_ms=" + Fixed(s.loading_stall_ms, 0) + " performance_limited_frames=" +
      std::to_string(s.slow_frames) + " delta<2.5ms=" + std::to_string(s.deltas.below_safe_floor) +
      " late_frames=" + std::to_string(s.limiter_late) + " physics_updates=" + std::to_string(s.physics_updates) +
      " anomalies=" + std::to_string(g.session_anomalies) + " timing_result=" + VerdictName(result.timing) +
      (result.timing_reason.empty() ? "" : " timing_reason=" + result.timing_reason) +
      " performance_result=" + VerdictName(result.performance) +
      (result.performance_reason.empty() ? "" : " reason=" + result.performance_reason));
  out.push_back(
      "[PERF-SUMMARY] selected=" + Selected(g.mode.selected) + " internal=" +
      std::to_string(InternalPercent()) + "% render=" + Cvar("resolution_scale") + "x gameplay_fps=" + Fixed(s.gameplay_fps(), 2) + " game_cpu_ms=" +
      Fixed(s.world_ms.avg(), 2) + " render_cpu_ms=" +
      Fixed(std::max(0.0, s.engine_tick_ms.avg() - s.world_ms.avg()), 2) + " render_sync_ms=" +
      Fixed(s.render_sync_ms.avg(), 2) + " swap_ms=" + Fixed(s.swap_ms.avg(), 2) + " cp_idle_pct=" +
      Fixed(g.session_cp_idle_pct.avg(), 1) + " cp_fence_pct=" + Fixed(g.session_cp_fence_pct.avg(), 1) +
      " unaccounted_ms=" + Fixed(g.session_unaccounted_ms.avg(), 2) + " bottleneck_seconds=LIMITER:" +
      std::to_string(g.bottlenecks[0]) + ",GAME_CPU:" + std::to_string(g.bottlenecks[1]) + ",RENDER_CPU:" +
      std::to_string(g.bottlenecks[2]) + ",GPU_OR_D3D12:" + std::to_string(g.bottlenecks[3]) + ",SWAP:" +
      std::to_string(g.bottlenecks[4]) + ",MIXED:" + std::to_string(g.bottlenecks[5]) + ",UNKNOWN:" +
      std::to_string(g.bottlenecks[6]));
}

void OpenSessionLocked(std::vector<std::string>& out, Clock::time_point now) {
  g.session = Window{};
  g.session_frames = Histogram{};
  g.session_anomalies = 0;
  g.session_open = true;
  g.session_start = now;
  g.long_game = g.long_wall = 0;
  g.long_reports = 0;
  g.bottlenecks = {};
  g.session_cp_idle_pct = Stat{};
  g.session_cp_fence_pct = Stat{};
  g.session_unaccounted_ms = Stat{};
  out.push_back(ModeMarker(g.mode));
  out.push_back(RenderLine());
}

void SyncModeLocked(std::vector<std::string>& out, Clock::time_point now) {
  const Mode mode = CurrentMode();
  if (g.mode_known && mode.selected == g.mode.selected) {
    return;
  }
  if (g.mode_known) {
    ++g.window.rate_changes;
    CloseSessionLocked(out);
  }
  g.mode = mode;
  g.mode_known = true;
  if (g.have_world && now - g.last_world < std::chrono::seconds(1)) {
    OpenSessionLocked(out, now);
  }
}

struct CommandProcessorFigures {
  bool available = false;
  double idle_pct = 0;
  double fence_pct = 0;
  double draw_ms = 0;
  double copy_ms = 0;
  double swap_ms = 0;
};

// The graphics layer publishes microseconds spent in each phase over its last second.
CommandProcessorFigures ReadCommandProcessor(double frames) {
  CommandProcessorFigures figures;
  const std::string idle = Cvar("gpu_cp_idle_us");
  const std::string fence = Cvar("gpu_cp_fence_us");
  if (idle.empty() || fence.empty()) {
    return figures;
  }
  figures.available = true;
  figures.idle_pct = std::atof(idle.c_str()) / 10000.0;
  figures.fence_pct = std::atof(fence.c_str()) / 10000.0;
  if (frames > 0) {
    figures.draw_ms = std::atof(Cvar("gpu_cp_draw_us").c_str()) / 1000.0 / frames;
    figures.copy_ms = std::atof(Cvar("gpu_cp_copy_us").c_str()) / 1000.0 / frames;
    figures.swap_ms = std::atof(Cvar("gpu_cp_swap_us").c_str()) / 1000.0 / frames;
  }
  return figures;
}

void Report(Clock::time_point now) {
  std::vector<std::string> lines;
  Window w;
  Mode mode;
  double elapsed = 0;
  bool gameplay = false;
  {
    std::lock_guard lock(g.mutex);
    elapsed = std::chrono::duration<double>(now - g.window_start).count();
    g.window_start = now;
    SyncModeLocked(lines, now);
    gameplay = g.have_world && std::chrono::duration<double>(now - g.last_world).count() < 1.5;
    if (g.session_open && !gameplay && std::chrono::duration<double>(now - g.last_world).count() > kIdleSeconds) {
      CloseSessionLocked(lines);
    }
    w = g.window;
    g.window = Window{};
    mode = g.mode;
    if (g.session_open) {
      Merge(g.session, w);
    }
    if (w.swaps == 0 && w.world_ticks == 0) {
      for (const std::string& line : lines) {
        Log(line);
      }
      return;
    }
    g.long_game += w.game_seconds;
    g.long_wall += w.wall_seconds;
    ++g.long_reports;
  }

  const double fps = w.actual_fps(elapsed);
  lines.push_back("[FPS] selected=" + Selected(mode.selected) + " cap=" + std::to_string(mode.cap) +
                  " actual=" + Fixed(fps, 1) + " loop=" + std::to_string(w.loop_ticks) +
                  " engine=" + std::to_string(w.engine_ticks) + " world=" + std::to_string(w.world_ticks) +
                  " flips=" + std::to_string(w.flips) + " swaps=" + std::to_string(w.swaps) +
                  " engine_present=" + std::to_string(w.engine_presents) + " prepare=" +
                  std::to_string(w.prepares) + " d3d_swap=" + std::to_string(w.d3d_swaps) + " groups=" +
                  std::to_string(w.groups[0]) + "/" + std::to_string(w.groups[1]) + "/" +
                  std::to_string(w.groups[2]) + "/" + std::to_string(w.groups[3]) + "/" +
                  std::to_string(w.groups[4]) + " gameplay_frames=" + std::to_string(w.valid_frames));
  lines.push_back("[DELTA] delta_ms avg=" + Fixed(w.delta_ms.avg(), 2) + " min=" + Fixed(w.delta_ms.min(), 2) +
                  " max=" + Fixed(w.delta_ms.max(), 2) + " engine_avg=" + Fixed(w.engine_delta_ms.avg(), 2) +
                  " | frame_ms avg=" + Fixed(w.frame_ms.stat().avg(), 2) + " p50=" +
                  Fixed(w.frame_ms.Percentile(0.5), 2) + " p95=" + Fixed(w.frame_ms.Percentile(0.95), 2) +
                  " p99=" + Fixed(w.frame_ms.Percentile(0.99), 2) + " max=" + Fixed(w.frame_ms.stat().max(), 1) +
                  " | <0.5ms=" + std::to_string(w.deltas.below_engine_floor) + " <2.5ms=" +
                  std::to_string(w.deltas.below_safe_floor) + " >20ms=" + std::to_string(w.deltas.over_20ms) +
                  " >33.3ms=" + std::to_string(w.deltas.over_33ms) + " >50ms=" +
                  std::to_string(w.deltas.over_50ms) + " >100ms=" + std::to_string(w.deltas.over_100ms) +
                  " hitches=" + std::to_string(w.stalls) + " loading_stalls=" + std::to_string(w.loading_stalls));
  lines.push_back("[LIMITER] target=" + Selected(mode.selected) + " target_ms=" + Fixed(mode.target_ms(), 3) +
                  " wait_requested_avg=" + Fixed(w.limiter_requested_ms.avg(), 2) + " wait_actual_avg=" +
                  Fixed(w.limiter_actual_ms.avg(), 2) + " overshoot_max_ms=" + Fixed(w.limiter_overshoot_max_ms, 2) +
                  " undershoot_max_ms=" + Fixed(w.limiter_undershoot_max_ms, 2) + " late=" +
                  std::to_string(w.limiter_late) + " nowait=" + std::to_string(w.limiter_nowait) + " reset=" +
                  std::to_string(w.limiter_resets) + " rate_changes=" + std::to_string(w.rate_changes));
  if (HasEnoughFrames(w)) {
    PerfInputs in;
    in.fps = fps;
    in.cap = mode.cap;
    in.frame_ms = w.frame_ms.stat().avg();
    in.world_ms = w.world_ms.avg();
    in.engine_ms = w.engine_tick_ms.avg();
    in.sync_ms = w.render_sync_ms.avg();
    in.swap_ms = w.swap_ms.avg();
    const CommandProcessorFigures cp = ReadCommandProcessor(double(w.swaps));
    in.cp_available = cp.available;
    in.cp_idle_pct = cp.idle_pct;
    in.cp_fence_pct = cp.fence_pct;
    const Bottleneck bottleneck = Classify(in);
    const double render_cpu = std::max(0.0, in.engine_ms - in.world_ms);
    const double unaccounted = std::max(0.0, in.frame_ms - in.engine_ms - in.sync_ms);
    const double fence_ms_per_frame = cp.available && fps > 0 ? cp.fence_pct * 10.0 / fps : 0.0;
    lines.push_back("[PERF] fps=" + Fixed(fps, 0) + " target=" + (mode.unlimited ? "unlimited" : std::to_string(mode.cap)) +
                    " internal=" + std::to_string(InternalPercent()) + "% cpu_game_ms=" + Fixed(in.world_ms, 2) +
                    " cpu_render_ms=" + Fixed(render_cpu, 2) + " render_sync_ms=" + Fixed(in.sync_ms, 2) +
                    " gpu_or_submit_ms=" + (cp.available ? Fixed(fence_ms_per_frame, 2) : std::string("n/a")) +
                    " swap_ms=" + Fixed(in.swap_ms, 2) + " limiter_wait_ms=" +
                    Fixed(w.limiter_actual_ms.avg(), 2) + " unaccounted_ms=" + Fixed(unaccounted, 2) +
                    " cp_idle_pct=" + (cp.available ? Fixed(cp.idle_pct, 1) : std::string("n/a")) +
                    " cp_fence_pct=" + (cp.available ? Fixed(cp.fence_pct, 1) : std::string("n/a")) +
                    " cp_draw_ms=" + (cp.available ? Fixed(cp.draw_ms, 2) : std::string("n/a")) + " cp_copy_ms=" +
                    (cp.available ? Fixed(cp.copy_ms, 2) : std::string("n/a")) + " cp_swap_ms=" +
                    (cp.available ? Fixed(cp.swap_ms, 2) : std::string("n/a")) +
                    " bottleneck=" + BottleneckName(bottleneck));
    std::lock_guard lock(g.mutex);
    ++g.bottlenecks[size_t(bottleneck)];
    if (cp.available) {
      g.session_cp_idle_pct.Add(cp.idle_pct);
      g.session_cp_fence_pct.Add(cp.fence_pct);
    }
    g.session_unaccounted_ms.Add(unaccounted);
  }
  const double drift_ms = (w.game_seconds - w.wall_seconds) * 1000.0;
  std::string timing = "[TIMING] wall=" + Fixed(w.wall_seconds, 3) + "s game=" + Fixed(w.game_seconds, 3) +
                       "s drift=" + Fixed(drift_ms, 1) + "ms ratio=" +
                       Fixed(TimeRatio(w.game_seconds, w.wall_seconds), 5) + " paused_ticks=" +
                       std::to_string(w.paused_ticks);
  std::vector<std::string> anomalies = DetectAnomalies(w, mode, elapsed, gameplay);
  {
    std::lock_guard lock(g.mutex);
    if (g.long_reports >= kLongWindowReports) {
      const double long_drift = (g.long_game - g.long_wall) * 1000.0;
      timing += " | w10 wall=" + Fixed(g.long_wall, 3) + "s game=" + Fixed(g.long_game, 3) + "s drift=" +
                Fixed(long_drift, 1) + "ms ratio=" + Fixed(TimeRatio(g.long_game, g.long_wall), 5);
      if (g.long_wall > 5.0 && std::fabs(g.long_game - g.long_wall) > 0.02 &&
          std::fabs(TimeRatio(g.long_game, g.long_wall) - 1.0) > 0.005) {
        anomalies.push_back("game_time_drift_ms=" + Fixed(long_drift, 0) + " over 10s selected_fps=" +
                            Selected(mode.selected));
      }
      g.long_game = g.long_wall = 0;
      g.long_reports = 0;
    }
    if (g.session_open) {
      timing += " | session ratio=" + Fixed(TimeRatio(g.session.game_seconds, g.session.wall_seconds), 5);
    }
    g.session_anomalies += anomalies.size();
  }
  lines.push_back(timing);
  for (const std::string& anomaly : anomalies) {
    lines.push_back("[ANOMALY] " + anomaly);
  }
  for (const std::string& line : lines) {
    Log(line);
  }
}

bool Plausible(uint32_t address) {
  return address >= 0x10000 && address < 0xA0000000;
}

// WorldInfo of the running world: GWorld+80 -> +60 -> first element.
uint32_t WorldInfo(const uint8_t* base) {
  const uint32_t world = LoadBE32(base, kGWorld);
  if (!Plausible(world)) {
    return 0;
  }
  const uint32_t level = LoadBE32(base, world + 80);
  if (!Plausible(level)) {
    return 0;
  }
  const uint32_t list = LoadBE32(base, level + 60);
  if (!Plausible(list)) {
    return 0;
  }
  const uint32_t info = LoadBE32(base, list);
  return Plausible(info) ? info : 0;
}

void OnWorldTick(bool timed, double before, double after, double world_ms) {
  std::vector<std::string> lines;
  double stall_ms = 0;
  std::string selected;
  {
    std::lock_guard lock(g.mutex);
    const auto now = Clock::now();
    SyncModeLocked(lines, now);
    if (!g.session_open) {
      OpenSessionLocked(lines, now);
      g.have_world = false;
      g.warm_count = 0;
    }
    ++g.window.world_ticks;
    if (g.have_world) {
      const double wall = std::chrono::duration<double>(now - g.last_world).count();
      if (wall >= kLoadingStallSeconds) {
        ++g.window.loading_stalls;
        g.window.loading_stall_ms += wall * 1000.0;
        g.warm_count = 0;
        stall_ms = wall * 1000.0;
        selected = Selected(g.mode.selected);
      } else {
        ++g.warm_count;
        const bool advancing = timed && after - before > 0.0;
        if (!advancing && timed) {
          ++g.window.paused_ticks;
        }
        if (Warm() && advancing) {
          ++g.window.valid_frames;
          g.window.game_seconds += after - before;
          g.window.wall_seconds += wall;
          g.window.world_ms.Add(world_ms);
        }
      }
    }
    g.last_world = now;
    g.have_world = true;
  }
  if (stall_ms > 0) {
    const CommandProcessorFigures cp = ReadCommandProcessor(0);
    lines.push_back(std::string("[STALL] kind=") + StallKind(cp.available, cp.fence_pct) + " wall_ms=" +
                    Fixed(stall_ms, 0) + " cp_fence_pct=" + (cp.available ? Fixed(cp.fence_pct, 1) : std::string("n/a")) +
                    " focus=" + (::LocalInputAccepted() ? "active" : "inactive") +
                    " excluded_from_gameplay_timing=yes selected_fps=" + selected);
  }
  for (const std::string& line : lines) {
    Log(line);
  }
}

void Count(uint64_t Window::*field) {
  std::lock_guard lock(g.mutex);
  ++(g.window.*field);
}

void CountGroup(int index) {
  std::lock_guard lock(g.mutex);
  ++g.window.groups[size_t(index)];
}

struct Timer {
  Clock::time_point begin = Clock::now();
  double ms() const { return Ms(Clock::now() - begin); }
};

}  // namespace

void OnSwap(double swap_ms) {
  const auto now = Clock::now();
  std::lock_guard lock(g.mutex);
  ++g.window.swaps;
  if (g.have_swap) {
    const double interval = Ms(now - g.last_swap);
    if (interval < kLoadingStallSeconds * 1000.0) {
      g.window.frame_ms.Add(interval);
      g.session_frames.Add(interval);
      g.window.stalls += interval > kHitchSeconds * 1000.0;
      if (Warm()) {
        ++g.window.valid_swaps;
        g.window.swap_ms.Add(swap_ms);
        g.window.slow_frames += !g.mode.unlimited && interval > 1.25 * g.mode.target_ms();
      }
    }
  }
  g.last_swap = now;
  g.have_swap = true;
}

void OnFlip(uint32_t requested_interval, uint32_t patched_interval) {
  std::string line;
  {
    std::lock_guard lock(g.mutex);
    ++g.window.flips;
    if (requested_interval != g.last_requested_interval) {
      g.last_requested_interval = requested_interval;
      line = std::string("[PRESENT] requested_interval=") + IntervalName(requested_interval) +
             " patched_interval=" + IntervalName(patched_interval) + " selected_fps=" +
             Selected(ConfiguredFrameRate());
    }
  }
  if (!line.empty()) {
    Log(line);
  }
}

void OnLimiterWait(double requested_ms, double actual_ms, double overshoot_ms, bool waited, bool reset, bool late) {
  std::lock_guard lock(g.mutex);
  Window& w = g.window;
  if (reset) {
    ++w.limiter_resets;
    return;
  }
  if (waited) {
    w.limiter_requested_ms.Add(requested_ms);
    w.limiter_actual_ms.Add(actual_ms);
    w.limiter_overshoot_max_ms = std::max(w.limiter_overshoot_max_ms, overshoot_ms);
    w.limiter_undershoot_max_ms = std::max(w.limiter_undershoot_max_ms, -overshoot_ms);
  } else {
    ++w.limiter_nowait;
  }
  w.limiter_late += late;
}

void FlushSessionSummary() {
  std::vector<std::string> lines;
  {
    std::lock_guard lock(g.mutex);
    CloseSessionLocked(lines);
  }
  for (const std::string& line : lines) {
    Log(line);
  }
}

}  // namespace eck::debug

REX_EXTERN(__imp__sub_82A42C30);
REX_HOOK_RAW(sub_82A42C30) {
  {
    std::lock_guard lock(eck::debug::g.mutex);
    ++eck::debug::g.window.loop_ticks;
  }
  __imp__sub_82A42C30(ctx, base);
  const auto now = eck::debug::Clock::now();
  bool due;
  {
    std::lock_guard lock(eck::debug::g.mutex);
    due = std::chrono::duration<double>(now - eck::debug::g.window_start).count() >= eck::debug::kReportSeconds;
  }
  if (due) {
    eck::debug::Report(now);
  }
}

REX_EXTERN(__imp__sub_82A40CB0);
REX_HOOK_RAW(sub_82A40CB0) {
  __imp__sub_82A40CB0(ctx, base);
  const double dt = eck::LoadBEDouble(base, eck::debug::kGDeltaTime);
  std::lock_guard lock(eck::debug::g.mutex);
  eck::debug::g.window.delta_ms.Add(dt * 1000.0);
  if (eck::debug::Warm()) {
    eck::debug::g.window.deltas.Add(dt);
  }
}

REX_EXTERN(__imp__sub_825A4D98);
REX_HOOK_RAW(sub_825A4D98) {
  const double dt = ctx.f1.f64;
  const eck::debug::Timer timer;
  __imp__sub_825A4D98(ctx, base);
  const double ms = timer.ms();
  {
    std::lock_guard lock(eck::debug::g.mutex);
    ++eck::debug::g.window.engine_ticks;
    eck::debug::g.window.engine_delta_ms.Add(dt * 1000.0);
    if (eck::debug::Warm()) {
      eck::debug::g.window.engine_tick_ms.Add(ms);
    }
  }
  eck::OnlineEngineTick(ctx, base);
  eck::TestControlTick(ctx, base);
}

REX_EXTERN(__imp__sub_825E91E0);
REX_HOOK_RAW(sub_825E91E0) {
  const uint32_t info = eck::debug::WorldInfo(base);
  const double before = info ? eck::LoadBEFloat(base, info + eck::debug::kWorldInfoTimeSeconds) : 0.0;
  const eck::debug::Timer timer;
  __imp__sub_825E91E0(ctx, base);
  const double ms = timer.ms();
  const double after = info ? eck::LoadBEFloat(base, info + eck::debug::kWorldInfoTimeSeconds) : 0.0;
  eck::debug::OnWorldTick(info != 0, before, after, ms);
}

// The game thread waits here for the render thread's previous frame.
REX_EXTERN(__imp__sub_82558F28);
REX_HOOK_RAW(sub_82558F28) {
  const eck::debug::Timer timer;
  __imp__sub_82558F28(ctx, base);
  const double ms = timer.ms();
  std::lock_guard lock(eck::debug::g.mutex);
  if (eck::debug::Warm()) {
    eck::debug::g.window.render_sync_ms.Add(ms);
  }
}

// RBPhysScene per-frame setup: f1 is the frame delta.
REX_EXTERN(__imp__sub_82794FA8);
REX_HOOK_RAW(sub_82794FA8) {
  {
    std::lock_guard lock(eck::debug::g.mutex);
    ++eck::debug::g.window.physics_updates;
    eck::debug::g.last_physics_dt_ms = ctx.f1.f64 * 1000.0;
    eck::debug::g.window.physics_dt_ms.Add(eck::debug::g.last_physics_dt_ms);
  }
  __imp__sub_82794FA8(ctx, base);
}

// NxScene::setTiming: f1 is the sub-step size, r5 the iteration limit.
REX_EXTERN(__imp__sub_832A88D8);
REX_HOOK_RAW(sub_832A88D8) {
  {
    std::lock_guard lock(eck::debug::g.mutex);
    eck::debug::Window& w = eck::debug::g.window;
    const double step_ms = ctx.f1.f64 * 1000.0;
    ++w.timing_calls;
    w.substep_ms.Add(step_ms);
    w.max_iterations = std::max(w.max_iterations, ctx.r5.u32);
    if (step_ms > 0.0) {
      const double estimate = std::ceil(eck::debug::g.last_physics_dt_ms / step_ms - 1e-9);
      w.estimated_substeps.Add(std::clamp(estimate, 1.0, double(std::max<uint32_t>(ctx.r5.u32, 1))));
    }
  }
  __imp__sub_832A88D8(ctx, base);
}

#define ECK_DEBUG_TICK_GROUP(address, index)                       \
  REX_EXTERN(__imp__sub_##address);                                \
  REX_HOOK_RAW(sub_##address) {                                    \
    eck::debug::CountGroup(index);                                 \
    __imp__sub_##address(ctx, base);                               \
  }

ECK_DEBUG_TICK_GROUP(825ED758, 0)
ECK_DEBUG_TICK_GROUP(825ED948, 1)
ECK_DEBUG_TICK_GROUP(825EDB40, 2)
ECK_DEBUG_TICK_GROUP(825EDD38, 3)
ECK_DEBUG_TICK_GROUP(825EDF30, 4)

REX_EXTERN(__imp__sub_829EAAA8);
REX_HOOK_RAW(sub_829EAAA8) {
  eck::debug::Count(&eck::debug::Window::engine_presents);
  __imp__sub_829EAAA8(ctx, base);
}

REX_EXTERN(__imp__sub_82B58560);
REX_HOOK_RAW(sub_82B58560) {
  eck::debug::Count(&eck::debug::Window::prepares);
  __imp__sub_82B58560(ctx, base);
}

REX_EXTERN(__imp__sub_82B592A8);
REX_HOOK_RAW(sub_82B592A8) {
  eck::debug::Count(&eck::debug::Window::d3d_swaps);
  __imp__sub_82B592A8(ctx, base);
}
