#pragma once

#include <cstdint>
#include <optional>

#include <rex/ppc/func.h>


namespace eck {

// Runs on the game thread between two engine ticks, where a console command can start a travel.
void OnlineEngineTick(PPCContext& ctx, uint8_t* base);

// With ECK_TEST_CONTROL=1, carries out what a local test client asked for: a console command, a pad, a picture, the online
// side. On the game thread, because only it may touch the guest. Called every engine tick, whatever Online play is set to.
void TestControlTick(PPCContext& ctx, uint8_t* base);

// The same channel's requests that need no guest work, on the UI thread: the pad, a picture, the online side, help.
void TestControlUiTick();

// Records at startup that the online guest hooks are part of this build and which developer switches are
// set.
void LogOnlineGameHooks();

// With ECK_ONLINE_DEBUG=1, logs the room, seats, paths and the kart slots they would take, on change.
// Called once per presented frame.
void PumpOnlineGameDebug();

}  // namespace eck
