// Online on the character screen (src/online_menu.h, src/online_character_select.h) against a scripted service.
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "online_character_select.h"
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

online::PeerSnapshot Peer(int seat, online::PeerPath path) {
  online::PeerSnapshot peer;
  peer.seat = seat;
  peer.connected = true;
  peer.path = path;
  return peer;
}

// A service that does what the real one does to the snapshot, and counts what the menu asks of it.
class FakeApi final : public Api {
 public:
  online::Snapshot snap;
  int invites_sent = 0, accepts = 0, declines = 0;
  std::string last_invite, last_accept;
  bool invite_ok = true;
  bool answers_later = false;
  std::optional<online::RoomRules> last_rules;

  FakeApi() { snap.connection = online::Connection::kOnline; }

  online::Snapshot Snapshot() override { return snap; }
  bool SendInvite(const std::string& player, const std::optional<online::RoomRules>& rules) override {
    if (!invite_ok) return false;
    ++invites_sent;
    last_invite = player;
    last_rules = rules;
    if (answers_later) {
      snap.sending_invite = player;
      snap.creating_session = !snap.room;
      return true;
    }
    for (const online::Friend& f : snap.friends) {
      if (f.player == player) snap.invited.push_back({"id", player, f.name, 60.0});
    }
    return true;
  }
  bool AcceptInvite(const std::string& invite) override {
    ++accepts;
    last_accept = invite;
    return true;
  }
  void DeclineInvite(const std::string&) override { ++declines; }
  void LeaveSession(const char* source) override {
    ++leaves;
    last_leave = source;
    snap.room.reset();
    snap.seat = -1;
  }
  int leaves = 0;
  std::string last_leave;
};

online::Invite MakeInvite(const std::string& id) {
  online::Invite invite;
  invite.id = id;
  invite.host_name = "Saulo";
  return invite;
}

const std::vector<std::pair<std::string, std::string>> kPlayers = {
    {"AAAA-1111", "Saulo"}, {"BBBB-2222", "Carlos"}, {"CCCC-3333", "Maria"}, {"DDDD-4444", "Caio"}};

online::Snapshot Session(int players, int seat = 0) {
  online::Snapshot snap;
  snap.connection = online::Connection::kOnline;
  snap.room = MakeRoom(4, {kPlayers.begin(), kPlayers.begin() + players});
  snap.seat = seat;
  return snap;
}

online::SentInvite Pending(const std::string& name) { return {"id-" + name, "code-" + name, name, 60.0}; }

std::string ReadSource(const std::string& name) {
  std::ifstream file(std::string(ECK_REPO_DIR) + "/src/" + name, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

std::vector<std::string> Texts(const OnlineMenu& menu) {
  std::vector<std::string> out;
  for (const Row& row : menu.Rows()) out.push_back(row.text);
  return out;
}

bool Press(OnlineMenu& menu, Act act) {
  const std::vector<Row> rows = menu.Rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].act == act && rows[i].enabled) {
      menu.SetCursor(int(i));
      menu.Activate();
      return true;
    }
  }
  return false;
}

