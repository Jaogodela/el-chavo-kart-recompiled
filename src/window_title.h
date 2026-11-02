#pragma once

#include <charconv>
#include <optional>
#include <string>

#include "window_host.h"

namespace eck {

// The game title, then the frame rate when it is shown, separated by " | ".
inline std::string WindowTitleText(std::optional<double> fps) {
  std::string title = kWindowTitle;
  if (fps) {
    char text[32];
    const auto end = std::to_chars(text, text + sizeof(text), *fps, std::chars_format::fixed, 2).ptr;
    title += " | FPS: " + std::string(text, end);
  }
  return title;
}

}  // namespace eck
