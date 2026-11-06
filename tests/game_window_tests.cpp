#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "game_window.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

using eck::DpiWindowSize;
using eck::WindowFrame;

struct Display {
  const char* name;
  DpiWindowSize work;
  WindowFrame frame;
  float scale;
};

constexpr Display kDisplays[] = {
    {"1920x1080 at 100%", {1920, 1040}, {16, 39}, 1.00f},
    {"1920x1080 at 150%", {1920, 1020}, {24, 58}, 1.50f},
    {"1920x1080 at 225%", {1920, 990}, {36, 88}, 2.25f},
    {"2560x1440 at 100%", {2560, 1400}, {16, 39}, 1.00f},
    {"2560x1440 at 150%", {2560, 1380}, {24, 58}, 1.50f},
};

// The SDK's Window::SizeToPhysical: rounds up.
int SdkPhysical(int logical, float scale) {
  const std::uint64_t dpi = std::uint64_t(std::lround(scale * 96.0f));
  return int((std::uint64_t(logical) * dpi + 95) / 96);
}

bool Within(int actual, int expected, int tolerance) {
  return std::abs(actual - expected) <= tolerance;
}

void RequestedSizeIsDpiIndependent() {
  for (const Display& display : kDisplays) {
    const eck::GameWindowPlan plan = eck::PlanGameWindow({1280, 720}, display.work, display.frame, display.scale);
    const std::string name = display.name;
    Check(plan.client.width == 1280 && plan.client.height == 720, "1280x720 stays 1280x720 on " + name);
    Check(!plan.fitted, "1280x720 is not fitted on " + name);
    Check(Within(SdkPhysical(plan.logical.width, display.scale), 1280, 1) &&
              Within(SdkPhysical(plan.logical.height, display.scale), 720, 1),
          "the SDK window is 1280x720 physical on " + name);
  }
}

void LogicalSizeShrinksWithScale() {
  const Display& at_100 = kDisplays[0];
  const Display& at_150 = kDisplays[1];
  const Display& at_225 = kDisplays[2];
  const eck::GameWindowPlan p100 = eck::PlanGameWindow({1280, 720}, at_100.work, at_100.frame, at_100.scale);
  const eck::GameWindowPlan p150 = eck::PlanGameWindow({1280, 720}, at_150.work, at_150.frame, at_150.scale);
  const eck::GameWindowPlan p225 = eck::PlanGameWindow({1280, 720}, at_225.work, at_225.frame, at_225.scale);
  Check(p100.logical == DpiWindowSize{1280, 720}, "100% logical size is 1280x720");
  Check(p150.logical == DpiWindowSize{853, 480}, "150% logical size is 853x480");
  Check(p225.logical == DpiWindowSize{569, 320}, "225% logical size is 569x320");
}

void CreationScaleDecidesTheLogicalSize() {
  const Display& target = kDisplays[0];
  const eck::GameWindowPlan plan = eck::PlanGameWindow({1280, 720}, target.work, target.frame, 1.5f);
  Check(plan.client == DpiWindowSize{1280, 720} && plan.logical == DpiWindowSize{853, 480},
        "the logical size follows the scale the SDK creates the window with");
}

void WindowThatFitsIsKept() {
  for (const Display& display : kDisplays) {
    const eck::GameWindowPlan plan = eck::PlanGameWindow({1366, 768}, display.work, display.frame, display.scale);
    Check(plan.client == DpiWindowSize{1366, 768} && !plan.fitted, std::string("1366x768 is kept on ") + display.name);
  }
  const Display& large = kDisplays[3];
  const eck::GameWindowPlan plan = eck::PlanGameWindow({2560, 1300}, large.work, large.frame, large.scale);
  Check(plan.fitted, "a window whose frame does not fit the work area is fitted");
}

