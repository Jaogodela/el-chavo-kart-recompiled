#include <rex/cvar.h>
#include <rex/hook.h>

#include "settings.h"

// Starts the startup movie sequence; returning here is the engine's own nostartupmovies path.
REX_EXTERN(__imp__sub_823BF3E8);
REX_HOOK_RAW(sub_823BF3E8) {
  if (REXCVAR_GET(eck_skip_intro)) {
    return;
  }
  __imp__sub_823BF3E8(ctx, base);
}
