#include "online/rendezvous.h"

#include <algorithm>
#include <cstring>

#include "online/net_addr.h"
#include "online/random.h"
#include "online/stun.h"

namespace eck::online {
namespace {

constexpr uint64_t kStunMs = 2500;
constexpr uint64_t kPunchMs = 10000;
constexpr uint64_t kPunchIntervalMs = 100;
constexpr uint8_t kPunchMagic[4] = {'E', 'K', 'P', 'N'};
constexpr size_t kPunchSize = 14;
constexpr uint32_t kLoopbackAddress = 0x7F000001;

bool LooksLikeStun(const uint8_t* data, size_t size) {
  return size >= 20 && (data[0] & 0xc0) == 0 && data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xa4 &&
         data[7] == 0x42;
}

bool IsLoopbackText(const std::string& ip) { return ip.rfind("127.", 0) == 0; }

// Parses a service-supplied IPv4 literal.
bool ParseAddress(const std::string& text, uint32_t& address) {
  return UsableAddress(ParsePeerAddress(text, address));
}

bool SeatIndexOk(int seat) { return seat >= 0 && seat < kMaxSeats; }

}  // namespace

const char* CandidateTypeName(CandidateType type) {
  switch (type) {
    case CandidateType::kLan: return "lan";
    case CandidateType::kStun: return "stun";
    case CandidateType::kUpnp: return "upnp";
    case CandidateType::kObserved: return "observed";
    default: return "peer";
  }
}

std::string EndpointText(const Endpoint& endpoint) { return net::EndpointText({endpoint.address, endpoint.port}); }

std::string EndpointLogText(const Endpoint& endpoint) {
  return std::to_string(endpoint.address >> 24) + ".x.x.x:" + std::to_string(endpoint.port);
}

std::vector<Candidate> PeerCandidates(const PeerInfo& peer, const std::vector<std::string>& my_lan,
                                      const Endpoint& my_public) {
  std::vector<Candidate> out;
  const auto add = [&out](CandidateType type, uint32_t address, uint16_t port) {
    const Endpoint endpoint{address, port};
    if (!address || !port) return;
    if (std::any_of(out.begin(), out.end(), [&](const Candidate& c) { return c.endpoint == endpoint; })) return;
    out.push_back({type, endpoint});
  };
  uint32_t peer_public = 0;
  const bool has_public = peer.public_port && ParseAddress(peer.public_ip, peer_public);
  const bool shared_network = peer.same_network || (has_public && my_public.address == peer_public);
  if (shared_network) {
    std::vector<std::string> remote;
    for (const std::string& ip : peer.lan) {
      if (!IsLoopbackText(ip)) remote.push_back(ip);
    }
    const bool same_pc = remote.empty() || std::any_of(remote.begin(), remote.end(), [&](const std::string& ip) {
      return std::find(my_lan.begin(), my_lan.end(), ip) != my_lan.end();
    });
    if (same_pc) add(CandidateType::kLan, kLoopbackAddress, peer.port);
    for (const std::string& ip : remote) add(CandidateType::kLan, net::ParseIp(ip), peer.port);
    for (const EndpointCandidate& c : peer.candidates) {
      uint32_t address = 0;
      if (c.kind == "host" && ParseAddress(c.ip, address) && !IsLoopbackText(c.ip)) {
        add(CandidateType::kLan, address, c.port);
      }
    }
  }
  if (has_public) add(CandidateType::kStun, peer_public, peer.public_port);
  for (const EndpointCandidate& c : peer.candidates) {
    uint32_t address = 0;
    if (c.kind == "host" || !ParseAddress(c.ip, address)) continue;
    add(c.kind == "upnp" ? CandidateType::kUpnp : CandidateType::kStun, address, c.port);
  }
  if (uint32_t observed = 0; ParseAddress(peer.ip, observed)) add(CandidateType::kObserved, observed, peer.port);
  return out;
}

uint64_t PunchKey(const std::string& room) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const char c : room) hash = (hash ^ uint8_t(c)) * 0x100000001b3ull;
  return hash ? hash : 1;
}

std::vector<uint8_t> EncodePunch(const PunchPacket& packet) {
  std::vector<uint8_t> out(kPunchSize);
  std::memcpy(out.data(), kPunchMagic, sizeof kPunchMagic);
  out[4] = uint8_t(packet.kind);
  out[5] = packet.side;
  for (int i = 0; i < 8; ++i) out[6 + i] = uint8_t(packet.key >> (56 - 8 * i));
  return out;
}

