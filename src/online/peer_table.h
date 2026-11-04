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

// Per-seat transport state of the current room, for a client at seat `my_seat`. Pure: no sockets, no clock of its
// own. The host (seat 0) decides direct or relay for each peer; any other seat follows the host for the host's path
// and decides on its own for the paths between two non-host seats.
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
  void SetRelayCoordinated(bool value) { relay_coordinated_ = value; }
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

  // New choices since the last call.
  std::vector<PathChoice> Decide(double now);

  const PeerState* Find(int seat) const;
  const std::vector<PeerState>& peers() const { return peers_; }
  // Every connected peer has a path (direct or relay).
  bool AllEstablished() const;
  bool AnyFailed() const;
  // The seat whose proven direct endpoint is `endpoint`, or -1.
  int SeatOf(const Endpoint& endpoint) const;
  // The relay stopped: seats that used it have no path any more.
  void FailRelayPaths();

 private:
  PeerState* Mutable(int seat);
  PeerState& Ensure(int seat);
  bool RelayPossible(const PeerState& peer) const { return relay_coordinated_ && peer.relay_capable; }

  std::string room_;
  int my_seat_ = -1;
  bool relay_coordinated_ = false;
  std::vector<PeerState> peers_;
};

}  // namespace eck::online
