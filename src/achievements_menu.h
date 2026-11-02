#pragma once

#include <functional>

namespace eck {

// Called on a guest thread when the game's own Achievements menu asks the system to show its UI.
void SetAchievementsMenuHandler(std::function<void()> open);

}  // namespace eck