std::optional<PunchPacket> DecodePunch(const uint8_t* data, size_t size) {
  if (size != kPunchSize || std::memcmp(data, kPunchMagic, sizeof kPunchMagic) != 0) return std::nullopt;
  if (data[4] != uint8_t(PunchKind::kPunch) && data[4] != uint8_t(PunchKind::kAck)) return std::nullopt;
  PunchPacket packet;
  packet.kind = PunchKind(data[4]);
  packet.side = data[5];
  for (int i = 0; i < 8; ++i) packet.key = packet.key << 8 | data[6 + i];
  return packet;
}

void Rendezvous::StartStun(const std::vector<Endpoint>& servers, uint16_t local_port, uint64_t now_ms, Transport& out) {
  stun_.clear();
  mapped_ = {};
  logged_second_ = false;
  local_port_ = local_port;
  for (const Endpoint& server : servers) {
    StunQuery query;
    query.server = server;
    if (server.address && server.port && RandomBytes(query.id.data(), query.id.size())) stun_.push_back(query);
  }
  if (stun_.empty()) {
    stun_stage_ = Stage::kFailed;
    Write("stun mapped endpoint=none reason=no_server local_port=" + std::to_string(local_port_));
    return;
  }
  stun_stage_ = Stage::kRunning;
  stun_deadline_ = now_ms + kStunMs;
  stun_attempt_ = 0;
  stun_next_send_ = now_ms;
  Tick(now_ms, out);
}

void Rendezvous::BeginRoom(uint64_t key, int my_seat) {
  for (SeatPunch& seat : seats_) seat = {};
  key_ = key;
  my_seat_ = my_seat;
  punch_next_send_ = 0;
}

void Rendezvous::StartPunch(int seat, const std::vector<Candidate>& candidates, uint64_t now_ms, Transport& out) {
  if (!SeatIndexOk(seat) || seat == my_seat_ || !key_) return;
  SeatPunch& punch = seats_[size_t(seat)];
  punch = {};
  for (const Candidate& candidate : candidates) {
    if (!Known(punch, candidate.endpoint)) punch.targets.push_back({candidate});
  }
  if (punch.targets.empty()) {
    punch.stage = Stage::kFailed;
    Write("udp rendezvous failed seat=" + std::to_string(seat) + " reason=no_candidates");
    return;
  }
  punch.stage = Stage::kRunning;
  punch.deadline = now_ms + kPunchMs;
  punch_next_send_ = now_ms;
  Tick(now_ms, out);
}

Rendezvous::Stage Rendezvous::punch_stage(int seat) const {
  return SeatIndexOk(seat) ? seats_[size_t(seat)].stage : Stage::kIdle;
}

const Candidate& Rendezvous::selected(int seat) const {
  static const Candidate none;
  return SeatIndexOk(seat) ? seats_[size_t(seat)].selected : none;
}

const std::vector<Endpoint>& Rendezvous::heard(int seat) const {
  static const std::vector<Endpoint> none;
  return SeatIndexOk(seat) ? seats_[size_t(seat)].heard : none;
}

void Rendezvous::StopSeat(int seat) {
  if (SeatIndexOk(seat)) seats_[size_t(seat)] = {};
}

void Rendezvous::KeepAlive(const Endpoint& to, Transport& out) const {
  const std::vector<uint8_t> packet = EncodePunch({PunchKind::kPunch, uint8_t(my_seat_ + 1), key_});
  out.Send(to, packet.data(), packet.size());
}

void Rendezvous::Stop() {
  stun_stage_ = Stage::kIdle;
  stun_.clear();
  mapped_ = {};
  key_ = 0;
  my_seat_ = -1;
  for (SeatPunch& seat : seats_) seat = {};
}

bool Rendezvous::active() const {
  return stun_stage_ == Stage::kRunning ||
         std::any_of(seats_.begin(), seats_.end(), [](const SeatPunch& s) { return s.stage == Stage::kRunning; });
}

void Rendezvous::Tick(uint64_t now_ms, Transport& out) {
  if (stun_stage_ == Stage::kRunning) {
    if (now_ms >= stun_deadline_) {
      stun_stage_ = Stage::kFailed;
      Write("stun mapped endpoint=none reason=no_answer local_port=" + std::to_string(local_port_));
    } else if (now_ms >= stun_next_send_) {
      SendStun(out);
      stun_next_send_ = now_ms + (250u << std::min(stun_attempt_++, 3));
    }
  }
  const bool send_due = now_ms >= punch_next_send_;
  bool sent = false;
  for (int seat = 0; seat < kMaxSeats; ++seat) {
    SeatPunch& punch = seats_[size_t(seat)];
    if (punch.stage != Stage::kRunning) continue;
    if (now_ms >= punch.deadline) {
      punch.stage = Stage::kFailed;
      Write("udp rendezvous failed seat=" + std::to_string(seat) + " reason=punch_timeout candidates=" +
            std::to_string(punch.targets.size()) + " peer_heard=" + (punch.heard.empty() ? "false" : "true"));
    } else if (send_due) {
      SendPunches(punch, seat, out);
      sent = true;
    }
  }
  if (sent) punch_next_send_ = now_ms + kPunchIntervalMs;
}

