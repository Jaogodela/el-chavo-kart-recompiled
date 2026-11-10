#include <cstring>
#include <string>
#include <vector>

#include "online/connection_test.h"
#include "online/establishment.h"
#include "online/friend_view.h"
#include "online/json.h"
#include "online/protocol.h"
#include "online/relay_frame.h"
#include "online_test_support.h"
#include "ui_text.h"

using online_test::Check;
namespace online = eck::online;
namespace json = eck::online::json;

namespace {

const char* kSeatEmpty = R"({"seat":SEAT,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""})";

std::string Empty(int seat) {
  std::string text = kSeatEmpty;
  text.replace(text.find("SEAT"), 4, std::to_string(seat));
  return text;
}

std::string Room(const std::string& state, const std::string& seats, int capacity) {
  return R"({"room":"0123456789abcdef","host":"ABCD-2345","host_name":"Saulo","capacity":)" + std::to_string(capacity) +
         R"(,"players":2,"state":")" + state +
         R"(","image":"eck-usa-1db4e96b","build":1,"rules":{"mode":1,"track":3,"laps":3},"seats":[)" + seats +
         R"(],"created":10,"updated":20})";
}

const char* kHost =
    R"({"seat":0,"player":"ABCD-2345","name":"Saulo","state":"connected","ready":false,"platform":"Windows","version":"0.1.0","transport":""})";
const char* kGuest =
    R"({"seat":1,"player":"WXYZ-9876","name":"Ana","state":"reconnecting","ready":true,"platform":"Windows","version":"0.1.0","transport":"relay"})";

void TestEncoders() {
  online::HelloFields hello;
  hello.player = "TCSA-EF6F";
  hello.token = std::string(64, 'b');
  hello.name = "Saulo";
  hello.version = "0.1.0";
  hello.platform = "Windows";
  hello.relay = true;
  const std::optional<json::Value> root = json::Parse(online::EncodeHello(1, hello));
  Check(root && root->IsObject(), "hello is JSON");
  Check(root->Find("op")->AsString() == "hello" && root->Find("proto")->AsInt64() == 1, "hello op and proto");
  Check(root->Find("game")->AsString() == "el-chavo-kart-pc", "hello game id");
  // The value itself is pinned by tests/wire-vectors.txt, which the service's tests read too.
  Check(root->Find("image")->AsString() == "eck-usa-1db4e96b" && root->Find("build")->AsInt64() == online::kBuildNumber,
        "hello image and build");
  Check(root->Find("presence")->AsBool() && root->Find("relay")->AsBool(), "hello flags");

  const auto op = [](const std::string& text) {
    const std::optional<json::Value> v = json::Parse(text);
    return v && v->Find("op") ? v->Find("op")->AsString() : std::string();
  };
  Check(op(online::EncodeRoomHost(2, 4, {{"mode", 1}, {"laps", 3}})) == "room.host", "room.host");
  Check(online::EncodeRoomHost(2, 4, {{"mode", 1}, {"laps", 3}}).find("\"capacity\":4") != std::string::npos, "capacity");
  Check(online::EncodeRoomHost(2, 4, {{"mode", 1}}).find("\"rules\":{\"mode\":1}") != std::string::npos, "rules");
  Check(op(online::EncodeRoomStart(5, "room")) == "room.start", "room.start");
  Check(op(online::EncodeRoomLeave(5, "room")) == "room.leave", "room.leave");
  Check(op(online::EncodeInviteAccept(6, "inv")) == "invite.accept", "invite.accept");
  Check(op(online::EncodeMatchResume(7, "room")) == "match.resume", "match.resume");
  const std::string ready = online::EncodeMatchReady(8, "room", true);
  Check(op(ready) == "match.ready" && ready.find("\"ready\":true") != std::string::npos, "match.ready");
  const std::string pick = online::EncodeMatchPick(8, "room", online::PickState::kConfirmed, 7);
  Check(op(pick) == "match.pick" && pick.find("\"pick\":2") != std::string::npos && pick.find("\"character\":7") != std::string::npos &&
            pick.find("\"seat\"") == std::string::npos,
        "match.pick carries the choice and never a seat");
  const std::string transport = online::EncodeMatchTransport(9, "room", 2, online::PathKind::kRelay);
  Check(transport.find("\"seat\":2") != std::string::npos && transport.find("\"transport\":\"relay\"") != std::string::npos,
        "match.transport names the seat");

  online::EndpointFields endpoint;
  endpoint.port = 7101;
  endpoint.lan = {"192.168.1.5"};
  endpoint.public_ip = "203.0.113.9";
  endpoint.public_port = 50001;
  endpoint.candidates = {{"203.0.113.9", 50001, "srflx"}};
  endpoint.relay = true;
  const std::optional<json::Value> ep = json::Parse(online::EncodeMatchEndpoint(10, "room", endpoint));
  Check(ep && ep->Find("port")->AsInt64() == 7101 && ep->Find("public_port")->AsInt64() == 50001, "match.endpoint ports");
  Check(ep->Find("candidates")->AsArray().size() == 1 && ep->Find("relay")->AsBool(), "match.endpoint candidates and relay");
  endpoint.public_port = 0;
  Check(online::EncodeMatchEndpoint(10, "room", endpoint).find("public_ip") == std::string::npos,
        "match.endpoint leaves a missing public endpoint out");
  Check(online::EncodeFriendsAdd(1, "a\"b").find("a\\\"b") != std::string::npos, "strings are escaped");
}

