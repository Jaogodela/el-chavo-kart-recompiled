#include <algorithm>
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

std::string OtherRoom(std::string room) {
  const std::string id = "0123456789abcdef";
  room.replace(room.find(id), id.size(), "fedcba9876543210");
  return room;
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

void TestSessionLifecycle() {
  Harness h;
  h.SignIn();
  h.service->HostSession({{"laps", 3}});
  Check(h.wire().Count("room.host") == 1, "the hidden session is requested");
  Check(h.wire().sent.back().find("\"capacity\":4") != std::string::npos, "always four seats");
  std::vector<std::string> alone = {Seat(0, "AAAA-2222", "Me", "connected"),
                                    R"({"seat":1,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""})",
                                    kEmpty3};
  h.Reply("room.host", R"(,"room":)" + Room("open", alone) + R"(,"seat":0,"relay_token":77)");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(s.room && s.seat == 0 && s.hosting, "the host is seat 0");
  h.service->HostSession({});
  Check(h.wire().Count("room.host") == 1, "a second request while seated sends nothing");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.created","room":)" + Room("open", alone) + R"(,"seat":0,"relay_token":77})");
  h.Tick();
  Check(h.service->snapshot().seat == 0, "the room.created twin of the reply changes nothing");

  std::vector<std::string> joined = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "BBBB-3333", "Ana", "connected"), kEmpty3};
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + Room("open", joined) + "}");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room->seats[1].name == "Ana" && s.peers.size() == 1 && s.peers[0].seat == 1 && s.peers[0].connected,
        "a joining player appears as a peer");

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
  Check(!s.room && s.closed_reason == "host_left" && s.seat == -1, "closing the session clears the seat");
  h.service->LeaveSession();
  Check(!h.service->snapshot().hosting, "leaving ends the intent");
}

void TestInvites() {
  Harness h;
  h.SignIn();
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv1","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":4,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  Check(h.service->snapshot().invites.size() == 1, "an invite is listed");
  Check(!h.service->AcceptInvite("nope"), "an unknown invite cannot be accepted");
  Check(h.service->AcceptInvite("inv1") && h.service->snapshot().invites.empty(), "accepting removes it");
  Check(h.service->snapshot().joining && h.service->snapshot().accepted_generation == 1, "and starts the join");
  Check(!h.service->AcceptInvite("inv1") && h.wire().Count("invite.accept") == 1, "one accept, one request");
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(h.wire().LastSeq("invite.accept")) + R"(,"code":"room_full"})");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(!s.room && !s.joining, "a refused join leaves nothing behind");
  Check(!h.service->TakeNotices().empty(), "and says why");

  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv2","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":3,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  const std::vector<std::string> seats = {Seat(0, "BBBB-3333", "Ana", "connected"), Seat(1, "AAAA-2222", "Me", "connected"), kEmpty3};
  Check(h.service->AcceptInvite("inv2"), "a new invite after a failed join");
  h.Reply("invite.accept", R"(,"room":)" + Room("open", seats) + R"(,"seat":1)");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room && s.seat == 1 && s.peers.size() == 1 && s.peers[0].seat == 0, "the joiner holds the seat the service gave");
  Check(!h.service->StartRace() && !h.service->SendInvite("CCCC-4444"), "only the host starts and invites");
  h.service->LeaveSession();
  Check(h.wire().Count("room.leave") == 1 && !h.service->snapshot().room, "leaving the session");

  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv3","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":3,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"invite.ended","invite":"inv3","reason":"host_left"})");
  h.Tick();
  Check(h.service->snapshot().invites.empty() && !h.service->AcceptInvite("inv3"), "an ended invite is gone");

  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv4","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":3,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  Check(h.service->AcceptInvite("inv4"), "accepted");
  h.Tick(11.0);
  Check(!h.service->snapshot().joining, "a join the service never answers gives up");
  h.Reply("invite.accept", R"(,"room":)" + Room("open", seats) + R"(,"seat":1)");
  h.Tick();
  Check(!h.service->snapshot().room && h.wire().Count("room.leave") == 2, "and a late answer gives the seat back");
}

