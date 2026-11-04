// The Online menu model (src/online_menu.h) and the Start Race seam (src/online_start_race.h) against a scripted service.
#include <cstring>
#include <string>
#include <vector>

#include "online_menu.h"
#include "online_test_support.h"

using online_test::Check;
using namespace eck;
using namespace eck::menu;

namespace {

online::Seat Seated(int seat, const std::string& player, const std::string& name) {
  online::Seat s;
  s.seat = seat;
  s.player = player;
  s.name = name;
  s.state = online::SeatState::kConnected;
  return s;
}

online::RoomView MakeRoom(int capacity, std::vector<std::pair<std::string, std::string>> players) {
  online::RoomView room;
  room.room = "0123456789abcdef";
  room.host = players[0].first;
  room.host_name = players[0].second;
  room.capacity = capacity;
  room.players = int(players.size());
  room.rules = {{"laps", 3}, {"items", 1}};
  for (int i = 0; i < capacity; ++i) {
    online::Seat seat;
    seat.seat = i;
    if (i < int(players.size())) seat = Seated(i, players[size_t(i)].first, players[size_t(i)].second);
    room.seats.push_back(seat);
  }
  return room;
}

online::Friend MakeFriend(const std::string& code, const std::string& name, online::FriendState state, bool online,
                          bool compatible = true) {
  online::Friend f;
  f.player = code;
  f.name = name;
  f.online = online;
  f.state = state;
  f.compatible = compatible;
  return f;
}

// A service that does what the real one does to the snapshot, and counts what the menu asks of it.
class FakeApi final : public Api {
 public:
  online::Snapshot snap;
  int creates = 0, searches = 0, joins = 0, starts = 0, leaves = 0, invites_sent = 0, accepts = 0, declines = 0;
  int last_capacity = 0;
  online::RoomRules last_rules;
  std::string last_join, last_invite, last_accept;
  bool accept_ok = true;

  FakeApi() { snap.connection = online::Connection::kOnline; }

  online::Snapshot Snapshot() override { return snap; }
  bool CreateRoom(int capacity, const online::RoomRules& rules) override {
    ++creates;
    last_capacity = capacity;
    last_rules = rules;
    snap.room = MakeRoom(capacity, {{"AAAA-1111", "Saulo"}});
    snap.seat = 0;
    return true;
  }
  void SearchRooms() override { ++searches; }
  bool JoinRoom(const std::string& handle) override {
    ++joins;
    last_join = handle;
    return true;
  }
  bool StartRace() override {
    ++starts;
    if (!snap.room) return false;
    snap.room->state = online::RoomState::kStarting;
    return true;
  }
  void LeaveRoom() override {
    ++leaves;
    snap.room.reset();
    snap.seat = -1;
  }
  bool SendInvite(const std::string& player) override {
    ++invites_sent;
    last_invite = player;
    return true;
  }
  bool AcceptInvite(const std::string& invite) override {
    ++accepts;
    last_accept = invite;
    return accept_ok;
  }
  void DeclineInvite(const std::string&) override { ++declines; }
};

struct Rig {
  FakeApi api;
  std::vector<online_start::Plan> plans;
  bool real_entry = false;
  OnlineMenu menu;
  double now = 1.0;

  explicit Rig(TextPackage package = TextPackage::kInt)
      : menu(api, package, [this](const online_start::Plan& plan) {
          plans.push_back(plan);
          online_start::Verdict v;
          v.real_entry = real_entry;
          v.reason = "test";
          return v;
        }) {}

