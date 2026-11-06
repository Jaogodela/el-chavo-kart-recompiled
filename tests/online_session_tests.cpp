// The invite-only flow end to end: real OnlineService instances on real loopback gameplay sockets, against a small
// in-process hub that speaks the wire protocol of server/eck-lobby. The clock is the test's; UDP is not simulated.
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "online/json.h"
#include "online/online_service.h"
#include "online_lobby.h"
#include "online_setup_capture.h"
#include "online_test_support.h"
#include "ui_text.h"

using online_test::Check;
namespace online = eck::online;
namespace json = eck::online::json;

namespace {

constexpr double kStep = 0.02;
constexpr uint32_t kRelayToken = 4242;

struct Wire {
  std::deque<online::ChannelEvent> incoming;
  std::vector<std::string> sent;
  size_t read = 0;
  bool closed = false;
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

struct Player {
  std::string token;
  std::string code;
  std::string name;
  std::shared_ptr<Wire> wire;
  std::vector<std::shared_ptr<Wire>> wires;
  std::vector<std::string> log;
  online::OnlineGameSocket socket;
  std::unique_ptr<online::OnlineService> service;
  uint16_t port = 0;       // the published endpoint; 0 until match.endpoint
  bool blocked = false;    // no UDP path to or from this player
  bool no_relay = false;   // the service refuses to relay this seat
  bool silent = false;     // its endpoints never reach the other side
  bool unreachable = false;  // the service cannot be reached any more
  bool online = false;

  online::Snapshot Snap() const { return service->snapshot(); }
  bool Logged(const std::string& text) const {
    return std::any_of(log.begin(), log.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }
  int Sent(const std::string& op) const {
    int n = 0;
    for (const std::shared_ptr<Wire>& w : wires) {
      for (const std::string& text : w->sent) n += text.find("\"op\":\"" + op + "\"") != std::string::npos;
    }
    return n;
  }
  const online::PeerSnapshot* PeerAt(int seat) const {
    static online::Snapshot held;
    held = Snap();
    for (const online::PeerSnapshot& peer : held.peers) {
      if (peer.seat == seat) return &peer;
    }
    return nullptr;
  }
};

struct Room {
  std::string id;
  std::string state = "open";
  std::vector<Player*> seats = std::vector<Player*>(4, nullptr);
  std::vector<bool> ready = std::vector<bool>(4, false);
  std::vector<std::string> transport = std::vector<std::string>(4);
  std::vector<bool> away = std::vector<bool>(4, false);
  std::vector<int> pick = std::vector<int>(4, 0);
  std::vector<int> character = std::vector<int>(4, 0);
  std::string rules = R"({"laps":3,"items":1})";
  uint32_t token = kRelayToken;
};

struct PendingInvite {
  std::string id;
  Room* room = nullptr;
  Player* target = nullptr;
};

std::string Q(const std::string& text) { return "\"" + text + "\""; }

class Hub {
 public:
  std::vector<std::unique_ptr<Player>> players;
  std::vector<std::unique_ptr<Room>> rooms;
  std::vector<PendingInvite> invites;
  bool relay_offered = true;
  double now = 0;

  Player& Add(const std::string& name) {
    auto player = std::make_unique<Player>();
    Player* raw = player.get();
    raw->name = name;
    raw->token = std::string(63, 'a') + char('0' + players.size());
    raw->code = online::DerivePlayerId(raw->token);
    Check(raw->socket.Open({0}, true), "a loopback gameplay socket");
    online::ServiceConfig config;
    config.url = "ws://127.0.0.1:1/";
    config.version = "0.1.0";
    config.platform = "Windows";
    config.game_socket = &raw->socket;
    config.synchronous_probes = true;
    config.resolve_stun = [](const std::string&) { return std::vector<online::Endpoint>(); };
    config.identity = [raw] { return online::Identity{raw->token, raw->code, false, ""}; };
    config.log = [raw](const std::string& line) { raw->log.push_back(line); };
    config.open_channel = [raw](const std::string&) {
      raw->wire = std::make_shared<Wire>();
      raw->wires.push_back(raw->wire);
      if (!raw->unreachable) raw->wire->incoming.push_back({online::ChannelEvent::Kind::kOpen, {}});
      return std::unique_ptr<online::LobbyChannel>(new FakeChannel(raw->wire));
    };
    raw->service = std::make_unique<online::OnlineService>(std::move(config));
    raw->service->Apply({true, name});
    players.push_back(std::move(player));
    return *raw;
  }

  void Step() {
    now += kStep;
    for (const auto& p : players) p->service->Update(now);
    for (const auto& p : players) Serve(*p);
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }

  void Deliver(Player& player, const std::string& text) { Handle(player, text); }

  template <typename Done>
  bool Until(Done done, double seconds = 5.0) {
    const double limit = now + seconds;
    while (now < limit) {
      if (done()) return true;
      Step();
    }
    return done();
  }

  bool AllSigned() {
    return std::all_of(players.begin(), players.end(), [](const auto& p) { return p->service->SignedIn(); });
  }

  Room* RoomOf(const Player& player) {
    for (const auto& room : rooms) {
      if (std::find(room->seats.begin(), room->seats.end(), &player) != room->seats.end()) return room.get();
    }
    return nullptr;
  }

  // The player's link dies; the seat is held, as the service holds it.
  void Drop(Player& player) {
    player.wire->incoming.push_back({online::ChannelEvent::Kind::kClosed, "network"});
    player.online = false;
    if (Room* room = RoomOf(player)) {
      room->away[SeatOf(*room, player)] = true;
      Announce(*room);
    }
  }

  // The grace period ran out on a player who never came back.
  void Expire(Player& player) {
    Room* room = RoomOf(player);
    if (!room) return;
    if (room->seats[0] == &player) {
      CloseRoom(*room, "disconnect", &player);
      return;
    }
    const int seat = SeatOf(*room, player);
    room->seats[size_t(seat)] = nullptr;
    room->away[size_t(seat)] = false;
    room->ready[size_t(seat)] = false;
    room->transport[size_t(seat)].clear();
    Announce(*room);
  }

  int SeatOf(const Room& room, const Player& player) const {
    return int(std::find(room.seats.begin(), room.seats.end(), &player) - room.seats.begin());
  }

 private:
  void Push(Player& to, const std::string& text) {
    if (to.wire) to.wire->incoming.push_back({online::ChannelEvent::Kind::kText, text});
  }

  void Serve(Player& player) {
    if (!player.wire) return;
    const std::shared_ptr<Wire> wire = player.wire;
    while (wire->read < wire->sent.size()) Handle(player, wire->sent[wire->read++]);
  }

  static std::string RulesText(const std::string& request) {
    const size_t start = request.find("\"rules\":");
    return request.substr(start + 8, request.find('}', start) - start - 7);
  }

  std::string SeatJson(const Room& room, int i) const {
    const Player* p = room.seats[size_t(i)];
    if (!p) return R"({"seat":)" + std::to_string(i) + R"(,"player":"","name":"","state":"empty","ready":false,"platform":"","version":"","transport":""})";
    return R"({"seat":)" + std::to_string(i) + R"(,"player":)" + Q(p->code) + R"(,"name":)" + Q(p->name) + R"(,"state":)" +
           Q(room.away[size_t(i)] ? "reconnecting" : "connected") + R"(,"ready":)" + (room.ready[size_t(i)] ? "true" : "false") +
           R"(,"platform":"Windows","version":"0.1.0","transport":)" + Q(room.transport[size_t(i)]) + R"(,"pick":)" +
           std::to_string(room.pick[size_t(i)]) + R"(,"character":)" + std::to_string(room.character[size_t(i)]) + "}";
  }

