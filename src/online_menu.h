#pragma once

// The Online part of Free Race / Multiplayer as a model: pages, rows, navigation and reactions to the service's state.
// Pure (no ImGui, no guest memory); online_menu_overlay.cpp draws it and feeds it the game's menu input.

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "online/online_service.h"
#include "online_start_race.h"
#include "online_text.h"

namespace eck::menu {

// Strings of the room screens that online_text.h does not have. Same languages: INT, ESM, ESN, PTB.
inline constexpr OnlineString kMenuStrings[] = {
    {"Local", "Local", "Local", "Local"},
    {"Players", "Jugadores", "Jugadores", "Jogadores"},
    {"Player", "Jugador", "Jugador", "Jogador"},
    {"2 Players", "2 jugadores", "2 jugadores", "2 jogadores"},
    {"3 Players", "3 jugadores", "3 jugadores", "3 jogadores"},
    {"4 Players", "4 jugadores", "4 jugadores", "4 jogadores"},
    {"Host", "Anfitri\xC3\xB3n", "Anfitri\xC3\xB3n", "Anfitri\xC3\xA3o"},
    {"You", "T\xC3\xBA", "T\xC3\xBA", "Voc\xC3\xAA"},
    {"Invited", "Invitado", "Invitado", "Convidado"},
    {"Host left", "El anfitri\xC3\xB3n se fue", "El anfitri\xC3\xB3n se ha marchado", "O anfitri\xC3\xA3o saiu"},
    {"Back", "Volver", "Volver", "Voltar"},
    {"Connecting", "Conectando", "Conectando", "Conectando"},
    {"No rooms found", "No hay salas", "No hay salas", "Nenhuma sala encontrada"},
    {"Refresh", "Actualizar", "Actualizar", "Atualizar"},
    {"Invite sent", "Invitaci\xC3\xB3n enviada", "Invitaci\xC3\xB3n enviada", "Convite enviado"},
    {"Race starting", "La carrera va a empezar", "La carrera va a comenzar", "Corrida iniciando"},
    {"Race not available yet", "Carrera a\xC3\xBAn no disponible", "Carrera a\xC3\xBAn no disponible",
     "Corrida ainda indispon\xC3\xADvel"},
    {" invited you to a Multiplayer race", " te invit\xC3\xB3 a una carrera multijugador",
     " te ha invitado a una carrera multijugador", " convidou voc\xC3\xAA para uma corrida multiplayer"},
    {" joined", " se uni\xC3\xB3", " se ha unido", " entrou"},
    {"Need 2 players", "Se necesitan 2 jugadores", "Se necesitan 2 jugadores", "Precisa de 2 jogadores"},
    {"No friends to invite", "No hay amigos para invitar", "No hay amigos para invitar", "Nenhum amigo para convidar"},
    {"Waiting for the host", "Esperando al anfitri\xC3\xB3n", "Esperando al anfitri\xC3\xB3n", "Aguardando o anfitri\xC3\xA3o"},
};

inline const char* MenuText(const char* english, TextPackage package) {
  for (const OnlineString& row : kMenuStrings) {
    if (std::strcmp(row.key, english) != 0) continue;
    switch (package) {
      case TextPackage::kEsm: return row.esm;
      case TextPackage::kEsn: return row.esn;
      case TextPackage::kPtb: return row.ptb;
      default: return row.key;
    }
  }
  return OnlineText(english, package);
}

// Every string the menu shows, so a test can prove each language is complete.
inline std::vector<const char*> AllMenuKeys() {
  std::vector<const char*> keys;
  for (const OnlineString& row : kMenuStrings) keys.push_back(row.key);
  for (const char* key : {"Online", "Create Room", "Find Room", "Invite Friends", "Start Race", "Leave Room",
                          "Waiting for players", "Room full", "Player left", "Connection lost", "Unable to connect",
                          "Version mismatch", "Accept", "Decline"}) {
    keys.push_back(key);
  }
  return keys;
}

// The abstraction over the service, so the model runs headless against a script.
class Api {
 public:
  virtual ~Api() = default;
  virtual online::Snapshot Snapshot() = 0;
  virtual bool CreateRoom(int capacity, const online::RoomRules& rules) = 0;
  virtual void SearchRooms() = 0;
  virtual bool JoinRoom(const std::string& handle) = 0;
  virtual bool StartRace() = 0;
  virtual void LeaveRoom() = 0;
  virtual bool SendInvite(const std::string& player) = 0;
  virtual bool AcceptInvite(const std::string& invite) = 0;
  virtual void DeclineInvite(const std::string& invite) = 0;
};

class ServiceApi final : public Api {
 public:
  explicit ServiceApi(online::OnlineService& service) : service_(service) {}
  online::Snapshot Snapshot() override { return service_.snapshot(); }
  bool CreateRoom(int capacity, const online::RoomRules& rules) override { return service_.CreateRoom(capacity, rules); }
  void SearchRooms() override { service_.SearchRooms(); }
  bool JoinRoom(const std::string& handle) override { return service_.JoinRoom(handle); }
  bool StartRace() override { return service_.StartRace(); }
  void LeaveRoom() override { service_.LeaveRoom(); }
  bool SendInvite(const std::string& player) override { return service_.SendInvite(player); }
  bool AcceptInvite(const std::string& invite) override { return service_.AcceptInvite(invite); }
  void DeclineInvite(const std::string& invite) override { service_.DeclineInvite(invite); }

