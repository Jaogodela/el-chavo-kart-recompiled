#include "achievements_menu.h"

#include <mutex>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

namespace eck {
namespace {

std::mutex g_mutex;
std::function<void()> g_open;

std::function<void()> Handler() {
  std::lock_guard lock(g_mutex);
  return g_open;
}

constexpr uint32_t kErrorSuccess = 0;

}  // namespace

void SetAchievementsMenuHandler(std::function<void()> open) {
  std::lock_guard lock(g_mutex);
  g_open = std::move(open);
}

}  // namespace eck

// The game's XamShowAchievementsUI wrapper (user index in r3, no overlapped). Its caller reports success when
// r3 is 0, which ReXGlue's stub leaves unset.
REX_EXTERN(__imp__sub_830D8E58);
REX_HOOK_RAW(sub_830D8E58) {
  const std::function<void()> open = eck::Handler();
  if (!open) {
    __imp__sub_830D8E58(ctx, base);
    return;
  }
  REXLOG_INFO("[DEBUG_SAULO][Achievement] game menu requested the achievements UI");
  open();
  ctx.r3.u64 = eck::kErrorSuccess;
}
