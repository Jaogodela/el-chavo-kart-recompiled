#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "online/json.h"
#include "online/online_service.h"
#include "online/websocket.h"
#include "online_test_support.h"
#include "ui_text.h"

using online_test::Check;
namespace online = eck::online;
namespace json = eck::online::json;

namespace {

// One scripted connection to the service: the test pushes what the service would send and reads what the client sent.
struct Wire {
  std::deque<online::ChannelEvent> incoming;
  std::vector<std::string> sent;
  bool closed = false;

  void Push(online::ChannelEvent::Kind kind, std::string text = {}) { incoming.push_back({kind, std::move(text)}); }

  // The seq of the last request with this op, or -1.
  int LastSeq(const std::string& op) const {
    for (size_t i = sent.size(); i-- > 0;) {
      const std::optional<json::Value> v = json::Parse(sent[i]);
      if (v && v->Find("op") && v->Find("op")->AsString() == op) return int(v->Find("seq")->AsInt64().value_or(-1));
    }
    return -1;
  }
  int Count(const std::string& op) const {
    int n = 0;
    for (const std::string& text : sent) n += text.find("\"op\":\"" + op + "\"") != std::string::npos;
    return n;
  }
};

class FakeChannel final : public online::LobbyChannel {
 public:
  explicit FakeChannel(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
  bool Poll(online::ChannelEvent& out) override {
    if (wire_->incoming.empty()) return false;
    out = std::move(wire_->incoming.front());
    wire_->incoming.pop_front();
    return true;
  }
  void Send(const std::string& text) override { wire_->sent.push_back(text); }
  void Close() override { wire_->closed = true; }

 private:
  std::shared_ptr<Wire> wire_;
};

std::string Seat(int seat, const char* player, const char* name, const char* state) {
  return R"({"seat":)" + std::to_string(seat) + R"(,"player":")" + player + R"(","name":")" + name + R"(","state":")" + state +
         R"(","ready":false,"platform":"Windows","version":"0.1.0","transport":""})";
}

std::string Room(const std::string& state, const std::vector<std::string>& seats) {
  std::string joined;
  for (size_t i = 0; i < seats.size(); ++i) joined += (i ? "," : "") + seats[i];
  return R"({"room":"0123456789abcdef","host":"AAAA-2222","host_name":"Me","capacity":)" + std::to_string(seats.size()) +
         R"(,"players":2,"state":")" + state + R"(","image":"eck-usa-1db4e96b","build":1,"rules":{"laps":3},"seats":[)" + joined +
         R"(],"created":1,"updated":2})";
}

const std::string kEmpty3 = R"({"seat":2,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""})";

struct Harness {
  std::vector<std::shared_ptr<Wire>> wires;
  std::vector<std::string> log;
  std::unique_ptr<online::OnlineService> service;
  double now = 0;

  Harness() {
    online::ServiceConfig config;
    config.url = "ws://127.0.0.1:1/";
    config.version = "0.1.0";
    config.platform = "Windows";
    config.identity = [] { return online::Identity{std::string(64, 'c'), online::DerivePlayerId(std::string(64, 'c')), false, ""}; };
    config.log = [this](const std::string& line) { log.push_back(line); };
    config.open_channel = [this](const std::string&) {
      wires.push_back(std::make_shared<Wire>());
      return std::unique_ptr<online::LobbyChannel>(new FakeChannel(wires.back()));
    };
    service = std::make_unique<online::OnlineService>(std::move(config));
    service->Apply({true, "Me"});
  }

  Wire& wire() { return *wires.back(); }
  void Tick(double seconds = 0.1) {
    now += seconds;
    service->Update(now);
  }
  void Reply(const std::string& op, const std::string& fields) {
    wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(wire().LastSeq(op)) + R"(,"code":"ok")" + fields + "}");
  }
  void SignIn(const std::string& resume = "null") {
    Tick();
    wire().Push(online::ChannelEvent::Kind::kOpen);
    Tick();
    Reply("hello", R"(,"friend_code":"AAAA-2222","capabilities":{"protocol_version":1,"relay_supported":true,"relay_max_payload":1472,"public_endpoints":true,"min_players":2,"max_players":4},"resume":)" + resume);
    Tick();
  }
};

