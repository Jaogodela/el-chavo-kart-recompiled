#pragma once

namespace eck {

// The shared shutdown for closing the window and for the in-game Exit Game row.
[[noreturn]] void HardExit(const char* reason);

}  // namespace eck