void TestShortcut() {
  online::Snapshot none;
  none.connection = online::Connection::kOnline;
  Check(!ShortcutFor(false, true, none).visible, "Online play off has no Invite Player");
  Check(!ShortcutFor(false, true, Session(2)).visible, "not even with a session");
  Check(!ShortcutFor(true, false, none).visible, "Online play on shows nothing before the character screen");
  const InviteShortcut alone = ShortcutFor(true, true, none);
  Check(alone.visible && alone.remaining == 3, "the character screen offers 3 invites");
  for (const online::Connection connection : {online::Connection::kChecking, online::Connection::kError}) {
    online::Snapshot unsigned_in;
    unsigned_in.connection = connection;
    Check(ShortcutFor(true, true, unsigned_in).visible, "the hint does not wait for the service");
    Check(OnInvitePress(unsigned_in, true) == InvitePress::kConnecting, "X then says it is connecting");
    Check(OnInvitePress(unsigned_in, false) == InvitePress::kConnecting, "whatever the game's objects say");
  }

  online::Snapshot state = Session(1);
  Check(ShortcutFor(true, true, state).remaining == 3, "a host alone can invite 3");
  state = Session(2);
  Check(ShortcutFor(true, true, state).remaining == 2, "host and B: 2 more");
  state = Session(3);
  Check(ShortcutFor(true, true, state).remaining == 1, "host, B and C: 1 more");
  state = Session(4);
  Check(!ShortcutFor(true, true, state).visible, "a full session has no Invite Player");

  state = Session(2);
  state.invited = {Pending("Maria"), Pending("Caio")};
  Check(!ShortcutFor(true, true, state).visible, "pending invites use up the seats");
  state.invited.pop_back();
  const InviteShortcut after = ShortcutFor(true, true, state);
  Check(after.visible && after.remaining == 1, "a declined or expired invite gives its seat back");

  Check(!ShortcutFor(true, true, Session(2, 1)).visible, "only the host invites");
  Check(std::string(ShortcutFor(true, true, Session(2, 1)).hidden_because) == "guest", "and the log says why");
  Check(std::string(ShortcutFor(true, true, Session(4)).hidden_because) == "session_full", "as it does for a full session");
  Check(std::string(ShortcutFor(false, true, none).hidden_because) == "online_play_off", "and for Online play off");
  online::Snapshot joining = none;
  joining.joining = true;
  Check(ShortcutFor(true, true, joining).visible, "still there while joining");
  Check(OnInvitePress(joining, true) == InvitePress::kConnecting, "X waits for the join");

  Check(OnInvitePress(none, true) == InvitePress::kPicker, "X opens the picker when the service is ready");
  Check(OnInvitePress(none, false) == InvitePress::kPickerWithoutSetup, "and even without a readable setup");
  online::Snapshot hosting = none;
  hosting.hosting = true;
  Check(OnInvitePress(hosting, false) == InvitePress::kPicker, "a session on its way needs no second reading");
  Check(OnInvitePress(Session(2), false) == InvitePress::kPicker, "nor does one that exists");
}

void TestStatusWithoutService() {
  SessionNotes notes(TextPackage::kPtb);
  notes.NoteStatus("Connecting to online service", 10.0);
  Check(notes.Toast(10.5) == MenuText("Connecting to online service", TextPackage::kPtb), "X before sign-in says so");
  Check(notes.Toast(10.0 + kToastSeconds + 1).empty(), "for a moment");
  Check(std::strlen(MenuText("Invite Player", TextPackage::kPtb)) && std::string(MenuText("Invite Player", TextPackage::kPtb)) == "Convidar jogador",
        "PT-BR hint");
  Check(std::string(MenuText("Invite Player", TextPackage::kEsm)) == "Invitar jugador", "Spanish hint");
  Check(std::string(MenuText("Invite Player", TextPackage::kInt)) == "Invite Player", "English hint");
}

void TestPickerWithoutSetup() {
  FakeApi api;
  api.snap.friends = {MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true)};
  OnlineMenu menu(api, TextPackage::kInt, Page::kInvite, std::nullopt, true);
  const std::vector<Row> rows = menu.Rows();
  Check(rows.front().text == "Game setup unavailable" && rows.front().act == Act::kNone, "the picker says why");
  bool friend_listed = false;
  for (const Row& row : rows) {
    if (row.act != Act::kInviteFriend) continue;
    friend_listed = true;
    Check(!row.enabled, "and cannot send");
  }
  Check(friend_listed, "but stays the same picker");
  Check(rows.back().act == Act::kBack, "with a way back");
  menu.Activate();
  Check(api.invites_sent == 0 && menu.closed(), "the cursor lands on Back and nothing was sent");
}

