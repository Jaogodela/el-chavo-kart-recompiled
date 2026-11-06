#include <cstring>
#include <string>
#include <vector>

#include "online/game_socket.h"
#include "online/peer_table.h"
#include "online/rendezvous.h"
#include "online_test_support.h"

using online_test::Check;
namespace online = eck::online;
using online::Endpoint;
using online::PeerPath;

namespace {

struct Sent {
  Endpoint to;
  std::vector<uint8_t> data;
};

class RecordingTransport final : public online::Transport {
 public:
  bool Send(const Endpoint& to, const uint8_t* data, size_t size) override {
    sent.push_back({to, std::vector<uint8_t>(data, data + size)});
    return true;
  }
  size_t Receive(uint8_t*, size_t, Endpoint&) override { return 0; }
  size_t CountTo(const Endpoint& to) const {
    size_t n = 0;
    for (const Sent& s : sent) n += s.to == to;
    return n;
  }
  std::vector<Sent> sent;
};

online::PeerInfo Peer(const std::string& room, int seat, const std::string& public_ip, uint16_t public_port, bool relay) {
  online::PeerInfo peer;
  peer.room = room;
  peer.seat = seat;
  peer.player = "PLAY-000" + std::to_string(seat);
  peer.name = "P" + std::to_string(seat);
  peer.ip = public_ip;
  peer.port = uint16_t(7101 + seat);
  peer.public_ip = public_ip;
  peer.public_port = public_port;
  peer.relay = relay;
  return peer;
}

std::vector<online::Seat> Seats(std::initializer_list<online::SeatState> states) {
  std::vector<online::Seat> seats;
  int index = 0;
  for (const online::SeatState state : states) {
    online::Seat seat;
    seat.seat = index;
    seat.state = state;
    if (state != online::SeatState::kEmpty) {
      seat.player = "PLAY-000" + std::to_string(index);
      seat.name = "P" + std::to_string(index);
    }
    seats.push_back(seat);
    ++index;
  }
  return seats;
}

constexpr Endpoint kSeat0{0xCB007101, 50000};  // 203.0.113.1
constexpr Endpoint kSeat2{0xCB007102, 50002};
constexpr Endpoint kSeat3{0xCB007103, 50003};

void TestRendezvousThreePeers() {
  const std::string room = "0123456789abcdef";
  online::Rendezvous rendezvous;
  RecordingTransport out;
  rendezvous.BeginRoom(online::PunchKey(room), 1);

  const Endpoint my_public{0xCB0070FE, 50001};
  std::vector<std::vector<online::Candidate>> candidates;
  const Endpoint endpoints[3] = {kSeat0, kSeat2, kSeat3};
  const int seats[3] = {0, 2, 3};
  for (int i = 0; i < 3; ++i) {
    online::PeerInfo peer = Peer(room, seats[i], "203.0.113." + std::to_string(i == 0 ? 1 : seats[i]), endpoints[i].port, false);
    peer.ip = "198.51.100." + std::to_string(seats[i]);
    const auto list = online::PeerCandidates(peer, {}, my_public);
    Check(list.size() == 2 && list[0].type == online::CandidateType::kStun && list[0].endpoint.port == endpoints[i].port,
          "STUN endpoint comes first, observed one second");
    Check(list[1].type == online::CandidateType::kObserved && list[1].endpoint.port == peer.port, "observed endpoint is the fallback");
    rendezvous.StartPunch(seats[i], list, 1000, out);
  }
  Check(rendezvous.active(), "three seats are punching");
  Check(out.CountTo(kSeat0) >= 1 && out.CountTo(kSeat2) >= 1 && out.CountTo(kSeat3) >= 1, "every peer's STUN endpoint got a punch");
  const Endpoint first_stun = online::PeerCandidates(Peer(room, 0, "203.0.113.1", 50000, false), {}, my_public)[0].endpoint;

  // A peer's ack selects only that seat.
  const auto ack_from = [&](int seat, uint64_t key) {
    return online::EncodePunch({online::PunchKind::kAck, uint8_t(seat + 1), key});
  };
  RecordingTransport reply;
  const std::vector<uint8_t> wrong = ack_from(0, 42);
  Check(rendezvous.Take(first_stun, wrong.data(), wrong.size(), reply), "a punch of another room is swallowed");
  Check(rendezvous.punch_stage(0) == online::Rendezvous::Stage::kRunning, "...but selects nothing");
  const std::vector<uint8_t> good = ack_from(0, online::PunchKey(room));
  Check(rendezvous.Take(first_stun, good.data(), good.size(), reply), "ack taken");
  Check(rendezvous.punch_stage(0) == online::Rendezvous::Stage::kDone && rendezvous.selected(0).endpoint == first_stun,
        "seat 0 selected by its ack");
  Check(rendezvous.punch_stage(2) == online::Rendezvous::Stage::kRunning && rendezvous.punch_stage(3) == online::Rendezvous::Stage::kRunning,
        "seats 2 and 3 keep punching");

  // A peer's own punch is answered with an ack carrying our seat, and its source becomes a candidate.
  const Endpoint new_source{0xC6336402, 61000};
  const std::vector<uint8_t> punch = online::EncodePunch({online::PunchKind::kPunch, 3, online::PunchKey(room)});
  rendezvous.Take(new_source, punch.data(), punch.size(), reply);
  Check(reply.sent.size() == 1 && reply.sent[0].to == new_source, "punch is acknowledged to its source");
  const auto decoded = online::DecodePunch(reply.sent[0].data.data(), reply.sent[0].data.size());
  Check(decoded && decoded->kind == online::PunchKind::kAck && decoded->side == 2, "ack names our seat (1) + 1");
  Check(rendezvous.heard(2).size() == 1 && rendezvous.heard(2)[0] == new_source, "seat 2 was heard at its source");

  // Our own seat never selects itself, and the punch times out per seat.
  const std::vector<uint8_t> self = ack_from(1, online::PunchKey(room));
  rendezvous.Take(kSeat2, self.data(), self.size(), reply);
  Check(rendezvous.punch_stage(1) == online::Rendezvous::Stage::kIdle, "own seat is ignored");
  out.sent.clear();
  rendezvous.Tick(1000 + 11000, out);
  Check(rendezvous.punch_stage(2) == online::Rendezvous::Stage::kFailed && rendezvous.punch_stage(3) == online::Rendezvous::Stage::kFailed,
        "unanswered seats fail after the punch window");
  Check(rendezvous.punch_stage(0) == online::Rendezvous::Stage::kDone, "the finished seat stays done");

  rendezvous.StopSeat(2);
  Check(rendezvous.punch_stage(2) == online::Rendezvous::Stage::kIdle, "a seat can be stopped");
  rendezvous.Stop();
  const std::vector<uint8_t> after = ack_from(0, online::PunchKey(room));
  Check(!rendezvous.Take(first_stun, after.data(), after.size(), reply) || rendezvous.punch_stage(0) == online::Rendezvous::Stage::kIdle,
        "after Stop nothing is selected");

  // Candidates on one network use the LAN endpoints.
  online::PeerInfo lan = Peer(room, 2, "203.0.113.9", 50002, false);
  lan.same_network = true;
  lan.lan = {"192.168.1.7"};
  const auto lan_list = online::PeerCandidates(lan, {"192.168.1.5"}, my_public);
  Check(!lan_list.empty() && lan_list[0].type == online::CandidateType::kLan, "same network tries the LAN endpoint first");
  Check(online::PunchKey("a") != online::PunchKey("b") && online::PunchKey("") != 0, "punch key depends on the room");
}

void TestDirectVersusRelayAsHost() {
  online::PeerTable table;
  table.Begin("room", 0, true);
  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kConnected,
                           online::SeatState::kConnected}));
  Check(table.host() && table.peers().size() == 3, "host sees three peers");
  for (int seat = 1; seat <= 3; ++seat) table.SetPeer(Peer("room", seat, "203.0.113.9", 50000 + seat, seat != 3));
  for (int seat = 1; seat <= 3; ++seat) table.BeginProbe(seat, 10.0);
  Check(table.Decide(10.5).empty(), "nothing is decided inside the probe window");

  table.DirectProven(1, {0xCB007101, 50001}, 11.0);
  auto choices = table.Decide(11.0);
  Check(choices.size() == 1 && choices[0].seat == 1 && choices[0].path == PeerPath::kDirect && choices[0].announce,
        "a proven direct path is chosen and announced to its peer");
  Check(table.SeatOf({0xCB007101, 50001}) == 1, "datagrams from the proven endpoint belong to seat 1");

  choices = table.Decide(12.6);
  Check(choices.size() == 1 && choices[0].seat == 2 && choices[0].path == PeerPath::kRelay && choices[0].announce,
        "a relay-capable silent peer falls back to relay");
  Check(table.Find(3)->path == PeerPath::kProbing, "a silent peer without relay is not given one and keeps probing");
}