const std::string kFriendAna = R"({"player":"BBBB-3333","name":"Ana","online":true,"state":"online","platform":"Windows","version":"0.1.0","image":"x","build":1,"compatible":true})";

std::string SoloRoom() {
  std::string room = Room("open", {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "", "", "empty"), kEmpty3});
  const std::string two = R"("players":2)";
  room.replace(room.find(two), two.size(), R"("players":1)");
  return room;
}

const online::RoomRules kRules = {{"setup", 1}};

void SignInWithAFriend(Harness& h) {
  h.SignIn();
  h.Reply("friends.list", R"(,"friends":[)" + kFriendAna + "]");
  h.Tick();
}

void HostAndSend(Harness& h) {
  Check(h.service->SendInvite("BBBB-3333", kRules), "the first invite starts");
  h.Reply("room.host", R"(,"room":)" + SoloRoom() + R"(,"seat":0,"relay_token":5)");
  h.Tick();
}

void Ended(Harness& h, const std::string& reason) {
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"invite.ended","invite":"inv1","reason":")" + reason + R"("})");
  h.Tick();
}

void TestFirstInviteMakesTheSession() {
  Harness h;
  SignInWithAFriend(h);
  Check(!h.service->SendInvite("BBBB-3333"), "with no session and no game to carry, nothing starts");
  Check(h.wire().Count("room.host") == 0, "no room comes of it");
  online::Snapshot s = h.service->snapshot();
  Check(!s.room && s.seat == -1 && !s.creating_session && s.sending_invite.empty(), "the player stays free");

  Check(h.service->SendInvite("BBBB-3333", kRules), "choosing a friend starts the send");
  Check(!h.service->SendInvite("BBBB-3333", kRules) && !h.service->SendInvite("CCCC-4444", kRules), "one send at a time");
  Check(h.wire().Count("room.host") == 1, "exactly one session is asked for");
  Check(h.wire().Count("invite.send") == 0, "and no invite goes before seat 0 is confirmed");
  h.Tick(1.0);
  s = h.service->snapshot();
  Check(s.creating_session && s.sending_invite == "BBBB-3333", "creating_session");
  Check(s.invited.empty(), "no seat is held on a local call");

  h.Reply("room.host", R"(,"room":)" + SoloRoom() + R"(,"seat":0,"relay_token":5)");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room && s.seat == 0 && !s.creating_session, "host seat 0");
  Check(h.wire().Count("invite.send") == 1, "then the invite goes, once");
  Check(s.invited.empty() && s.sending_invite == "BBBB-3333", "and still holds nothing");

  h.Reply("invite.send", R"(,"invite":"inv1","expires":9)");
  h.Tick();
  s = h.service->snapshot();
  Check(s.invited.size() == 1 && s.invited[0].id == "inv1" && s.invited[0].name == "Ana", "the service took it: one seat is held");
  Check(s.sending_invite.empty(), "and the send is over");
  Check(!h.service->SendInvite("BBBB-3333"), "the same friend is not invited twice");
  Check(h.service->SendInvite("CCCC-4444"), "the next invite reuses the session");
  Check(h.wire().Count("room.host") == 1 && h.wire().Count("invite.send") == 2, "without another room");
}

