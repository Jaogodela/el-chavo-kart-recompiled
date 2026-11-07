// The Vulkan renderer's decision on the Internal render resolution, compiled from the header the patched ReXGlue source
// (patch 0051) includes in VulkanRenderTargetCache::Initialize: 100%, 75% and 50% draw as asked on the host render
// target path, and any other path falls back to 100% of the draw resolution with a warning. Run: eck_vulkan_scale_tests

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include <rex/graphics/vulkan/internal_render_scale.h>

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

using rex::graphics::vulkan::InternalRenderScale;
using rex::graphics::vulkan::ResolveInternalRenderScale;

// What TextureCache::GetConfigInternalRenderScale hands the render target cache for eck_internal_render_scale.
uint32_t QuartersPerScale(int percent) { return percent == 75 ? 3 : (percent == 50 ? 2 : 4); }

InternalRenderScale Resolve(bool host_render_targets, int percent, uint32_t draw_scale) {
  const uint32_t requested = draw_scale * QuartersPerScale(percent);
  return ResolveInternalRenderScale(host_render_targets, requested, requested, draw_scale, draw_scale);
}

int PercentOf(const InternalRenderScale& scale, uint32_t draw_scale) {
  return int(scale.quarters_x * 100 / (draw_scale << 2));
}

void TestHostRenderTargetsKeepEveryScale() {
  std::printf("the host render target path draws at 100%%, 75%% and 50%% as asked\n");
  for (const uint32_t draw : {1u, 2u, 3u}) {
    for (const int percent : {100, 75, 50}) {
      const InternalRenderScale scale = Resolve(true, percent, draw);
      CHECK(scale.quarters_x == draw * QuartersPerScale(percent));
      CHECK(scale.quarters_y == draw * QuartersPerScale(percent));
      CHECK(PercentOf(scale, draw) == percent);
      CHECK(!scale.warn);
    }
  }
}

void TestOtherPathFallsBackTo100WithAWarning() {
  std::printf("a path that cannot draw below the draw resolution falls back to 100%% and warns\n");
  for (const uint32_t draw : {1u, 2u, 3u}) {
    for (const int percent : {75, 50}) {
      const InternalRenderScale scale = Resolve(false, percent, draw);
      CHECK(scale.quarters_x == (draw << 2));
      CHECK(scale.quarters_y == (draw << 2));
      CHECK(PercentOf(scale, draw) == 100);
      CHECK(scale.warn);
    }
    const InternalRenderScale full = Resolve(false, 100, draw);
    CHECK(full.quarters_x == (draw << 2) && full.quarters_y == (draw << 2));
    CHECK(!full.warn);
  }
}

void TestWarningIsRaisedExactlyOnAFallback() {
  std::printf("the warning is raised exactly when a smaller scale was asked for and refused\n");
  int warnings = 0;
  for (const bool host : {true, false}) {
    for (const uint32_t draw : {1u, 2u, 4u}) {
      for (const int percent : {100, 75, 50}) {
        const InternalRenderScale scale = Resolve(host, percent, draw);
        const bool expected = !host && percent != 100;
        CHECK(scale.warn == expected);
        CHECK(PercentOf(scale, draw) == (host ? percent : 100));
        warnings += scale.warn ? 1 : 0;
      }
    }
  }
  CHECK(warnings == 6);
}

void TestUnsupportedRequestOnOneAxis() {
  std::printf("a scale reduced on one axis only falls back on both\n");
  const InternalRenderScale scale = ResolveInternalRenderScale(false, 3, 4, 1, 1);
  CHECK(scale.quarters_x == 4 && scale.quarters_y == 4 && scale.warn);
  const InternalRenderScale kept = ResolveInternalRenderScale(true, 3, 4, 1, 1);
  CHECK(kept.quarters_x == 3 && kept.quarters_y == 4 && !kept.warn);
}

}  // namespace

int main() {
  TestHostRenderTargetsKeepEveryScale();
  TestOtherPathFallsBackTo100WithAWarning();
  TestWarningIsRaisedExactlyOnAFallback();
  TestUnsupportedRequestOnOneAxis();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
