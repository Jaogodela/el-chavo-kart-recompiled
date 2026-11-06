#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "online/protocol.h"
#include "online/udp_transport.h"

namespace eck::online {

// How long a host lets a direct path try before it falls back to the relay, counted from the first probe.
inline constexpr double kDirectProbeWindowSeconds = 2.5;
// A punch that starts late still gets this long before the fallback.
inline constexpr double kMinPunchSeconds = 1.5;

enum class PeerPath { kPending, kProbing, kDirect, kRelay, kFailed };

const char* PeerPathName(PeerPath path);

// One seat's choice, as returned by PeerTable::Decide. `announce` is set for the host's path to a peer: the caller
// reports it to the service (match.transport) so the peer follows.
struct PathChoice {
  int seat = -1;
  PeerPath path = PeerPath::kPending;
  bool announce = false;
};

struct PeerState {
  int seat = -1;
  bool present = false;  // the seat is occupied and connected
  bool lost = false;     // reconnecting
  std::string player;
  std::string name;
  bool endpoint_known = false;
  PeerInfo info;
  bool relay_capable = false;  // the peer advertised relay in its endpoint
  PeerPath path = PeerPath::kPending;
  bool direct_proven = false;
  bool direct_failed = false;
  Endpoint direct;                          // the proven UDP endpoint
  PathKind announced = PathKind::kUnknown;  // the host's choice for its path to this client, on the host's entry
  double probe_deadline = 0;
};

// Per-seat transport state of the current session, for a client at seat `my_seat`. Pure: no sockets, no clock of its
// own. A star: the host (seat 0) has one path to each seat and decides direct or relay for each; any other seat has
// only its path to the host and follows that decision.
class PeerTable {
 public:
  void Begin(const std::string& room, int my_seat, bool relay_coordinated);
  void Clear();
  bool active() const { return !room_.empty(); }
  const std::string& room() const { return room_; }
  int my_seat() const { return my_seat_; }
  bool host() const { return my_seat_ == 0; }
  bool relay_coordinated() const { return relay_coordinated_; }
  // The relay of this room stopped working: no more relay choices.
  void SetRelayCoordinated(bool value) {
    relay_coordinated_ = value;
    relay_paused_ = false;
  }
  // The service link dropped: relay seats wait for the seat to be resumed, then choose again.
  void PauseRelay();

  // From room.updated / room.created / match.resume: seat occupancy. A seat that emptied is forgotten; one that is
  // reconnecting keeps its path but is marked lost.
  void UpdateSeats(const std::vector<Seat>& seats);
  // A peer's endpoint arrived (again). A republished endpoint of a seat that was lost, or from another address, starts
  // that seat's path over.
  void SetPeer(const PeerInfo& peer);
  void BeginProbe(int seat, double now);
  void PunchStarted(int seat, double now);
  void DirectProven(int seat, const Endpoint& endpoint, double now);
  void DirectFailed(int seat);
  // match.transport event: the host's choice for its path to this client.
  void HostAnnounced(PathKind kind);
  // The service would not relay this seat.
  void RelayRefused(int seat);
  // The seat ran out of time to establish a path.
  void Expire(int seat);

  // New choices since the last call.
  std::vector<PathChoice> Decide(double now);

  const PeerState* Find(int seat) const;
  const std::vector<PeerState>& peers() const { return peers_; }
  // Every connected peer has a path (direct or relay).
  bool AllEstablished() const;
  // Every peer is on a proven direct path: nothing here depends on the service.
  bool AllDirect() const;
  // The seat whose proven direct endpoint is `endpoint`, or -1.
  int SeatOf(const Endpoint& endpoint) const;
  // The relay stopped: seats that used it have no path any more.
  void FailRelayPaths();

 private:
  PeerState* Mutable(int seat);
  bool OnMyStar(int seat) const { return host() || seat == 0; }
  PeerState& Ensure(int seat);
  // Until a peer's endpoint arrives its relay support is unknown: the service answers for it when the relay is chosen.
  bool RelayPossible(const PeerState& peer) const { return relay_coordinated_ && (peer.relay_capable || !peer.endpoint_known); }

  std::string room_;
  int my_seat_ = -1;
  bool relay_coordinated_ = false;
  bool relay_paused_ = false;
  std::vector<PeerState> peers_;
};

}  // namespace eck::online
