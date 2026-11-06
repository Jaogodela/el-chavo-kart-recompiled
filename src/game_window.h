#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "dpi.h"

namespace eck {

// The window_width/height settings are the game window's client size in physical pixels, whatever the display scale.
// The SDK sizes its window from logical pixels, so the plan converts the fitted client size once, at startup.

// Title bar and borders around the client area, in physical pixels.
struct WindowFrame {
  int width = 0;
  int height = 0;
};

struct GameWindowPlan {
  DpiWindowSize requested;
  DpiWindowSize client;
  DpiWindowSize logical;
  bool fitted = false;
};

struct GameWindowStartup {
  GameWindowPlan plan;
  float scale = 1.0f;
  DpiWindowSize monitor;
  DpiWindowSize work;
  WindowFrame frame;
};

// Shrinks the client size by one common factor until the framed window fits the work area. Never enlarges.
inline DpiWindowSize FitClientToWorkArea(DpiWindowSize requested, DpiWindowSize work, WindowFrame frame) {
  const int room_width = work.width - frame.width;
  const int room_height = work.height - frame.height;
  if (requested.width <= 0 || requested.height <= 0 || room_width <= 0 || room_height <= 0) {
    return requested;
  }
  if (requested.width <= room_width && requested.height <= room_height) {
    return requested;
  }
  const bool width_limited = std::int64_t(room_width) * requested.height <= std::int64_t(room_height) * requested.width;
  const int width =
      width_limited ? room_width : int(std::int64_t(room_height) * requested.width / requested.height);
  const int height =
      width_limited ? int(std::int64_t(room_width) * requested.height / requested.width) : room_height;
  return {std::max(1, width), std::max(1, height)};
}

// `creation_scale` is the scale the SDK multiplies the logical size by when it creates the window.
inline GameWindowPlan PlanGameWindow(DpiWindowSize requested, DpiWindowSize work, WindowFrame frame,
                                     float creation_scale) {
  GameWindowPlan plan;
  plan.requested = requested;
  plan.client = FitClientToWorkArea(requested, work, frame);
  plan.logical = {ScalePhysicalToLogical(plan.client.width, creation_scale),
                  ScalePhysicalToLogical(plan.client.height, creation_scale)};
  plan.fitted = plan.client != requested;
  return plan;
}

inline std::string SizeText(DpiWindowSize size) {
  return std::to_string(size.width) + "x" + std::to_string(size.height);
}

inline std::string FormatGameWindowLine(const GameWindowStartup& startup, DpiWindowSize window, DpiWindowSize pixels) {
  return "Window: requested=" + SizeText(startup.plan.requested) +
         " | dpi=" + std::to_string(int(std::lround(double(ValidDpiScale(startup.scale)) * 100.0))) + "%" +
         " | monitor=" + SizeText(startup.monitor) + " | work=" + SizeText(startup.work) +
         " | frame=" + SizeText({startup.frame.width, startup.frame.height}) +
         " | client=" + SizeText(startup.plan.client) + " | logical=" + SizeText(startup.plan.logical) +
         " | fitted=" + (startup.plan.fitted ? "yes" : "no") + " | window=" + SizeText(window) +
         " | pixels=" + SizeText(pixels);
}

}  // namespace eck