  std::string RoomJson(const Room& room) const {
    int count = 0;
    std::string seats;
    for (int i = 0; i < 4; ++i) {
      count += room.seats[size_t(i)] != nullptr;
      seats += (i ? "," : "") + SeatJson(room, i);
    }
    return R"({"room":)" + Q(room.id) + R"(,"host":)" + Q(room.seats[0]->code) + R"(,"host_name":)" + Q(room.seats[0]->name) +
           R"(,"capacity":4,"players":)" + std::to_string(count) + R"(,"state":)" + Q(room.state) +
           R"(,"image":"eck-usa-1db4e96b","build":1,"rules":)" + room.rules + R"(,"seats":[)" + seats + R"(],"created":1,"updated":2})";
  }

  void Announce(const Room& room) {
    const std::string event = R"({"ev":"room.updated","room":)" + RoomJson(room) + "}";
    for (Player* p : room.seats) {
      if (p && p->online) Push(*p, event);
    }
  }

  std::string PeerEvent(const Room& room, const Player& from, const Player& to) const {
    const bool usable = !from.blocked && !to.blocked;
    return R"({"ev":"match.peer","room":)" + Q(room.id) + R"(,"seat":)" + std::to_string(SeatOf(room, from)) + R"(,"player":)" +
           Q(from.code) + R"(,"name":)" + Q(from.name) + R"(,"ip":)" + Q(usable ? "127.0.0.1" : "203.0.113.50") + R"(,"port":)" +
           std::to_string(usable ? from.port : 9) + R"(,"lan":[],"candidates":[],"same_network":)" + (usable ? "true" : "false") +
           R"(,"relay":)" + (from.no_relay ? "false" : "true") + "}";
  }

  // The star: the publisher and the host learn each other, two guests never do.
  void Introduce(const Room& room, Player& publisher) {
    const bool host = room.seats[0] == &publisher;
    for (Player* other : room.seats) {
      if (!other || other == &publisher || (!host && other != room.seats[0]) || !other->port || !other->online) continue;
      if (!publisher.silent && !other->silent) {
        Push(*other, PeerEvent(room, publisher, *other));
        Push(publisher, PeerEvent(room, *other, publisher));
      }
    }
  }

  void Reply(Player& player, int seq, const std::string& fields = {}, const std::string& code = "ok") {
    Push(player, R"({"re":)" + std::to_string(seq) + R"(,"code":)" + Q(code) + fields + "}");
  }

  std::string Entry(const Room& room, int seat, const Player& player) const {
    const std::string relay = relay_offered && !player.no_relay ? R"(,"relay_token":)" + std::to_string(room.token) : "";
    return R"(,"room":)" + RoomJson(room) + R"(,"seat":)" + std::to_string(seat) + relay;
  }

  void CloseRoom(Room& room, const std::string& reason, Player* actor) {
    invites.erase(std::remove_if(invites.begin(), invites.end(), [&](const PendingInvite& i) { return i.room == &room; }), invites.end());
    for (Player* p : room.seats) {
      if (p && p != actor) Push(*p, R"({"ev":"room.closed","room":)" + Q(room.id) + R"(,"reason":)" + Q(reason) + "}");
      if (p) p->port = 0;
    }
    rooms.erase(std::remove_if(rooms.begin(), rooms.end(), [&](const auto& r) { return r.get() == &room; }), rooms.end());
  }

  void FriendsReply(Player& player, int seq) {
    std::string list;
    for (const auto& other : players) {
      if (other.get() == &player) continue;
      list += (list.empty() ? "" : ",") + (R"({"player":)" + Q(other->code) + R"(,"name":)" + Q(other->name) +
                                           R"(,"online":true,"state":"online","platform":"Windows","version":"0.1.0","image":"eck-usa-1db4e96b","build":1,"compatible":true})");
    }
    Reply(player, seq, R"(,"friends":[)" + list + "]");
  }

  void Handle(Player& player, const std::string& text) {
    const std::optional<json::Value> message = json::Parse(text);
    const std::string op = message->Find("op")->AsString();
    const int seq = int(message->Find("seq")->AsInt64().value_or(-1));
    Room* room = RoomOf(player);
    const bool needs_room = op == "invite.send" || op == "match.transport" || op == "match.ready" || op == "match.pick" ||
                            op == "room.start" || op == "room.update";
    if (needs_room && !room) return Reply(player, seq, {}, "not_found");
    if (op == "hello") {
      player.online = true;
      const std::string resume = room ? Q(room->id) : "null";
      Reply(player, seq, R"(,"friend_code":)" + Q(player.code) +
                             R"(,"capabilities":{"protocol_version":1,"relay_supported":true,"relay_max_payload":1472,"public_endpoints":true,"room_reopen":true,"min_players":2,"max_players":4},"resume":)" + resume);
    } else if (op == "friends.list") {
      FriendsReply(player, seq);
    } else if (op == "friends.requests") {
      Reply(player, seq, R"(,"incoming":[],"outgoing":[])");
    } else if (op == "room.host") {
      if (!room) {
        rooms.push_back(std::make_unique<Room>());
        room = rooms.back().get();
        room->id = "00000000000000" + std::to_string(10 + rooms.size());
        room->seats[0] = &player;
        room->rules = RulesText(text);
      }
      Reply(player, seq, Entry(*room, 0, player));
      Push(player, R"({"ev":"room.created")" + Entry(*room, 0, player) + "}");
    } else if (op == "invite.send") {
      const std::string target = message->Find("player")->AsString();
      const int taken = int(std::count_if(room->seats.begin(), room->seats.end(), [](Player* p) { return p != nullptr; }));
      if (taken >= 4) return Reply(player, seq, {}, "room_full");
      const std::string id = "invite" + std::to_string(++next_invite_);
      for (const auto& other : players) {
        if (other->code != target) continue;
        invites.push_back({id, room, other.get()});
        Push(*other, R"({"ev":"invite.received","invite":{"id":)" + Q(id) + R"(,"host":)" + Q(player.code) + R"(,"host_name":)" +
                         Q(player.name) + R"(,"players":1,"capacity":4,"rules":)" + room->rules + R"(,"image":"eck-usa-1db4e96b","build":1,"platform":"Windows","version":"0.1.0","expires":9}})");
      }
      Reply(player, seq, R"(,"invite":)" + Q(id) + R"(,"expires":9)");
    } else if (op == "invite.accept" || op == "invite.decline") {
      const std::string id = message->Find("invite")->AsString();
      const auto found = std::find_if(invites.begin(), invites.end(), [&](const PendingInvite& i) { return i.id == id && i.target == &player; });
      if (found == invites.end()) return Reply(player, seq, {}, "not_found");
      Room* target = found->room;
      invites.erase(found);
      Player& host = *target->seats[0];
      if (op == "invite.decline") {
        Push(host, R"({"ev":"invite.ended","invite":)" + Q(id) + R"(,"room":)" + Q(target->id) + R"(,"reason":"declined"})");
        return Reply(player, seq);
      }
      if (room) CloseRoom(*room, "cancelled", &player);
      const int seat = int(std::find(target->seats.begin(), target->seats.end(), nullptr) - target->seats.begin());
      if (seat == 4) return Reply(player, seq, {}, "room_full");
      target->seats[size_t(seat)] = &player;
      Reply(player, seq, Entry(*target, seat, player));
      Push(host, R"({"ev":"invite.ended","invite":)" + Q(id) + R"(,"room":)" + Q(target->id) + R"(,"reason":"accepted"})");
      Announce(*target);
    } else if (op == "match.endpoint") {
      player.port = uint16_t(message->Find("port")->AsInt64().value_or(0));
      Reply(player, seq);
      if (room) Introduce(*room, player);
    } else if (op == "match.transport") {
      const int seat = int(message->Find("seat")->AsInt64().value_or(-1));
      const std::string kind = message->Find("transport")->AsString();
      Player* guest = room->seats[size_t(seat)];
      if (kind == "relay" && (!guest || guest->no_relay)) return Reply(player, seq, {}, "relay_unavailable");
      room->transport[size_t(seat)] = kind;
      Reply(player, seq);
      Push(*guest, R"({"ev":"match.transport","room":)" + Q(room->id) + R"(,"seat":0,"transport":)" + Q(kind) + "}");
      Announce(*room);
    } else if (op == "match.ready") {
      room->ready[size_t(SeatOf(*room, player))] = message->Find("ready")->AsBool();
      Reply(player, seq);
      bool all = true;
      for (int i = 0; i < 4; ++i) all = all && (!room->seats[size_t(i)] || room->ready[size_t(i)]);
      if (room->state == "starting" && all) room->state = "racing";
      Announce(*room);
    } else if (op == "match.pick") {
      const int mine = SeatOf(*room, player);
      const json::Value* claimed = message->Find("seat");
      if (claimed && int(claimed->AsInt64().value_or(-1)) != mine) return Reply(player, seq, {}, "not_your_seat");
      const int pick = int(message->Find("pick")->AsInt64().value_or(-1));
      const int character = int(message->Find("character")->AsInt64().value_or(-1));
      if (pick < 0 || pick > 2 || (pick != 0 && (character < 0 || character >= online::kCharacters))) {
        return Reply(player, seq, {}, "bad_request");
      }
      room->pick[size_t(mine)] = pick;
      room->character[size_t(mine)] = character;
      Reply(player, seq);
      Announce(*room);
    } else if (op == "room.update") {
      room->rules = RulesText(text);
      Reply(player, seq, R"(,"room":)" + RoomJson(*room));
      Announce(*room);
    } else if (op == "room.reopen") {
      room->rules = RulesText(text);
      room->state = "open";
      room->ready.assign(4, false);
      Reply(player, seq, R"(,"room":)" + RoomJson(*room));
      Announce(*room);
    } else if (op == "room.start") {
      room->state = "starting";
      room->ready.assign(4, false);
      for (const PendingInvite& invite : invites) {
        if (invite.room == room) Push(*invite.target, R"({"ev":"invite.ended","invite":)" + Q(invite.id) + R"(,"room":)" + Q(room->id) + R"(,"reason":"room_started"})");
      }
      invites.erase(std::remove_if(invites.begin(), invites.end(), [&](const PendingInvite& i) { return i.room == room; }), invites.end());
      Reply(player, seq, R"(,"room":)" + RoomJson(*room));
      Announce(*room);
    } else if (op == "room.leave" || op == "match.leave") {
      if (room && room->id == message->Find("room")->AsString()) {
        if (room->seats[0] == &player) {
          CloseRoom(*room, "host_left", &player);
        } else {
          const int seat = SeatOf(*room, player);
          room->seats[size_t(seat)] = nullptr;
          room->ready[size_t(seat)] = false;
          room->transport[size_t(seat)].clear();
          player.port = 0;
          Announce(*room);
        }
      }
      Reply(player, seq);
    } else if (op == "match.resume") {
      if (!room) return Reply(player, seq, {}, "not_found");
      const int seat = SeatOf(*room, player);
      room->away[size_t(seat)] = false;
      player.port = 0;
      Reply(player, seq, Entry(*room, seat, player));
      Announce(*room);
    }
  }