 private:
  online::OnlineService& service_;
};

enum class Page { kMode, kOnline, kCreate, kFind, kRoom, kInvite, kIncoming };
enum class Act {
  kNone, kLocal, kOnline, kCreateRoom, kFindRoom, kPlayers, kCreate, kRefresh, kJoin, kInviteFriends, kStartRace,
  kLeave, kBack, kInviteFriend, kAccept, kDecline
};
enum class Outcome { kNone, kLocal, kClose };

struct Row {
  std::string text;
  Act act = Act::kNone;  // kNone: a line of information, not selectable
  int arg = 0;
  bool enabled = true;
};

inline constexpr double kToastSeconds = 4.0;
inline constexpr double kInviteMarkerSeconds = 60.0;
inline constexpr double kSearchThrottleSeconds = 2.0;
inline constexpr double kJoiningSeconds = 8.0;

// The rules a new room carries until the track and mode come from the game's own selection screens.
inline online::RoomRules DefaultRules() { return {{"laps", 3}, {"items", 1}}; }

class OnlineMenu {
 public:
  using StartFn = std::function<online_start::Verdict(const online_start::Plan&)>;

  OnlineMenu(Api& api, TextPackage package, StartFn start) : api_(api), package_(package), start_(std::move(start)) {
    snap_ = api_.Snapshot();
    page_ = snap_.room ? Page::kRoom : Page::kMode;
  }

  // The page after the invite and room rules: an incoming invite wins while this player has no room.
  Page page() const { return !snap_.room && !snap_.invites.empty() && page_ != Page::kRoom ? Page::kIncoming : page_; }
  int capacity() const { return capacity_; }
  int cursor() const { return cursor_; }
  const std::string& race_note() const { return race_note_; }
  Outcome TakeOutcome() {
    const Outcome out = outcome_;
    outcome_ = Outcome::kNone;
    return out;
  }
  bool HasIncomingInvite() const { return !snap_.room && !snap_.invites.empty(); }
  const online::Snapshot& snapshot() const { return snap_; }

  std::string Toast(double now) const { return now < toast_until_ ? toast_ : std::string(); }

  const char* Tx(const char* english) const { return MenuText(english, package_); }

  std::string Title() const {
    switch (page()) {
      case Page::kMode: return "Free Race / Multiplayer";
      case Page::kOnline: return Tx("Online");
      case Page::kCreate: return Tx("Create Room");
      case Page::kFind: return Tx("Find Room");
      case Page::kRoom: return Tx("Online");
      case Page::kInvite: return Tx("Invite Friends");
      case Page::kIncoming: return Tx("Online");
    }
    return {};
  }

  std::vector<Row> Rows() const {
    std::vector<Row> rows;
    const bool connected = snap_.connection == online::Connection::kOnline;
    switch (page()) {
      case Page::kMode:
        rows.push_back({Tx("Local"), Act::kLocal});
        rows.push_back({Tx("Online"), Act::kOnline});
        break;
      case Page::kOnline:
        if (!connected || joining_) {
          rows.push_back({snap_.connection == online::Connection::kChecking || joining_ ? Tx("Connecting")
                                                                                         : Tx("Unable to connect"),
                          Act::kNone});
        }
        rows.push_back({Tx("Create Room"), Act::kCreateRoom, 0, connected && !joining_});
        rows.push_back({Tx("Find Room"), Act::kFindRoom, 0, connected && !joining_});
        rows.push_back({Tx("Back"), Act::kBack});
        break;
      case Page::kCreate:
        rows.push_back({Tx("Players"), Act::kNone});
        rows.push_back({Tx(capacity_ == 2 ? "2 Players" : capacity_ == 3 ? "3 Players" : "4 Players"), Act::kPlayers});
        rows.push_back({Tx("Create Room"), Act::kCreate, 0, connected});
        rows.push_back({Tx("Back"), Act::kBack});
        break;
      case Page::kFind:
        FindRows(rows);
        break;
      case Page::kRoom:
        RoomRows(rows);
        break;
      case Page::kInvite:
        InviteRows(rows);
        break;
      case Page::kIncoming: {
        const online::Invite& invite = snap_.invites.front();
        rows.push_back({invite.host_name + Tx(" invited you to a Multiplayer race"), Act::kNone});
        rows.push_back({Tx("Accept"), Act::kAccept});
        rows.push_back({Tx("Decline"), Act::kDecline});
        break;
      }
    }
    return rows;
  }