void TestSignInAndFriends() {
  Harness h;
  h.SignIn();
  Check(h.service->SignedIn() && h.service->Profile().friend_code == "AAAA-2222", "signed in with a friend code");
  Check(h.wire().Count("hello") == 1, "one hello");
  const std::optional<json::Value> hello = json::Parse(h.wire().sent[0]);
  Check(hello && hello->Find("game")->AsString() == "el-chavo-kart-pc" && hello->Find("name")->AsString() == "Me", "hello fields");
  Check(h.service->Capabilities().relay_supported && h.service->Capabilities().max_players == 4, "capabilities are kept");
  Check(h.wire().Count("friends.list") == 1 && h.wire().Count("friends.requests") == 1, "friends are fetched after sign-in");

  h.Reply("friends.list", R"(,"friends":[{"player":"BBBB-3333","name":"Ana","online":true,"state":"online","platform":"Windows","version":"0.1.0","image":"x","build":1,"compatible":true}])");
  h.Tick();
  Check(h.service->snapshot().friends.size() == 1 && h.service->snapshot().fetched, "friends list is shown");

  using Result = online::FriendRequestResult;
  Check(h.service->AddFriend("AAAA-2222") == Result::kOwnCode, "own code is refused");
  Check(h.service->AddFriend("bbbb 3333") == Result::kAlreadyFriends, "an existing friend is refused");
  Check(h.service->AddFriend("nope") == Result::kInvalid, "a malformed code is refused");
  Check(h.service->AddFriend("CCCC-4444") == Result::kSent && h.wire().Count("friends.add") == 1, "a request is sent");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"friend.request","player":"DDDD-5555","name":"Caio"})");
  h.Tick();
  Check(!h.service->TakeNotices().empty(), "an incoming request raises a notice");
}

void TestRoomLifecycle() {
  Harness h;
  h.SignIn();
  Check(!h.service->CreateRoom(5, {}) && !h.service->CreateRoom(1, {}), "capacity must be 2 to 4");
  Check(h.service->CreateRoom(3, {{"laps", 3}}), "a room is requested");
  const std::vector<std::string> seats = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "", "", "empty"), kEmpty3};
  std::vector<std::string> empty_seats = {Seat(0, "AAAA-2222", "Me", "connected"),
                                          R"({"seat":1,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""})",
                                          kEmpty3};
  h.Reply("room.host", R"(,"room":)" + Room("open", empty_seats) + R"(,"seat":0,"relay_token":77)");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(s.room && s.room->capacity == 3 && s.seat == 0 && s.room->seats.size() == 3, "host is seat 0 of a 3-seat room");
  Check(!h.service->CreateRoom(2, {}), "a second room is refused while seated");

  std::vector<std::string> joined = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "BBBB-3333", "Ana", "connected"), kEmpty3};
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + Room("open", joined) + "}");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room->seats[1].name == "Ana" && s.peers.size() == 1 && s.peers[0].seat == 1 && s.peers[0].connected,
        "a joining player appears as a peer");
  Check(h.service->UpdateRoom(4, nullptr) && h.wire().Count("room.update") == 1, "the host resizes the room");
  Check(h.service->SetReady(true) && h.wire().Count("match.ready") == 1, "ready is sent");
  Check(h.service->StartRace() && h.wire().Count("room.start") == 1, "the host starts the race");
  h.Reply("room.start", R"(,"room":)" + Room("starting", joined));
  h.Tick();
  Check(h.service->snapshot().room->state == online::RoomState::kStarting, "the room is starting");
  Check(!h.service->UpdateRoom(4, nullptr), "a starting room cannot be resized");

  std::vector<std::string> left = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "", "", "empty"), kEmpty3};
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + Room("starting", left) + "}");
  h.Tick();
  Check(h.service->snapshot().peers.empty(), "a leaving player is dropped from the peers");
  bool said_left = false;
  for (const std::string& notice : h.service->TakeNotices()) said_left |= notice.find("Player left") != std::string::npos;
  Check(said_left, "the leaving player is announced");

  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.closed","room":"0123456789abcdef","reason":"host_left"})");
  h.Tick();
  s = h.service->snapshot();
  Check(!s.room && s.closed_reason == "host_left" && s.seat == -1, "closing the room clears the seat");
  Check(h.service->CreateRoom(2, {}), "a new room can be made after");
}

void TestJoinSearchAndInvites() {
  Harness h;
  h.SignIn();
  h.service->SearchRooms();
  h.Reply("room.search", R"(,"rooms":[{"handle":"abcd1234","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":4,"rules":{},"platform":"Windows","version":"0.1.0"}])");
  h.Tick();
  Check(h.service->snapshot().rooms.size() == 1 && h.service->snapshot().search_generation == 1, "search results");
  Check(h.service->JoinRoom("abcd1234") && h.wire().Count("room.join") == 1, "a room is joined by handle");
  const std::vector<std::string> seats = {Seat(0, "BBBB-3333", "Ana", "connected"), Seat(1, "AAAA-2222", "Me", "connected"),
                                          kEmpty3, kEmpty3};
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(h.wire().LastSeq("room.join")) + R"(,"code":"version_mismatch"})");
  h.Tick();
  Check(!h.service->snapshot().room, "a refused join leaves no room");
  Check(!h.service->TakeNotices().empty(), "and says why");

  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv1","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":3,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  Check(h.service->snapshot().invites.size() == 1, "an invite is listed");
  Check(h.service->AcceptInvite("inv1") && h.service->snapshot().invites.empty(), "accepting removes it");
  h.Reply("invite.accept", R"(,"room":)" + Room("open", {seats[0], seats[1], kEmpty3}) + R"(,"seat":1)");
  h.Tick();
  const online::Snapshot s = h.service->snapshot();
  Check(s.room && s.seat == 1 && s.peers.size() == 1 && s.peers[0].seat == 0, "the joiner holds the seat the service gave");
  Check(!h.service->StartRace() && !h.service->SendInvite("CCCC-4444"), "only the host starts and invites");
  h.service->LeaveRoom();
  Check(h.wire().Count("room.leave") == 1 && !h.service->snapshot().room, "leaving the room");
}

