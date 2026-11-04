#include "online/peer_table.h"

#include <algorithm>

namespace eck::online {

const char* PeerPathName(PeerPath path) {
  switch (path) {
    case PeerPath::kProbing: return "probing";
    case PeerPath::kDirect: return "direct";
    case PeerPath::kRelay: return "relay";
    case PeerPath::kFailed: return "failed";
    default: return "pending";
  }
}

void PeerTable::Begin(const std::string& room, int my_seat, bool relay_coordinated) {
  room_ = room;
  my_seat_ = my_seat;
  relay_coordinated_ = relay_coordinated;
  peers_.clear();
}

void PeerTable::Clear() {
  room_.clear();
  my_seat_ = -1;
  relay_coordinated_ = false;
  peers_.clear();
}

PeerState* PeerTable::Mutable(int seat) {
  for (PeerState& peer : peers_) {
    if (peer.seat == seat) return &peer;
  }
  return nullptr;
}

PeerState& PeerTable::Ensure(int seat) {
  if (PeerState* peer = Mutable(seat)) return *peer;
  PeerState fresh;
  fresh.seat = seat;
  const auto at = std::find_if(peers_.begin(), peers_.end(), [seat](const PeerState& p) { return p.seat > seat; });
  return *peers_.insert(at, std::move(fresh));
}

const PeerState* PeerTable::Find(int seat) const {
  for (const PeerState& peer : peers_) {
    if (peer.seat == seat) return &peer;
  }
  return nullptr;
}

void PeerTable::UpdateSeats(const std::vector<Seat>& seats) {
  if (!active()) return;
  for (const Seat& seat : seats) {
    if (seat.seat == my_seat_) continue;
    if (seat.state == SeatState::kEmpty) {
      peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const PeerState& p) { return p.seat == seat.seat; }),
                   peers_.end());
      continue;
    }
    PeerState& peer = Ensure(seat.seat);
    if (!peer.player.empty() && peer.player != seat.player) {
      peer = PeerState{};
      peer.seat = seat.seat;
    }
    peer.player = seat.player;
    peer.name = seat.name;
    peer.present = seat.state == SeatState::kConnected;
    peer.lost = seat.state == SeatState::kReconnecting;
  }
}

void PeerTable::SetPeer(const PeerInfo& info) {
  if (!active() || info.room != room_ || info.seat == my_seat_ || info.seat < 0) return;
  PeerState& peer = Ensure(info.seat);
  const bool moved = peer.endpoint_known && (peer.info.ip != info.ip || peer.info.port != info.port ||
                                             peer.info.public_ip != info.public_ip ||
                                             peer.info.public_port != info.public_port);
  if (peer.lost || moved) {
    const std::string player = peer.player;
    const std::string name = peer.name;
    peer = PeerState{};
    peer.seat = info.seat;
    peer.player = player;
    peer.name = name;
  }
  peer.present = true;
  peer.lost = false;
  if (!info.player.empty()) peer.player = info.player;
  if (!info.name.empty()) peer.name = info.name;
  peer.info = info;
  peer.endpoint_known = true;
  peer.relay_capable = info.relay;
}

void PeerTable::BeginProbe(int seat, double now) {
  PeerState* peer = Mutable(seat);
  if (!peer || peer->path != PeerPath::kPending) return;
  peer->path = PeerPath::kProbing;
  peer->probe_deadline = now + kDirectProbeWindowSeconds;
}

void PeerTable::PunchStarted(int seat, double now) {
  PeerState* peer = Mutable(seat);
  if (peer && peer->path == PeerPath::kProbing) {
    peer->probe_deadline = std::max(peer->probe_deadline, now + kMinPunchSeconds);
  }
}

void PeerTable::DirectProven(int seat, const Endpoint& endpoint, double now) {
  PeerState* peer = Mutable(seat);
  if (!peer || peer->path == PeerPath::kDirect || peer->path == PeerPath::kRelay) return;
  BeginProbe(seat, now);
  peer->direct = endpoint;
  peer->direct_proven = true;
  peer->direct_failed = false;
}

void PeerTable::DirectFailed(int seat) {
  PeerState* peer = Mutable(seat);
  if (peer && peer->path != PeerPath::kDirect && peer->path != PeerPath::kRelay && !peer->direct_proven) {
    peer->direct_failed = true;
  }
}

void PeerTable::HostAnnounced(PathKind kind) {
  if (host()) return;
  Ensure(0).announced = kind;
}

std::vector<PathChoice> PeerTable::Decide(double now) {
  std::vector<PathChoice> out;
  for (PeerState& peer : peers_) {
    const bool open = peer.path == PeerPath::kPending || peer.path == PeerPath::kProbing;
    if (!peer.present || !open || !peer.endpoint_known) continue;
    const bool expired = peer.path == PeerPath::kProbing && now >= peer.probe_deadline;
    const bool give_up = !peer.direct_proven && (peer.direct_failed || expired);
    const bool follows_host = !host() && peer.seat == 0 && RelayPossible(peer);
    PeerPath choice = PeerPath::kPending;
    bool announce = false;
    if (follows_host) {
      if (peer.announced == PathKind::kRelay) choice = PeerPath::kRelay;
      else if (peer.announced == PathKind::kDirect && peer.direct_proven) choice = PeerPath::kDirect;
      else if (peer.announced == PathKind::kDirect && peer.direct_failed) choice = PeerPath::kFailed;
    } else if (peer.direct_proven) {
      choice = PeerPath::kDirect;
      announce = host();
    } else if (give_up) {
      if (RelayPossible(peer)) {
        choice = PeerPath::kRelay;
        announce = host();
      } else if (peer.direct_failed) {
        choice = PeerPath::kFailed;
      }
    }
    if (choice == PeerPath::kPending) continue;
    peer.path = choice;
    out.push_back({peer.seat, choice, announce});
  }
  return out;
}

bool PeerTable::AllEstablished() const {
  bool any = false;
  for (const PeerState& peer : peers_) {
    if (!peer.present) continue;
    any = true;
    if (peer.path != PeerPath::kDirect && peer.path != PeerPath::kRelay) return false;
  }
  return any;
}

bool PeerTable::AnyFailed() const {
  return std::any_of(peers_.begin(), peers_.end(),
                     [](const PeerState& peer) { return peer.present && peer.path == PeerPath::kFailed; });
}

void PeerTable::PauseRelay() {
  relay_coordinated_ = false;
  for (PeerState& peer : peers_) {
    if (peer.path == PeerPath::kRelay) {
      peer.path = PeerPath::kProbing;
      peer.probe_deadline = 0;
    }
  }
}

void PeerTable::FailRelayPaths() {
  relay_coordinated_ = false;
  for (PeerState& peer : peers_) {
    if (peer.path == PeerPath::kRelay) peer.path = PeerPath::kFailed;
  }
}

int PeerTable::SeatOf(const Endpoint& endpoint) const {
  for (const PeerState& peer : peers_) {
    if (peer.path == PeerPath::kDirect && peer.direct == endpoint) return peer.seat;
  }
  return -1;
}

}  // namespace eck::online