void TestInviteReservationEnds() {
  Harness h;
  SignInWithAFriend(h);
  HostAndSend(h);
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(h.wire().LastSeq("invite.send")) + R"(,"code":"offline"})");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(s.invited.empty() && s.sending_invite.empty(), "a rejected send holds nothing");
  Check(s.invite_outcome.generation == 1 && s.invite_outcome.name == "Ana" && !s.invite_outcome.declined, "and says who");
  Check(s.room && s.seat == 0, "the session stays for the retry");

  const auto invite = [&] {
    Check(h.service->SendInvite("BBBB-3333"), "the friend can be tried again");
    h.Reply("invite.send", R"(,"invite":"inv1","expires":9)");
    h.Tick();
    Check(h.service->snapshot().invited.size() == 1, "one seat is held");
  };
  invite();
  Ended(h, "declined");
  s = h.service->snapshot();
  Check(s.invited.empty() && s.invite_outcome.declined, "a decline gives it back");

  invite();
  Ended(h, "expired");
  Check(h.service->snapshot().invited.empty(), "so does the service's expiry");

  invite();
  h.Tick(61.0);
  Check(h.service->snapshot().invited.empty(), "and this side's own, if that event is lost");

  invite();
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"friend.offline","player":"BBBB-3333"})");
  h.Tick();
  Check(h.service->snapshot().invited.empty(), "a friend who disconnects gives it back");
  Check(h.service->snapshot().room->room == "0123456789abcdef", "all of it on one session");
}

void TestHostCreationFails() {
  Harness h;
  SignInWithAFriend(h);
  Check(h.service->SendInvite("BBBB-3333", kRules), "the send starts");
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"re":)" + std::to_string(h.wire().LastSeq("room.host")) + R"(,"code":"rate_limited"})");
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(!s.room && !s.hosting && !s.creating_session && s.sending_invite.empty(), "a refused session leaves nothing");
  Check(s.invite_outcome.generation == 1, "and is reported");
  h.Tick(10.0);
  Check(h.wire().Count("room.host") == 1, "nothing asks again by itself");
  Check(h.service->SendInvite("BBBB-3333", kRules), "the retry is a new send");
  Check(h.wire().Count("room.host") == 2, "with a new request");

  h.Tick(16.0);
  s = h.service->snapshot();
  Check(!s.creating_session && s.sending_invite.empty() && s.invite_outcome.generation == 2, "a service that never answers ends the send");
  h.Reply("room.host", R"(,"room":)" + SoloRoom() + R"(,"seat":0,"relay_token":5)");
  h.Tick();
  Check(!h.service->snapshot().room && h.wire().Count("room.leave") == 1, "and its late room is given back");

  Harness lost;
  SignInWithAFriend(lost);
  Check(lost.service->SendInvite("BBBB-3333", kRules), "a send in flight");
  lost.wire().Push(online::ChannelEvent::Kind::kClosed, "network");
  lost.Tick();
  s = lost.service->snapshot();
  Check(s.sending_invite.empty() && !s.hosting && !s.room, "a lost link ends it");

  Harness closed;
  SignInWithAFriend(closed);
  HostAndSend(closed);
  closed.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.closed","room":"0123456789abcdef","reason":"expired"})");
  closed.Tick();
  s = closed.service->snapshot();
  Check(!s.room && !s.hosting && s.sending_invite.empty(), "a closed session ends the send");
  closed.Tick(70.0);
  Check(closed.wire().Count("room.host") == 1, "and no hidden session comes back by itself");
}