bool Rendezvous::Take(const Endpoint& from, const uint8_t* data, size_t size, Transport& reply) {
  if (LooksLikeStun(data, size)) {
    TakeStun(data, size);
    return true;
  }
  if (size != kPunchSize || std::memcmp(data, kPunchMagic, sizeof kPunchMagic) != 0) return false;
  if (const std::optional<PunchPacket> packet = DecodePunch(data, size); packet && key_ && packet->key == key_) {
    TakePunch(from, *packet, reply);
  }
  return true;
}

void Rendezvous::SendStun(Transport& out) {
  for (const StunQuery& query : stun_) {
    if (query.answered) continue;
    uint8_t request[net::stun::kRequestSize];
    net::stun::MakeRequest(query.id, request);
    out.Send(query.server, request, sizeof request);
  }
}

void Rendezvous::TakeStun(const uint8_t* data, size_t size) {
  for (StunQuery& query : stun_) {
    net::Endpoint mapped;
    if (query.answered || !net::stun::ParseResponse(data, size, query.id, mapped)) continue;
    query.answered = true;
    const Endpoint endpoint{mapped.ip, mapped.port};
    if (stun_stage_ == Stage::kRunning) {
      stun_stage_ = Stage::kDone;
      mapped_ = endpoint;
      Write("stun mapped endpoint=" + EndpointLogText(endpoint) + " local_port=" + std::to_string(local_port_));
    } else if (stun_stage_ == Stage::kDone && !logged_second_) {
      logged_second_ = true;
      Write(std::string("stun second endpoint agrees=") + (endpoint == mapped_ ? "true" : "false"));
    }
    return;
  }
}

void Rendezvous::TakePunch(const Endpoint& from, const PunchPacket& packet, Transport& reply) {
  const int seat = int(packet.side) - 1;
  if (!SeatIndexOk(seat) || seat == my_seat_) return;
  SeatPunch& punch = seats_[size_t(seat)];
  if (std::find(punch.heard.begin(), punch.heard.end(), from) == punch.heard.end()) {
    punch.heard.push_back(from);
    Write("udp punch rx seat=" + std::to_string(seat) + " endpoint=" + EndpointLogText(from));
  }
  if (packet.kind == PunchKind::kPunch) {
    const std::vector<uint8_t> ack = EncodePunch({PunchKind::kAck, uint8_t(my_seat_ + 1), key_});
    reply.Send(from, ack.data(), ack.size());
    if (punch.stage == Stage::kRunning && !Known(punch, from)) {
      punch.targets.push_back({{CandidateType::kPeer, from}});
      Write("peer candidate received seat=" + std::to_string(seat) + " type=peer");
    }
    return;
  }
  if (punch.stage != Stage::kRunning) return;
  punch.selected = {CandidateType::kPeer, from};
  for (const Target& target : punch.targets) {
    if (target.candidate.endpoint == from) punch.selected.type = target.candidate.type;
  }
  punch.stage = Stage::kDone;
  Write("udp candidate selected seat=" + std::to_string(seat) + " endpoint=" + EndpointLogText(from) +
        " type=" + CandidateTypeName(punch.selected.type));
}

void Rendezvous::SendPunches(SeatPunch& punch, int seat, Transport& out) {
  const std::vector<uint8_t> packet = EncodePunch({PunchKind::kPunch, uint8_t(my_seat_ + 1), key_});
  for (Target& target : punch.targets) {
    out.Send(target.candidate.endpoint, packet.data(), packet.size());
    if (target.sent) continue;
    target.sent = true;
    Write("udp punch tx seat=" + std::to_string(seat) + " endpoint=" + EndpointLogText(target.candidate.endpoint) +
          " type=" + CandidateTypeName(target.candidate.type));
  }
}

bool Rendezvous::Known(const SeatPunch& punch, const Endpoint& endpoint) {
  return std::any_of(punch.targets.begin(), punch.targets.end(),
                     [&](const Target& target) { return target.candidate.endpoint == endpoint; });
}

void Rendezvous::Write(const std::string& line) const {
  if (log_) log_(line);
}

}  // namespace eck::online