  int next_invite_ = 0;
};

std::string FirstInvite(Player& player) {
  const online::Snapshot s = player.Snap();
  return s.invites.empty() ? std::string() : s.invites.front().id;
}

// A hub with a host in the hidden session it opens from the menu.
struct Party {
  Hub hub;
  Player* host = nullptr;
  std::vector<Player*> guests;

  explicit Party(int friends, const online::RoomRules& rules = {{"laps", 3}, {"items", 1}}) {
    eck::SetActiveUiLanguage("en-US");
    host = &hub.Add("Host");
    for (int i = 0; i < friends; ++i) guests.push_back(&hub.Add("Guest" + std::to_string(i + 1)));
    Check(hub.Until([&] { return hub.AllSigned(); }), "everybody signs in");
    host->service->HostSession(rules);
    Check(hub.Until([&] { return host->Snap().room.has_value(); }), "the hidden session exists without any prompt");
  }

  void Invite(Player& guest) {
    Check(hub.Until([&] { return host->Snap().sending_invite.empty(); }), "the last send is over");
    Check(host->service->SendInvite(guest.code), "the host invites");
    Check(hub.Until([&] { return !FirstInvite(guest).empty() && host->Snap().sending_invite.empty(); }), "the friend receives the invite");
  }

  void Accept(Player& guest) {
    Check(guest.service->AcceptInvite(FirstInvite(guest)), "the friend accepts");
    Check(hub.Until([&] { return guest.Snap().room.has_value() && !guest.Snap().joining; }), "and is seated");
  }

  void Join(Player& guest) {
    Invite(guest);
    Accept(guest);
  }

