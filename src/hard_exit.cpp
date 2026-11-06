#include "hard_exit.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include <rex/logging.h>

#include "debug_telemetry.h"
#include "online_runtime.h"
#include "session_log.h"

namespace eck {

namespace {
// The link writes on its own thread, and nothing it queued survives the exit below.
constexpr std::chrono::milliseconds kLeaveFlush{200};
}  // namespace

void HardExit(const char* reason) {
  REXLOG_INFO("{}", reason);
  if (LeaveOnlineForExit()) std::this_thread::sleep_for(kLeaveFlush);
  eck::debug::FlushSessionSummary();  // DEBUG_SAULO
  PrepareHardExit();
  std::_Exit(0);
}

}  // namespace eck