  void Tick() {
    now += 0.1;
    menu.Update(now);
  }
  void Press(Act act) {
    const std::vector<Row> rows = menu.Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].act == act && rows[i].enabled) {
        menu.SetCursor(int(i));
        menu.Activate();
        return;
      }
    }
    Check(false, "no enabled row for the action");
  }
  std::vector<std::string> Texts() const {
    std::vector<std::string> out;
    for (const Row& row : menu.Rows()) out.push_back(row.text);
    return out;
  }
  bool Has(const std::string& text) const {
    for (const std::string& t : Texts()) {
      if (t == text) return true;
    }
    return false;
  }
  const Row* Find(Act act) const {
    static std::vector<Row> rows;
    rows = menu.Rows();
    for (const Row& row : rows) {
      if (row.act == act) return &row;
    }
    return nullptr;
  }
};

void TestLocalOnlineChoice() {
  Rig rig;
  Check(rig.menu.page() == Page::kMode, "the menu starts at the Local / Online choice");
  const std::vector<Row> rows = rig.menu.Rows();
  Check(rows.size() == 2 && rows[0].text == "Local" && rows[0].act == Act::kLocal, "first row is Local");
  Check(rows[1].text == "Online" && rows[1].act == Act::kOnline, "second row is Online");
  Check(rows[0].enabled && rows[1].enabled, "both choices are selectable");

  Rig offline;
  offline.api.snap.connection = online::Connection::kOff;
  offline.menu.Update(1.0);
  offline.menu.Activate();
  Check(offline.menu.TakeOutcome() == Outcome::kLocal, "Local works with no online service");
  Check(offline.api.creates + offline.api.searches + offline.api.joins + offline.api.starts + offline.api.leaves +
                offline.api.invites_sent + offline.api.accepts == 0,
        "Local never calls the online service");
  Check(offline.menu.TakeOutcome() == Outcome::kNone, "the outcome is taken once");

  Rig back;
  back.menu.Back();
  Check(back.menu.TakeOutcome() == Outcome::kClose, "B at the choice closes the menu");
}

void TestOnlinePage() {
  Rig rig;
  rig.Press(Act::kOnline);
  Check(rig.menu.page() == Page::kOnline, "Online opens the Online page");
  Check(rig.Has("Create Room") && rig.Has("Find Room") && rig.Has("Back"), "Online offers Create Room and Find Room");
  Check(rig.Find(Act::kCreateRoom)->enabled && rig.Find(Act::kFindRoom)->enabled, "both enabled when connected");

  Rig offline;
  offline.api.snap.connection = online::Connection::kError;
  offline.menu.Update(1.0);
  offline.Press(Act::kOnline);
  Check(!offline.Find(Act::kCreateRoom)->enabled && !offline.Find(Act::kFindRoom)->enabled, "disabled when not connected");
  Check(offline.Has("Unable to connect"), "the failure is shown");
  Check(offline.menu.cursor() == int(offline.menu.Rows().size()) - 1, "the cursor rests on Back");
  offline.menu.Back();
  Check(offline.menu.page() == Page::kMode, "Back returns to the choice");
}

void TestCreateRoom() {
  for (int capacity : {2, 3, 4}) {
    Rig rig;
    rig.Press(Act::kOnline);
    rig.Press(Act::kCreateRoom);
    Check(rig.menu.page() == Page::kCreate, "Create Room page");
    Check(rig.menu.capacity() == 4, "four players by default");
    while (rig.menu.capacity() != capacity) rig.Press(Act::kPlayers);
    Check(rig.Has(std::to_string(capacity) + " Players"), "the selector shows " + std::to_string(capacity) + " Players");
    rig.Press(Act::kCreate);
    Check(rig.api.creates == 1 && rig.api.last_capacity == capacity, "Create asks for the selected capacity");
    Check(rig.api.last_rules == DefaultRules(), "Create sends the room rules");
    rig.Tick();
    Check(rig.menu.page() == Page::kRoom, "the room page opens when the service seats the host");
  }
  Rig rig;
  rig.Press(Act::kOnline);
  rig.Press(Act::kCreateRoom);
  rig.menu.SetCursor(1);
  for (int i = 0; i < 5; ++i) rig.menu.Adjust(-1);
  Check(rig.menu.capacity() == 2, "left stops at 2 players");
  for (int i = 0; i < 5; ++i) rig.menu.Adjust(1);
  Check(rig.menu.capacity() == 4, "right stops at 4 players");
}