void TestScreenTracking() {
  constexpr uint32_t kMenu = 0x100, kCompetition = 0x200, kManager = 0x300, kPanel = 0x400, kTrack = 0x500;
  ScreenTracker screens;
  Check(screens.current() == GameScreen::kOther, "nothing is known at first");
  screens.NoteMovie("/ package/ChavoKart_MainMenu/ChavoKartMainMenu");
  screens.NoteCall(kMenu, 0x10, "ShowScreen");
  Check(screens.current() == GameScreen::kMainMenu, "the main menu");
  screens.NoteCall(kMenu, 0x10, "HideScreen");
  screens.NoteMovie("/ package/ChavoKart_CompetitionSelect/ChavoKartCompetitionSelect");
  screens.NoteCall(kCompetition, 0x10, "ShowScreen");
  Check(screens.current() == GameScreen::kCompetition, "CompetitionSelect comes straight after Free Race");

  screens.NoteCall(kCompetition, 0x10, "OnPressXButton");
  Check(!screens.TakeInvitePress(), "X does nothing on CompetitionSelect");

  screens.NoteCall(kCompetition, 0x10, "HideScreen");
  screens.NoteMovie("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager");
  screens.NoteCall(kManager, 0x10, "ShowScreen");
  screens.NoteMovie("/ package/ChavoKart_PlayerSelect/ChavoKartPlayerSelect");
  for (int panel = 0; panel < 4; ++panel) screens.NoteCall(kPanel, 0x10, "ShowScreen");
  Check(screens.current() == GameScreen::kCharacter, "PlayerSelectManager is the character screen, whatever its panels do");
  screens.NoteCall(kManager, 0x10, "OnPressAButton");
  Check(!screens.TakeInvitePress(), "A stays the game's");
  screens.NoteCall(kManager, 0x10, "OnPressXButton");
  Check(screens.TakeInvitePress(), "X opens Invite Player");
  Check(!screens.TakeInvitePress(), "one press, one picker");

  screens.NoteCall(kManager, 0x10, "HideScreen");
  screens.NoteMovie("/ package/ChavoKart_TrackSelect/ChavoKartTrackSelect");
  screens.NoteCall(kTrack, 0x10, "ShowScreen");
  Check(screens.current() == GameScreen::kTrack, "the track screen is not the lobby");
  screens.NoteCall(kTrack, 0x10, "OnPressXButton");
  Check(!screens.TakeInvitePress(), "X does nothing there");

  screens.NoteCall(kTrack, 0x10, "HideScreen");
  screens.NoteCall(kManager, 0x10, "ShowScreen");
  Check(screens.current() == GameScreen::kCharacter, "coming back needs no movie: the class is known for the whole run");
  screens.NoteMapChange();
  Check(screens.current() == GameScreen::kOther, "a race leaves the character screen without hiding it");

  screens.NoteCall(kManager, 0x10, "ShowScreen");
  screens.NoteCall(kManager, 0x10, "");
  Check(screens.current() == GameScreen::kCharacter, "a call with no literal method changes nothing");

  Check(ScreenOfMovie("/ package/ChavoKart_PlayerSelect/ChavoKartPlayerSelect") == GameScreen::kOther, "a panel is not the screen");
}

void TestPicker() {
  FakeApi api;
  api.snap = Session(2);
  api.snap.friends = {
      MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true),
      MakeFriend("CCCC-3333", "Beto", online::FriendState::kOnline, true),
      MakeFriend("DDDD-4444", "Caio", online::FriendState::kOffline, false),
      MakeFriend("EEEE-5555", "Dani", online::FriendState::kInGame, true),
      MakeFriend("FFFF-6666", "Edu", online::FriendState::kOnline, true, false),
      MakeFriend("GGGG-7777", "Fabi", online::FriendState::kHosting, true),
  };
  OnlineMenu menu(api, TextPackage::kInt, Page::kInvite);
  Check(menu.Title() == "Invite Player", "the picker's title");
  const std::vector<std::string> rows = Texts(menu);
  bool seated = false;
  for (const std::string& row : rows) seated = seated || row.find("Carlos") != std::string::npos;
  Check(!seated, "a seated friend is not listed");
  const auto enabled = [&](const std::string& name) {
    for (const Row& row : menu.Rows()) {
      if (row.text.rfind(name, 0) == 0) return row.enabled;
    }
    return false;
  };
  Check(enabled("Beto") && enabled("Fabi"), "free friends can be invited, one alone in their own session too");
  Check(!enabled("Caio") && !enabled("Dani") && !enabled("Edu"), "offline, busy and incompatible friends show their state");
  Check(Press(menu, Act::kInviteFriend) && api.last_invite == "CCCC-3333", "selecting a friend sends the invite");
  Check(menu.closed(), "and closes the picker: the player is back on the character screen");

  FakeApi refused;
  refused.snap = Session(1);
  refused.snap.friends = {MakeFriend("CCCC-3333", "Beto", online::FriendState::kOnline, true)};
  refused.invite_ok = false;
  OnlineMenu stays(refused, TextPackage::kInt, Page::kInvite);
  Press(stays, Act::kInviteFriend);
  Check(!stays.closed(), "a refused invite stays on the picker");

  FakeApi full;
  full.snap = Session(2);
  full.snap.invited = {Pending("Maria"), Pending("Caio")};
  full.snap.friends = {MakeFriend("CCCC-3333", "Beto", online::FriendState::kOnline, true)};
  OnlineMenu spent(full, TextPackage::kInt, Page::kInvite);
  spent.Update();
  Check(!Press(spent, Act::kInviteFriend) && full.invites_sent == 0, "no invite past the seats");

  FakeApi early;
  early.snap.connection = online::Connection::kChecking;
  OnlineMenu waiting(early, TextPackage::kInt, Page::kInvite);
  Check(Texts(waiting)[0] == "Connecting", "a service on its way is told");
  waiting.Back();
  Check(waiting.closed(), "and B goes back");
  FakeApi down;
  down.snap.connection = online::Connection::kError;
  OnlineMenu failed(down, TextPackage::kInt, Page::kInvite);
  Check(Texts(failed)[0] == "Unable to connect", "no service is told");

  FakeApi nobody;
  nobody.snap = Session(1);
  OnlineMenu lonely(nobody, TextPackage::kInt, Page::kInvite);
  Check(Texts(lonely)[0] == "No friends to invite", "no friends are told");
  Press(lonely, Act::kBack);
  Check(lonely.closed(), "Back leaves the picker");
}