void TestHostFallbackRules() {
  online::PeerTable table;
  table.Begin("room", 0, true);
  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kConnected}));
  table.SetPeer(Peer("room", 1, "203.0.113.9", 50001, true));
  table.SetPeer(Peer("room", 2, "203.0.113.9", 50002, false));
  table.BeginProbe(1, 0.0);
  table.BeginProbe(2, 0.0);
  table.DirectFailed(2);
  auto choices = table.Decide(0.1);
  Check(choices.size() == 1 && choices[0].seat == 2 && choices[0].path == PeerPath::kFailed && !choices[0].announce,
        "a failed punch without relay fails that seat only");
  Check(table.Find(2)->path == PeerPath::kFailed && !table.AllEstablished(), "a failed seat is reported");
  table.PunchStarted(1, 2.0);
  Check(table.Decide(3.0).empty(), "a late punch still gets its minimum time");
  choices = table.Decide(3.6);
  Check(choices.size() == 1 && choices[0].path == PeerPath::kRelay && choices[0].announce, "then the host falls back to relay");

  online::PeerTable no_relay;
  no_relay.Begin("room", 0, false);
  no_relay.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  no_relay.SetPeer(Peer("room", 1, "203.0.113.9", 50001, true));
  no_relay.BeginProbe(1, 0.0);
  Check(no_relay.Decide(100.0).empty(), "without a room relay the host keeps waiting for direct");
  no_relay.DirectProven(1, {1, 2}, 100.0);
  Check(no_relay.Decide(100.0).size() == 1 && no_relay.AllEstablished(), "and takes direct when it proves");
}

