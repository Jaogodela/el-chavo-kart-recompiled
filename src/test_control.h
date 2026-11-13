#pragma once

// A local control channel for tests, off unless ECK_TEST_CONTROL=1. It exists so a test can start a race, drive it and look
// at the result without a keyboard, a pad or a window in front: one line in, one line back, over a socket bound to
// 127.0.0.1 that only this PC can reach.
//
// The requests are parsed by test_control_request.h, which is pure; this file is the transport, the pad the game reads
// instead of the physical one, and the picture of the game's window.

#include <cstdint>
#include <string>

#include "online_race.h"
#include "test_control_request.h"

namespace eck::test_control {

// Set to 1 to open the control channel; anything else leaves the game exactly as it ships.
inline constexpr char kEnv[] = "ECK_TEST_CONTROL";
// The port the channel listens on, loopback only; 8799 unless a test says otherwise.
inline constexpr char kPortEnv[] = "ECK_TEST_CONTROL_PORT";

bool Enabled();

// Starts the pipe thread. Idempotent, and does nothing unless the environment asked for it. Call once, after the window
// exists, with the window's native handle so a screenshot can be taken of it.
void Start(void* native_window);

// The oldest request this thread can carry out, for the game thread; kNone when there is nothing to do. Only the game
// thread may touch the guest, so a console command, a race start and the state are its own.
Request TakeForGameThread();

// The same for the UI thread, which answers what does not need the guest: the pad, a picture, the online side, help. It runs
// every presented frame, so the channel answers at the menu too, where the game thread does not tick.
Request TakeForUiThread();

// A console command and the state can only be carried out with a map loaded, where the engine ticks. These let the UI thread
// notice that nothing will serve such a request, and answer it instead of leaving the client waiting.
bool HasPendingGuest();
void DropPendingGuest(const std::string& reason);

// One line back to the test client. The executor ends its answer with kOk or kError + reason; anything before that is
// payload (the state, a path, the help text).
void Answer(const std::string& line);
void Answered();
void Failed(const std::string& reason);

// The pad a test asked for: while it is set the game reads this instead of the physical pad, so a test drives without a
// controller and without the window having focus.
bool PadOverride(race::PadState& out);
void SetPad(const race::PadState& pad);
void ClearPad();

// A picture of the game's window, as a BMP so any viewer opens it. False when there is no window or the capture failed.
bool CaptureTo(const std::string& path);

}  // namespace eck::test_control
