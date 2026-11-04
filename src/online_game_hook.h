#pragma once

namespace eck {

// DEBUG_SAULO: records at startup that the online guest hooks are part of this build and which developer switches are
// set.
void LogOnlineGameHooks();

// DEBUG_SAULO: with ECK_ONLINE_DEBUG=1, logs the room, seats, paths and the kart slots they would take, on change.
// Called once per presented frame.
void PumpOnlineGameDebug();

}  // namespace eck