void TestFindRoom() {
  Rig rig;
  online::RoomAd open;
  open.handle = "h1";
  open.host = "BBBB-2222";
  open.host_name = "Saulo";
  open.players = 1;
  open.capacity = 4;
  online::RoomAd full = open;
  full.handle = "h2";
  full.host = "CCCC-3333";
  full.host_name = "Ana";
  full.players = 2;
  full.capacity = 2;
  online::RoomAd old = open;
  old.handle = "h3";
  old.host = "DDDD-4444";
  old.host_name = "Beto";
  rig.api.snap.friends = {MakeFriend("DDDD-4444", "Beto", online::FriendState::kHosting, true, false)};
  rig.Press(Act::kOnline);
  rig.Press(Act::kFindRoom);
  Check(rig.api.searches == 1, "entering Find Room searches once");
  rig.api.snap.rooms = {open, full, old};
  rig.api.snap.search_generation = 1;
  rig.Tick();
  Check(rig.Has("Saulo 1/4"), "a room shows the host and the players over the capacity");
  Check(rig.Has("Ana 2/2 - Room full"), "a full room is marked");
  Check(rig.Has("Beto 1/4 - Version mismatch"), "an incompatible host is marked");
  bool leaks = false;
  for (const std::string& text : rig.Texts()) {
    if (text.find("BBBB") != std::string::npos || text.find("h1") != std::string::npos) leaks = true;
  }
  Check(!leaks, "no id or handle is shown");
  Check(rig.Find(Act::kJoin)->enabled, "the open room is joinable");
  int disabled = 0;
  for (const Row& row : rig.menu.Rows()) disabled += row.act == Act::kJoin && !row.enabled;
  Check(disabled == 2, "the full and incompatible rooms cannot be joined");
  rig.menu.SetCursor(0);
  rig.menu.Activate();
  Check(rig.api.joins == 1 && rig.api.last_join == "h1", "joining sends the selected room's handle");
  rig.menu.Move(1);
  rig.menu.Move(-1);
  rig.Press(Act::kRefresh);
  Check(rig.api.searches == 1, "Refresh is throttled");
  rig.now += 3;
  rig.Tick();
  rig.Press(Act::kRefresh);
  Check(rig.api.searches == 2, "Refresh searches again later");

  Rig empty;
  empty.Press(Act::kOnline);
  empty.Press(Act::kFindRoom);
  empty.Tick();
  Check(empty.Has("No rooms found"), "an empty list says so");
  empty.menu.Back();
  Check(empty.menu.page() == Page::kOnline, "Back from Find Room");
}

void TestSeatList() {
  Rig rig;
  rig.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  rig.api.snap.seat = 0;
  rig.menu.Update(1.0);
  Check(rig.menu.page() == Page::kRoom, "a seated player sees the room");
  const std::vector<std::string> texts = rig.Texts();
  Check(texts.size() >= 8, "four seats, status and actions");
  Check(texts[0] == "Player 1: Saulo (Host, You)", "seat 1 is the host and you: " + texts[0]);
  Check(texts[1] == "Player 2: Ana", "seat 2: " + texts[1]);
  Check(texts[2] == "Player 3: Waiting for players" && texts[3] == "Player 4: Waiting for players", "empty seats wait");
  Check(texts[4] == "Waiting for players", "the status line waits");

  Rig guest;
  guest.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  guest.api.snap.seat = 1;
  guest.menu.Update(1.0);
  const std::vector<std::string> g = guest.Texts();
  Check(g[0] == "Player 1: Saulo (Host)" && g[1] == "Player 2: Ana (You)", "a guest sees who is the host and who is you");
  Check(g[2] == "Room full", "a full room says so");
  Check(!guest.Find(Act::kStartRace) && !guest.Find(Act::kInviteFriends), "a guest has no host actions");
  Check(guest.Find(Act::kLeave) != nullptr, "every player can leave");
  Check(guest.Has("Waiting for the host"), "the guest waits for the host");

  Rig lost;
  lost.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  lost.api.snap.room->seats[1].state = online::SeatState::kReconnecting;
  lost.api.snap.seat = 0;
  lost.menu.Update(1.0);
  Check(lost.Texts()[1] == "Player 2: Ana (Connection lost)", "a reconnecting seat is marked");
}