void TestJoinerFollowsHost() {
  online::PeerTable table;
  table.Begin("room", 2, true);
  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kConnected,
                           online::SeatState::kConnected}));
  Check(!table.host() && table.peers().size() == 1 && table.Find(0), "a guest's only peer is the host: no mesh");
  table.SetPeer(Peer("room", 1, "203.0.113.9", 50001, true));
  table.SetPeer(Peer("room", 3, "203.0.113.9", 50003, true));
  Check(table.peers().size() == 1, "another guest's endpoint is ignored");
  table.SetPeer(Peer("room", 0, "203.0.113.9", 50000, true));
  table.BeginProbe(0, 0.0);
  table.DirectProven(0, {0xCB007101, 50000}, 0.5);
  Check(table.Decide(0.5).empty() && table.Find(0)->path != PeerPath::kDirect, "the joiner waits for the host's announcement");
  Check(table.Decide(4.0).empty(), "even past the window: only the host decides");
  table.HostAnnounced(online::PathKind::kDirect);
  const auto choices = table.Decide(4.1);
  Check(choices.size() == 1 && choices[0].seat == 0 && choices[0].path == PeerPath::kDirect && !choices[0].announce,
        "the host's direct announcement is followed without announcing back");
  Check(table.AllEstablished(), "and the guest is established");

  online::PeerTable follower;
  follower.Begin("room", 1, true);
  follower.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  follower.SetPeer(Peer("room", 0, "203.0.113.9", 50000, true));
  follower.BeginProbe(0, 0.0);
  follower.HostAnnounced(online::PathKind::kRelay);
  const auto relay = follower.Decide(0.1);
  Check(relay.size() == 1 && relay[0].path == PeerPath::kRelay, "a relay announcement is followed at once");

  online::PeerTable plain;
  plain.Begin("room", 1, false);
  plain.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  plain.SetPeer(Peer("room", 0, "203.0.113.9", 50000, false));
  plain.BeginProbe(0, 0.0);
  plain.DirectFailed(0);
  const auto failed = plain.Decide(0.1);
  Check(failed.size() == 1 && failed[0].path == PeerPath::kFailed, "no relay and no direct path fails");
}