void TestReconnectResumesSeat() {
  Harness h;
  h.SignIn();
  h.service->CreateRoom(2, {});
  const std::vector<std::string> seats = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "BBBB-3333", "Ana", "connected")};
  h.Reply("room.host", R"(,"room":)" + Room("racing", seats) + R"(,"seat":0,"relay_token":5)");
  h.Tick();
  Check(h.service->snapshot().room.has_value(), "seated");

  h.wire().Push(online::ChannelEvent::Kind::kClosed, "network");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(!h.service->SignedIn() && s.room && s.reconnecting, "a lost link keeps the seat and says it is reconnecting");
  Check(s.connection == online::Connection::kChecking, "the connection is shown as reconnecting");

  h.Tick(2.0);  // past the retry delay
  Check(h.wires.size() == 2, "a new connection is opened");
  h.wire().Push(online::ChannelEvent::Kind::kOpen);
  h.Tick();
  h.Reply("hello", R"(,"friend_code":"AAAA-2222","capabilities":{"protocol_version":1,"relay_supported":true},"resume":"0123456789abcdef")");
  h.Tick();
  Check(h.wire().Count("match.resume") == 1, "the seat is resumed after sign-in");
  h.Reply("match.resume", R"(,"room":)" + Room("racing", {seats[0], Seat(1, "BBBB-3333", "Ana", "reconnecting")}) + R"(,"seat":0,"relay_token":6)");
  h.Tick();
  s = h.service->snapshot();
  Check(h.service->SignedIn() && s.room && !s.reconnecting && s.seat == 0, "the seat is back");
  Check(s.peers.size() == 1 && !s.peers[0].connected, "a peer that is itself reconnecting is shown as such");

  // A hold that expired: the resume is refused and the room is gone.
  h.wire().Push(online::ChannelEvent::Kind::kClosed, "network");
  h.Tick();
  h.Tick(5.0);
  h.wire().Push(online::ChannelEvent::Kind::kOpen);
  h.Tick();
  h.Reply("hello", R"(,"friend_code":"AAAA-2222","resume":null)");
  h.Tick();
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(h.wire().LastSeq("match.resume")) + R"(,"code":"not_found"})");
  h.Tick();
  s = h.service->snapshot();
  Check(!s.room && s.closed_reason == "disconnect", "an expired hold ends the room");
}

void TestSettingsAndDisable() {
  Harness h;
  h.SignIn();
  h.service->Apply({false, "Me"});
  h.Tick();
  Check(!h.service->SignedIn() && h.service->snapshot().connection == online::Connection::kOff, "turning Online play off signs out");
  Check(h.service->snapshot().friends.empty(), "and forgets the friends");

  online::ServiceConfig config;
  config.url = "";
  config.log = [](const std::string&) {};
  online::OnlineService unconfigured(std::move(config));
  unconfigured.Apply({true, "Me"});
  unconfigured.Update(1.0);
  Check(unconfigured.snapshot().connection == online::Connection::kError && !unconfigured.SignedIn(),
        "an unconfigured service stays disconnected");
  Check(std::string(online::kDefaultServiceUrl) == "wss://eck-lobby.saulobenigno.workers.dev", "the production service is the default");
  Check(online::ServiceConfig{}.url == online::kDefaultServiceUrl, "a default configuration needs no address");
  online::WsUrl parsed;
  Check(online::parse_ws_url(online::kDefaultServiceUrl, parsed, nullptr) && parsed.tls && parsed.port == 443 && parsed.path == "/" &&
            parsed.host == "eck-lobby.saulobenigno.workers.dev",
        "the default address is a secure WebSocket at the service root");
}

}  // namespace

int main() {
  eck::SetActiveUiLanguage("en-US");
  TestSignInAndFriends();
  TestRoomLifecycle();
  TestJoinSearchAndInvites();
  TestReconnectResumesSeat();
  TestSettingsAndDisable();
  return online_test::Finish();
}