void TestRoleReplacement() {
  Harness h;
  SignInWithAFriend(h);
  HostAndSend(h);
  h.Reply("invite.send", R"(,"invite":"inv1","expires":9)");
  h.Tick();
  h.service->SendPick(online::PickState::kConfirmed, 3);
  const int old_pick = h.wire().LastSeq("match.pick");
  online::Snapshot s = h.service->snapshot();
  const int generation = s.session_generation;
  Check(s.room && s.seat == 0 && s.invited.size() == 1, "A hosts alone, with one invite out");

  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv9","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":4,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  Check(h.service->AcceptInvite("inv9"), "A takes Ana's invite instead");
  const int leave = h.wire().LastSeq("room.leave");
  const int accept = h.wire().LastSeq("invite.accept");
  Check(leave > 0 && leave < accept, "the old room is left before the join is asked for");
  s = h.service->snapshot();
  Check(!s.room && s.seat == -1 && !s.hosting && s.joining, "nothing of the host is left");
  Check(s.invited.empty() && s.sending_invite.empty() && s.peers.empty(), "no invite, no path");
  Check(s.session_generation > generation, "and the old session is void");

  const std::vector<std::string> seats = {Seat(0, "BBBB-3333", "Ana", "connected"), Seat(1, "AAAA-2222", "Me", "connected"), kEmpty3};
  h.Reply("invite.accept", R"(,"room":)" + OtherRoom(Room("open", seats)) + R"(,"seat":1,"relay_token":7)");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room && s.room->room == "fedcba9876543210" && s.seat == 1 && !s.joining, "A is seat 1 in the inviter's room");
  const int joined = s.session_generation;
  Check(joined > generation + 1, "as a session of its own");

  // Everything the old room still has on the wire.
  std::vector<std::string> mine = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "CCCC-4444", "Eve", "connected"), kEmpty3};
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + Room("open", mine) + "}");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.closed","room":"0123456789abcdef","reason":"cancelled"})");
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"match.transport","room":"0123456789abcdef","seat":0,"transport":"relay"})");
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"match.peer","room":"0123456789abcdef","seat":2,"player":"CCCC-4444","name":"Eve","ip":"1.2.3.4","port":7101,"lan":[],"candidates":[],"same_network":false,"relay":false})");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"re":)" + std::to_string(old_pick) + R"(,"code":"not_found"})");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"relay.closed","room":"0123456789abcdef","reason":"gone"})");
  h.Tick();
  s = h.service->snapshot();
  Check(s.room && s.room->room == "fedcba9876543210" && s.seat == 1, "no event of the old room moves A");
  Check(s.room->seats[1].player == "AAAA-2222" && s.peers.size() == 1 && s.peers[0].seat == 0, "nor its seats or peers");
  Check(s.session_generation == joined, "and nothing restarts");
  Check(s.closed_reason.empty(), "nor says the session ended");
  Check(std::none_of(h.log.begin(), h.log.end(), [](const std::string& line) { return line.find("online request failed") != std::string::npos; }),
        "an answer to a request of the old room is dropped");
  Check(!h.service->SendInvite("CCCC-4444"), "a guest does not invite");
}

void TestAcceptWhileASessionIsOnItsWay() {
  Harness h;
  SignInWithAFriend(h);
  Check(h.service->SendInvite("BBBB-3333", kRules), "a first invite is creating a session");
  h.wire().Push(online::ChannelEvent::Kind::kText,
                R"({"ev":"invite.received","invite":{"id":"inv9","host":"BBBB-3333","host_name":"Ana","players":1,"capacity":4,"rules":{},"image":"x","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
  h.Tick();
  Check(h.service->AcceptInvite("inv9"), "the friend's invite wins");
  online::Snapshot s = h.service->snapshot();
  Check(s.sending_invite.empty() && !s.hosting && !s.creating_session, "the send is dropped");
  h.Reply("room.host", R"(,"room":)" + SoloRoom() + R"(,"seat":0,"relay_token":5)");
  h.Tick();
  Check(!h.service->snapshot().room && h.wire().Count("room.leave") == 1, "and the room it asked for is given back");
  Check(h.wire().Count("invite.send") == 0, "with no invite sent from it");
}

bool Logged(const Harness& h, const std::string& text) {
  return std::any_of(h.log.begin(), h.log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
}

void Seated(Harness& h, const std::vector<std::string>& seats, int seat, const char* state = "racing") {
  h.SignIn();
  h.service->HostSession({});
  h.Reply("room.host", R"(,"room":)" + Room(state, seats) + R"(,"seat":)" + std::to_string(seat) + R"(,"relay_token":5)");
  h.Tick();
}

const std::vector<std::string> kPair = {Seat(0, "BBBB-3333", "Ana", "connected"), Seat(1, "AAAA-2222", "Me", "connected"), kEmpty3};

void TestLocalLeave() {
  Harness h;
  Seated(h, kPair, 1);
  const int generation = h.service->snapshot().session_generation;
  Check(h.service->LeaveSession("guest_menu"), "the service was told");
  Check(h.wire().Count("room.leave") == 1, "one room.leave");
  online::Snapshot s = h.service->snapshot();
  Check(!s.room && s.seat == -1 && s.peers.empty() && s.closed_reason.empty(), "nothing of the session is left");
  Check(s.session_generation == generation + 1, "and it is void");
  Check(Logged(h, "[ONLINE-LIFECYCLE] local_leave requested seat=1 source=guest_menu"), "said once in the log");
  Check(Logged(h, "[ONLINE-LIFECYCLE] cleanup room=0123456789abcdef"), "with its cleanup");

  Check(!h.service->LeaveSession("guest_menu") && h.wire().Count("room.leave") == 1, "a second leave sends nothing");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + Room("racing", kPair) + "}");
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.closed","room":"0123456789abcdef","reason":"host_left"})");
  h.Tick();
  s = h.service->snapshot();
  Check(!s.room && s.closed_reason.empty() && s.session_generation == generation + 1, "old room events change nothing after a leave");

  Harness gone;
  Seated(gone, kPair, 1);
  gone.wire().Push(online::ChannelEvent::Kind::kClosed, "network");
  gone.Tick();
  Check(!gone.service->LeaveSession("window_close"), "with no link there is nobody to tell");
}

