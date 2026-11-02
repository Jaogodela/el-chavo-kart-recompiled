#pragma once

namespace eck {

// The game renders 1280x720 (ResX/ResY in its SystemSettings); the guest video mode is the same.
inline constexpr int kGuestWidth = 1280;
inline constexpr int kGuestHeight = 720;

struct WindowSize {
  int width;
  int height;
};

inline constexpr WindowSize kWindowSizes[] = {
    {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},
};

}  // namespace eck