  void SetCursor(int index) { cursor_ = index; }

  void Move(int delta) {
    const std::vector<Row> rows = Rows();
    if (rows.empty()) return;
    int at = cursor_;
    for (size_t step = 0; step < rows.size(); ++step) {
      at = (at + delta + int(rows.size())) % int(rows.size());
      if (rows[size_t(at)].act != Act::kNone && rows[size_t(at)].enabled) {
        cursor_ = at;
        return;
      }
    }
  }

  // Left or right on the Players selector.
  void Adjust(int delta) {
    const std::vector<Row> rows = Rows();
    if (cursor_ >= int(rows.size()) || rows[size_t(cursor_)].act != Act::kPlayers) return;
    capacity_ = std::clamp(capacity_ + delta, online::kMinSeats, online::kMaxSeats);
  }

  void Activate() {
    const std::vector<Row> rows = Rows();
    if (cursor_ < 0 || cursor_ >= int(rows.size())) return;
    const Row row = rows[size_t(cursor_)];
    if (row.act == Act::kNone || !row.enabled) return;
    switch (row.act) {
      case Act::kLocal: outcome_ = Outcome::kLocal; break;
      case Act::kOnline: Go(Page::kOnline); break;
      case Act::kCreateRoom: Go(Page::kCreate); break;
      case Act::kFindRoom:
        rooms_.clear();
        shown_generation_ = -1;
        Go(Page::kFind);
        Search();
        break;
      case Act::kPlayers: capacity_ = capacity_ >= online::kMaxSeats ? online::kMinSeats : capacity_ + 1; break;
      case Act::kCreate:
        if (!api_.CreateRoom(capacity_, DefaultRules())) Say(Tx("Unable to connect"));
        break;
      case Act::kRefresh: Search(); break;
      case Act::kJoin:
        if (!api_.JoinRoom(row_handles_.at(size_t(row.arg)))) Say(Tx("Unable to connect"));
        break;
      case Act::kInviteFriends: Go(Page::kInvite); break;
      case Act::kInviteFriend: Invite(row.arg); break;
      case Act::kStartRace:
        if (!api_.StartRace()) Say(Tx("Unable to connect"));
        break;
      case Act::kLeave:
        left_by_me_ = true;
        api_.LeaveRoom();
        Go(Page::kOnline);
        break;
      case Act::kBack: Back(); break;
      case Act::kAccept:
        joining_ = true;
        joining_until_ = now_ + kJoiningSeconds;
        api_.AcceptInvite(snap_.invites.front().id);
        snap_.invites.erase(snap_.invites.begin());
        Go(Page::kOnline);
        break;
      case Act::kDecline:
        api_.DeclineInvite(snap_.invites.front().id);
        snap_.invites.erase(snap_.invites.begin());
        break;
      case Act::kNone: break;
    }
    ClampCursor();
  }

  void Back() {
    switch (page()) {
      case Page::kMode: outcome_ = Outcome::kClose; break;
      case Page::kOnline: Go(Page::kMode); break;
      case Page::kCreate:
      case Page::kFind: Go(Page::kOnline); break;
      case Page::kRoom: outcome_ = Outcome::kClose; break;
      case Page::kInvite: Go(Page::kRoom); break;
      case Page::kIncoming:
        cursor_ = 2;
        Activate();
        break;
    }
  }

  // Once per frame. Reacts to what the service reports, not to a timer.
  void Update(double now) {
    now_ = now;
    const online::Snapshot next = api_.Snapshot();
    Observe(snap_, next);
    snap_ = next;
    if (joining_ && (snap_.room || now > joining_until_)) joining_ = false;
    invited_.erase(std::remove_if(invited_.begin(), invited_.end(), [&](const Invited& i) { return now > i.until; }),
                   invited_.end());
    if (page_ == Page::kFind && snap_.search_generation != shown_generation_) RebuildRooms();
    ClampCursor();
  }

 private:
  struct Invited {
    std::string player;
    std::string name;
    double until = 0;
  };