void TestHostGone() {
  for (const char* reason : {"host_left", "disconnect"}) {
    Harness h;
    Seated(h, kPair, 1);
    const int generation = h.service->snapshot().session_generation;
    const std::string closed = std::string(R"({"ev":"room.closed","room":"0123456789abcdef","reason":")") + reason + R"("})";
    h.wire().Push(online::ChannelEvent::Kind::kText, closed);
    h.wire().Push(online::ChannelEvent::Kind::kText, closed);
    h.Tick();
    const online::Snapshot s = h.service->snapshot();
    Check(!s.room && s.seat == -1 && s.closed_reason == reason, "the service's word ends the session");
    Check(s.session_generation == generation + 1, "once, however often it is said");
    Check(h.wire().Count("room.leave") == 0, "and there is nothing left to leave");
    Check(Logged(h, "[ONLINE-LIFECYCLE] host_left seat=0"), "the log says who");
  }

  // What must never read as the host leaving.
  Harness h;
  Seated(h, kPair, 1);
  const auto update = [&](const std::string& room) {
    h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + room + "}");
    h.Tick();
  };
  std::string not_ready = Room("racing", kPair);
  update(not_ready);
  update(Room("racing", {Seat(0, "BBBB-3333", "Ana", "reconnecting"), kPair[1], kEmpty3}));
  Check(h.service->snapshot().room.has_value(), "a host reconnecting is still the host");
  update(Room("racing", kPair));
  Check(h.service->snapshot().room.has_value() && h.service->snapshot().closed_reason.empty(), "and comes back");
  Check(!Logged(h, "host_left"), "none of it is a host leaving");

  Harness watched;
  Seated(watched, kPair, 1);
  watched.service->AbandonSession("host_left");
  const online::Snapshot s = watched.service->snapshot();
  Check(!s.room && s.closed_reason == "host_left", "the fallback ends the session with what the player is told");
  Check(watched.wire().Count("room.leave") == 1, "and tells the service, in case the seat is still there");
}

void TestGuestGoneFromTheHost() {
  Harness h;
  const std::vector<std::string> mine = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "BBBB-3333", "Ana", "connected"), kEmpty3};
  Seated(h, mine, 0);
  const int generation = h.service->snapshot().session_generation;
  std::string left = Room("racing", {mine[0], Seat(1, "", "", "empty"), kEmpty3});
  h.wire().Push(online::ChannelEvent::Kind::kText, R"({"ev":"room.updated","room":)" + left + "}");
  h.Tick();
  const online::Snapshot s = h.service->snapshot();
  Check(s.room && s.seat == 0 && s.session_generation == generation, "the host stays in the session");
  Check(s.room->seats[1].state == online::SeatState::kEmpty, "the seat is free");
  Check(Logged(h, "[ONLINE-LIFECYCLE] guest_left seat=1"), "guest_left");
  Check(!Logged(h, "host_left"), "and nobody says the host left");
  Check(s.closed_reason.empty(), "the session is not closed");
}

