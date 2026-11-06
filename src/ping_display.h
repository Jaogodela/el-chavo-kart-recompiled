#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "online/online_service.h"

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eck {

// "Ping: 42 ms" for one peer; "Ping: 38 / 51 / 44 ms" for several in seat order, with "--" for a peer whose round trip
// has not been measured; "Ping: --" with no peer or nothing measured yet.
inline std::string PingText(const std::vector<std::optional<int>>& rtts) {
  if (std::none_of(rtts.begin(), rtts.end(), [](const std::optional<int>& rtt) { return rtt.has_value(); })) {
    return "Ping: --";
  }
  std::string text = "Ping: ";
  for (size_t i = 0; i < rtts.size(); ++i) {
    text += (i ? " / " : "") + (rtts[i] ? std::to_string(*rtts[i]) : std::string("--"));
  }
  return text + " ms";
}

// The round trips of the other seats that are connected, lowest seat first.
inline std::vector<std::optional<int>> MeasuredRtts(const std::vector<online::PeerSnapshot>& peers) {
  std::vector<std::optional<int>> out;
  for (const online::PeerSnapshot& peer : peers) {
    if (peer.connected) out.push_back(peer.rtt_ms);
  }
  return out;
}

// Shown with Online play on and the display switched on; without a connected peer it reads "Ping: --".
constexpr bool PingVisible(bool online_enabled, bool display_on) { return online_enabled && display_on; }

inline constexpr char kPingDisplayKey[] = "F3";

// Adds the in-game Ping panel and its F3 toggle.
void CreatePingDisplay(rex::ui::ImGuiDrawer* drawer);

}  // namespace eck