void TestDecoders() {
  using Kind = online::ServerMessage::Kind;
  const auto hello = online::DecodeServerMessage(
      R"({"re":1,"code":"ok","player":"ABCD-2345","name":"Saulo","friend_code":"ABCD-2345","time":5,"capabilities":{"protocol_version":1,"worker_build":"x","public_endpoints":true,"relay_supported":true,"relay_max_payload":1472,"min_players":2,"max_players":4,"four_player_rooms":true,"seat_routing":true},"resume":null})");
  Check(hello && hello->kind == Kind::kResponse && hello->code == "ok" && hello->friend_code == "ABCD-2345", "hello reply");
  Check(hello->capabilities && hello->capabilities->relay_supported && hello->capabilities->max_players == 4, "capabilities");
  Check(hello->capabilities->four_player_rooms && hello->capabilities->seat_routing, "room capabilities");
  Check(hello->resume.empty(), "null resume is empty");
  const auto resumed = online::DecodeServerMessage(R"({"re":1,"code":"ok","friend_code":"ABCD-2345","resume":"0123456789abcdef"})");
  Check(resumed && resumed->resume == "0123456789abcdef", "resume room id");
  const auto refused = online::DecodeServerMessage(R"({"re":2,"code":"version_mismatch"})");
  Check(refused && refused->code == "version_mismatch", "error reply");

  const std::string four = std::string(kHost) + "," + kGuest + "," + Empty(2) + "," + Empty(3);
  const auto hosted = online::DecodeServerMessage(
      R"({"re":3,"code":"ok","room":)" + Room("open", four, 4) + R"(,"seat":0,"relay_token":123})");
  Check(hosted && hosted->room && hosted->seat == 0 && hosted->relay_token == 123, "room.host reply");
  const online::RoomView& room = *hosted->room;
  Check(room.capacity == 4 && room.seats.size() == 4 && room.state == online::RoomState::kOpen, "four-seat room");
  Check(room.seats[1].state == online::SeatState::kReconnecting && room.seats[1].ready &&
            room.seats[1].transport == online::PathKind::kRelay,
        "seat state, ready and transport");
  Check(room.seats[2].state == online::SeatState::kEmpty && room.seats[3].player.empty(), "empty seats");
  Check(room.seats[0].pick == online::PickState::kNone, "a Worker without picks reads as no pick");
  const auto pick_of = [](const std::string& fields) {
    const std::string seat = R"({"seat":0,"player":"A","name":"A","state":"connected","ready":false,"platform":"","version":"","transport":"")" +
                             fields + "}";
    const std::string text = R"({"re":3,"code":"ok","room":)" + Room("open", seat + "," + Empty(1), 2) + "}";
    const auto decoded = online::DecodeServerMessage(text);
    return decoded && decoded->room ? decoded->room->seats[0] : online::Seat{};
  };
  const online::Seat picked = pick_of(R"(,"pick":2,"character":11)");
  Check(picked.pick == online::PickState::kConfirmed && picked.character == 11, "a seat's pick");
  Check(pick_of(R"(,"pick":2,"character":12)").pick == online::PickState::kNone, "an invalid character is dropped");
  Check(pick_of(R"(,"pick":3,"character":1)").pick == online::PickState::kNone, "an invalid state is dropped");
  Check(pick_of(R"(,"pick":1,"character":-1)").pick == online::PickState::kNone, "a negative character is dropped");
  Check(room.rules.size() == 3 && room.image == "eck-usa-1db4e96b" && room.build == 1, "rules, image and build");

  const std::string three = std::string(kHost) + "," + Empty(1) + "," + Empty(2);
  const auto small = online::DecodeServerMessage(R"({"ev":"room.updated","room":)" + Room("starting", three, 3) + "}");
  Check(small && small->kind == Kind::kRoomUpdated && small->room->seats.size() == 3 &&
            small->room->state == online::RoomState::kStarting,
        "three-seat room.updated");
  Check(!online::DecodeServerMessage(R"({"ev":"room.updated","room":)" + Room("open", three, 4) + "}"),
        "a room whose seats do not match its capacity is refused");
  Check(!online::DecodeServerMessage(R"({"ev":"room.updated","room":)" + Room("open", std::string(kHost) + "," + Empty(2), 2) + "}"),
        "seat numbers must be in order");
  const auto created = online::DecodeServerMessage(
      R"({"ev":"room.created","seat":0,"relay_token":9,"room":)" + Room("open", std::string(kHost) + "," + Empty(1), 2) + "}");
  Check(created && created->kind == Kind::kRoomCreated && created->relay_token == 9, "room.created");

  const auto closed = online::DecodeServerMessage(R"({"ev":"room.closed","room":"0123456789abcdef","reason":"host_left"})");
  Check(closed && closed->kind == Kind::kRoomClosed && closed->reason == "host_left", "room.closed");

  const auto peer = online::DecodeServerMessage(
      R"({"ev":"match.peer","room":"r","seat":2,"player":"WXYZ-9876","name":"Ana","ip":"198.51.100.4","port":7102,"lan":["192.168.1.7"],"public_ip":"203.0.113.9","public_port":50002,"candidates":[{"ip":"203.0.113.9","port":50002,"kind":"srflx"}],"same_network":true,"relay":true})");
  Check(peer && peer->kind == Kind::kMatchPeer && peer->peer.seat == 2 && peer->peer.public_port == 50002 &&
            peer->peer.candidates.size() == 1 && peer->peer.same_network && peer->peer.relay,
        "match.peer per seat");
  Check(!online::DecodeServerMessage(R"({"ev":"match.peer","room":"r","seat":5,"ip":"1.2.3.4","port":7102})"),
        "match.peer with a seat past 3 is refused");

  const auto move = online::DecodeServerMessage(R"({"ev":"match.transport","room":"r","seat":0,"transport":"relay"})");
  Check(move && move->kind == Kind::kMatchTransport && move->transport == online::PathKind::kRelay && move->transport_seat == 0,
        "match.transport from the host");
  const auto relay_closed = online::DecodeServerMessage(R"({"ev":"relay.closed","room":"r","reason":"rate_limit"})");
  Check(relay_closed && relay_closed->kind == Kind::kRelayClosed, "relay.closed");

  const auto invite = online::DecodeServerMessage(
      R"({"ev":"invite.received","invite":{"id":"i1","host":"ABCD-2345","host_name":"Saulo","players":1,"capacity":3,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":99}})");
  Check(invite && invite->kind == Kind::kInviteReceived && invite->invite->capacity == 3, "invite.received");
  const auto friends = online::DecodeServerMessage(
      R"({"re":5,"code":"ok","friends":[{"player":"ABCD-2345","name":"Saulo","online":true,"state":"hosting","platform":"Windows","version":"0.1.0","image":"x","build":1,"compatible":true},{"player":"WXYZ-9876","name":"Ana","online":false,"state":"offline"}]})");
  Check(friends && friends->friends && friends->friends->size() == 2 &&
            (*friends->friends)[0].state == online::FriendState::kHosting && !(*friends->friends)[1].online,
        "friends list");
  Check(!online::DecodeServerMessage("not json") && !online::DecodeServerMessage(R"({"foo":1})"), "garbage is refused");
}