void TestStartRaceButton() {
  Rig one;
  one.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}});
  one.api.snap.seat = 0;
  one.menu.Update(1.0);
  const Row* start = one.Find(Act::kStartRace);
  Check(start && !start->enabled, "Start Race is disabled with one player");
  Check(start && start->text.find("Need 2 players") != std::string::npos, "and says why");
  one.menu.SetCursor(0);
  one.menu.Activate();
  Check(one.api.starts == 0, "a disabled Start Race does nothing");

  Rig two;
  two.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  two.api.snap.seat = 0;
  two.menu.Update(1.0);
  Check(two.Find(Act::kStartRace)->enabled, "Start Race is enabled at 2 of 4: the room need not be full");
  Check(two.Find(Act::kInviteFriends)->enabled, "Invite Friends is enabled while seats are free");

  Rig full;
  full.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  full.api.snap.seat = 0;
  full.menu.Update(1.0);
  Check(full.Find(Act::kStartRace)->enabled && !full.Find(Act::kInviteFriends)->enabled,
        "a full room starts and cannot invite");
  Check(full.Has("Room full"), "Room full is shown");
}

void TestStartRaceFlow() {
  Rig host;
  host.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  host.api.snap.seat = 0;
  host.menu.Update(1.0);
  host.Press(Act::kStartRace);
  Check(host.api.starts == 1, "Start Race asks the service to freeze the seats");
  Check(host.plans.empty(), "the race plan waits for the service to confirm");
  host.Tick();
  Check(host.plans.size() == 1, "the host maps the seats once the room leaves open");
  if (!host.plans.empty()) {
    const online_start::Plan& plan = host.plans[0];
    Check(plan.host && plan.local_seat == 0 && plan.config.participants.size() == 2, "host plan: seat 0, two humans");
    Check(plan.karts.size() == 8 && plan.karts[0].seat == 0 && plan.karts[0].local && plan.karts[1].seat == 1 &&
              plan.karts[1].remote_human,
          "seat 0 takes slot 0 and seat 1 slot 1 as a remote human");
    Check(plan.config.participants[0].character == online_start::kUnknownSelection, "selections are recorded as unknown");
  }
  Check(host.menu.race_note() == "Race not available yet", "a blocked start is told, not silent");
  host.Tick();
  Check(host.plans.size() == 1, "the start is handled once");
  host.Press(Act::kLeave);
  Check(host.api.leaves == 1 && host.menu.page() == Page::kOnline, "a started room can still be left");

  Rig guest;
  guest.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  guest.api.snap.seat = 1;
  guest.menu.Update(1.0);
  guest.api.snap.room->state = online::RoomState::kStarting;
  guest.Tick();
  Check(guest.plans.size() == 1 && !guest.plans[0].host && guest.plans[0].local_seat == 1, "the guest maps its own seat");
  if (!guest.plans.empty()) {
    Check(guest.plans[0].karts[1].local && guest.plans[0].karts[0].remote_human, "the guest drives slot 1 and sees slot 0 remote");
    Check(guest.plans[0].config.race_id == host.plans[0].config.race_id && guest.plans[0].config.seed == host.plans[0].config.seed,
          "race id and seed agree on every PC");
  }
  Check(guest.Has("Race not available yet"), "the guest sees the blocked note");
  guest.real_entry = true;
  Rig ok;
  ok.real_entry = true;
  ok.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  ok.api.snap.seat = 1;
  ok.menu.Update(1.0);
  ok.api.snap.room->state = online::RoomState::kRacing;
  ok.Tick();
  Check(ok.menu.race_note() == "Race starting", "a proven entry shows Race starting");
  ok.Press(Act::kLeave);
  Check(ok.api.leaves == 1 && ok.menu.page() == Page::kOnline, "a joiner returns to the menus without restarting");
}