  bool Settled() {
    return hub.Until([&] {
      for (Player* guest : guests) {
        if (!guest->Snap().paths_ready) return false;
      }
      return host->Snap().paths_ready;
    });
  }
};

void CheckStarRace(Party& party) {
  const int seats = int(party.guests.size()) + 1;
  const online::Snapshot host = party.host->Snap();
  Check(int(host.peers.size()) == seats - 1, "the host has one peer per guest");
  for (Player* guest : party.guests) {
    const online::Snapshot s = guest->Snap();
    Check(s.room && s.room->room == host.room->room, "the guest is in the host's very session");
    Check(s.peers.size() == 1 && s.peers[0].seat == 0, "a guest has only the host as peer: no mesh");
    Check(!guest->Logged("online peer seat=" + std::to_string(s.seat == 1 ? 2 : 1)), "guests never exchange endpoints");
  }
  Check(party.hub.rooms.size() == 1, "one session on the service");
  Check(party.host->service->StartRace(), "the host can start");
  Check(party.hub.Until([&] { return party.host->Snap().room->state == online::RoomState::kRacing; }),
        "every seat reports ready: starting becomes racing");
  const online::Snapshot raced = party.host->Snap();
  Check(std::all_of(raced.room->seats.begin(), raced.room->seats.begin() + seats, [](const online::Seat& s) { return s.ready; }),
        "ready on every seat");
  for (const auto& p : party.hub.players) {
    for (const std::string& line : p->log) Check(line.find(p->token) == std::string::npos, "the token is never logged");
  }
}

void TestTwoPlayers() {
  Party party(1);
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Check(a.Snap().seat == 0 && a.Snap().room->capacity == online::kMaxSeats, "seat 0 of a session of four");
  Check(!a.Snap().room->seats[1].player.size() && a.Sent("room.host") == 1, "created once, alone");
  Check(!a.service->StartRace(), "Start needs somebody to race");
  party.Invite(b);
  Check(a.Snap().invited.size() == 1 && a.Snap().invited[0].name == "Guest1", "the host sees the friend invited");
  party.Accept(b);
  Check(b.Sent("room.host") == 0 && b.Sent("room.search") == 0, "accepting neither creates nor searches");
  Check(b.Snap().seat == 1, "B takes seat 1");
  Check(party.Settled(), "both ends settle");
  Check(a.PeerAt(1)->path == online::PeerPath::kDirect && b.PeerAt(0)->path == online::PeerPath::kDirect, "direct on both ends");
  Check(a.Snap().invited.empty(), "a seated friend is no longer invited");
  Check(a.Logged("[ONLINE] invite send friend=") && b.Logged("[ONLINE] invite received"), "diagnostics: invite");
  Check(b.Logged("[ONLINE] invite accepted") && b.Logged("[ONLINE] session joined seat=1"), "diagnostics: join");
  Check(a.Logged("[ONLINE] endpoint seat=0 public=no lan=0"), "diagnostics: endpoint");
  Check(a.Logged("[ONLINE] peer seat=1 probing") && a.Logged("[ONLINE] peer seat=1 transport=direct"), "diagnostics: path");
  Check(b.Logged("invite transition consumed generation=1"), "the accepted invite is consumed once");
  CheckStarRace(party);
  Check(a.Logged("[ONLINE] peer seat=1 ready=yes"), "diagnostics: ready");
}

const eck::lobby::GameSetup kHostSetup{eck::lobby::kGameTypeStartMenu, 4, 2};

// Accept in the real service, then the guest's own flow on what the service delivered.
void TestGuestEntersTheHostsGame() {
  namespace lobby = eck::lobby;
  Party party(1, lobby::SetupRules(kHostSetup));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  lobby::LobbyFlow flow;
  Check(flow.Update(b.Snap(), eck::menu::GameScreen::kMainMenu, true, false).travel.kind == lobby::TravelKind::kNone,
        "no travel before the accept");
  party.Join(b);
  const online::Snapshot joined = b.Snap();
  Check(joined.seat == 1, "guest joins seat 1");
  const auto setup = lobby::SetupOfRules(joined.room->rules);
  Check(setup && setup->mode == kHostSetup.mode, "guest competition matches host");
  Check(setup && setup->difficulty == kHostSetup.difficulty, "guest difficulty matches host");
  Check(setup && setup->game_type == kHostSetup.game_type, "guest game type matches host");

  const lobby::FlowResult first = flow.Update(joined, eck::menu::GameScreen::kMainMenu, true, false);
  Check(first.travel.kind == lobby::TravelKind::kEntry, "guest travels from the main menu");
  const std::string command = lobby::TravelCommand(first.travel, 0);
  Check(command.find("GoBackType=2") != std::string::npos, "through the game's own Back to Character Select");
  Check(command.find("NumGameMode=4") != std::string::npos && command.find("KartAIDiffculty=2") != std::string::npos,
        "with the host's competition and difficulty");
  Check(!first.leave, "without leaving the session");
  flow.Update(joined, eck::menu::GameScreen::kCompetition, true, false);
  flow.Update(joined, eck::menu::GameScreen::kCharacter, true, false);
  Check(flow.in_lobby(), "guest enters PlayerSelectManager without touching CompetitionSelect");
  Check(b.Sent("room.host") == 0 && b.Sent("room.search") == 0, "guest does not create another session");
  Check(party.hub.rooms.size() == 1, "one session on the service");

  Check(party.Settled(), "the path comes up while the guest travels");
  Check(a.PeerAt(1)->path == online::PeerPath::kDirect && b.PeerAt(0)->path == online::PeerPath::kDirect, "Direct preserved");
}

// The whole host side with what the movies committed and nothing read from any screen.
void TestCapturedSetupInvites() {
  namespace lobby = eck::lobby;
  lobby::SetupCapture setup;
  setup.NoteMovieCall(lobby::kSelectMenu, std::nullopt, lobby::kFreeRaceMenu);
  setup.NoteMovieCall(lobby::kSelectCompetition, 5, "");
  setup.NoteMovieCall(lobby::kSetDifficulty, 1, "");
  const auto game = setup.setup();
  Check(game.has_value(), "the setup is complete with no panel bridge");
  if (!game) return;
  Party party(1, lobby::SetupRules(*game));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Check(a.Logged("session host requested") && a.Snap().room.has_value(), "selecting a friend has a hidden session");
  party.Invite(b);
  Check(a.Logged("[ONLINE] invite send friend="), "the invite is sent");
  const auto invited = lobby::SetupOfRules(b.Snap().invites.at(0).rules);
  Check(invited && invited->mode == 5 && invited->difficulty == 1, "the friend's invite carries the captured setup");
  party.Accept(b);
  const auto seated = lobby::SetupOfRules(b.Snap().room->rules);
  Check(seated && seated->mode == 5 && seated->difficulty == 1 && seated->game_type == lobby::kGameTypeStartMenu,
        "and so does the session the guest joins");
  Check(party.Settled(), "the network comes up as before");
}

constexpr char kFinalUrl[] =
    "start PistaBrasilM_Circuito0?CharArray=1-5-9-10-0-4-11-7?IsBotArray=0-0-1-1-1-1-1-1?PlayersArray=0-1-99-99-99-99-99-99"
    "?TeamData=0-1-0-1-0-1-0-1-?PrimaryControllerID=0?NumSelectedTrack=0?NumHumanKarts=2?TotalKarts=8?KartAIDiffculty=0?GameType0";

eck::lobby::CanonicalRace FinalRace(int humans) {
  eck::lobby::CanonicalRace race = *eck::lobby::CanonicalOfUrl(*eck::race_url::ParseRaceUrl(kFinalUrl), {0, 1, 2, 3}, 1).race;
  race.humans = humans;
  race.bots = 0xFF & ~((1u << humans) - 1);
  for (int seat = 0; seat < humans; ++seat) race.seats[size_t(seat)] = seat;
  return race;
}

// The host's whole start through the real service: published, every seat ready, every PC told to travel.
void StartRaceThrough(Party& party, const eck::lobby::CanonicalRace& race, std::vector<eck::lobby::LobbyFlow>& guests) {
  namespace lobby = eck::lobby;
  Player& a = *party.host;
  lobby::HostRaceStart start;
  start.Begin(party.hub.now);
  bool committed = false;
  Check(party.hub.Until([&] {
    const online::Snapshot state = a.Snap();
    const lobby::RaceStep step = start.Update(state, party.hub.now);
    if (step.publish && a.service->PublishRace(lobby::RaceRules(state.room->rules, race))) start.NotePublished();
    committed = committed || step.commit;
    return committed;
  }), "every seat ready: the host commits");
  for (size_t i = 0; i < party.guests.size(); ++i) {
    const online::Snapshot state = party.guests[i]->Snap();
    Check(state.room->state == online::RoomState::kRacing, "the room is racing for the guest");
    const lobby::FlowResult flow = guests[i].Update(state, eck::menu::GameScreen::kCharacter, true, false);
    Check(flow.travel.kind == lobby::TravelKind::kRace && !flow.leave, "the guest travels without leaving");
    const std::string command = lobby::TravelCommand(flow.travel, 0);
    Check(command.find("StartMenu") == std::string::npos, "and not to StartMenu");
    Check(command.find("CharArray=1-5-9-10-0-4-11-7") != std::string::npos, "with the host's characters");
    Check(*lobby::SlotOfSeat(flow.travel.race, state.seat) == state.seat, "in its own kart slot");
  }
}

void CheckSessionAlive(Party& party) {
  Player& a = *party.host;
  for (int step = 0; step < 100; ++step) party.hub.Until([] { return false; }, 0.02);
  Check(a.Snap().room.has_value() && a.Sent("room.leave") == 0, "the host never left");
  Check(a.Snap().closed_reason.empty(), "no host_left");
  for (Player* guest : party.guests) {
    const online::Snapshot state = guest->Snap();
    Check(state.room.has_value() && state.room->room == a.Snap().room->room, "the guest keeps its seat across the travel");
    Check(state.closed_reason.empty() && guest->Sent("room.leave") == 0, "and never saw the host leave");
    Check(state.peers.size() == 1 && state.peers[0].connected, "the path to the host stays");
  }
  Check(party.hub.rooms.size() == 1, "one session on the service");
}

void TestRaceKeepsTheSession() {
  namespace lobby = eck::lobby;
  for (const int humans : {2, 3, 4}) {
    Party party(humans - 1, lobby::SetupRules({lobby::kGameTypeStartMenu, 0, 0}));
    for (Player* guest : party.guests) party.Join(*guest);
    Check(party.Settled(), "everybody connected");
    std::vector<lobby::LobbyFlow> flows(party.guests.size());
    StartRaceThrough(party, FinalRace(humans), flows);
    CheckSessionAlive(party);
    Check(party.host->PeerAt(1)->path == online::PeerPath::kDirect, "Direct remains assigned during loading");
    Check(party.host->Snap().peers.size() == size_t(humans - 1), "one path per remote seat");
    for (Player* guest : party.guests) {
      Check(guest->Snap().peers[0].seat == 0, "a guest only has the host: no mesh");
      Check(!guest->service->PublishRace({}), "a guest cannot start a race");
    }
    Check(!party.host->service->PublishRace({}), "and the same room does not start twice");
  }
}

void TestRaceOverRelay() {
  namespace lobby = eck::lobby;
  Party party(1, lobby::SetupRules({lobby::kGameTypeStartMenu, 0, 0}));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  b.blocked = true;
  party.Join(b);
  Check(party.Settled(), "the seat settles on the relay");
  std::vector<lobby::LobbyFlow> flows(1);
  StartRaceThrough(party, FinalRace(2), flows);
  CheckSessionAlive(party);
  Check(a.PeerAt(1)->path == online::PeerPath::kRelay, "Relay remains assigned during loading");
}

void TestLeavingARace() {
  namespace lobby = eck::lobby;
  {
    Party party(1, lobby::SetupRules({lobby::kGameTypeStartMenu, 0, 0}));
    Player& a = *party.host;
    Player& b = *party.guests[0];
    party.Join(b);
    Check(party.Settled(), "connected");
    std::vector<lobby::LobbyFlow> flows(1);
    StartRaceThrough(party, FinalRace(2), flows);
    a.service->LeaveSession();
    Check(party.hub.Until([&] { return !b.Snap().room.has_value(); }), "a host who really leaves ends the session");
    Check(b.Snap().closed_reason == "host_left", "and that alone is host_left");
    Check(flows[0].Update(b.Snap(), eck::menu::GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu,
          "a racing guest is taken out of the race");
  }
  {
    Party party(2, lobby::SetupRules({lobby::kGameTypeStartMenu, 0, 0}));
    Player& a = *party.host;
    party.Join(*party.guests[0]);
    party.Join(*party.guests[1]);
    Check(party.Settled(), "connected");
    std::vector<lobby::LobbyFlow> flows(2);
    StartRaceThrough(party, FinalRace(3), flows);
    party.guests[0]->service->LeaveSession();
    Check(party.hub.Until([&] { return a.Snap().room->players == 2; }), "a guest who leaves frees only their seat");
    Check(party.guests[1]->Snap().room.has_value() && a.Snap().closed_reason.empty(), "the race goes on for the others");
  }
}

void TestPicksReplicate() {
  namespace lobby = eck::lobby;
  using online::PickState;
  Party party(2, lobby::SetupRules(kHostSetup));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Player& c = *party.guests[1];
  party.Join(b);
  party.Join(c);
  Check(party.Settled(), "everybody connected");

  const auto seat_of = [](const Player& p, int seat) { return p.Snap().room->seats[size_t(seat)]; };
  Check(seat_of(a, 1).pick == PickState::kNone, "a seat starts without a pick");

  a.service->SendPick(PickState::kConfirmed, 3);
  Check(party.hub.Until([&] { return seat_of(b, 0).pick == PickState::kConfirmed && seat_of(b, 0).character == 3; }),
        "host character replicated to guest");
  b.service->SendPick(PickState::kSelecting, 5);
  Check(party.hub.Until([&] { return seat_of(a, 1).pick == PickState::kSelecting && seat_of(a, 1).character == 5; }),
        "guest selecting replicated to host");
  b.service->SendPick(PickState::kConfirmed, 5);
  Check(party.hub.Until([&] { return seat_of(a, 1).pick == PickState::kConfirmed && seat_of(c, 1).pick == PickState::kConfirmed; }),
        "guest character replicated to host and to the other guest");
  b.service->SendPick(PickState::kSelecting, 6);
  Check(party.hub.Until([&] { return seat_of(a, 1).pick == PickState::kSelecting && seat_of(a, 1).character == 6; }),
        "backing out of a choice replicates");
  Check(seat_of(a, 0).character == 3 && seat_of(a, 0).pick == PickState::kConfirmed, "a guest's pick never moves the host's seat");

  const int picks = b.Sent("match.pick");
  b.service->SendPick(PickState::kSelecting, 6);
  Check(b.Sent("match.pick") == picks, "the same pick is not sent twice");

  Check(party.hub.Until([&] { return a.PeerAt(1)->path == online::PeerPath::kDirect; }), "picks do not disturb the path");

  // A spoofed or invalid message is the hub's to refuse: nothing about the seats changes.
  party.hub.Deliver(b, R"({"op":"match.pick","seq":90,"room":"x","pick":2,"character":1,"seat":0})");
  party.hub.Deliver(b, R"({"op":"match.pick","seq":91,"room":"x","pick":2,"character":12})");
  party.hub.Deliver(b, R"({"op":"match.pick","seq":92,"room":"x","pick":7,"character":1})");
  party.hub.Until([] { return false; }, 0.2);
  Check(seat_of(a, 0).character == 3 && seat_of(a, 1).character == 6 && seat_of(a, 1).pick == PickState::kSelecting,
        "seat spoof, invalid character and invalid state change nothing");

  b.service->LeaveSession();
  Check(party.hub.Until([&] { return seat_of(a, 1).state == online::SeatState::kEmpty; }), "guest leave is clean");
  Check(a.Snap().room.has_value() && a.Snap().seat == 0, "the host stays when others remain");
  a.service->LeaveSession();
  Check(party.hub.Until([&] { return !c.Snap().room.has_value(); }), "host leave ends the session for the guests");
  Check(!c.Snap().room, "the guest has no room to read picks from");
}

void TestPicksOverRelay() {
  namespace lobby = eck::lobby;
  Party party(1, lobby::SetupRules(kHostSetup));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  b.blocked = true;
  party.Join(b);
  Check(party.Settled(), "the seat settles on the relay");
  Check(a.PeerAt(1)->path == online::PeerPath::kRelay, "Relay preserved");
  b.service->SendPick(online::PickState::kConfirmed, 9);
  Check(party.hub.Until([&] { return a.Snap().room->seats[1].character == 9; }), "a pick needs no UDP path");
  Check(a.PeerAt(1)->path == online::PeerPath::kRelay, "and leaves the relay as it was");
}

void TestThreePlayers() {
  Party party(2);
  party.Join(*party.guests[0]);
  party.Join(*party.guests[1]);
  Check(party.Settled(), "three players settle");
  Check(party.guests[0]->Snap().seat == 1 && party.guests[1]->Snap().seat == 2, "seats 1 and 2");
  Check(party.host->PeerAt(1) && party.host->PeerAt(2), "each remote seat has its own state");
  CheckStarRace(party);
}

void TestFourPlayers() {
  Party party(4);
  Player& fifth = *party.guests.back();
  party.guests.pop_back();
  for (Player* guest : party.guests) party.Join(*guest);
  Check(party.Settled(), "four players settle");
  Check(party.host->Snap().room->players == 4, "the session is full");
  Check(!party.host->service->SendInvite(fifth.code), "a fifth player cannot be invited");
  Check(FirstInvite(fifth).empty(), "and hears nothing");
  CheckStarRace(party);
}

void TestStartWithAnInvitePending() {
  Party party(2);
  Player& late = *party.guests.back();
  party.guests.pop_back();
  party.Join(*party.guests[0]);
  Check(party.Settled(), "two players settle");
  party.Invite(late);
  Check(party.host->Snap().invited.size() == 1 && party.host->Snap().paths_ready, "an unanswered invite does not hold the race");
  CheckStarRace(party);
  const online::Snapshot host = party.host->Snap();
  Check(host.room->players == 2 && host.invited.empty(), "the race is the two connected players");
  Check(party.hub.Until([&] { return FirstInvite(late).empty(); }), "the third friend's invite is cancelled");
  Check(party.hub.invites.empty(), "and the service has none left");
}

void TestPendingInvitesUseUpSeats() {
  Party party(4);
  Player& extra = *party.guests.back();
  party.guests.pop_back();
  for (Player* guest : party.guests) party.Invite(*guest);
  Check(party.host->Snap().invited.size() == 3, "three invites fill the session");
  Check(!party.host->service->SendInvite(extra.code), "a fourth is refused while they are unanswered");
  party.guests[0]->service->DeclineInvite(FirstInvite(*party.guests[0]));
  Check(party.hub.Until([&] { return party.host->Snap().invited.size() == 2; }), "a declined invite ends");
  Check(party.host->service->SendInvite(extra.code), "and gives its seat back");
  Check(party.hub.Until([&] { return party.host->Snap().invited.empty(); }, 70.0), "unanswered invites expire on their own");
}

void TestSeatsFollowAcceptOrder() {
  Party party(3);
  Player& b = *party.guests[0];
  Player& c = *party.guests[1];
  Player& d = *party.guests[2];
  for (Player* guest : party.guests) party.Invite(*guest);
  party.Accept(c);
  party.Accept(b);
  party.Accept(d);
  Check(c.Snap().seat == 1 && b.Snap().seat == 2 && d.Snap().seat == 3, "seats follow acceptance, not invitation");
  Check(party.Settled(), "and every seat settles");
}

void TestMixedTransports() {
  Party party(3);
  party.guests[1]->blocked = true;
  for (Player* guest : party.guests) party.Join(*guest);
  Check(party.Settled(), "a blocked seat still settles, through the relay");
  Check(party.host->PeerAt(1)->path == online::PeerPath::kDirect, "seat 1 direct");
  Check(party.host->PeerAt(2)->path == online::PeerPath::kRelay, "seat 2 relay");
  Check(party.host->PeerAt(3)->path == online::PeerPath::kDirect, "seat 3 direct");
  Check(party.guests[1]->PeerAt(0)->path == online::PeerPath::kRelay, "the relayed guest follows the host");
  Check(party.guests[0]->PeerAt(0)->path == online::PeerPath::kDirect, "the others stay direct: one relay forces nobody");
  Check(party.host->socket.relay_selected(2) && !party.host->socket.relay_selected(1), "the socket relays that seat alone");
}

void TestRelayWithoutEndpoint() {
  Party party(1);
  Player& b = *party.guests[0];
  b.silent = true;
  party.Join(b);
  Check(party.Settled(), "an endpoint that never arrives still ends in the relay, as in Street");
  Check(party.host->PeerAt(1)->path == online::PeerPath::kRelay && b.PeerAt(0)->path == online::PeerPath::kRelay, "both on the relay");
  Check(party.host->Logged("peer seat=1 transport=relay"), "the host decided");
}

void TestNoPathEndsCleanly() {
  Party party(1);
  Player& b = *party.guests[0];
  b.blocked = true;
  b.no_relay = true;
  party.Join(b);
  const double started = party.hub.now;
  Check(party.hub.Until([&] { return !b.Snap().room.has_value(); }, 20.0), "no path never means Connecting forever");
  Check(party.hub.now - started <= online::kEstablishmentBudgetSeconds + 1.0, "it ends inside the budget");
  Check(b.Snap().closed_reason == "no_path", "the guest knows why");
  Check(party.hub.Until([&] { return party.host->Snap().room->players == 1 && party.host->Snap().peers.empty(); }), "the seat is free again");

  b.blocked = false;
  b.no_relay = false;
  party.Invite(b);
  party.Accept(b);
  Check(party.Settled(), "a new invite works without restarting");
  Check(b.Snap().closed_reason.empty(), "the failure is forgotten");
}

void TestRelayRefusedFailsThatSeatOnly() {
  Party party(2);
  party.guests[0]->blocked = true;
  party.guests[0]->no_relay = true;
  party.Join(*party.guests[1]);
  party.Join(*party.guests[0]);
  Check(party.hub.Until([&] {
    const online::PeerSnapshot* peer = party.host->PeerAt(2);
    return !peer || peer->path == online::PeerPath::kFailed;
  }, 20.0), "the unreachable seat ends");
  Check(party.host->PeerAt(1)->path == online::PeerPath::kDirect, "the other seat is untouched");
}

void TestLateSeatInAnIdleSession() {
  Party party(2);
  Player& b = *party.guests[0];
  party.Join(b);
  Check(party.hub.Until([&] { return b.Snap().paths_ready; }), "the first guest is up");
  const int published = party.host->Sent("match.endpoint");
  for (int i = 0; i < 1250; ++i) party.hub.Step();
  party.Join(*party.guests[1]);
  Check(party.Settled(), "a guest who joins much later settles too");
  Check(party.host->Sent("match.endpoint") > published, "the host looks up its mapping again for the newcomer");
  Check(party.host->PeerAt(1)->path == online::PeerPath::kDirect && b.PeerAt(0)->path == online::PeerPath::kDirect,
        "the first guest stays connected");
}

void TestResumeKeepsDirect() {
  Party party(2);
  party.Join(*party.guests[0]);
  party.Join(*party.guests[1]);
  Check(party.Settled(), "settled before the drop");
  Player& b = *party.guests[0];
  const std::string session = b.Snap().room->room;
  party.hub.Drop(b);
  Check(party.hub.Until([&] { return !b.service->SignedIn(); }), "B loses the service");
  Check(b.Snap().reconnecting && b.Snap().room.has_value(), "the seat is kept meanwhile");
  Check(b.PeerAt(0)->path == online::PeerPath::kDirect, "the direct path does not die with the WebSocket");
  Check(party.hub.Until([&] { return !party.host->PeerAt(1)->connected; }), "the host sees the seat reconnecting");
  Check(party.hub.Until([&] { return b.service->SignedIn() && b.Sent("match.resume") == 1; }, 10.0), "B signs in and resumes");
  Check(party.hub.Until([&] { return !b.Snap().reconnecting && party.host->PeerAt(1)->connected; }), "the seat is back");
  Check(b.Snap().seat == 1 && b.Snap().room->room == session, "same seat, same session");
  Check(b.PeerAt(0)->path == online::PeerPath::kDirect && party.host->PeerAt(1)->path == online::PeerPath::kDirect, "still direct");
  Check(party.host->PeerAt(2)->path == online::PeerPath::kDirect, "the other seat never noticed");
  Check(b.Sent("match.endpoint") >= 2, "the endpoint is published again");
}

void TestRelayedSeatRecovers() {
  Party party(1);
  Player& b = *party.guests[0];
  b.blocked = true;
  party.Join(b);
  Check(party.Settled(), "relayed");
  party.hub.Drop(b);
  Check(party.hub.Until([&] { return !b.service->SignedIn(); }), "the relay's link is gone");
  Check(party.hub.Until([&] { return b.service->SignedIn() && !b.Snap().reconnecting; }, 10.0), "B returns");
  Check(party.hub.Until([&] { return b.PeerAt(0)->path == online::PeerPath::kRelay; }), "and follows the relay again");
  Check(b.socket.relay_selected(0), "on the new connection");
}

void TestServiceGoneEndsTheSession() {
  Party relayed(1);
  Player& b = *relayed.guests[0];
  b.blocked = true;
  relayed.Join(b);
  Check(relayed.Settled(), "relayed");
  b.unreachable = true;
  relayed.hub.Drop(b);
  const double started = relayed.hub.now;
  Check(relayed.hub.Until([&] { return !b.Snap().room.has_value(); }, 40.0), "a relayed seat does not wait for the service forever");
  Check(relayed.hub.now - started <= 31.0 && b.Snap().closed_reason == "connection_lost", "it ends with the service's own hold");

  Party direct(1);
  Player& c = *direct.guests[0];
  direct.Join(c);
  Check(direct.Settled(), "direct");
  c.unreachable = true;
  direct.hub.Drop(c);
  for (int i = 0; i < 1800; ++i) direct.hub.Step();
  Check(c.Snap().room.has_value() && c.PeerAt(0)->path == online::PeerPath::kDirect, "a direct seat outlives the service");
}

void TestOneShotInvite() {
  Party party(1);
  Player& b = *party.guests[0];
  party.Invite(b);
  const std::string id = FirstInvite(b);
  Check(b.service->AcceptInvite(id), "the first accept counts");
  Check(!b.service->AcceptInvite(id), "the same invite cannot be accepted again");
  for (int i = 0; i < 20; ++i) party.hub.Step();
  Check(b.Sent("invite.accept") == 1, "one accepted invite is one request");
  Check(b.Snap().accepted_generation == 1, "and one generation");
  Check(b.Snap().room && b.Snap().seat == 1, "and one join");
}

void TestInviteWhileAloneInOwnSession() {
  Party party(1);
  Player& b = *party.guests[0];
  b.service->HostSession({{"laps", 3}});
  Check(party.hub.Until([&] { return b.Snap().room.has_value(); }), "B sits in its own hidden session");
  const std::string own = b.Snap().room->room;
  party.Invite(b);
  party.Accept(b);
  Check(b.Snap().room->room != own && b.Snap().seat == 1, "accepting moves B into the host's session");
  Check(!b.Snap().hosting, "and B no longer hosts");
  Check(party.hub.rooms.size() == 1, "its own hidden session is gone");
  Check(party.Settled(), "and the connection comes up");
}

// Both players sent a first invite, so both host. `b` accepts `a`'s.
void TestDualHost(bool first_wins) {
  Hub hub;
  Player& one = hub.Add("One");
  Player& two = hub.Add("Two");
  Player& a = first_wins ? one : two;
  Player& b = first_wins ? two : one;
  Check(hub.Until([&] { return hub.AllSigned(); }), "both sign in");
  Check(hub.rooms.empty() && !a.Snap().room && !b.Snap().room, "signing in creates no session");

  Check(a.service->SendInvite(b.code, online::RoomRules{{"laps", 3}}), "A sends a first invite");
  Check(hub.Until([&] { return a.Snap().room.has_value() && !FirstInvite(b).empty(); }), "A hosts and B has the invite");
  const std::string invite_of_a = FirstInvite(b);
  Check(b.service->SendInvite(a.code, online::RoomRules{{"laps", 5}}), "B sends one before answering");
  Check(hub.Until([&] { return b.Snap().room.has_value() && !FirstInvite(a).empty(); }), "so B hosts too");
  Check(hub.rooms.size() == 2 && a.Snap().seat == 0 && b.Snap().seat == 0, "two hosts, each on seat 0");

  const std::string own = b.Snap().room->room;
  const int generation = b.Snap().session_generation;
  Check(b.service->AcceptInvite(invite_of_a), "B accepts A");
  const online::Snapshot moving = b.Snap();
  Check(!moving.room && moving.seat == -1 && !moving.hosting && moving.invited.empty(), "B's old room is gone at once");
  Check(moving.session_generation > generation, "and void");

  Check(hub.Until([&] { return b.Snap().room.has_value() && !b.Snap().joining; }), "B is seated");
  const online::Snapshot seated = b.Snap();
  Check(seated.room->room != own && seated.room->room == a.Snap().room->room, "in A's room");
  Check(seated.seat == 1 && a.Snap().seat == 0, "B is Player 2, A stays Player 1");
  Check(hub.rooms.size() == 1, "B's room is destroyed");
  Check(seated.peers.size() == 1 && seated.peers[0].seat == 0, "B knows only the seats of A's room");
  const eck::lobby::Plan plan = eck::lobby::MakePlan(seated, true, {});
  Check(plan.local_seat == 1 && eck::lobby::LocalPanel(plan, 0) == 1, "the panels are rebuilt on seat 1");
  Check(hub.Until([&] { return b.Snap().paths_ready; }), "and the connection comes up");
  Check(a.Snap().paths_ready, "on both sides");
}

void TestRoomHostRepliesOnce() {
  Hub hub;
  Player& a = hub.Add("Host");
  Player& b = hub.Add("Friend");
  Check(hub.Until([&] { return hub.AllSigned(); }), "signed in");
  for (int i = 0; i < 5; ++i) a.service->SendInvite(b.code, online::RoomRules{{"laps", 3}});
  Check(hub.Until([&] { return !FirstInvite(b).empty() && a.Snap().sending_invite.empty(); }), "rapid presses make one send");
  Check(a.Sent("room.host") == 1 && a.Sent("invite.send") == 1, "one room and one invite");
  Check(hub.rooms.size() == 1 && a.Snap().invited.size() == 1, "and one seat held");
}

namespace lobby = eck::lobby;

void TestContinueAfterTheRace() {
  using eck::menu::GameScreen;
  Party party(2, lobby::SetupRules({lobby::kGameTypeStartMenu, 1, 0}));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Player& c = *party.guests[1];
  party.Join(b);
  party.Join(c);
  Check(party.Settled(), "three players");
  std::vector<lobby::LobbyFlow> flows(2);
  StartRaceThrough(party, FinalRace(3), flows);
  flows[0].Update(b.Snap(), GameScreen::kOther, false, false);
  flows[1].Update(c.Snap(), GameScreen::kOther, false, false);

  // Sitting on the results for a minute ends nothing.
  const int generation = b.Snap().session_generation;
  for (int i = 0; i < 3600; ++i) party.hub.Step();
  Check(b.Snap().room.has_value() && b.Snap().session_generation == generation, "the room survives the wait");
  Check(flows[0].Update(b.Snap(), GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "and moves nobody");

  // One guest leaves from the results; the host and the other guest are unmoved.
  Check(c.service->LeaveSession("guest_menu"), "C leaves from the results");
  Check(party.hub.Until([&] { return a.Snap().room->seats[2].state == online::SeatState::kEmpty; }), "the host sees it");
  Check(a.Snap().seat == 0 && b.Snap().room.has_value(), "and stays, with B");

  const lobby::AfterRaceStep step{1, lobby::AfterRace::kCharacterSelect};
  Check(a.service->ReopenRoom(lobby::AfterRaceRules(a.Snap().room->rules, step)), "the host continues");
  Check(!a.service->ReopenRoom(lobby::AfterRaceRules(a.Snap().room->rules, step)), "once");
  Check(party.hub.Until([&] { return b.Snap().room->state == online::RoomState::kOpen; }), "the guest hears over the service");
  Check(a.Sent("room.reopen") == 1, "from one event");
  const lobby::FlowResult follow = flows[0].Update(b.Snap(), GameScreen::kOther, false, false);
  Check(follow.travel.kind == lobby::TravelKind::kEntry, "and follows");
  Check(flows[0].Update(b.Snap(), GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "once");
  Check(b.Snap().session_generation == generation && b.PeerAt(0)->path == online::PeerPath::kDirect, "still the same session, on the same path");

  // And the room can race again.
  const eck::lobby::CanonicalRace again = [] {
    eck::lobby::CanonicalRace race = FinalRace(2);
    race.generation = 2;
    return race;
  }();
  lobby::HostRaceStart start;
  start.Begin(party.hub.now);
  bool committed = false;
  Check(party.hub.Until([&] {
    const online::Snapshot state = a.Snap();
    const lobby::RaceStep next = start.Update(state, party.hub.now);
    if (next.publish && a.service->PublishRace(lobby::RaceRules(lobby::BaseRules(state.room->rules), again))) start.NotePublished();
    committed = committed || next.commit;
    return committed;
  }), "the next race starts");
  const lobby::FlowResult raced = flows[0].Update(b.Snap(), GameScreen::kCharacter, true, false);
  Check(raced.travel.kind == lobby::TravelKind::kRace && raced.travel.race.generation == 2, "and the guest goes");
}

void TestHostLeavesFromTheResults() {
  using eck::menu::GameScreen;
  Party party(1, lobby::SetupRules({lobby::kGameTypeStartMenu, 1, 0}));
  Player& a = *party.host;
  Player& b = *party.guests[0];
  party.Join(b);
  Check(party.Settled(), "connected");
  std::vector<lobby::LobbyFlow> flows(1);
  StartRaceThrough(party, FinalRace(2), flows);
  flows[0].Update(b.Snap(), GameScreen::kOther, false, false);
  a.service->LeaveSession("host_menu");
  Check(party.hub.Until([&] { return !b.Snap().room.has_value(); }), "the guest hears");
  Check(b.Snap().closed_reason == "host_left", "that the host left");
  Check(flows[0].Update(b.Snap(), GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu, "and goes home");
}

void TestGuestLeavesTheRace() {
  Party party(2);
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Player& c = *party.guests[1];
  party.Join(b);
  party.Join(c);
  Check(party.Settled(), "three players");
  const int generation = a.Snap().session_generation;
  Check(b.service->LeaveSession("guest_menu"), "B leaves on purpose");
  Check(party.hub.Until([&] { return a.Snap().room->seats[1].state == online::SeatState::kEmpty; }), "the host sees the seat free");
  Check(a.Snap().seat == 0 && a.Snap().session_generation == generation, "the host stays");
  Check(c.Snap().room.has_value() && c.Snap().seat == 2, "C stays, in the same seat");
  Check(a.Snap().closed_reason.empty() && c.Snap().closed_reason.empty(), "nobody hears the host left");
  Check(party.hub.Until([&] { return a.PeerAt(1) == nullptr || !a.PeerAt(1)->connected; }), "and the host has no path to B");
  Check(a.Sent("room.leave") == 0 && b.Sent("room.leave") == 1, "one leave, from B");
  Check(!b.Snap().room && b.Snap().seat == -1, "B is idle");
}

void TestHostLeavesTheRace() {
  Party party(2);
  Player& a = *party.host;
  Player& b = *party.guests[0];
  Player& c = *party.guests[1];
  party.Join(b);
  party.Join(c);
  Check(party.Settled(), "three players");
  lobby::LobbyFlow b_flow, c_flow;
  b_flow.Update(b.Snap(), eck::menu::GameScreen::kOther, false, false);
  c_flow.Update(c.Snap(), eck::menu::GameScreen::kOther, false, false);
  a.service->LeaveSession("host_menu");
  Check(party.hub.Until([&] { return !b.Snap().room && !c.Snap().room; }), "every guest hears at once");
  Check(b.Snap().closed_reason == "host_left" && c.Snap().closed_reason == "host_left", "that the host left");
  Check(party.hub.rooms.empty(), "the room is gone");
  Check(b_flow.Update(b.Snap(), eck::menu::GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu &&
            c_flow.Update(c.Snap(), eck::menu::GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu,
        "and both go home");
}

void TestHardClose() {
  // The window is gone with no leave sent: only the socket says so.
  Party host_dies(1);
  Player& a = *host_dies.host;
  Player& b = *host_dies.guests[0];
  host_dies.Join(b);
  Check(host_dies.Settled(), "a race");
  a.unreachable = true;
  host_dies.hub.Drop(a);
  Check(host_dies.hub.Until([&] { return b.Snap().room->seats[0].state == online::SeatState::kReconnecting; }), "the guest sees the host's seat held");
  Check(b.Snap().room.has_value(), "which is not the host leaving yet");
  Check(lobby::WatchRace(b.Snap(), 1.0) == lobby::RaceWatch::kFine, "while anything could still come back");
  Check(lobby::WatchRace(b.Snap(), lobby::kPeerGoneSeconds) == lobby::RaceWatch::kHostLeft, "the silence settles it first");
  host_dies.hub.Expire(a);
  Check(host_dies.hub.Until([&] { return !b.Snap().room.has_value(); }), "or the service does");
  Check(b.Snap().closed_reason == "disconnect", "as a departure of the host's");
  Check(host_dies.hub.rooms.empty(), "and no session stays behind");

  Party guest_dies(2);
  Player& host = *guest_dies.host;
  Player& gone = *guest_dies.guests[0];
  Player& other = *guest_dies.guests[1];
  guest_dies.Join(gone);
  guest_dies.Join(other);
  Check(guest_dies.Settled(), "three players");
  gone.unreachable = true;
  guest_dies.hub.Drop(gone);
  guest_dies.hub.Expire(gone);
  Check(guest_dies.hub.Until([&] { return host.Snap().room->seats[1].state == online::SeatState::kEmpty; }), "the seat is removed");
  Check(host.Snap().room.has_value() && host.Snap().seat == 0 && host.Snap().closed_reason.empty(), "the host stays");
  Check(other.Snap().room.has_value() && other.Snap().closed_reason.empty(), "and so does the other guest");
}

void TestDeclineAndRefusal() {
  Party party(2);
  Player& b = *party.guests[0];
  party.Invite(b);
  b.service->DeclineInvite(FirstInvite(b));
  Check(party.hub.Until([&] { return party.host->Snap().invited.empty(); }), "the host hears the decline");
  const online::InviteOutcome outcome = party.host->Snap().invite_outcome;
  Check(outcome.generation == 1 && outcome.declined && outcome.name == "Guest1", "and who declined");
  Check(party.host->service->SendInvite(b.code), "the friend can be invited again");
  Check(!party.host->service->SendInvite(b.code), "but not twice at once");
  Check(party.hub.Until([&] { return party.host->Snap().invited.size() == 1; }), "the service took it");
  Check(!party.host->service->SendInvite(b.code), "nor again while it is out");
}

void TestNewSessionAfterTheLastOne() {
  Party party(1);
  Player& a = *party.host;
  Player& b = *party.guests[0];
  party.Join(b);
  Check(party.Settled(), "first session up");
  a.service->LeaveSession();
  Check(!a.Snap().hosting && !a.Snap().room, "the host left");
  Check(party.hub.Until([&] { return !b.Snap().room.has_value(); }), "the guest is told");
  Check(b.Snap().closed_reason == "host_left", "and why");
  a.service->HostSession({{"laps", 3}});
  Check(party.hub.Until([&] { return a.Snap().room.has_value(); }), "a new hidden session");
  party.Join(b);
  Check(party.Settled(), "a new invite after the session ended works");
}

void TestLeftBeforeTheServiceAnswered() {
  Hub hub;
  Player& a = hub.Add("Host");
  Check(hub.Until([&] { return hub.AllSigned(); }), "signed in");
  a.service->HostSession({});
  a.service->LeaveSession();
  for (int i = 0; i < 20; ++i) hub.Step();
  Check(!a.Snap().room && a.Sent("room.leave") == 1, "a reply that arrives after leaving does not seat the player");
  Check(hub.rooms.empty(), "and the session is closed");
}

void TestHostRecreatesAfterSignIn() {
  Hub hub;
  Player& a = hub.Add("Host");
  a.service->HostSession({});
  Check(hub.Until([&] { return a.Snap().room.has_value(); }), "hosting waits for the sign-in");
  Check(a.Sent("room.host") == 1, "and sends once");
  for (int i = 0; i < 200; ++i) hub.Step();
  Check(a.Sent("room.host") == 1, "no repeat inside the refresh window");
  hub.Until([&] { return a.Sent("room.host") >= 2; }, 70.0);
  Check(a.Sent("room.host") >= 2, "the session is refreshed while it waits");
  Check(hub.rooms.size() == 1, "without making another");
}

}  // namespace

int main() {
  eck::SetActiveUiLanguage("en-US");
  TestTwoPlayers();
  TestGuestEntersTheHostsGame();
  TestCapturedSetupInvites();
  TestRaceKeepsTheSession();
  TestRaceOverRelay();
  TestLeavingARace();
  TestPicksReplicate();
  TestPicksOverRelay();
  TestThreePlayers();
  TestFourPlayers();
  TestStartWithAnInvitePending();
  TestPendingInvitesUseUpSeats();
  TestSeatsFollowAcceptOrder();
  TestMixedTransports();
  TestRelayWithoutEndpoint();
  TestNoPathEndsCleanly();
  TestRelayRefusedFailsThatSeatOnly();
  TestLateSeatInAnIdleSession();
  TestResumeKeepsDirect();
  TestRelayedSeatRecovers();
  TestServiceGoneEndsTheSession();
  TestOneShotInvite();
  TestInviteWhileAloneInOwnSession();
  TestContinueAfterTheRace();
  TestHostLeavesFromTheResults();
  TestGuestLeavesTheRace();
  TestHostLeavesTheRace();
  TestHardClose();
  TestDualHost(true);
  TestDualHost(false);
  TestRoomHostRepliesOnce();
  TestDeclineAndRefusal();
  TestNewSessionAfterTheLastOne();
  TestLeftBeforeTheServiceAnswered();
  TestHostRecreatesAfterSignIn();
  return online_test::Finish();
}
