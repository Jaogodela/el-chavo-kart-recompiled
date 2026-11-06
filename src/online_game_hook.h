#pragma once

#include <cstdint>
#include <optional>

#include <rex/ppc/func.h>


namespace eck {

// Runs on the game thread between two engine ticks, where a console command can start a travel.
void OnlineEngineTick(PPCContext& ctx, uint8_t* base);

// Records at startup that the online guest hooks are part of this build and which developer switches are
// set.
void LogOnlineGameHooks();

// With ECK_ONLINE_DEBUG=1, logs the room, seats, paths and the kart slots they would take, on change.
// Called once per presented frame.
void PumpOnlineGameDebug();

}  // namespace eck