void TestPickerCreatesNothing() {
  FakeApi api;
  api.answers_later = true;
  api.snap.friends = {MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true)};
  const online::RoomRules rules = {{"setup", 1}};
  Check(OnInvitePress(api.snap, true) == InvitePress::kPicker, "X opens the picker");
  {
    OnlineMenu looked(api, TextPackage::kInt, Page::kInvite, std::nullopt, false, rules);
    Check(Texts(looked)[0].rfind("Carlos", 0) == 0, "with no session, the friends are there");
    looked.Update();
    looked.Back();
    Check(looked.closed(), "Back closes it");
  }
  OnlineMenu again(api, TextPackage::kInt, Page::kInvite, std::nullopt, false, rules);
  again.Update();
  Check(api.invites_sent == 0, "looking at the friends sends nothing");

  Check(Press(again, Act::kInviteFriend) && api.invites_sent == 1, "choosing a friend starts the send");
  Check(api.last_rules == rules, "with the game the session has to carry");
  Check(!again.closed(), "the picker waits for the service");
  Check(Texts(again)[0] == "Carlos    Connecting", "and says so: " + Texts(again)[0]);
  again.Update();
  Check(!Press(again, Act::kInviteFriend) && api.invites_sent == 1, "a second press sends nothing");
  Check(SeatsLeft(api.snap) == online::kMaxSeats - 1, "and no seat is taken yet");

  api.snap = Session(1);
  api.snap.sending_invite = "BBBB-2222";
  api.snap.friends = {MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true)};
  again.Update();
  Check(!again.closed(), "the session alone is not the invite");
  api.snap.sending_invite.clear();
  api.snap.invited = {Pending("Carlos")};
  api.snap.invited[0].player = "BBBB-2222";
  Check(SeatsLeft(api.snap) == 2, "the service took it: one seat is held");
  again.Update();
  Check(again.closed(), "and the player is back on the character screen");
}

void TestInviteFailureRetries() {
  FakeApi api;
  api.answers_later = true;
  api.snap.friends = {MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true)};
  OnlineMenu menu(api, TextPackage::kInt, Page::kInvite, std::nullopt, false, online::RoomRules{});
  Press(menu, Act::kInviteFriend);
  api.snap.sending_invite.clear();
  api.snap.creating_session = false;
  api.snap.invite_outcome = {1, "Carlos", false};
  menu.Update();
  Check(!menu.closed(), "a failed send stays on the picker");
  Check(Texts(menu)[0] == "Carlos cannot be invited", "and tells why: " + Texts(menu)[0]);
  Check(Press(menu, Act::kInviteFriend) && api.invites_sent == 2, "the friend can be tried again");
  menu.Update();
  Check(Texts(menu)[0].rfind("Carlos    ", 0) == 0, "and the error goes away");

  FakeApi closed;
  closed.invite_ok = false;
  closed.snap = Session(1);
  closed.snap.friends = {MakeFriend("BBBB-2222", "Carlos", online::FriendState::kOnline, true)};
  OnlineMenu refused(closed, TextPackage::kInt, Page::kInvite);
  Press(refused, Act::kInviteFriend);
  Check(!refused.closed() && Texts(refused)[0] == "Carlos cannot be invited", "a send the service cannot start says the same");
}

