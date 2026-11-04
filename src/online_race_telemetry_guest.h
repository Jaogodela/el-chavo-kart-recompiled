#pragma once

#include <cstdint>

#include "online/online_service.h"

namespace eck::race_telemetry {

// From the AActor::ConsoleCommand hook, on the guest thread: only remembers the race the URL describes.
void OnConsoleCommand(const uint8_t* base, uint32_t fstring);
// Once per UI frame. Reads the game only while a race is tracked on an already signed-in service.
void PumpRaceTelemetry(online::OnlineService& service, double now);

}  // namespace eck::race_telemetry