void TestInviteFriends() {
  Rig rig;
  rig.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  rig.api.snap.seat = 0;
  rig.api.snap.friends = {
      MakeFriend("BBBB-2222", "Ana", online::FriendState::kInGame, true),
      MakeFriend("CCCC-3333", "Beto", online::FriendState::kOnline, true),
      MakeFriend("DDDD-4444", "Caio", online::FriendState::kOffline, false),
      MakeFriend("EEEE-5555", "Dani", online::FriendState::kHosting, true),
      MakeFriend("FFFF-6666", "Edu", online::FriendState::kOnline, true, false),
      MakeFriend("GGGG-7777", "Fabi", online::FriendState::kOnline, true),
  };
  rig.menu.Update(1.0);
  rig.Press(Act::kInviteFriends);
  Check(rig.menu.page() == Page::kInvite && rig.menu.Title() == "Invite Friends", "Invite Friends page");
  Check(rig.Has("Beto") && rig.Has("Fabi"), "online compatible free friends are listed");
  Check(!rig.Has("Ana") && !rig.Has("Caio") && !rig.Has("Dani") && !rig.Has("Edu"),
        "seated, offline, busy and incompatible friends are not listed");
  rig.menu.SetCursor(0);
  rig.menu.Activate();
  Check(rig.api.invites_sent == 1 && rig.api.last_invite == "CCCC-3333", "the selected friend is invited");
  Check(rig.menu.Toast(rig.now) == "Invite sent", "a short confirmation shows");
  Check(rig.Has("Beto (Invited)") && !rig.Find(Act::kInviteFriend)->enabled, "an invited friend cannot be invited twice");
  rig.menu.SetCursor(1);
  rig.menu.Activate();
  Check(rig.api.invites_sent == 2 && rig.api.last_invite == "GGGG-7777", "several invites in a row");
  rig.menu.Back();
  Check(rig.menu.page() == Page::kRoom, "Back returns to the room");
  const std::vector<std::string> texts = rig.Texts();
  Check(texts[2] == "Player 3: Beto (Invited)" && texts[3] == "Player 4: Fabi (Invited)", "invited friends hold the free seats");

  rig.api.snap.room->seats[2] = Seated(2, "CCCC-3333", "Beto");
  rig.api.snap.room->players = 3;
  rig.Tick();
  Check(rig.Texts()[2] == "Player 3: Beto" && rig.Texts()[3] == "Player 4: Fabi (Invited)", "an accepted invite takes its seat");
  Check(rig.menu.Toast(rig.now) == "Beto joined", "and says so");

  rig.api.snap.room->seats[3] = Seated(3, "GGGG-7777", "Fabi");
  rig.api.snap.room->players = 4;
  rig.Tick();
  Check(rig.Has("Room full") && !rig.Find(Act::kInviteFriends)->enabled, "invites stop when the room is full");

  Rig none;
  none.api.snap.room = MakeRoom(3, {{"AAAA-1111", "Saulo"}});
  none.api.snap.seat = 0;
  none.menu.Update(1.0);
  none.Press(Act::kInviteFriends);
  Check(none.Has("No friends to invite"), "an empty list says so");
}