void TestStatusNeverNamesTheTransport() {
  const std::string connected_int = "Carlos \xE2\x80\xA2 Connected";
  online::Snapshot state = Session(2);
  state.peers = {Peer(1, online::PeerPath::kDirect)};
  for (const TextPackage package : {TextPackage::kInt, TextPackage::kPtb, TextPackage::kEsm, TextPackage::kEsn}) {
    for (const online::PeerPath path : {online::PeerPath::kDirect, online::PeerPath::kRelay}) {
      state.peers[0].path = path;
      for (const std::string& line : StatusLines(state, package)) {
        for (const char* word : {"Direct", "Direta", "Directa", "Relay", "Retransmiss", "Retransmisi", "P2P"}) {
          Check(line.find(word) == std::string::npos, "the status never says how a player is reached: " + line);
        }
      }
    }
  }
  state.peers[0].path = online::PeerPath::kRelay;
  Check(StatusLines(state, TextPackage::kInt)[0] == connected_int, "English, on the relay");
  state.peers[0].path = online::PeerPath::kDirect;
  Check(StatusLines(state, TextPackage::kInt)[0] == connected_int, "Direct to Relay and back reads the same");
  Check(StatusLines(state, TextPackage::kPtb)[0] == "Carlos \xE2\x80\xA2 Conectado", "PT-BR");
  Check(StatusLines(state, TextPackage::kEsm)[0] == "Carlos \xE2\x80\xA2 Conectado", "Spanish");
  Check(StatusLines(state, TextPackage::kEsn)[0] == "Carlos \xE2\x80\xA2 Conectado", "Spanish, Spain");

  state.peers[0].path = online::PeerPath::kPending;
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connecting", "connecting");
  state.peers[0].path = online::PeerPath::kFailed;
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connection lost", "connection lost");
  state.room->seats[1].state = online::SeatState::kReconnecting;
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Reconnecting", "reconnecting");
  Check(StatusLines(state, TextPackage::kPtb)[0] == "Carlos \xE2\x80\xA2 Reconectando", "reconnecting, PT-BR");

  state = Session(3);
  state.seat = 1;
  state.peers = {Peer(0, online::PeerPath::kDirect)};
  state.room->seats[2].transport = online::PathKind::kRelay;
  for (const std::string& line : StatusLines(state, TextPackage::kInt)) {
    Check(line.find("Connected") != std::string::npos, "a seat only the host reaches reads the same");
  }
}

void TestLeaveMenu() {
  const auto guest = [] {
    online::Snapshot state = Session(2);
    state.seat = 1;
    return state;
  };
  FakeApi api;
  api.snap = guest();
  OnlineMenu resume(api, TextPackage::kInt, Page::kLeave);
  Check(Texts(resume) == std::vector<std::string>({"Resume", "Leave Multiplayer"}), "a guest's two choices");
  Check(resume.cursor() == 0, "and the safe one comes first");
  Check(Press(resume, Act::kResume) && resume.closed() && api.leaves == 0, "Resume goes back to the race");

  OnlineMenu back(api, TextPackage::kInt, Page::kLeave);
  back.Back();
  Check(back.closed() && api.leaves == 0, "and so does B");

  OnlineMenu leave(api, TextPackage::kInt, Page::kLeave);
  Check(Press(leave, Act::kLeave) && leave.closed(), "Leave Multiplayer");
  Check(api.leaves == 1 && api.last_leave == "guest_menu", "sends one leave");
  leave.Activate();
  Check(api.leaves == 1, "and a second press sends nothing");

  FakeApi host;
  host.snap = Session(2);
  OnlineMenu confirm(host, TextPackage::kInt, Page::kLeave);
  Check(Texts(confirm)[0] == "This ends the race for everyone", "a host is told what it means");
  Check(Press(confirm, Act::kLeave) && host.last_leave == "host_menu", "before leaving");

  FakeApi ended;
  ended.snap = guest();
  OnlineMenu open(ended, TextPackage::kInt, Page::kLeave);
  ended.snap.room.reset();
  open.Update();
  Check(open.closed() && ended.leaves == 0, "a session that ends under the menu closes it");

  Check(std::string(MenuText("Host left", TextPackage::kInt)) == "Host left", "Host left, INT");
  Check(std::string(MenuText("Host left", TextPackage::kPtb)) == "O host saiu", "Host left, PTB");
  Check(std::string(MenuText("Host left", TextPackage::kEsm)) == "El host sali\xC3\xB3", "Host left, ESM");
  Check(std::string(MenuText("Host left", TextPackage::kEsn)) == "El host sali\xC3\xB3", "Host left, ESN");
  Check(std::string(MenuText("Connection lost", TextPackage::kPtb)) == "Conex\xC3\xA3o perdida", "Connection lost, PTB");
  Check(std::string(MenuText("Connection lost", TextPackage::kEsm)) == "Conexi\xC3\xB3n perdida", "Connection lost, Spanish");
  Check(std::string(MenuText("Leave Multiplayer", TextPackage::kPtb)) == "Sair do multiplayer", "Leave Multiplayer, PTB");
  Check(std::string(MenuText("Resume", TextPackage::kPtb)) == "Continuar", "Resume, PTB");
  Check(std::string(MenuText("Leave Multiplayer", TextPackage::kEsm)) == "Salir del multijugador", "Leave Multiplayer, Spanish");
}

