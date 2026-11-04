#pragma once

#include <optional>
#include <string>
#include <vector>

#include "online/online_service.h"

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace eck {

// "Ping: 38 / 51 / 44 ms" for the connected peers in seat order, with "--" for a peer whose round trip has not been
// measured; "Ping: --" with no peer.
inline std::string PingText(const std::vector<std::optional<int>>& rtts) {
  if (rtts.empty()) return "Ping: --";
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

// Shown with Online play on and while the player is seated in a room.
constexpr bool PingVisible(bool online_enabled, bool in_room) { return online_enabled && in_room; }

// Adds the in-game Ping panel under the frame counter's corner. There is no toggle key: F3 is reserved by the game's
// own bindings.
void CreatePingDisplay(rex::ui::ImGuiDrawer* drawer);

}  // namespace eck