void TestIncomingInvite() {
  Rig rig;
  online::Invite invite;
  invite.id = "inv1";
  invite.host = "BBBB-2222";
  invite.host_name = "Saulo";
  invite.players = 1;
  invite.capacity = 4;
  rig.api.snap.invites = {invite};
  rig.menu.Update(1.0);
  Check(rig.menu.HasIncomingInvite() && rig.menu.page() == Page::kIncoming, "an invite shows over the menus");
  Check(rig.Texts()[0] == "Saulo invited you to a Multiplayer race", "the invite text: " + rig.Texts()[0]);
  Check(rig.Has("Accept") && rig.Has("Decline"), "Accept and Decline");
  rig.Press(Act::kAccept);
  Check(rig.api.accepts == 1 && rig.api.last_accept == "inv1", "Accept answers that invite");
  Check(rig.api.creates == 0, "Accept never creates a room");
  rig.api.snap.invites.clear();
  rig.api.snap.room = MakeRoom(4, {{"BBBB-2222", "Saulo"}, {"AAAA-1111", "Me"}});
  rig.api.snap.seat = 1;
  rig.Tick();
  Check(rig.menu.page() == Page::kRoom && rig.Texts()[0] == "Player 1: Saulo (Host)", "the guest lands in the host's room");

  Rig decline;
  decline.api.snap.invites = {invite};
  decline.menu.Update(1.0);
  decline.menu.Back();
  Check(decline.api.declines == 1 && decline.api.accepts == 0, "B declines");
  decline.api.snap.invites.clear();
  decline.Tick();
  Check(decline.menu.page() == Page::kMode, "the menu returns to the choice");

  Rig pt(TextPackage::kPtb);
  pt.api.snap.invites = {invite};
  pt.menu.Update(1.0);
  Check(pt.Texts()[0] == "Saulo convidou voc\xC3\xAA para uma corrida multiplayer", "the invite is localized");

  Rig busy;
  busy.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Me"}});
  busy.api.snap.seat = 0;
  busy.api.snap.invites = {invite};
  busy.menu.Update(1.0);
  Check(!busy.menu.HasIncomingInvite() && busy.menu.page() == Page::kRoom, "a seated player is not interrupted");
}

void TestRoomEvents() {
  Rig leave;
  leave.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  leave.api.snap.seat = 0;
  leave.menu.Update(1.0);
  leave.Press(Act::kLeave);
  Check(leave.api.leaves == 1 && leave.menu.page() == Page::kOnline, "Leave Room leaves and returns to Online");
  leave.Tick();
  Check(leave.menu.Toast(leave.now).empty(), "leaving on purpose raises no alarm");

  Rig host_left;
  host_left.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  host_left.api.snap.seat = 1;
  host_left.menu.Update(1.0);
  host_left.api.snap.room.reset();
  host_left.api.snap.seat = -1;
  host_left.api.snap.closed_reason = "host_left";
  host_left.Tick();
  Check(host_left.menu.page() == Page::kOnline && host_left.menu.Toast(host_left.now) == "Host left", "host left");

  Rig lost;
  lost.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  lost.api.snap.seat = 1;
  lost.menu.Update(1.0);
  lost.api.snap.room.reset();
  lost.api.snap.closed_reason = "disconnect";
  lost.Tick();
  Check(lost.menu.Toast(lost.now) == "Connection lost", "a lost connection is told");

  Rig left;
  left.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  left.api.snap.seat = 0;
  left.menu.Update(1.0);
  left.api.snap.room->seats[1] = online::Seat{1};
  left.api.snap.room->players = 1;
  left.Tick();
  Check(left.menu.Toast(left.now) == "Player left: Ana", "a player who left is told: " + left.menu.Toast(left.now));

  Rig joined;
  joined.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}});
  joined.api.snap.seat = 0;
  joined.menu.Update(1.0);
  joined.api.snap.room->seats[1] = Seated(1, "BBBB-2222", "Ana");
  joined.api.snap.room->players = 2;
  joined.Tick();
  Check(joined.menu.Toast(joined.now) == "Ana joined", "a join is told");
  Check(joined.menu.Toast(joined.now + 10).empty(), "the notice fades");

  Rig reconnect;
  reconnect.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  reconnect.api.snap.seat = 1;
  reconnect.menu.Update(1.0);
  reconnect.api.snap.reconnecting = true;
  reconnect.Tick();
  Check(reconnect.Has("Connection lost") && reconnect.menu.Toast(reconnect.now) == "Connection lost",
        "a service reconnect is shown while the seat is held");
  reconnect.api.snap.reconnecting = false;
  reconnect.Tick();
  Check(!reconnect.Has("Connection lost"), "and clears when the service is back");

  Rig back;
  back.api.snap.room = MakeRoom(2, {{"AAAA-1111", "Saulo"}});
  back.api.snap.seat = 0;
  back.menu.Update(1.0);
  back.menu.Back();
  Check(back.menu.TakeOutcome() == Outcome::kClose && back.api.leaves == 0, "B hides the menu and keeps the room");
}