void TestSessionEndNotes() {
  const auto end = [](const std::string& reason) {
    SessionNotes notes(TextPackage::kInt);
    online::Snapshot state = Session(2);
    state.seat = 1;
    notes.Update(state, 1.0);
    state.room.reset();
    state.closed_reason = reason;
    notes.Update(state, 2.0);
    return notes.Toast(2.0);
  };
  Check(end("host_left") == "Host left", "the host left");
  Check(end("disconnect") == "Host left", "the service gave up on the host");
  Check(end("cancelled") == "Host left", "the host cancelled");
  Check(end("connection_lost") == "Connection lost", "this side lost the service");
  Check(end("").empty(), "a leave of this player's own tells nothing");

  // The note outlasts the trip to the menu.
  SessionNotes held(TextPackage::kInt);
  online::Snapshot state = Session(2);
  held.Update(state, 1.0);
  state.room.reset();
  state.closed_reason = "host_left";
  held.Update(state, 2.0);
  held.HoldToast(2.0);
  Check(held.Toast(2.0 + 60) == "Host left", "the load does not use it up");
  held.ReleaseToast(70.0);
  Check(held.Toast(71.0) == "Host left", "it starts once the menu is up");
  Check(held.Toast(70.0 + kToastSeconds + 1).empty(), "and ends like any other");
}

void TestRoleChangeIsNotALoss() {
  SessionNotes notes(TextPackage::kInt);
  online::Snapshot host = Session(1);
  notes.Update(host, 1.0);
  online::Snapshot replacing = host;
  replacing.room.reset();
  replacing.seat = -1;
  replacing.joining = true;
  notes.Update(replacing, 2.0);
  Check(notes.Toast(2.1).empty(), "leaving an own empty session for an invite is not a lost connection");
}

void TestInvitePrompt() {
  FakeApi api;
  api.snap.connection = online::Connection::kOnline;
  api.snap.invites = {MakeInvite("invite-1")};
  Check(CanTakeInvite(api.snap), "a free player takes an invite");
  OnlineMenu accept(api, TextPackage::kInt, Page::kIncoming, MakeInvite("invite-1"));
  Check(Texts(accept)[0] == "Saulo invited you to a Multiplayer race", "the prompt: " + Texts(accept)[0]);
  Check(accept.cursor() == 1, "Accept is highlighted");
  Check(Press(accept, Act::kAccept) && api.accepts == 1 && api.last_accept == "invite-1", "Accept joins that invite");
  Check(accept.closed(), "and nothing else opens");

  OnlineMenu decline(api, TextPackage::kInt, Page::kIncoming, MakeInvite("invite-1"));
  Press(decline, Act::kDecline);
  Check(api.declines == 1 && decline.closed(), "Decline leaves the player where they were");
  OnlineMenu back(api, TextPackage::kInt, Page::kIncoming, MakeInvite("invite-1"));
  back.Back();
  Check(api.declines == 2, "B declines too");

  OnlineMenu gone(api, TextPackage::kInt, Page::kIncoming, MakeInvite("invite-1"));
  api.snap.invites.clear();
  gone.Update();
  Check(gone.closed(), "an invite that ended closes its prompt");

  online::Snapshot alone = Session(1);
  alone.invites = {MakeInvite("x")};
  Check(CanTakeInvite(alone), "a host alone can take an invite");
  online::Snapshot busy = Session(2);
  busy.invites = {MakeInvite("x")};
  Check(!CanTakeInvite(busy), "a host with friends is not interrupted");
  online::Snapshot guest = Session(2, 1);
  guest.invites = {MakeInvite("x")};
  Check(!CanTakeInvite(guest), "a guest is not interrupted");
  online::Snapshot joining;
  joining.joining = true;
  joining.invites = {MakeInvite("x")};
  Check(!CanTakeInvite(joining), "nor a player already joining");
}

