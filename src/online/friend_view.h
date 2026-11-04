#pragma once

#include <string>
#include <vector>

#include "online/online_service.h"

namespace eck::online {

struct FriendRowView {
  std::string status;  // the line under the friend's name
  bool incompatible = false;
};

// What one friend's row shows, given what this player is doing right now.
FriendRowView DescribeFriend(const Snapshot& state, const Friend& entry);

// A row of the Invite Friends list: every friend, with only the ones who can receive an invite selectable.
struct InviteChoice {
  std::string player;
  std::string name;
  std::string status;  // Online, Hosting, In game, Offline, or why it cannot be chosen
  bool selectable = false;
};
std::vector<InviteChoice> InviteChoices(const std::vector<Friend>& friends);

// "Saulo (Direct)" or "Ana (Relay) 51 ms": one seat of the room, for lists.
std::string SeatLine(const Seat& seat, const PeerSnapshot* peer);

}  // namespace eck::online
