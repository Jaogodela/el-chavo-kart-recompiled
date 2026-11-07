// The production online client against the Worker's own code, running locally: node server/eck-lobby/local/server.js,
// then ECK_LOBBY_TEST_URL=ws://127.0.0.1:8791/ eck_online_integration_tests. Real sockets, no game, no window, and the
// production service is never contacted. Exits 77 without the variable.
//
// With --role it is one half of a two-process race between two builds (Windows and Linux): see tools/test.sh.
//   eck_online_integration_tests --role host|guest --lobby URL --sync DIR --platform Windows|Linux --expect-peer Windows|Linux

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "online/connection_test.h"
#include "online/game_socket.h"
#include "online/online_service.h"
#include "online_race.h"

using namespace eck::online;
namespace race = eck::race;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

double Now() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Client {
  std::unique_ptr<OnlineGameSocket> socket = std::make_unique<OnlineGameSocket>();
  std::unique_ptr<OnlineService> service;
  std::string player;
  std::vector<std::pair<int, std::vector<uint8_t>>> inbox;

  Client(const std::string& url, char fill, const char* name, const char* platform) {
    socket->Open({0}, true);
    ServiceConfig config;
    config.url = url;
    config.version = "0.1.0";
    config.platform = platform;
    config.lan = {"127.0.0.1"};
    config.upnp = false;
    config.stun = "off";
    config.synchronous_probes = true;
    config.game_socket = socket.get();
    config.resolve_stun = [](const std::string&) { return std::vector<Endpoint>(); };
    const std::string token(64, fill);
    player = DerivePlayerId(token);
    config.identity = [token] {
      Identity id;
      id.token = token;
      id.player_id = DerivePlayerId(token);
      return id;
    };
    service = std::make_unique<OnlineService>(std::move(config));
    service->Apply({true, name});
  }

  Snapshot Snap() const { return service->snapshot(); }
};