void TestEndpointNeverArrives() {
  online::PeerTable host;
  host.Begin("room", 0, true);
  host.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  host.BeginProbe(1, 0.0);
  Check(host.Decide(2.0).empty(), "the host waits out the window");
  const auto relay = host.Decide(2.6);
  Check(relay.size() == 1 && relay[0].path == PeerPath::kRelay && relay[0].announce, "then relays a seat whose endpoint never came");
  host.RelayRefused(1);
  Check(host.Find(1)->path == PeerPath::kFailed, "a relay the service refuses fails that seat");

  online::PeerTable guest;
  guest.Begin("room", 1, true);
  guest.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  guest.HostAnnounced(online::PathKind::kRelay);
  const auto follow = guest.Decide(0.1);
  Check(follow.size() == 1 && follow[0].path == PeerPath::kRelay, "a guest follows the relay without the host's endpoint");

  online::PeerTable refused;
  refused.Begin("room", 1, false);
  refused.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  refused.HostAnnounced(online::PathKind::kRelay);
  const auto failed = refused.Decide(0.1);
  Check(failed.size() == 1 && failed[0].path == PeerPath::kFailed, "a guest without the relay cannot follow it");

  online::PeerTable lone;
  lone.Begin("room", 0, false);
  lone.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  lone.BeginProbe(1, 0.0);
  Check(lone.Decide(9.0).empty() && lone.Find(1)->path == PeerPath::kProbing, "no relay: the table waits, the budget ends it");
  lone.Expire(1);
  Check(lone.Find(1)->path == PeerPath::kFailed, "and the seat is failed");

  online::PeerTable moved;
  moved.Begin("room", 1, true);
  moved.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected}));
  moved.SetPeer(Peer("room", 0, "203.0.113.9", 50000, true));
  moved.HostAnnounced(online::PathKind::kDirect);
  moved.SetPeer(Peer("room", 0, "203.0.113.9", 50009, true));
  Check(moved.Find(0)->announced == online::PathKind::kDirect, "a moved host keeps its announcement: it will not repeat it");
}

void TestKeepAlive() {
  online::Rendezvous rendezvous;
  RecordingTransport out;
  rendezvous.BeginRoom(online::PunchKey("room"), 1);
  const Endpoint host{0xCB007101, 7101};
  rendezvous.KeepAlive(host, out);
  Check(out.CountTo(host) == 1, "a keepalive is one datagram to the proven endpoint");
  const auto packet = online::DecodePunch(out.sent[0].data.data(), out.sent[0].data.size());
  Check(packet && packet->kind == online::PunchKind::kPunch && packet->side == 2 && packet->key == online::PunchKey("room"),
        "shaped as a punch of this session and seat");
}