void TestRelayFrames() {
  const uint8_t payload[3] = {9, 8, 7};
  // The same bytes the service's encodeRelayFrame makes for token 0x01020304, seq 7, dest 2, src 1.
  const std::vector<uint8_t> frame = online::EncodeRelayFrame(0x01020304, 7, 2, payload, sizeof payload);
  const uint8_t expected[] = {0x45, 0x4b, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x07,
                              0x02, 0x00, 0x00, 0x03, 0x09, 0x08, 0x07};
  Check(frame.size() == sizeof expected && std::memcmp(frame.data(), expected, sizeof expected) == 0, "frame layout");
  Check(frame.size() == online::kRelayHeaderSize + 3 && frame[0] == 'E' && frame[1] == 'K', "16-byte header with EK magic");

  const uint8_t from_service[] = {0x45, 0x4b, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x07,
                                  0x02, 0x01, 0x00, 0x03, 0x09, 0x08, 0x07};
  online::RelayFrameError error{};
  const auto decoded = online::DecodeRelayFrame(from_service, sizeof from_service, &error);
  Check(decoded && decoded->token == 0x01020304 && decoded->seq == 7 && decoded->dest == 2 && decoded->src == 1 &&
            decoded->payload == std::vector<uint8_t>({9, 8, 7}),
        "decode with the sender seat stamped by the service");

  const auto all = online::EncodeRelayFrame(1, 1, online::kRelayDestAll, payload, 3);
  Check(!all.empty() && all[12] == 0xFF, "dest 0xFF reaches every other seat");
  Check(online::EncodeRelayFrame(1, 1, 4, payload, 3).empty(), "a destination past seat 3 is refused");
  Check(online::EncodeRelayFrame(1, 1, 0, payload, 0).empty(), "an empty payload is refused");
  std::vector<uint8_t> big(online::kRelayMaxPayload + 1, 1);
  Check(online::EncodeRelayFrame(1, 1, 0, big.data(), big.size()).empty(), "an oversized payload is refused");
  big.resize(online::kRelayMaxPayload);
  Check(online::EncodeRelayFrame(1, 1, 0, big.data(), big.size()).size() == 16 + 1472, "a 1472-byte payload fits");

  std::vector<uint8_t> bad = frame;
  bad[0] = 'S';
  Check(!online::DecodeRelayFrame(bad.data(), bad.size(), &error) && error == online::RelayFrameError::kBadMagic, "bad magic");
  bad = frame;
  bad[2] = 2;
  Check(!online::DecodeRelayFrame(bad.data(), bad.size(), &error) && error == online::RelayFrameError::kBadVersion, "bad version");
  bad = frame;
  bad[12] = 9;
  Check(!online::DecodeRelayFrame(bad.data(), bad.size(), &error) && error == online::RelayFrameError::kBadDest, "bad dest");
  bad = frame;
  bad.pop_back();
  Check(!online::DecodeRelayFrame(bad.data(), bad.size(), &error) && error == online::RelayFrameError::kBadLength, "length mismatch");
  Check(!online::DecodeRelayFrame(frame.data(), 10, &error) && error == online::RelayFrameError::kTruncated, "truncated");

  for (int seat = 0; seat < online::kMaxSeats; ++seat) {
    Check(online::RelaySeatOf(online::RelayPeerEndpoint(seat)) == seat, "relay endpoint round trip");
  }
  Check(online::RelaySeatOf({0x7F000001, 7101}) == -1 && online::RelaySeatOf({0x08080808, 1}) == -1, "real endpoints are not relay seats");
}