void TestStatus() {
  online::Snapshot state = Session(1);
  Check(StatusLines(state, TextPackage::kInt).empty(), "the host alone has no rows: the game's panels are the players");
  state.invited = {Pending("Carlos")};
  std::vector<std::string> lines = StatusLines(state, TextPackage::kInt);
  Check(lines.size() == 1 && lines[0] == "Carlos \xE2\x80\xA2 Invited", "one real invite, one line");
  Check(StatusLines(state, TextPackage::kPtb)[0] == "Carlos \xE2\x80\xA2 Convidado", "localized");
  state.invited.clear();
  Check(StatusLines(state, TextPackage::kInt).empty(), "a declined or expired invite takes its line away");

  state = Session(2);
  Check(StatusLines(state, TextPackage::kInt) == std::vector<std::string>({"Carlos \xE2\x80\xA2 Connecting"}), "accepted but not ready");
  state.peers = {Peer(1, online::PeerPath::kProbing)};
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connecting", "still probing");
  state.peers = {Peer(1, online::PeerPath::kDirect)};
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connected", "ready direct");
  state.peers = {Peer(1, online::PeerPath::kRelay)};
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connected", "ready relay");
  state.peers = {Peer(1, online::PeerPath::kFailed)};
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Connection lost", "a failed seat says so");
  state.room->seats[1].state = online::SeatState::kReconnecting;
  Check(StatusLines(state, TextPackage::kInt)[0] == "Carlos \xE2\x80\xA2 Reconnecting", "a reconnecting seat is marked");

  state = Session(4);
  state.peers = {Peer(1, online::PeerPath::kDirect), Peer(2, online::PeerPath::kRelay), Peer(3, online::PeerPath::kDirect)};
  Check(StatusLines(state, TextPackage::kInt) == std::vector<std::string>({"Carlos \xE2\x80\xA2 Connected", "Maria \xE2\x80\xA2 Connected", "Caio \xE2\x80\xA2 Connected"}),
        "direct and relay seats coexist");

  state = Session(3, 1);
  state.peers = {Peer(0, online::PeerPath::kDirect)};
  state.room->seats[2].transport = online::PathKind::kRelay;
  Check(StatusLines(state, TextPackage::kInt) == std::vector<std::string>({"Saulo \xE2\x80\xA2 Connected", "Maria \xE2\x80\xA2 Connected"}),
        "a guest sees the host's path and what the host reported for the others");

  online::Snapshot joining;
  joining.joining = true;
  Check(StatusLines(joining, TextPackage::kInt) == std::vector<std::string>({"Connecting"}), "joining is told");
}

void TestNotes() {
  SessionNotes notes(TextPackage::kInt);
  double now = 1.0;
  online::Snapshot state = Session(1);
  notes.Update(state, now);
  Check(notes.Toast(now).empty(), "nothing yet");

  state.invite_outcome = {1, "Carlos", true};
  notes.Update(state, ++now);
  Check(notes.Toast(now) == "Carlos declined", "a decline is told");
  state.invite_outcome = {2, "Beto", false};
  notes.Update(state, ++now);
  Check(notes.Toast(now) == "Beto cannot be invited", "an undelivered invite is told");
  Check(notes.Toast(now + kToastSeconds + 1).empty(), "and fades");

  state.room->seats[1] = Seated(1, "BBBB-2222", "Carlos");
  state.room->players = 2;
  notes.Update(state, ++now);
  Check(notes.Toast(now) == "Carlos joined", "a join is told");
  state.room->seats[1] = online::Seat{};
  state.room->seats[1].seat = 1;
  state.room->players = 1;
  notes.Update(state, ++now);
  Check(notes.Toast(now) == "Player left: Carlos", "a leave is told");

  now += 10;
  state.room.reset();
  state.closed_reason = "host_left";
  notes.Update(state, now);
  Check(notes.Toast(now) == "Host left", "the host leaving is told");

  SessionNotes quiet(TextPackage::kInt);
  online::Snapshot mine = Session(2);
  quiet.Update(mine, 1.0);
  quiet.NoteLeaving();
  mine.room.reset();
  quiet.Update(mine, 2.0);
  Check(quiet.Toast(2.0).empty(), "leaving on purpose raises no alarm");

  SessionNotes lost(TextPackage::kInt);
  online::Snapshot seat = Session(2, 1);
  lost.Update(seat, 1.0);
  seat.room.reset();
  seat.closed_reason = "no_path";
  lost.Update(seat, 2.0);
  Check(lost.Toast(2.0) == "Unable to connect", "no path to the host ends in a clear failure");

  SessionNotes join(TextPackage::kInt);
  online::Snapshot trying;
  trying.joining = true;
  join.Update(trying, 1.0);
  trying.joining = false;
  join.Update(trying, 2.0);
  Check(join.Toast(2.0) == "Unable to connect", "a refused join is told");
}

