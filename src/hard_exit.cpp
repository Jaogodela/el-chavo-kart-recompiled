#include "hard_exit.h"

#include <cstdlib>

#include <rex/logging.h>

#include "debug_telemetry.h"
#include "session_log.h"

namespace eck {

void HardExit(const char* reason) {
  REXLOG_INFO("{}", reason);
  eck::debug::FlushSessionSummary();  // DEBUG_SAULO
  PrepareHardExit();
  std::_Exit(0);
}

}  // namespace eck