void TestFriendView() {
  eck::SetActiveUiLanguage("en-US");
  online::Snapshot state;
  online::Friend host{"ABCD-2345", "Saulo", true, online::FriendState::kHosting, "Windows", "0.1.0", "x", 1, true};
  online::Friend idle{"WXYZ-9876", "Ana", true, online::FriendState::kOnline, "Windows", "0.1.0", "x", 1, true};
  online::Friend other{"HJKL-2345", "Bia", true, online::FriendState::kOnline, "Windows", "0.0.9", "y", 2, false};
  online::Friend away{"MNPQ-2345", "Caio", false, online::FriendState::kOffline, "", "", "", 0, true};
  Check(online::DescribeFriend(state, host).status == "Hosting \xC2\xB7 Windows \xC2\xB7 v0.1.0", "hosting status line");
  Check(online::DescribeFriend(state, away).status == "Offline", "offline status line");
  Check(online::DescribeFriend(state, other).incompatible, "incompatible friend is flagged");

  const std::vector<online::InviteChoice> choices = online::InviteChoices({host, away, other, idle});
  Check(choices.size() == 4 && choices[0].selectable && choices[1].selectable, "free friends come first and are selectable");
  for (const online::InviteChoice& c : choices) {
    if (c.player == "ABCD-2345") Check(c.selectable, "a friend alone in their own session can be invited");
    if (c.player == "HJKL-2345") Check(!c.selectable && c.status == "Version mismatch", "a mismatched friend cannot be invited");
    if (c.player == "MNPQ-2345") Check(!c.selectable && c.status == "Offline", "an offline friend cannot be invited");
  }

  online::Seat seat;
  seat.state = online::SeatState::kConnected;
  seat.name = "Ana";
  online::PeerSnapshot peer;
  peer.connected = true;
  peer.path = online::PeerPath::kRelay;
  peer.rtt_ms = 51;
  Check(online::SeatLine(seat, &peer) == "Ana (Connected) 51 ms", "seat line with ping, and no word of the path");
  peer.path = online::PeerPath::kDirect;
  peer.rtt_ms.reset();
  Check(online::SeatLine(seat, &peer) == "Ana (Connected)", "seat line without ping");
  seat.state = online::SeatState::kReconnecting;
  Check(online::SeatLine(seat, &peer) == "Ana (Connection lost)", "reconnecting seat");
  seat.state = online::SeatState::kEmpty;
  Check(online::SeatLine(seat, nullptr) == "Waiting for players", "empty seat");
}

