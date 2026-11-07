#include "frame_limiter.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(eck_frame_rate, eck::kFrameRateDefault, "El Chavo Kart",
                     "Maximum frame rate the game renders: 30, 60, 90, 120, 144, 165, 240 or 0 for Unlimited");

namespace eck {

int ConfiguredFrameRate() {
  return FrameRateOrDefault(REXCVAR_GET(eck_frame_rate));
}

}  // namespace eck
