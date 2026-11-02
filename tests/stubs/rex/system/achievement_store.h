#pragma once

// The SDK's AchievementInfo, so the achievement list rules build without the ReXGlue SDK.
#include <cstdint>
#include <string>

namespace rex::system {

struct AchievementInfo {
  uint32_t id = 0;
  std::string label;
  std::string description;
  std::string unachieved_description;
  std::string icon_path;
  uint32_t image_id = 0;
  uint32_t gamerscore = 0;
  uint32_t flags = 0;
};

}  // namespace rex::system
