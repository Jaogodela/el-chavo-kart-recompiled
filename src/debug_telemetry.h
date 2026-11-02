#pragma once

// DEBUG_SAULO: temporary frame-pacing telemetry. Remove with debug_telemetry.cpp during final review.

#include <cstdint>

namespace eck::debug {

void OnSwap(double swap_ms);
void OnFlip(uint32_t requested_interval, uint32_t patched_interval);
void OnLimiterWait(double requested_ms, double actual_ms, double overshoot_ms, bool waited, bool reset, bool late);
void FlushSessionSummary();

}  // namespace eck::debug
