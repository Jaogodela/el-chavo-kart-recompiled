// Everything two PCs or the online service exchange is byte-for-byte the same on every platform: each encoder must
// reproduce tests/wire-vectors.txt exactly and each decoder must read it back. The same file is checked by the online
// service's tests (server/eck-lobby/test/wire.test.js), so Windows, Linux and the Worker agree on one layout.
//
//   eck_wire_parity_tests           compare with tests/wire-vectors.txt
//   eck_wire_parity_tests --print   print the vectors this build produces
//   eck_wire_parity_tests --peer F  compare with the vectors another platform printed into F, and decode them

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "online/constants.h"
#include "online/identity.h"
#include "online/protocol.h"
#include "online/relay_frame.h"
#include "online/rendezvous.h"
#include "online/stun.h"
#include "online/websocket_frame.h"
#include "online_race.h"
#include "online_test_support.h"

namespace online = eck::online;
namespace race = eck::race;
using online_test::Check;

namespace {

using Bytes = std::vector<uint8_t>;

std::string ToHex(const Bytes& bytes) {
  std::string out;
  char two[3];
  for (const uint8_t b : bytes) {
    std::snprintf(two, sizeof two, "%02x", b);
    out += two;
  }
  return out;
}

Bytes FromHex(const std::string& text) {
  Bytes out;
  for (size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
  return out;
}

Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

Bytes BigEndian(uint64_t value, int bytes) {
  Bytes out;
  for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(value >> (i * 8)));
  return out;
}

Bytes Pattern(size_t size, uint32_t seed) {
  Bytes out(size);
  uint32_t x = seed;
  for (uint8_t& b : out) {
    x = x * 1664525u + 1013904223u;
    b = uint8_t(x >> 24);
  }
  return out;
}

race::RaceConfig FourSeatConfig() {
  race::RaceConfig config;
  config.race_id = 0x1A2B3C4D;
  config.track = 0x0102;
  config.laps = 3;
  config.total_karts = 8;
  config.bot_difficulty = 2;
  config.rules = 1;
  config.seed = 0xCAFEF00D;
  config.participants = {{0, 1, 0}, {1, 5, 1}, {2, 7, 2}, {3, 11, 3}};
  return config;
}

race::KartState SampleKart() {
  race::KartState kart;
  kart.race_id = 0x1A2B3C4D;
  kart.seat = 2;
  kart.tick = 0x01020304;
  kart.position[0] = 1024.5f;
  kart.position[1] = -2048.25f;
  kart.position[2] = 64.125f;
  kart.rotation[0] = 16384;
  kart.rotation[1] = -32768;
  kart.rotation[2] = 65535;
  kart.velocity[0] = 100.0f;
  kart.velocity[1] = -0.5f;
  kart.velocity[2] = 0.0f;
  kart.steering = -0.75f;
  kart.throttle = 1.0f;
  kart.lap = 2;
  kart.checkpoint = 0x0A0B;
  kart.place = 3;
  kart.flags = 5;
  return kart;
}

race::InputMsg SampleInput() {
  race::InputMsg input;
  input.race_id = 0x1A2B3C4D;
  input.seat = 1;
  input.seq = 0x00000E10;
  input.snapshot_ack = 0x00000D00;
  input.loaded = true;
  input.pad = {0x9001, 0x40, 0xFF, -32768, 32767, 1234, -1234};
  return input;
}

race::SnapshotMsg SampleSnapshot(int karts) {
  race::SnapshotMsg snapshot;
  snapshot.race_id = 0x1A2B3C4D;
  snapshot.seq = 0x00000F00;
  snapshot.host_ms = 0x00ABCDEF;
  snapshot.race_state = 4;
  snapshot.begun = true;
  snapshot.input_ack = {0, 0x00000E10, 0x00000E0F, 0x00000E0E};
  snapshot.input_age_ms = {0, 16, 33, 255};
  for (int i = 0; i < karts; ++i) {
    race::KartSample kart;
    kart.slot = uint8_t(i);
    kart.position[0] = 10.5f * float(i + 1);
    kart.position[1] = -3.25f * float(i + 1);
    kart.position[2] = 0.125f;
    kart.velocity[0] = float(i);
    kart.speed = 42.0f + float(i);
    kart.lap = uint8_t(1 + i);
    kart.distance = 1234.5f * float(i + 1);
    kart.place = uint8_t(1 + i);
    kart.finished = i % 2 == 0;
    kart.finish_place = kart.finished ? uint8_t(1 + i) : 0;
    kart.racing_time = 12.25f * float(i + 1);
    snapshot.karts.push_back(kart);
  }
  return snapshot;
}

online::HelloFields SampleHello() {
  online::HelloFields hello;
  hello.player = "ABCD-2345";
  hello.token = std::string(64, 'a');
  hello.name = "Saulo";
  hello.version = "0.1.0";
  hello.platform = "Linux";
  hello.relay = true;
  return hello;
}

// The sender side of every vector: what this build puts on the wire.
std::map<std::string, Bytes> Produce() {
  std::map<std::string, Bytes> v;
  v["race.config"] = race::Encode(FourSeatConfig());
  v["race.phase"] = race::Encode(race::PhaseMsg{0x1A2B3C4D, race::Phase::kCountdown, 3});
  v["race.kart_state"] = race::Encode(SampleKart());
  v["race.finish"] = race::Encode(race::Finish{0x1A2B3C4D, 2, 0x0001E240, 1});
  race::RaceResult result;
  result.race_id = 0x1A2B3C4D;
  result.outcome = race::Outcome::kCompleted;
  result.entries = {{0, race::SeatResult::kFinished, 1, 0x0001E240}, {1, race::SeatResult::kLost, 0, 0}};
  v["race.result"] = race::Encode(result);
  v["race.leave"] = race::Encode(race::Leave{0x1A2B3C4D, 3});
  v["race.loaded"] = race::Encode(race::Loaded{0x1A2B3C4D, 1});
  v["race.input"] = race::Encode(SampleInput());
  v["race.snapshot.two"] = race::Encode(SampleSnapshot(2));
  v["race.snapshot.full"] = race::Encode(SampleSnapshot(race::kMaxKarts));

  const Bytes payload = Pattern(32, 0x45434B31);
  v["relay.seat"] = online::EncodeRelayFrame(0x89ABCDEF, 7, 2, payload.data(), payload.size());
  v["relay.broadcast"] = online::EncodeRelayFrame(0x00000001, 0xFFFFFFFF, online::kRelayDestAll, payload.data(), 1);

  v["punch.key"] = BigEndian(online::PunchKey("0123456789abcdef"), 8);
  v["punch.punch"] = online::EncodePunch({online::PunchKind::kPunch, 1, 0x0102030405060708ull});
  v["punch.ack"] = online::EncodePunch({online::PunchKind::kAck, 4, 0xFEDCBA9876543210ull});

  online::net::stun::TransactionId id{};
  for (size_t i = 0; i < id.size(); ++i) id[i] = uint8_t(0xA0 + i);
  Bytes stun(online::net::stun::kRequestSize);
  online::net::stun::MakeRequest(id, stun.data());
  v["stun.request"] = stun;

  const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
  const std::string small = "{\"t\":\"ping\",\"seq\":1}";
  v["ws.text.small"] = online::ws::ClientFrame(1, reinterpret_cast<const uint8_t*>(small.data()), small.size(), mask);
  const Bytes wide = Pattern(300, 7);
  v["ws.binary.wide"] = online::ws::ClientFrame(2, wide.data(), wide.size(), mask);
  v["ws.accept"] = Text(online::ws::AcceptFor("dGhlIHNhbXBsZSBub25jZQ=="));

  v["lobby.hello"] = Text(online::EncodeHello(1, SampleHello()));
  v["lobby.ping"] = Text(online::EncodePing(2));
  v["lobby.friends_add"] = Text(online::EncodeFriendsAdd(3, "WXYZ-9876"));
  v["lobby.room_host"] = Text(online::EncodeRoomHost(4, 4, {{"mode", 1}, {"track", 7}, {"laps", 3}}));
  v["lobby.room_start"] = Text(online::EncodeRoomStart(5, "0123456789abcdef"));
  v["lobby.invite_send"] = Text(online::EncodeInviteSend(6, "WXYZ-9876"));
  online::EndpointFields endpoint;
  endpoint.port = 7101;
  endpoint.lan = {"192.168.0.10"};
  endpoint.public_ip = "203.0.113.7";
  endpoint.public_port = 41000;
  endpoint.candidates = {{"203.0.113.7", 41000, "srflx"}, {"192.168.0.10", 7101, "host"}};
  endpoint.relay = true;
  v["lobby.match_endpoint"] = Text(online::EncodeMatchEndpoint(7, "0123456789abcdef", endpoint));
  v["lobby.match_ready"] = Text(online::EncodeMatchReady(8, "0123456789abcdef", true));
  v["lobby.match_pick"] = Text(online::EncodeMatchPick(9, "0123456789abcdef", online::PickState::kConfirmed, 5));
  v["lobby.match_transport"] = Text(online::EncodeMatchTransport(10, "0123456789abcdef", 1, online::PathKind::kRelay));
  online::RaceStartFields start;
  start.race = 42;
  start.room = "0123456789abcdef";
  start.competition = "free_race";
  start.players = 2;
  start.track = 7;
  start.characters = {1, 5};
  v["lobby.race_start"] = Text(online::EncodeRaceStart(11, start));
  online::RaceCompleteFields complete;
  complete.race = 42;
  complete.room = "0123456789abcdef";
  complete.duration_ms = 123456;
  complete.places = {1, 2};
  v["lobby.race_complete"] = Text(online::EncodeRaceComplete(12, complete));

  v["identity.player_id"] = Text(online::DerivePlayerId(std::string(64, 'a')));
  v["constants.protocol"] = Text(std::to_string(online::kProtocolVersion) + "/" + std::to_string(online::kBuildNumber) +
                                 "/" + online::kImageId + "/" + online::kGameId);
  v["constants.ports"] = BigEndian((uint64_t(online::kFirstGamePort) << 16) | online::kLastGamePort, 4);
  v["constants.relay"] = BigEndian((uint64_t(online::kRelayHeaderSize) << 16) | online::kRelayMaxPayload, 4);
  v["constants.race"] = Bytes{race::kMagic0, race::kMagic1, race::kVersion, uint8_t(race::kMaxDatagram >> 8),
                              uint8_t(race::kMaxDatagram & 0xFF), uint8_t(race::kMaxKarts)};
  return v;
}

std::map<std::string, Bytes> ReadVectors(const std::string& path) {
  std::map<std::string, Bytes> out;
  std::ifstream in(path);
  Check(in.good(), "open " + path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    out[line.substr(0, space)] = FromHex(line.substr(space + 1));
  }
  return out;
}

void CompareWith(const std::map<std::string, Bytes>& mine, const std::map<std::string, Bytes>& theirs) {
  Check(mine.size() == theirs.size(), "both sides know the same number of vectors");
  for (const auto& [name, bytes] : mine) {
    const auto other = theirs.find(name);
    if (other == theirs.end()) {
      Check(false, name + " is missing on the other side");
      continue;
    }
    Check(other->second == bytes, name + " differs: this build " + ToHex(bytes) + ", other side " + ToHex(other->second));
  }
}

// Decodes every datagram of the other side and encodes it again: a reader that only agrees on the writer's bytes is
// not enough when the two ends are different builds.
void DecodeVectors(const std::map<std::string, Bytes>& vectors) {
  for (const auto& [name, bytes] : vectors) {
    if (name.starts_with("race.") && name != "constants.race") {
      const std::optional<race::Message> message = race::Decode(bytes.data(), bytes.size());
      Check(message.has_value(), name + " decodes");
      if (message) {
        Check(race::Encode(*message) == bytes, name + " encodes back to the same bytes");
        Check(bytes.size() <= race::kMaxDatagram, name + " fits one datagram");
      }
    }
  }
  const auto kart = race::Decode(vectors.at("race.kart_state").data(), vectors.at("race.kart_state").size());
  Check(kart && std::get<race::KartState>(*kart) == SampleKart(), "the kart state floats keep their exact values");
  const auto config = race::Decode(vectors.at("race.config").data(), vectors.at("race.config").size());
  Check(config && std::get<race::RaceConfig>(*config) == FourSeatConfig(), "the race start reaches every seat unchanged");
  const auto input = race::Decode(vectors.at("race.input").data(), vectors.at("race.input").size());
  Check(input && std::get<race::InputMsg>(*input) == SampleInput(), "the pad keeps its signed axes");
  const auto snapshot = race::Decode(vectors.at("race.snapshot.full").data(), vectors.at("race.snapshot.full").size());
  Check(snapshot && std::get<race::SnapshotMsg>(*snapshot) == SampleSnapshot(race::kMaxKarts), "a full snapshot round trips");

  const Bytes& relay = vectors.at("relay.seat");
  online::RelayFrameError error = online::RelayFrameError::kNone;
  const auto frame = online::DecodeRelayFrame(relay.data(), relay.size(), &error);
  Check(frame && frame->token == 0x89ABCDEF && frame->seq == 7 && frame->dest == 2 && frame->src == 0 &&
            frame->payload == Pattern(32, 0x45434B31),
        "the relay frame decodes");
  const Bytes& broadcast = vectors.at("relay.broadcast");
  const auto all = online::DecodeRelayFrame(broadcast.data(), broadcast.size());
  Check(all && all->dest == online::kRelayDestAll && all->seq == 0xFFFFFFFF, "the relay broadcast decodes");

  const Bytes& punch = vectors.at("punch.punch");
  const auto packet = online::DecodePunch(punch.data(), punch.size());
  Check(packet && packet->kind == online::PunchKind::kPunch && packet->side == 1 && packet->key == 0x0102030405060708ull,
        "the punch decodes");
  const Bytes& ack = vectors.at("punch.ack");
  const auto reply = online::DecodePunch(ack.data(), ack.size());
  Check(reply && reply->kind == online::PunchKind::kAck && reply->side == 4 && reply->key == 0xFEDCBA9876543210ull,
        "the punch acknowledgement decodes");
}

void TestLayout() {
  const Bytes header = race::Encode(race::Leave{1, 2});
  Check(header.size() >= race::kHeaderSize && header[0] == 'E' && header[1] == 'R' && header[2] == race::kVersion,
        "a race datagram starts with its magic and version");
  const Bytes kart = race::Encode(SampleKart());
  const Bytes race_id = {0x4D, 0x3C, 0x2B, 0x1A};
  Check(kart.size() > 8 && std::equal(race_id.begin(), race_id.end(), kart.begin() + long(race::kHeaderSize)),
        "race fields are little-endian, written byte by byte");
  const Bytes relay = online::EncodeRelayFrame(0x89ABCDEF, 7, 2, race_id.data(), race_id.size());
  const Bytes token = BigEndian(0x89ABCDEF, 4);
  Check(relay.size() == online::kRelayHeaderSize + race_id.size() &&
            std::equal(token.begin(), token.end(), relay.begin() + 4),
        "relay headers are big-endian");
}

}  // namespace

int main(int argc, char** argv) {
  const std::map<std::string, Bytes> produced = Produce();
  if (argc == 2 && std::string(argv[1]) == "--print") {
    for (const auto& [name, bytes] : produced) std::printf("%s %s\n", name.c_str(), ToHex(bytes).c_str());
    return 0;
  }
  TestLayout();
  DecodeVectors(produced);
  CompareWith(produced, ReadVectors(std::string(ECK_SOURCE_DIR) + "/tests/wire-vectors.txt"));
  if (argc == 3 && std::string(argv[1]) == "--peer") {
    const std::map<std::string, Bytes> peer = ReadVectors(argv[2]);
    CompareWith(produced, peer);
    DecodeVectors(peer);
  }
  return online_test::Finish();
}