void TestConnectionVerdict() {
  online::ConnectionFacts ok;
  ok.lobby_ok = true;
  ok.lobby_latency_ms = 40;
  ok.udp_ok = true;
  ok.public_address = true;
  ok.second_answer = true;
  ok.same_mapping = true;
  const online::ConnectionReport ready = online::SummarizeConnection(ok);
  Check(ready.ready && ready.verdict == "Ready for online play", "open NAT is ready");
  Check(ready.latency_ms == 40, "service latency is reported");

  online::ConnectionFacts unreachable;
  const online::ConnectionReport down = online::SummarizeConnection(unreachable);
  Check(!down.ready && down.verdict == "Not ready: cannot reach the online service", "no service is not ready");

  online::ConnectionFacts refused = ok;
  refused.lobby_ok = false;
  refused.lobby_refusal = "x";
  Check(online::SummarizeConnection(refused).verdict == "Not ready: the online service did not accept this game", "refusal verdict");

  online::ConnectionFacts closed = ok;
  closed.closed_ports = "7101-7110";
  const online::ConnectionReport limited = online::SummarizeConnection(closed);
  Check(!limited.ready && limited.verdict.rfind("Limited:", 0) == 0, "closed ports are limited");

  online::ConnectionFacts symmetric = ok;
  symmetric.public_address = false;
  symmetric.same_mapping = false;
  Check(!online::SummarizeConnection(symmetric).ready, "symmetric NAT without relay or router is not ready");
  symmetric.relay_ok = true;
  Check(online::SummarizeConnection(symmetric).ready, "the relay makes symmetric NAT ready");
  symmetric.relay_ok = false;
  symmetric.router = online::ConnectionFacts::Router::kMapped;
  Check(online::SummarizeConnection(symmetric).ready, "an opened router makes symmetric NAT ready");
  Check(online::ClassifyRouter(0x0A000001) == online::ConnectionFacts::Router::kMappedBehindNat, "private router address is double NAT");
  Check(online::ClassifyRouter(0x08080808) == online::ConnectionFacts::Router::kMapped, "public router address");
}

void TestEstablishment() {
  online::Establishment e;
  e.Begin(100.0);
  Check(e.phase() == online::Establishment::Phase::kConnecting && !e.ConnectingExpired(114.9), "budget runs");
  Check(e.ConnectingExpired(115.0), "budget expires after 15 s");
  Check(e.Select(110.0) && !e.Select(111.0), "only the first selection counts");
  Check(e.HandshakeDeadline() == 115.0 && e.AbsoluteDeadline() == 115.0, "selection never moves the absolute deadline");
  e.Begin(0.0);
  Check(e.Select(1.0) && e.HandshakeDeadline() == 11.0, "handshake budget follows the selection");
  Check(e.Fail(2.0, "x") && !e.Fail(3.0, "y") && e.reason() == "x", "failure is recorded once");
}

}  // namespace

int main() {
  TestEncoders();
  TestDecoders();
  TestRelayFrames();
  TestFriendView();
  TestConnectionVerdict();
  TestEstablishment();
  return online_test::Finish();
}