void TestNavigation() {
  FakeApi api;
  api.snap = Session(1);
  api.snap.friends = {MakeFriend("CCCC-3333", "Beto", online::FriendState::kOnline, true),
                      MakeFriend("DDDD-4444", "Caio", online::FriendState::kOffline, false),
                      MakeFriend("GGGG-7777", "Fabi", online::FriendState::kOnline, true)};
  OnlineMenu menu(api, TextPackage::kInt, Page::kInvite);
  const std::vector<Row> rows = menu.Rows();
  Check(rows[size_t(menu.cursor())].enabled, "the cursor starts on a real choice");
  const int first = menu.cursor();
  menu.Move(1);
  Check(rows[size_t(menu.cursor())].enabled && menu.cursor() != first, "down skips what cannot be chosen");
  for (size_t i = 0; i < rows.size(); ++i) menu.Move(1);
  Check(rows[size_t(menu.cursor())].enabled, "and wraps around");
}

void TestLanguages() {
  const TextPackage packages[] = {TextPackage::kInt, TextPackage::kEsm, TextPackage::kEsn, TextPackage::kPtb};
  for (const char* key : AllMenuKeys()) {
    for (const TextPackage package : packages) {
      const char* text = MenuText(key, package);
      Check(text && text[0], std::string("not empty: ") + key);
    }
    Check(std::strcmp(MenuText(key, TextPackage::kPtb), key) != 0 || std::strcmp(key, "Connecting") == 0,
          std::string("translated to PTB: ") + key);
  }
  Check(std::strcmp(MenuText("Invite Player", TextPackage::kInt), "Invite Player") == 0, "English");
  Check(std::strcmp(MenuText("Invite Player", TextPackage::kPtb), "Convidar jogador") == 0, "Portuguese");
  Check(std::strcmp(MenuText("Invite Player", TextPackage::kEsm), "Invitar jugador") == 0, "Spanish");
  Check(std::strcmp(MenuText(" joined", TextPackage::kEsm), MenuText(" joined", TextPackage::kEsn)) != 0, "ESM and ESN differ where the game's do");
  Check(std::strcmp(MenuText("Nothing like this", TextPackage::kEsm), "Nothing like this") == 0, "an unknown text is unchanged");
}

void TestOneOnlinePath() {
  const char* banned[] = {"Create Room", "Find Room", "Room Browser", "Room Capacity", "Refresh Rooms", "Leave Room",
                          "Room full", "Join Room", "No rooms", "2 Players", "3 Players", "4 Players", "sala", "Sala",
                          "Start Race", "Waiting for players", "\"Local\"", "Local/Online", "Player 1", "participant"};
  for (const char* source : {"online_menu.h", "online_text.h", "online_menu_overlay.cpp", "online_character_select.h"}) {
    const std::string text = ReadSource(source);
    Check(!text.empty(), std::string("source found: ") + source);
    for (const char* word : banned) {
      if (std::strcmp(source, "online_text.h") == 0 && std::strcmp(word, "Waiting for players") == 0) continue;
      Check(text.find(word) == std::string::npos, std::string(source) + " has no " + word);
    }
  }
  Check(ReadSource("main_menu.h").find("ECK_ONLINE_ENTER") == std::string::npos, "Free Race sends nothing to the port");
  Check(ReadSource("main_menu_hooks.cpp").find("RequestOnlineMenu") == std::string::npos, "and opens nothing");
  const TextPackage packages[] = {TextPackage::kInt, TextPackage::kEsm, TextPackage::kEsn, TextPackage::kPtb};
  for (const char* key : AllMenuKeys()) {
    for (const TextPackage package : packages) {
      const std::string text = MenuText(key, package);
      for (const char* word : {"Room", "room", "sala", "Sala", "Session", "session"}) {
        Check(text.find(word) == std::string::npos, "no room or session wording: " + text);
      }
    }
  }
  const std::string overlay = ReadSource("online_menu_overlay.cpp");
  Check(overlay.find("IsMouseReleased") != std::string::npos, "a row is activated by the mouse or by Input(), never by both");
}


}  // namespace

int main() {
  TestShortcut();
  TestStatusWithoutService();
  TestPickerWithoutSetup();
  TestScreenTracking();
  TestPicker();
  TestPickerCreatesNothing();
  TestInviteFailureRetries();
  TestRoleChangeIsNotALoss();
  TestLeaveMenu();
  TestStatusNeverNamesTheTransport();
  TestSessionEndNotes();
  TestInvitePrompt();
  TestStatus();
  TestNotes();
  TestNavigation();
  TestLanguages();
  TestOneOnlinePath();
  return online_test::Finish();
}
