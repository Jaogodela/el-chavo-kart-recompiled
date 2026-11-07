// What src/settings.cpp asks the window system and the graphics drivers: fixed answers, so the settings tests run
// on a machine with no display and no graphics card. Nothing here touches the player file or its parsing.

#include "graphics_adapters.h"
#include "platform.h"

namespace eck {

const char* GraphicsBackendName() { return "Test"; }

MonitorInfo MonitorForSetting(int) {
  MonitorInfo monitor;
  monitor.scale = 1.0f;
  return monitor;
}

WindowFrame WindowFrameForScale(float) { return {}; }

std::vector<GraphicsAdapter> ListGraphicsAdapters() { return {}; }

GraphicsAdapterDetails EffectiveGraphicsAdapter(int) { return {}; }

}  // namespace eck