void TestNavigation() {
  Rig rig;
  rig.api.snap.room = MakeRoom(4, {{"AAAA-1111", "Saulo"}});
  rig.api.snap.seat = 0;
  rig.menu.Update(1.0);
  // Rows: 4 seats, status, Invite Friends, Start Race (disabled), Leave Room
  const std::vector<Row> rows = rig.menu.Rows();
  Check(rows[size_t(rig.menu.cursor())].act == Act::kInviteFriends, "the cursor starts on the first action");
  rig.menu.Move(1);
  Check(rows[size_t(rig.menu.cursor())].act == Act::kLeave, "down skips the disabled Start Race");
  rig.menu.Move(1);
  Check(rows[size_t(rig.menu.cursor())].act == Act::kInviteFriends, "and wraps around");
  rig.menu.Move(-1);
  Check(rows[size_t(rig.menu.cursor())].act == Act::kLeave, "up wraps back");
}

void TestLanguages() {
  const TextPackage packages[] = {TextPackage::kInt, TextPackage::kEsm, TextPackage::kEsn, TextPackage::kPtb};
  const char* required[] = {"Local", "Online", "Create Room", "Find Room", "Players", "2 Players", "3 Players",
                            "4 Players", "Invite Friends", "Start Race", "Leave Room", "Waiting for players",
                            "Room full", "Host", "You", "Invited", "Accept", "Decline", "Connection lost", "Host left",
                            "Player left", "Version mismatch"};
  for (const char* key : required) {
    bool listed = false;
    for (const char* all : AllMenuKeys()) listed = listed || std::strcmp(all, key) == 0;
    Check(listed, std::string("listed: ") + key);
  }
  for (const char* key : AllMenuKeys()) {
    for (const TextPackage package : packages) {
      const char* text = MenuText(key, package);
      Check(text && text[0], std::string("not empty: ") + key);
    }
    if (std::strcmp(key, "Local") && std::strcmp(key, "Online")) {
      Check(std::strcmp(MenuText(key, TextPackage::kPtb), key) != 0 || std::strcmp(key, "Players") == 0 ||
                std::strcmp(key, "Accept") == 0 || std::strcmp(key, "Friends") == 0,
            std::string("translated to PTB: ") + key);
    }
  }
  Check(std::strcmp(MenuText("Host left", TextPackage::kEsm), MenuText("Host left", TextPackage::kEsn)) != 0, "ESM and ESN differ where the game's do");
  Check(std::strcmp(MenuText("Start Race", TextPackage::kPtb), "Iniciar corrida") == 0, "online_text.h strings are reused");
  Check(std::strcmp(MenuText("Nothing like this", TextPackage::kEsm), "Nothing like this") == 0, "an unknown text is unchanged");
  Rig pt(TextPackage::kPtb);
  Check(pt.Texts()[0] == "Local" && pt.Texts()[1] == "Online", "the choice in PTB");
  Rig es(TextPackage::kEsm);
  es.Press(Act::kOnline);
  Check(es.Has("Crear sala") && es.Has("Buscar sala"), "Create and Find in ESM");
}