bool WaitFor(const std::function<bool()>& done, std::initializer_list<Client*> clients, double seconds = 15.0) {
  const double deadline = Now() + seconds;
  while (Now() < deadline) {
    for (Client* client : clients) client->service->Update(Now());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool SignedIn(const Client& c) { return c.Snap().connection == Connection::kOnline && !c.Snap().friend_code.empty(); }

std::string AsPasted(std::string code) {
  for (char& c : code) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return "  " + code + "\r\n";
}

bool BecomeFriends(Client& a, Client& b) {
  if (!WaitFor([&] { return SignedIn(a) && SignedIn(b); }, {&a, &b})) return false;
  if (a.service->AddFriend(AsPasted(b.Snap().friend_code)) != FriendRequestResult::kSent) return false;
  if (!WaitFor([&] { return !b.Snap().incoming.empty(); }, {&a, &b})) return false;
  b.service->AcceptFriend(a.player);
  return WaitFor([&] {
    return a.Snap().friends.size() == 1 && b.Snap().friends.size() == 1 && a.Snap().friends[0].online &&
           b.Snap().friends[0].online;
  }, {&a, &b});
}

const RoomRules kRules = {{"competition", 1}, {"track", 3}, {"laps", 3}};

bool Invite(Client& host, Client& guest, std::initializer_list<Client*> all) {
  if (!WaitFor([&] { return host.Snap().sending_invite.empty() && !host.Snap().creating_session; }, all)) return false;
  if (!host.service->SendInvite(guest.player, kRules)) return false;
  if (!WaitFor([&] { return !guest.Snap().invites.empty(); }, all)) return false;
  if (!guest.service->AcceptInvite(guest.Snap().invites[0].id)) return false;
  return WaitFor([&] { return guest.Snap().room.has_value() && guest.Snap().seat > 0; }, all);
}

race::KartState Kart(uint8_t seat) {
  race::KartState kart;
  kart.race_id = 7;
  kart.seat = seat;
  kart.tick = 1000 + seat;
  kart.position[0] = 100.5f * float(seat + 1);
  kart.position[1] = -20.25f;
  kart.position[2] = 3.0f;
  kart.rotation[1] = -16384 * (seat + 1);
  kart.velocity[0] = 55.5f;
  kart.steering = -0.5f;
  kart.throttle = 1.0f;
  kart.lap = 2;
  kart.checkpoint = uint16_t(40 + seat);
  kart.place = uint8_t(seat + 1);
  kart.flags = 5;
  return kart;
}

race::SnapshotMsg HostSnapshot() {
  race::SnapshotMsg snapshot;
  snapshot.race_id = 7;
  snapshot.seq = 9;
  snapshot.host_ms = 123456;
  snapshot.race_state = 4;
  snapshot.begun = true;
  snapshot.input_ack = {0, 11, 12, 13};
  for (int i = 0; i < 4; ++i) {
    race::KartSample sample;
    sample.slot = uint8_t(i);
    sample.position[0] = 10.0f * float(i + 1);
    sample.speed = 42.0f + float(i);
    snapshot.karts.push_back(sample);
  }
  return snapshot;
}

race::InputMsg GuestInput(uint8_t seat) {
  race::InputMsg input;
  input.race_id = 7;
  input.seat = seat;
  input.seq = 500 + seat;
  input.loaded = true;
  input.pad = {0x1000, 200, 100, -12000, 32000, 5, -5};
  return input;
}

// Sends `message` to the peer at `seat` over the game's own socket and waits for the other end to decode it.
bool Send(Client& from, int seat, const race::Message& message) {
  const std::vector<uint8_t> bytes = race::Encode(message);
  for (const GamePeer& peer : from.service->GamePeers()) {
    if (peer.seat == seat) return from.socket->Send(peer.endpoint, bytes.data(), bytes.size());
  }
  return false;
}

bool Receive(Client& to, std::initializer_list<Client*> all, const race::Message& expected, int expect_seat) {
  const std::vector<uint8_t> want = race::Encode(expected);
  bool matched = false;
  WaitFor([&] {
    uint8_t buffer[512];
    Endpoint from;
    while (const size_t n = to.socket->Receive(buffer, sizeof buffer, from)) {
      if (const std::optional<race::Message> message = race::Decode(buffer, n)) {
        to.inbox.emplace_back(to.service->SeatOfEndpoint(from), race::Encode(*message));
      }
    }
    for (auto it = to.inbox.begin(); it != to.inbox.end(); ++it) {
      if (it->first == expect_seat && it->second == want) {
        to.inbox.erase(it);
        matched = true;
        break;
      }
    }
    return matched;
  }, all, 10.0);
  return matched;
}

void TestFriendsConnectionTestAndTelemetry(const std::string& url) {
  Client a(url, 'a', "SauloA", "Windows");
  Client b(url, 'b', "SauloB", "Linux");
  CHECK(BecomeFriends(a, b));
  const Friend seen = a.Snap().friends[0];
  CHECK(seen.name == "SauloB" && seen.platform == "Linux" && seen.version == "0.1.0" && seen.compatible);
  CHECK(b.Snap().friends[0].platform == "Windows");
  CHECK(a.Snap().friend_code != b.Snap().friend_code);
  CHECK(a.service->Capabilities().relay_supported && a.service->Capabilities().race_telemetry);

  b.service->Apply({false, "SauloB"});
  CHECK(WaitFor([&] { return !a.Snap().friends[0].online; }, {&a, &b}));
  b.service->Apply({true, "SauloB"});
  CHECK(WaitFor([&] { return a.Snap().friends[0].online && b.Snap().friends.size() == 1; }, {&a, &b}));

  ConnectionTestRequest request;
  request.url = url;
  request.identity.token = std::string(64, 'c');
  request.identity.player_id = DerivePlayerId(request.identity.token);
  request.name = "Probe";
  request.stun = "off";
  request.game_ports = {};
  request.probe_upnp = [](uint16_t) {
    net::upnp::Status status;
    status.state = net::upnp::State::kMapped;
    status.external = {net::ParseIp("203.0.113.7"), 7101};
    status.detail = "the router accepts port mappings";
    return status;
  };
  const ConnectionReport report = RunConnectionTest(request);
  CHECK(report.ready && report.verdict == "Ready for online play");
  CHECK(report.latency_ms >= 0);
  bool relay_line = false;
  for (const ConnectionReport::Line& line : report.lines) relay_line |= line.label == "Relay";
  CHECK(relay_line);

  request.url = "ws://127.0.0.1:1/";
  CHECK(!RunConnectionTest(request).ready);
}

void TestFourPlayerRoom(const std::string& url) {
  Client host(url, 'd', "Host", "Windows");
  Client two(url, 'e', "Two", "Linux");
  Client three(url, 'f', "Three", "Windows");
  Client four(url, '1', "Four", "Linux");
  const auto all = {&host, &two, &three, &four};
  CHECK(BecomeFriends(host, two));
  CHECK(WaitFor([&] { return SignedIn(three) && SignedIn(four); }, all));
  for (Client* guest : {&three, &four}) {
    CHECK(host.service->AddFriend(guest->Snap().friend_code) == FriendRequestResult::kSent);
    CHECK(WaitFor([&] { return !guest->Snap().incoming.empty(); }, all));
    guest->service->AcceptFriend(host.player);
    CHECK(WaitFor([&] { return host.Snap().friends.size() >= 1 && guest->Snap().friends.size() == 1; }, all));
  }

  CHECK(Invite(host, two, all));
  CHECK(Invite(host, three, all));
  CHECK(Invite(host, four, all));
  CHECK(WaitFor([&] { return host.Snap().room && host.Snap().room->players == 4; }, all));
  CHECK(host.Snap().seat == 0 && two.Snap().seat == 1 && three.Snap().seat == 2 && four.Snap().seat == 3);
  const RoomView room = *host.Snap().room;
  CHECK(room.seats.size() == 4 && room.capacity == 4);
  CHECK(room.seats[0].platform == "Windows" && room.seats[1].platform == "Linux" && room.seats[2].platform == "Windows" &&
        room.seats[3].platform == "Linux");
  CHECK(room.seats[1].name == "Two" && room.seats[3].name == "Four");

  CHECK(WaitFor([&] { return host.Snap().paths_ready && two.Snap().paths_ready && three.Snap().paths_ready && four.Snap().paths_ready; }, all, 30.0));
  for (const PeerSnapshot& peer : host.Snap().peers) CHECK(peer.path == PeerPath::kDirect && peer.connected);

  host.service->SetPeerRtt(1, 23);
  CHECK(host.Snap().peers.size() == 3);
  bool rtt = false;
  for (const PeerSnapshot& peer : host.Snap().peers) rtt |= peer.seat == 1 && peer.rtt_ms == 23;
  CHECK(rtt);

  CHECK(WaitFor([&] { return host.service->StartRace(); }, all));
  CHECK(WaitFor([&] { return two.Snap().room && two.Snap().room->state == RoomState::kRacing; }, all));
  CHECK(host.service->ReportRaceStart({7, host.Snap().room->room, "free_race", 4, 3, {1, 5, 7, 11}}));
  const race::Message snapshot = HostSnapshot();
  for (Client* guest : {&two, &three, &four}) {
    CHECK(Send(host, guest->Snap().seat, snapshot));
    CHECK(Receive(*guest, all, snapshot, 0));
    const race::Message input = GuestInput(uint8_t(guest->Snap().seat));
    CHECK(Send(*guest, 0, input));
    CHECK(Receive(host, all, input, guest->Snap().seat));
    const race::Message kart = Kart(uint8_t(guest->Snap().seat));
    CHECK(Send(*guest, 0, kart));
    CHECK(Receive(host, all, kart, guest->Snap().seat));
  }
  CHECK(host.service->ReportRaceComplete({7, host.Snap().room->room, 91000, {2, 1, 4, 3}}));

  const std::string room_id = room.room;
  CHECK(two.service->LeaveSession());
  CHECK(WaitFor([&] { return host.Snap().room && host.Snap().room->seats[1].state == SeatState::kEmpty; }, all));
  const RoomView after = *host.Snap().room;
  CHECK(after.seats[2].player == three.player && after.seats[3].player == four.player);
  CHECK(three.Snap().seat == 2 && four.Snap().seat == 3);
  CHECK(three.Snap().room && three.Snap().room->seats[1].state == SeatState::kEmpty);
  const race::Message after_leave = Kart(3);
  CHECK(Send(four, 0, after_leave));
  CHECK(Receive(host, all, after_leave, 3));
  CHECK(Send(host, 2, snapshot) && Receive(three, all, snapshot, 0));

  CHECK(host.service->LeaveSession());
  CHECK(WaitFor([&] { return !three.Snap().room.has_value() && !four.Snap().room.has_value(); }, all));
  CHECK(!three.Snap().closed_reason.empty() || !four.Snap().closed_reason.empty());
  CHECK(three.service->GamePeers().empty() && !host.Snap().room.has_value());
}

// ---- Two processes, two platforms ---------------------------------------------------------------------------------

struct Role {
  std::string name, other, lobby, sync, platform, peer_platform;
};

void WriteFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  const fs::path temporary = path.string() + ".part";
  std::ofstream(temporary, std::ios::binary) << text;
  fs::rename(temporary, path);
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Barrier(const Role& role, Client& client, const std::string& step) {
  WriteFile(fs::path(role.sync) / (step + "." + role.name), "1");
  return WaitFor([&] { return fs::exists(fs::path(role.sync) / (step + "." + role.other)); }, {&client}, 60.0);
}

int RunRole(const Role& role) {
  const bool host = role.name == "host";
  Client me(role.lobby, host ? '7' : '8', host ? "Host" : "Guest", role.platform.c_str());
  CHECK(WaitFor([&] { return SignedIn(me); }, {&me}));
  WriteFile(fs::path(role.sync) / ("code." + role.name), me.Snap().friend_code);
  CHECK(Barrier(role, me, "codes"));
  const std::string peer_code = ReadFile(fs::path(role.sync) / ("code." + role.other));
  CHECK(peer_code.size() == 9 && peer_code != me.Snap().friend_code);

  if (host) {
    CHECK(me.service->AddFriend(peer_code) == FriendRequestResult::kSent);
  } else {
    CHECK(WaitFor([&] { return !me.Snap().incoming.empty(); }, {&me}));
    me.service->AcceptFriend(peer_code);
  }
  CHECK(WaitFor([&] { return me.Snap().friends.size() == 1 && me.Snap().friends[0].online; }, {&me}));
  CHECK(me.Snap().friends[0].platform == role.peer_platform && me.Snap().friends[0].version == "0.1.0" &&
        me.Snap().friends[0].compatible);
  CHECK(Barrier(role, me, "friends"));

  if (host) {
    CHECK(me.service->SendInvite(peer_code, kRules));
  } else {
    CHECK(WaitFor([&] { return !me.Snap().invites.empty(); }, {&me}));
    CHECK(me.Snap().invites[0].platform == role.peer_platform);
    CHECK(me.service->AcceptInvite(me.Snap().invites[0].id));
  }
  CHECK(WaitFor([&] { return me.Snap().room && me.Snap().room->players == 2; }, {&me}));
  CHECK(me.Snap().seat == (host ? 0 : 1));
  const RoomView room = *me.Snap().room;
  CHECK(room.seats[0].platform == (host ? role.platform : role.peer_platform));
  CHECK(room.seats[1].platform == (host ? role.peer_platform : role.platform));
  CHECK(room.seats[0].version == "0.1.0" && room.seats[1].version == "0.1.0");

  CHECK(WaitFor([&] { return me.Snap().paths_ready; }, {&me}, 45.0));
  const bool relay = !me.service->GamePeers().empty() && me.service->GamePeers()[0].relay;
  std::printf("%s (%s) reaches %s (%s) over the %s path\n", role.name.c_str(), role.platform.c_str(), role.other.c_str(),
              role.peer_platform.c_str(), relay ? "relay" : "direct");
  CHECK(Barrier(role, me, "paths"));

  const int peer_seat = host ? 1 : 0;
  if (host) {
    CHECK(WaitFor([&] { return me.service->StartRace(); }, {&me}));
  } else {
    CHECK(WaitFor([&] { return me.Snap().room && me.Snap().room->state == RoomState::kRacing; }, {&me}));
  }
  CHECK(Barrier(role, me, "race"));

  const race::Message config = [] {
    race::RaceConfig value;
    value.race_id = 7;
    value.track = 3;
    value.seed = 0xC0FFEE;
    value.participants = {{0, 1, 0}, {1, 5, 1}};
    return race::Message(value);
  }();
  const race::Message snapshot = HostSnapshot();
  const race::Message input = GuestInput(1);
  const race::Message kart = Kart(uint8_t(host ? 0 : 1));
  if (host) {
    CHECK(Send(me, peer_seat, config));
    CHECK(Send(me, peer_seat, snapshot));
    CHECK(Receive(me, {&me}, input, peer_seat));
    CHECK(Receive(me, {&me}, Kart(1), peer_seat));
  } else {
    CHECK(Receive(me, {&me}, config, peer_seat));
    CHECK(Receive(me, {&me}, snapshot, peer_seat));
    CHECK(Send(me, peer_seat, input));
    CHECK(Send(me, peer_seat, Kart(1)));
  }
  (void)kart;
  me.service->SetPeerRtt(peer_seat, host ? 31 : 29);
  bool rtt = false;
  for (const PeerSnapshot& peer : me.Snap().peers) rtt |= peer.seat == peer_seat && peer.rtt_ms == (host ? 31 : 29);
  CHECK(rtt);
  CHECK(Barrier(role, me, "exchanged"));

  if (host) {
    CHECK(me.service->ReportRaceStart({7, me.Snap().room->room, "free_race", 2, 3, {1, 5}}));
    CHECK(me.service->ReportRaceComplete({7, me.Snap().room->room, 88000, {1, 2}}));
  } else {
    CHECK(me.service->LeaveSession());
    CHECK(WaitFor([&] { return !me.Snap().room.has_value(); }, {&me}));
  }
  CHECK(Barrier(role, me, "finished"));
  if (host) {
    CHECK(WaitFor([&] { return me.Snap().room && me.Snap().room->seats[1].state == SeatState::kEmpty; }, {&me}));
    CHECK(me.service->LeaveSession());
  }
  CHECK(Barrier(role, me, "left"));
  std::printf("%s %s: %d checks, %d failures\n", role.name.c_str(), role.platform.c_str(), g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Role role;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i], value = argv[i + 1];
    if (flag == "--role") role.name = value;
    else if (flag == "--lobby") role.lobby = value;
    else if (flag == "--sync") role.sync = value;
    else if (flag == "--platform") role.platform = value;
    else if (flag == "--expect-peer") role.peer_platform = value;
  }
  if (!role.name.empty()) {
    role.other = role.name == "host" ? "guest" : "host";
    return RunRole(role);
  }
  const char* url = std::getenv("ECK_LOBBY_TEST_URL");
  if (!url || !*url) {
    std::printf("skipped: ECK_LOBBY_TEST_URL is not set\n");
    return 77;
  }
  TestFriendsConnectionTestAndTelemetry(url);
  TestFourPlayerRoom(url);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