  void Go(Page page) {
    page_ = page;
    cursor_ = 0;
    ClampCursor(true);
  }

  void ClampCursor(bool first = false) {
    const std::vector<Row> rows = Rows();
    if (rows.empty()) {
      cursor_ = 0;
      return;
    }
    cursor_ = std::clamp(cursor_, 0, int(rows.size()) - 1);
    if (rows[size_t(cursor_)].act == Act::kNone || (!rows[size_t(cursor_)].enabled && !first)) {
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].act != Act::kNone && rows[i].enabled) {
          cursor_ = int(i);
          return;
        }
      }
    }
  }

  void Say(const std::string& text) {
    toast_ = text;
    toast_until_ = now_ + kToastSeconds;
  }

  void Search() {
    if (now_ - last_search_ < kSearchThrottleSeconds && last_search_ >= 0) return;
    last_search_ = now_;
    api_.SearchRooms();
  }

  void RebuildRooms() {
    shown_generation_ = snap_.search_generation;
    rooms_ = snap_.rooms;
  }

  void FindRows(std::vector<Row>& rows) const {
    row_handles_.clear();
    int shown = 0;
    for (const online::RoomAd& ad : rooms_) {
      const online::Friend* host = nullptr;
      for (const online::Friend& f : snap_.friends) {
        if (f.player == ad.host) host = &f;
      }
      const bool mismatch = host && !host->compatible;
      std::string text = ad.host_name + " " + std::to_string(ad.players) + "/" + std::to_string(ad.capacity);
      if (mismatch) text += std::string(" - ") + Tx("Version mismatch");
      else if (ad.full()) text += std::string(" - ") + Tx("Room full");
      row_handles_.push_back(ad.handle);
      rows.push_back({text, Act::kJoin, shown++, !mismatch && !ad.full()});
    }
    if (rooms_.empty()) rows.push_back({Tx("No rooms found"), Act::kNone});
    rows.push_back({Tx("Refresh"), Act::kRefresh});
    rows.push_back({Tx("Back"), Act::kBack});
  }

  bool Seated(const std::string& player) const {
    if (!snap_.room) return false;
    return std::any_of(snap_.room->seats.begin(), snap_.room->seats.end(), [&](const online::Seat& s) {
      return s.state != online::SeatState::kEmpty && s.player == player;
    });
  }

  const Invited* InvitedEntry(const std::string& player) const {
    for (const Invited& i : invited_) {
      if (i.player == player) return &i;
    }
    return nullptr;
  }

  void RoomRows(std::vector<Row>& rows) const {
    if (!snap_.room) return;
    const online::RoomView& room = *snap_.room;
    const bool open = room.state == online::RoomState::kOpen;
    const bool host = snap_.seat == 0;
    size_t invited_at = 0;
    for (const online::Seat& seat : room.seats) {
      std::string line = std::string(Tx("Player")) + " " + std::to_string(seat.seat + 1) + ": ";
      if (seat.state == online::SeatState::kEmpty) {
        if (open && invited_at < invited_.size()) {
          line += invited_[invited_at++].name + " (" + Tx("Invited") + ")";
        } else {
          line += Tx("Waiting for players");
        }
      } else {
        std::vector<std::string> tags;
        if (seat.seat == 0) tags.push_back(Tx("Host"));
        if (seat.seat == snap_.seat) tags.push_back(Tx("You"));
        if (seat.state == online::SeatState::kReconnecting) tags.push_back(Tx("Connection lost"));
        line += seat.name;
        for (size_t i = 0; i < tags.size(); ++i) line += (i ? ", " : " (") + tags[i];
        if (!tags.empty()) line += ")";
      }
      rows.push_back({line, Act::kNone});
    }
    if (snap_.reconnecting) rows.push_back({Tx("Connection lost"), Act::kNone});
    else if (!open) rows.push_back({race_note_.empty() ? std::string(Tx("Race starting")) : race_note_, Act::kNone});
    else if (room.players >= room.capacity) rows.push_back({Tx("Room full"), Act::kNone});
    else rows.push_back({Tx("Waiting for players"), Act::kNone});
    if (host) {
      rows.push_back({Tx("Invite Friends"), Act::kInviteFriends, 0, open && room.players < room.capacity});
      rows.push_back({room.players < online::kMinSeats && open
                          ? std::string(Tx("Start Race")) + " (" + Tx("Need 2 players") + ")"
                          : std::string(Tx("Start Race")),
                      Act::kStartRace, 0, open && room.players >= online::kMinSeats});
    } else if (open) {
      rows.push_back({Tx("Waiting for the host"), Act::kNone});
    }
    rows.push_back({Tx("Leave Room"), Act::kLeave});
  }

  void InviteRows(std::vector<Row>& rows) const {
    friend_codes_.clear();
    const bool full = snap_.room && snap_.room->players >= snap_.room->capacity;
    if (full) rows.push_back({Tx("Room full"), Act::kNone});
    int shown = 0;
    for (const online::Friend& f : snap_.friends) {
      const bool free = f.online && f.compatible && f.state == online::FriendState::kOnline && !Seated(f.player);
      if (!free) continue;
      const bool invited = InvitedEntry(f.player) != nullptr;
      friend_codes_.push_back(f.player);
      rows.push_back({f.name + (invited ? std::string(" (") + Tx("Invited") + ")" : std::string()), Act::kInviteFriend,
                      shown++, !full && !invited});
    }
    if (shown == 0 && !full) rows.push_back({Tx("No friends to invite"), Act::kNone});
    rows.push_back({Tx("Back"), Act::kBack});
  }

  void Invite(int index) {
    if (index < 0 || size_t(index) >= friend_codes_.size()) return;
    const std::string& code = friend_codes_[size_t(index)];
    std::string name;
    for (const online::Friend& f : snap_.friends) {
      if (f.player == code) name = f.name;
    }
    if (api_.SendInvite(code)) {
      invited_.push_back({code, name, now_ + kInviteMarkerSeconds});
      Say(Tx("Invite sent"));
    } else {
      Say(Tx("Unable to connect"));
    }
  }

  void Observe(const online::Snapshot& before, const online::Snapshot& after) {
    if (!before.room && after.room) {
      page_ = Page::kRoom;
      cursor_ = 0;
      left_by_me_ = false;
      race_note_.clear();
      started_ = false;
      invited_.clear();
    } else if (before.room && !after.room) {
      if (page_ == Page::kRoom || page_ == Page::kInvite) page_ = Page::kOnline;
      if (!left_by_me_) {
        const bool host_left = after.closed_reason == "host_left" || after.closed_reason == "cancelled";
        Say(host_left ? Tx("Host left") : Tx("Connection lost"));
      }
      left_by_me_ = false;
      invited_.clear();
      race_note_.clear();
      started_ = false;
      cursor_ = 0;
    } else if (before.room && after.room) {
      ObserveSeats(*before.room, *after.room, after.seat);
      if (before.reconnecting != after.reconnecting && after.reconnecting) Say(Tx("Connection lost"));
      if (!started_ && after.room->state != online::RoomState::kOpen) OnStarted(after);
    }
    if (!before.room && after.room && after.room->state != online::RoomState::kOpen) OnStarted(after);
  }

  void ObserveSeats(const online::RoomView& was, const online::RoomView& now, int mine) {
    for (const online::Seat& seat : now.seats) {
      if (seat.seat == mine || size_t(seat.seat) >= was.seats.size()) continue;
      const online::Seat& old = was.seats[size_t(seat.seat)];
      if (old.state != online::SeatState::kEmpty && seat.state == online::SeatState::kEmpty) {
        Say(std::string(Tx("Player left")) + ": " + old.name);
      } else if (old.state == online::SeatState::kEmpty && seat.state != online::SeatState::kEmpty) {
        Say(seat.name + Tx(" joined"));
        invited_.erase(std::remove_if(invited_.begin(), invited_.end(),
                                      [&](const Invited& i) { return i.player == seat.player; }),
                       invited_.end());
      }
    }
  }

  // The host pressed Start Race: the room left "open" for every seat, and every PC maps the same seats to karts.
  void OnStarted(const online::Snapshot& state) {
    started_ = true;
    const online_start::Plan plan = online_start::MakePlan(*state.room, state.seat);
    const online_start::Verdict verdict = start_(plan);
    race_note_ = verdict.real_entry ? Tx("Race starting") : Tx("Race not available yet");
    Say(race_note_);
  }

  Api& api_;
  TextPackage package_;
  StartFn start_;
  online::Snapshot snap_;
  Page page_ = Page::kMode;
  int cursor_ = 0;
  int capacity_ = online::kMaxSeats;
  double now_ = 0;
  Outcome outcome_ = Outcome::kNone;
  std::string toast_;
  double toast_until_ = 0;
  std::string race_note_;
  bool left_by_me_ = false;
  bool started_ = false;
  bool joining_ = false;
  double joining_until_ = 0;
  double last_search_ = -1;
  int shown_generation_ = -1;
  std::vector<online::RoomAd> rooms_;
  std::vector<Invited> invited_;
  mutable std::vector<std::string> row_handles_;
  mutable std::vector<std::string> friend_codes_;
};

}  // namespace eck::menu