void TestDryRun() {
  online::RoomView room = MakeRoom(4, {{"AAAA-1111", "Saulo"}, {"BBBB-2222", "Ana"}});
  room.rules = {{"laps", 5}, {"track", 7}, {"items", 0}};
  const online_start::Plan plan = online_start::MakePlan(room, 1);
  Check(plan.config.laps == 5 && plan.config.track == 7 && plan.config.rules == 0, "room rules reach the race config");
  Check(plan.karts.size() == 8 && race::CountKarts(plan.karts).bots == 6, "two humans and six bots");
  online_start::SetStartRaceHook({});
  const online_start::Decision debug = online_start::Decide(plan, true);
  std::string all;
  for (const std::string& line : debug.lines) all += line + "\n";
  Check(debug.verdict.dry_run && !debug.verdict.real_entry, "ECK_ONLINE_DEBUG=1 runs the dry run and enters no race");
  Check(all.find("host_seat=0 local_seat=1 local_player_index=0 role=guest") != std::string::npos, "host seat, local seat and index printed");
  Check(all.find("seat=0 remote kart_slot=0 player_controller_slot=none") != std::string::npos, "remote seat mapping printed");
  Check(all.find("seat=1 local kart_slot=1 player_controller_slot=0") != std::string::npos, "local seat mapping printed");
  Check(all.find("?NumHumanKarts=2?TotalKarts=8?NumSelectedTrack=7?PrimaryControllerID=0") != std::string::npos, "expected URL fields printed");
  Check(all.find("[DEBUG_SAULO][ONLINE-RACE] start blocked reason=final guest hook unproven") != std::string::npos,
        "the dry run still reports that the real start is blocked");
  const online_start::Decision off = online_start::Decide(plan, false);
  Check(off.lines.size() == 1 && off.lines[0] == std::string(online_start::kLogTag) + " start blocked reason=" + online_start::kNoDebugReason,
        "without the env var only the blocked line is logged");
  Check(!off.verdict.dry_run && !off.verdict.real_entry, "and nothing runs");
  online_start::Plan empty;
  Check(online_start::Decide(empty, true).lines[0].find("reason=invalid room plan") != std::string::npos, "an invalid plan is blocked, not silent");
  const online_start::Plan unknown_track = online_start::MakePlan(MakeRoom(2, {{"A", "a"}, {"B", "b"}}), 0);
  Check(unknown_track.config.track == online_start::kUnknownTrack, "an absent track is recorded as unknown");

  online_start::SetStartRaceHook([](const online_start::Plan&) -> std::optional<online_start::Verdict> {
    online_start::Verdict v;
    v.real_entry = true;
    return v;
  });
  Check(online_start::Decide(plan, false).verdict.real_entry, "the seam hands over to a proven hook");
  online_start::SetStartRaceHook([](const online_start::Plan&) -> std::optional<online_start::Verdict> {
    online_start::Verdict v;
    v.reason = "remote kart state not proven";
    return v;
  });
  const online_start::Decision hooked = online_start::Decide(plan, true);
  Check(!hooked.verdict.real_entry && hooked.lines.size() == 1 && hooked.lines[0].find("remote kart state not proven") != std::string::npos,
        "a hook's own block reason is logged");
  online_start::SetStartRaceHook({});
}

}  // namespace

int main() {
  TestLocalOnlineChoice();
  TestOnlinePage();
  TestCreateRoom();
  TestFindRoom();
  TestSeatList();
  TestStartRaceButton();
  TestStartRaceFlow();
  TestInviteFriends();
  TestIncomingInvite();
  TestRoomEvents();
  TestNavigation();
  TestLanguages();
  TestDryRun();
  return online_test::Finish();
}
