#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "online/game_socket.h"
#include "online/protocol.h"

namespace eck::online {

enum class CandidateType { kLan, kStun, kUpnp, kObserved, kPeer };

// A complete endpoint a peer may be reached at. kObserved is the signaling service's view of the peer's address with
// its local port (unproven guesswork); kPeer is where the peer's own punches came from.
struct Candidate {
  CandidateType type = CandidateType::kStun;
  Endpoint endpoint;
  friend bool operator==(const Candidate&, const Candidate&) = default;
};

const char* CandidateTypeName(CandidateType type);
std::string EndpointText(const Endpoint& endpoint);
// The text of the endpoint with the address reduced to its first octet, for logs.
std::string EndpointLogText(const Endpoint& endpoint);

// A peer's candidates, most direct first: its LAN endpoints only when both players share a network, its advertised
// STUN/UPnP endpoints, then the observed one as a fallback. `my_public` is this socket's own STUN endpoint.
std::vector<Candidate> PeerCandidates(const PeerInfo& peer, const std::vector<std::string>& my_lan,
                                      const Endpoint& my_public);

// Every member of a room derives it from the room id, so punches of any other room are ignored.
uint64_t PunchKey(const std::string& room);

enum class PunchKind : uint8_t { kPunch = 1, kAck = 2 };

struct PunchPacket {
  PunchKind kind = PunchKind::kPunch;
  uint8_t side = 0;  // the sender's seat + 1
  uint64_t key = 0;
};

std::vector<uint8_t> EncodePunch(const PunchPacket& packet);
std::optional<PunchPacket> DecodePunch(const uint8_t* data, size_t size);

// Room setup on the gameplay socket: learns this socket's public endpoint from STUN, then punches every peer seat's
// candidates until one answers. Only a punch acknowledgement from a seat selects a candidate for it (the round trip
// proves the path both ways), and it selects the address the acknowledgement actually came from.
// Not thread-safe: every call runs under the OnlineGameSocket's lock (inside Use, or as its tap).
class Rendezvous final : public DatagramTap {
 public:
  enum class Stage { kIdle, kRunning, kDone, kFailed };
  using Log = std::function<void(const std::string&)>;

  explicit Rendezvous(Log log = {}) : log_(std::move(log)) {}

  void StartStun(const std::vector<Endpoint>& servers, uint16_t local_port, uint64_t now_ms, Transport& out);
  Stage stun_stage() const { return stun_stage_; }
  const Endpoint& mapped() const { return mapped_; }

  // Sets the room's key and this client's seat; seats are added with StartPunch.
  void BeginRoom(uint64_t key, int my_seat);
  // Starts (or restarts) punching one peer seat.
  void StartPunch(int seat, const std::vector<Candidate>& candidates, uint64_t now_ms, Transport& out);
  Stage punch_stage(int seat) const;
  const Candidate& selected(int seat) const;
  // Where this seat's punches came from, first heard first.
  const std::vector<Endpoint>& heard(int seat) const;
  void StopSeat(int seat);

  // Forgets the room: from now on its STUN answers and punches are dropped unanswered.
  void Stop();
  void Tick(uint64_t now_ms, Transport& out);
  bool active() const;

  bool Take(const Endpoint& from, const uint8_t* data, size_t size, Transport& reply) override;

 private:
  struct StunQuery {
    Endpoint server;
    std::array<uint8_t, 12> id{};
    bool answered = false;
  };
  struct Target {
    Candidate candidate;
    bool sent = false;
  };
  struct SeatPunch {
    Stage stage = Stage::kIdle;
    std::vector<Target> targets;
    std::vector<Endpoint> heard;
    uint64_t deadline = 0;
    Candidate selected;
  };

  void SendStun(Transport& out);
  void TakeStun(const uint8_t* data, size_t size);
  void TakePunch(const Endpoint& from, const PunchPacket& packet, Transport& reply);
  void SendPunches(SeatPunch& punch, int seat, Transport& out);
  static bool Known(const SeatPunch& punch, const Endpoint& endpoint);
  void Write(const std::string& line) const;

  Log log_;

  Stage stun_stage_ = Stage::kIdle;
  std::vector<StunQuery> stun_;
  uint16_t local_port_ = 0;
  uint64_t stun_deadline_ = 0;
  uint64_t stun_next_send_ = 0;
  int stun_attempt_ = 0;
  Endpoint mapped_;
  bool logged_second_ = false;

  uint64_t key_ = 0;
  int my_seat_ = -1;
  std::array<SeatPunch, kMaxSeats> seats_;
  uint64_t punch_next_send_ = 0;
};

}  // namespace eck::online