void TestDisconnectAndReconnect() {
  online::PeerTable table;
  table.Begin("room", 0, true);
  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kConnected}));
  for (const int seat : {1, 2}) {
    table.SetPeer(Peer("room", seat, "203.0.113.9", 50000 + seat, true));
    table.BeginProbe(seat, 0.0);
  }
  table.DirectProven(1, {0xCB007101, 50001}, 0.2);
  table.Decide(0.2);
  table.Decide(5.0);  // seat 2 falls back to the relay
  Check(table.AllEstablished() && table.Find(2)->path == PeerPath::kRelay, "both peers are established");

  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kReconnecting}));
  Check(table.Find(2)->lost && !table.Find(2)->present, "a reconnecting seat is marked lost");
  Check(table.AllEstablished(), "the connected peers alone decide whether the room is established");
  Check(table.Find(2)->path == PeerPath::kRelay, "its path is kept while the seat is held");

  table.SetPeer(Peer("room", 2, "203.0.113.9", 50099, true));
  Check(!table.Find(2)->lost && table.Find(2)->path == PeerPath::kPending, "its new endpoint starts the seat's path over");
  Check(table.Find(1)->path == PeerPath::kDirect, "other seats are untouched");

  table.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kEmpty}));
  Check(table.Find(2) == nullptr && table.peers().size() == 1, "an emptied seat is forgotten");

  // The host's service link drops: relay seats wait, direct ones carry on.
  online::PeerTable linked;
  linked.Begin("room", 0, true);
  linked.UpdateSeats(Seats({online::SeatState::kConnected, online::SeatState::kConnected, online::SeatState::kConnected}));
  linked.SetPeer(Peer("room", 1, "203.0.113.9", 50001, true));
  linked.SetPeer(Peer("room", 2, "203.0.113.9", 50002, true));
  linked.BeginProbe(1, 0.0);
  linked.BeginProbe(2, 0.0);
  linked.DirectProven(1, {0xCB007101, 50001}, 0.1);
  linked.Decide(0.1);
  linked.Decide(5.0);
  linked.PauseRelay();
  Check(linked.Find(1)->path == PeerPath::kDirect && linked.Find(2)->path == PeerPath::kProbing, "relay seat waits, direct seat stays");
  Check(linked.Decide(6.0).empty(), "no relay choice while the link is down");
  linked.SetRelayCoordinated(true);
  const auto again = linked.Decide(6.0);
  Check(again.size() == 1 && again[0].seat == 2 && again[0].path == PeerPath::kRelay, "the relay is chosen again after the seat is resumed");

  linked.FailRelayPaths();
  Check(linked.Find(2)->path == PeerPath::kFailed, "a closed relay fails its seats");
}

void TestGameSocketRelay() {
  online::OnlineGameSocket socket;
  std::vector<std::vector<uint8_t>> frames;
  socket.OpenRelay(0x0A0B0C0D, [&](std::vector<uint8_t>&& frame) {
    frames.push_back(std::move(frame));
    return true;
  });
  const uint8_t data[2] = {1, 2};
  Check(!socket.Send(online::RelayPeerEndpoint(2), data, 2), "a seat not selected for relay is not sent through it");
  socket.SelectRelay(2, true);
  Check(socket.Send(online::RelayPeerEndpoint(2), data, 2) && frames.size() == 1, "a relay seat is sent as a relay frame");
  const auto frame = online::DecodeRelayFrame(frames[0].data(), frames[0].size());
  Check(frame && frame->dest == 2 && frame->token == 0x0A0B0C0D && frame->src == 0, "the frame is addressed to the seat, src left to the service");
  Check(socket.Send(online::RelayPeerEndpoint(2), data, 2) && online::DecodeRelayFrame(frames[1].data(), frames[1].size())->seq == 2,
        "sequence numbers count up");

  const std::vector<uint8_t> incoming = online::EncodeRelayFrame(0x0A0B0C0D, 1, 1, data, 2);
  std::vector<uint8_t> stamped = incoming;
  stamped[13] = 3;  // the service stamps the sender's seat
  Check(socket.DeliverRelay(stamped.data(), stamped.size()), "a frame of the open relay is accepted");
  uint8_t buffer[16];
  Endpoint from;
  Check(socket.Receive(buffer, sizeof buffer, from) == 2 && from == online::RelayPeerEndpoint(3), "it comes out of Receive as seat 3");
  std::vector<uint8_t> wrong_token = online::EncodeRelayFrame(1, 1, 1, data, 2);
  Check(!socket.DeliverRelay(wrong_token.data(), wrong_token.size()), "a frame of another token is refused");
  Check(socket.traffic().relay_rejected == 1 && socket.traffic().relay_tx_packets == 2 && socket.traffic().relay_rx_packets == 1,
        "traffic counters");
  socket.CloseRelay();
  Check(!socket.relay_open() && !socket.Send(online::RelayPeerEndpoint(2), data, 2), "closing the relay forgets the seats");
}

}  // namespace

int main() {
  TestRendezvousThreePeers();
  TestDirectVersusRelayAsHost();
  TestHostFallbackRules();
  TestJoinerFollowsHost();
  TestEndpointNeverArrives();
  TestKeepAlive();
  TestDisconnectAndReconnect();
  TestGameSocketRelay();
  return online_test::Finish();
}
