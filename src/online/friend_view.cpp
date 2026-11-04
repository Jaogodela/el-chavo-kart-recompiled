#include "online/friend_view.h"

#include <algorithm>

#include "ui_text.h"

namespace eck::online {

std::vector<InviteChoice> InviteChoices(const std::vector<Friend>& friends) {
  std::vector<InviteChoice> choices;
  for (const Friend& f : friends) {
    InviteChoice choice{f.player, f.name, Tr("Offline"), false};
    if (f.online) {
      const bool busy = f.state == FriendState::kHosting || f.state == FriendState::kConnecting ||
                        f.state == FriendState::kInGame;
      choice.status = f.state == FriendState::kHosting ? Tr("Hosting")
                      : busy                           ? Tr("In game")
                                                       : Tr("Online");
      if (!f.compatible) choice.status = Tr("Version mismatch");
      choice.selectable = f.compatible && !busy;
    }
    choices.push_back(std::move(choice));
  }
  std::stable_partition(choices.begin(), choices.end(), [](const InviteChoice& c) { return c.selectable; });
  return choices;
}

FriendRowView DescribeFriend(const Snapshot& state, const Friend& entry) {
  FriendRowView view;
  view.status = FriendStatusLine(entry);
  if (state.room) {
    for (const Seat& seat : state.room->seats) {
      if (seat.player == entry.player && seat.state != SeatState::kEmpty) {
        view.status = seat.state == SeatState::kReconnecting ? Tr("Connecting") : Tr("In game");
        return view;
      }
    }
  }
  view.incompatible = entry.online && !entry.compatible;
  return view;
}

std::string SeatLine(const Seat& seat, const PeerSnapshot* peer) {
  if (seat.state == SeatState::kEmpty) return Tr("Waiting for players");
  std::string line = seat.name;
  if (seat.state == SeatState::kReconnecting || (peer && !peer->connected)) {
    return line + " (" + Tr("Connection lost") + ")";
  }
  if (peer && (peer->path == PeerPath::kDirect || peer->path == PeerPath::kRelay)) {
    line += std::string(" (") + (peer->path == PeerPath::kDirect ? Tr("Direct") : Tr("Relay")) + ")";
    if (peer->rtt_ms) line += " " + std::to_string(*peer->rtt_ms) + " ms";
  }
  return line;
}

}  // namespace eck::online
