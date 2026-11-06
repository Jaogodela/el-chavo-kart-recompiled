#pragma once

#include <cstdint>
#include <optional>

#include <rex/ppc/context.h>

#include "online_lobby.h"

namespace eck {

// Once per engine tick on the game thread, between world ticks. True while a session's race owns the pads.
bool TickRaceSync(PPCContext& ctx, uint8_t* base, const lobby::Plan& plan, std::optional<int> own_pad, lobby::Bridge& bridge);

}  // namespace eck