void OversizedWindowIsFittedKeepingAspect() {
  struct Case {
    DpiWindowSize requested;
    const Display& display;
  };
  const Case cases[] = {
      {{1920, 1080}, kDisplays[1]}, {{1920, 1080}, kDisplays[2]}, {{3840, 2160}, kDisplays[3]},
      {{3840, 2160}, kDisplays[4]}, {{2560, 1440}, kDisplays[0]},
  };
  for (const Case& c : cases) {
    const Display& d = c.display;
    const eck::GameWindowPlan plan = eck::PlanGameWindow(c.requested, d.work, d.frame, d.scale);
    const std::string name = std::to_string(c.requested.width) + "x" + std::to_string(c.requested.height) + " on " + d.name;
    Check(plan.fitted, "fitted: " + name);
    Check(plan.client.width + d.frame.width <= d.work.width && plan.client.height + d.frame.height <= d.work.height,
          "framed window fits the work area: " + name);
    Check(plan.client.width < c.requested.width && plan.client.height < c.requested.height, "shrunk: " + name);
    const double requested_aspect = double(c.requested.width) / c.requested.height;
    const double fitted_aspect = double(plan.client.width) / plan.client.height;
    Check(std::fabs(requested_aspect - fitted_aspect) < 0.01, "aspect is kept: " + name);
    Check(Within(SdkPhysical(plan.logical.width, d.scale), plan.client.width, 1) &&
              Within(SdkPhysical(plan.logical.height, d.scale), plan.client.height, 1),
          "logical size maps back to the fitted client: " + name);
  }
  const Display& d = kDisplays[1];
  const eck::GameWindowPlan plan = eck::PlanGameWindow({1920, 1080}, d.work, d.frame, d.scale);
  Check(plan.client == DpiWindowSize{1710, 962}, "1920x1080 at 150% fits to 1710x962");
}

void DegenerateInputsAreUntouched() {
  const WindowFrame frame{16, 39};
  Check(eck::FitClientToWorkArea({1280, 720}, {0, 0}, frame) == DpiWindowSize{1280, 720}, "no work area keeps the size");
  Check(eck::FitClientToWorkArea({1280, 720}, {10, 10}, frame) == DpiWindowSize{1280, 720},
        "a work area smaller than the frame keeps the size");
  Check(eck::FitClientToWorkArea({0, 0}, {1920, 1040}, frame) == DpiWindowSize{0, 0}, "no request stays empty");
  Check(eck::ScalePhysicalToLogical(1280, 0.0f) == 1280, "an unusable scale counts as 100%");
}

void PreviousCalculationDoubledTheScale() {
  const eck::DpiWindowSize old_fit = eck::FitWindowToWorkArea(1280, 720, 1920, 1020, 1.5f, 0.9);
  Check(old_fit == DpiWindowSize{1152, 612}, "the old logical fit gave 1152x612");
  Check(eck::ScaleLogicalToPhysical(old_fit.width, 1.5f) == 1728 && eck::ScaleLogicalToPhysical(old_fit.height, 1.5f) == 918,
        "which the SDK then scaled to 1728x918");
}

void StartupLine() {
  eck::GameWindowStartup startup;
  startup.scale = 1.5f;
  startup.monitor = {1920, 1080};
  startup.work = {1920, 1020};
  startup.frame = {24, 58};
  startup.plan = eck::PlanGameWindow({1280, 720}, startup.work, startup.frame, 1.5f);
  const std::string line = eck::FormatGameWindowLine(startup, {1280, 720}, {1280, 720});
  Check(line == "Window: requested=1280x720 | dpi=150% | monitor=1920x1080 | work=1920x1020 | frame=24x58 | "
                "client=1280x720 | logical=853x480 | fitted=no | window=1280x720 | pixels=1280x720",
        "startup line: " + line);
}

}  // namespace

int main() {
  RequestedSizeIsDpiIndependent();
  LogicalSizeShrinksWithScale();
  CreationScaleDecidesTheLogicalSize();
  WindowThatFitsIsKept();
  OversizedWindowIsFittedKeepingAspect();
  DegenerateInputsAreUntouched();
  PreviousCalculationDoubledTheScale();
  StartupLine();
  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