void TestReopen() {
  const std::vector<std::string> mine = {Seat(0, "AAAA-2222", "Me", "connected"), Seat(1, "BBBB-3333", "Ana", "connected"), kEmpty3};
  Harness old;
  Seated(old, mine, 0);
  Check(!old.service->ReopenRoom({{"after_race", 1}}), "a service that cannot reopen is not asked");
  Check(old.wire().Count("room.reopen") == 0, "and nothing goes out");

  Harness h;
  h.Tick();
  h.wire().Push(online::ChannelEvent::Kind::kOpen);
  h.Tick();
  h.Reply("hello", R"(,"friend_code":"AAAA-2222","capabilities":{"protocol_version":1,"room_reopen":true},"resume":null)");
  h.Tick();
  h.service->HostSession({});
  h.Reply("room.host", R"(,"room":)" + Room("racing", mine) + R"(,"seat":0,"relay_token":5)");
  h.Tick();
  const int generation = h.service->snapshot().session_generation;
  Check(h.service->ReopenRoom({{"after_race", 1}, {"after_dest", 2}}), "the host's Continue");
  Check(!h.service->ReopenRoom({{"after_race", 1}, {"after_dest", 2}}), "a duplicate is not sent");
  Check(h.wire().Count("room.reopen") == 1, "one event");
  h.Reply("room.reopen", R"(,"room":)" + Room("open", mine));
  h.Tick();
  online::Snapshot s = h.service->snapshot();
  Check(s.room && s.room->state == online::RoomState::kOpen && s.seat == 0, "the room is open again");
  Check(s.session_generation == generation, "as the same session");
  Check(!h.service->ReopenRoom({}), "and an open room has nothing to reopen");

  Harness refused;
  refused.Tick();
  refused.wire().Push(online::ChannelEvent::Kind::kOpen);
  refused.Tick();
  refused.Reply("hello", R"(,"friend_code":"AAAA-2222","capabilities":{"protocol_version":1,"room_reopen":true},"resume":null)");
  refused.Tick();
  refused.service->HostSession({});
  refused.Reply("room.host", R"(,"room":)" + Room("racing", mine) + R"(,"seat":0,"relay_token":5)");
  refused.Tick();
  Check(refused.service->ReopenRoom({}), "sent");
  refused.wire().Push(online::ChannelEvent::Kind::kText,
                      R"({"re":)" + std::to_string(refused.wire().LastSeq("room.reopen")) + R"(,"code":"room_not_racing"})");
  refused.Tick();
  Check(!refused.service->snapshot().room && refused.wire().Count("room.leave") == 1, "a refused step ends the session: nobody is left waiting");
}

void TestStaleSeatFromAnEarlierRun() {
  Harness h;
  h.SignIn(R"("0123456789abcdef")");
  Check(h.wire().Count("match.resume") == 0, "a seat this process knows nothing of is not resumed");
  Check(h.wire().Count("room.leave") == 1, "it is given back, so it cannot keep this player busy");
  Check(!h.service->snapshot().room, "and nothing is seated");
}

void TestReconnectResumesSeat() {
  Harness h;
  h.SignIn();
  h.service->HostSession({});
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
  Check(!s.room && s.closed_reason == "connection_lost", "an expired hold ends the room, as this side's own loss");
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
  TestSessionLifecycle();
  TestInvites();
  TestFirstInviteMakesTheSession();
  TestInviteReservationEnds();
  TestHostCreationFails();
  TestRoleReplacement();
  TestAcceptWhileASessionIsOnItsWay();
  TestLocalLeave();
  TestHostGone();
  TestGuestGoneFromTheHost();
  TestReopen();
  TestStaleSeatFromAnEarlierRun();
  TestReconnectResumesSeat();
  TestSettingsAndDisable();
  return online_test::Finish();
}
