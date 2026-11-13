#pragma once

// Online on the game's own character screen as a model: the Invite Player shortcut, the friend picker, the invite prompt
// and the compact status. Pure (no ImGui, no guest memory); online_menu_overlay.cpp draws it.

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "online/friend_view.h"
#include "online/online_service.h"
#include "online_text.h"

namespace eck::menu {

// Strings of the Online overlays that online_text.h does not have. Same languages: INT, ESM, ESN, PTB.
inline constexpr OnlineString kMenuStrings[] = {
    {"Invited", "Invitado", "Invitado", "Convidado"},
    {"Host left", "El host sali\xC3\xB3", "El host sali\xC3\xB3", "O host saiu"},
    {"Connected", "Conectado", "Conectado", "Conectado"},
    {"Reconnecting", "Reconectando", "Reconectando", "Reconectando"},
    {"Waiting for a player", "Esperando a un jugador", "Esperando a un jugador", "Aguardando um jogador"},
    {"Resume", "Continuar", "Continuar", "Continuar"},
    {"Leave Multiplayer", "Salir del multijugador", "Salir del multijugador", "Sair do multiplayer"},
    {"This ends the race for everyone", "Esto termina la carrera para todos", "Esto termina la carrera para todos",
     "Isso encerra a corrida para todos"},
    {"Back", "Volver", "Volver", "Voltar"},
    {"Connecting", "Conectando", "Conectando", "Conectando"},
    {" invited you to a Multiplayer race", " te invit\xC3\xB3 a una carrera multijugador",
     " te ha invitado a una carrera multijugador", " convidou voc\xC3\xAA para uma corrida multiplayer"},
    {" joined", " se uni\xC3\xB3", " se ha unido", " entrou"},
    {" declined", " rechaz\xC3\xB3", " ha rechazado", " recusou"},
    {" cannot be invited", " no se puede invitar", " no se puede invitar", " n\xC3\xA3o pode ser convidado"},
    {"No friends to invite", "No hay amigos para invitar", "No hay amigos para invitar", "Nenhum amigo para convidar"},
    {"Connecting to online service", "Conectando al servicio en l\xC3\xADnea", "Conectando al servicio en l\xC3\xADnea",
     "Conectando ao servi\xC3\xA7o online"},
    {"Game setup unavailable", "Configuraci\xC3\xB3n de partida no disponible", "Configuraci\xC3\xB3n de partida no disponible",
     "Configura\xC3\xA7\xC3\xA3o da partida indispon\xC3\xADvel"},
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

// Every string the overlays show, so a test can prove each language is complete.
inline std::vector<const char*> AllMenuKeys() {
  std::vector<const char*> keys;
  for (const OnlineString& row : kMenuStrings) keys.push_back(row.key);
  for (const char* key : {"Invite Player", "Player left", "Connection lost", "Unable to connect", "Accept", "Decline"}) {
    keys.push_back(key);
  }
  return keys;
}

// The abstraction over the service, so the model runs headless against a script.
class Api {
 public:
  virtual ~Api() = default;
  virtual online::Snapshot Snapshot() = 0;
  virtual bool SendInvite(const std::string& player, const std::optional<online::RoomRules>& session_rules) = 0;
  virtual bool AcceptInvite(const std::string& invite) = 0;
  virtual void DeclineInvite(const std::string& invite) = 0;
  virtual void LeaveSession(const char* source) = 0;
};

class ServiceApi final : public Api {
 public:
  explicit ServiceApi(online::OnlineService& service) : service_(service) {}
  online::Snapshot Snapshot() override { return service_.snapshot(); }
  bool SendInvite(const std::string& player, const std::optional<online::RoomRules>& session_rules) override {
    return service_.SendInvite(player, session_rules);
  }
  bool AcceptInvite(const std::string& invite) override { return service_.AcceptInvite(invite); }
  void DeclineInvite(const std::string& invite) override { service_.DeclineInvite(invite); }
  void LeaveSession(const char* source) override { service_.LeaveSession(source); }

 private:
  online::OnlineService& service_;
};

inline constexpr double kToastSeconds = 4.0;

// Seated players and invites the service took both hold a seat; the rest can still be invited.
inline int SeatsLeft(const online::Snapshot& state) {
  if (!state.room) return online::kMaxSeats - 1;
  return state.room->capacity - state.room->players - int(state.invited.size());
}

struct InviteShortcut {
  bool visible = false;
  int remaining = 0;
  const char* hidden_because = "none";
};

// Off, or anywhere but the character screen, there is nothing: the game is the original game. On that screen only the
// session's own rules hide it, so nothing about the service or the game's objects can take it away.
inline InviteShortcut ShortcutFor(bool online_play, bool character_screen, const online::Snapshot& state) {
  if (!online_play) return {false, 0, "online_play_off"};
  if (!character_screen) return {false, 0, "not_character_select"};
  if (state.room && state.seat != 0) return {false, 0, "guest"};
  const int remaining = SeatsLeft(state);
  if (remaining <= 0) return {false, 0, "session_full"};
  return {true, remaining};
}

enum class InvitePress { kConnecting, kPicker, kPickerWithoutSetup };

// What X does. Without a game setup a guest would have nothing to enter, so the picker shows why it cannot send.
inline InvitePress OnInvitePress(const online::Snapshot& state, bool setup_readable) {
  if (state.connection != online::Connection::kOnline || state.joining) return InvitePress::kConnecting;
  const bool needs_session = !state.room && !state.hosting;
  return needs_session && !setup_readable ? InvitePress::kPickerWithoutSetup : InvitePress::kPicker;
}

// A player alone in their own hidden session is free; anybody else in a session is not interrupted.
inline bool CanTakeInvite(const online::Snapshot& state) {
  if (state.invites.empty() || state.joining) return false;
  if (!state.room) return true;
  return state.seat == 0 && state.room->state == online::RoomState::kOpen && state.room->players <= 1;
}

// Whether a seat is there, never how it is reached: a guest only has a path to the host, and shows what the host reported for
// the others.
inline std::string SeatStatusText(const online::Seat& seat, const online::Snapshot& state, TextPackage package) {
  if (seat.state == online::SeatState::kReconnecting) return MenuText("Reconnecting", package);
  for (const online::PeerSnapshot& peer : state.peers) {
    if (peer.seat != seat.seat) continue;
    switch (peer.path) {
      case online::PeerPath::kDirect:
      case online::PeerPath::kRelay: return MenuText("Connected", package);
      case online::PeerPath::kFailed: return MenuText("Connection lost", package);
      default: return MenuText("Connecting", package);
    }
  }
  const bool settled = seat.transport == online::PathKind::kDirect || seat.transport == online::PathKind::kRelay;
  return MenuText(settled ? "Connected" : "Connecting", package);
}

inline constexpr char kStatusSeparator[] = " \xE2\x80\xA2 ";

// "Carlos \xE2\x80\xA2 Connected": the other real players and invites, never the seats nobody is in.
inline std::vector<std::string> StatusLines(const online::Snapshot& state, TextPackage package) {
  std::vector<std::string> lines;
  if (state.joining) lines.push_back(MenuText("Connecting", package));
  if (!state.room) return lines;
  for (const online::Seat& seat : state.room->seats) {
    if (seat.state == online::SeatState::kEmpty || seat.seat == state.seat) continue;
    lines.push_back(seat.name + kStatusSeparator + SeatStatusText(seat, state, package));
  }
  for (const online::SentInvite& sent : state.invited) {
    lines.push_back(sent.name + kStatusSeparator + MenuText("Invited", package));
  }
  return lines;
}

// Short notes of what just happened to the session. It reacts to what the service reports, not to a timer.
class SessionNotes {
 public:
  explicit SessionNotes(TextPackage package) : package_(package) {}

  void Update(const online::Snapshot& next, double now) {
    now_ = now;
    if (seen_) Observe(last_, next);
    last_ = next;
    seen_ = true;
  }

  // The local player left on purpose: nothing to tell.
  void NoteLeaving() { leaving_ = true; }

  void NoteStatus(const char* english, double now) {
    now_ = now;
    Say(Tx(english));
  }

  std::string Toast(double now) const { return held_ || now < until_ ? toast_ : std::string(); }

  // A note about a session the game is only now travelling away from: it starts counting once the menu is up.
  void HoldToast(double now) { held_ = now < until_; }
  void ReleaseToast(double now) {
    if (held_) until_ = now + kToastSeconds;
    held_ = false;
  }

 private:
  const char* Tx(const char* english) const { return MenuText(english, package_); }

  void Say(const std::string& text) {
    toast_ = text;
    until_ = now_ + kToastSeconds;
  }

  void Observe(const online::Snapshot& before, const online::Snapshot& after) {
    if (after.invite_outcome.generation != before.invite_outcome.generation) {
      Say(after.invite_outcome.name + Tx(after.invite_outcome.declined ? " declined" : " cannot be invited"));
    }
    const bool same_session = before.room && after.room && before.room->room == after.room->room;
    if (before.joining && !after.joining && (!after.room || same_session)) Say(Tx("Unable to connect"));
    if (before.room && !after.room) {
      if (!leaving_ && !after.closed_reason.empty()) {
        const bool host_left = after.closed_reason == "host_left" || after.closed_reason == "cancelled" || after.closed_reason == "disconnect";
        Say(host_left ? Tx("Host left") : after.closed_reason == "no_path" ? Tx("Unable to connect") : Tx("Connection lost"));
      }
      leaving_ = false;
    } else if (same_session) {
      ObserveSeats(*before.room, *after.room, after.seat);
      if (after.reconnecting && !before.reconnecting) Say(Tx("Connection lost"));
    }
  }

  void ObserveSeats(const online::RoomView& was, const online::RoomView& now, int mine) {
    for (const online::Seat& seat : now.seats) {
      if (seat.seat == mine || size_t(seat.seat) >= was.seats.size()) continue;
      const online::Seat& old = was.seats[size_t(seat.seat)];
      if (old.state != online::SeatState::kEmpty && seat.state == online::SeatState::kEmpty) {
        Say(std::string(Tx("Player left")) + ": " + old.name);
      } else if (old.state == online::SeatState::kEmpty && seat.state != online::SeatState::kEmpty) {
        Say(seat.name + Tx(" joined"));
      }
    }
  }

  TextPackage package_;
  online::Snapshot last_;
  bool seen_ = false;
  bool leaving_ = false;
  bool held_ = false;
  std::string toast_;
  double until_ = 0;
  double now_ = 0;
};

enum class Page { kInvite, kIncoming, kLeave };
enum class Act { kNone, kInviteFriend, kAccept, kDecline, kBack, kResume, kLeave };

struct Row {
  std::string text;
  Act act = Act::kNone;  // kNone: a line of information, not selectable
  int arg = 0;
  bool enabled = true;
};

// One dialog over the game: the friend picker, the prompt for one invite, or the way out of a session.
class OnlineMenu {
 public:
  OnlineMenu(Api& api, TextPackage package, Page page, std::optional<online::Invite> invite = std::nullopt,
             bool setup_missing = false, std::optional<online::RoomRules> session_rules = std::nullopt)
      : api_(api),
        package_(package),
        page_(page),
        invite_(std::move(invite)),
        setup_missing_(setup_missing),
        session_rules_(std::move(session_rules)) {
    snap_ = api_.Snapshot();
    ClampCursor();
  }

  Page page() const { return page_; }
  int cursor() const { return cursor_; }
  bool closed() const { return closed_; }
  const char* Tx(const char* english) const { return MenuText(english, package_); }

  std::string Title() const { return page_ == Page::kInvite ? Tx("Invite Player") : Tx("Online"); }

  std::vector<Row> Rows() const {
    std::vector<Row> rows;
    if (page_ == Page::kInvite) {
      InviteRows(rows);
    } else if (page_ == Page::kLeave) {
      if (snap_.seat == 0) rows.push_back({Tx("This ends the race for everyone"), Act::kNone});
      rows.push_back({Tx("Resume"), Act::kResume});
      rows.push_back({Tx("Leave Multiplayer"), Act::kLeave});
    } else if (invite_) {
      rows.push_back({invite_->host_name + Tx(" invited you to a Multiplayer race"), Act::kNone});
      rows.push_back({Tx("Accept"), Act::kAccept});
      rows.push_back({Tx("Decline"), Act::kDecline});
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

  void Activate() {
    const std::vector<Row> rows = Rows();
    if (closed_ || cursor_ < 0 || cursor_ >= int(rows.size())) return;
    const Row row = rows[size_t(cursor_)];
    if (row.act == Act::kNone || !row.enabled) return;
    switch (row.act) {
      case Act::kInviteFriend: Invite(row.arg); break;
      case Act::kAccept:
        api_.AcceptInvite(invite_->id);
        closed_ = true;
        break;
      case Act::kDecline:
        api_.DeclineInvite(invite_->id);
        closed_ = true;
        break;
      case Act::kBack:
      case Act::kResume: closed_ = true; break;
      case Act::kLeave:
        api_.LeaveSession(snap_.seat > 0 ? "guest_menu" : "host_menu");
        closed_ = true;
        break;
      case Act::kNone: break;
    }
  }

  void Back() {
    if (page_ == Page::kIncoming && invite_) api_.DeclineInvite(invite_->id);
    closed_ = true;
  }

  void Update() {
    snap_ = api_.Snapshot();
    if (page_ == Page::kIncoming && std::none_of(snap_.invites.begin(), snap_.invites.end(), [&](const online::Invite& i) {
          return invite_ && i.id == invite_->id;
        })) {
      closed_ = true;
    }
    if (page_ == Page::kLeave && !snap_.room) closed_ = true;
    FollowSend();
    ClampCursor();
  }

 private:
  void ClampCursor() {
    const std::vector<Row> rows = Rows();
    if (rows.empty()) {
      cursor_ = 0;
      return;
    }
    cursor_ = std::clamp(cursor_, 0, int(rows.size()) - 1);
    if (rows[size_t(cursor_)].act != Act::kNone && rows[size_t(cursor_)].enabled) return;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].act != Act::kNone && rows[i].enabled) {
        cursor_ = int(i);
        return;
      }
    }
  }

  bool Seated(const std::string& player) const {
    if (!snap_.room) return false;
    return std::any_of(snap_.room->seats.begin(), snap_.room->seats.end(), [&](const online::Seat& s) {
      return s.state != online::SeatState::kEmpty && s.player == player;
    });
  }

  bool Invited(const std::string& player) const {
    return std::any_of(snap_.invited.begin(), snap_.invited.end(), [&](const online::SentInvite& sent) { return sent.player == player; });
  }

  void InviteRows(std::vector<Row>& rows) const {
    friend_codes_.clear();
    if (setup_missing_) rows.push_back({Tx("Game setup unavailable"), Act::kNone});
    if (!error_.empty()) rows.push_back({error_, Act::kNone});
    if (snap_.connection != online::Connection::kOnline) {
      rows.push_back({Tx(snap_.connection == online::Connection::kChecking ? "Connecting" : "Unable to connect"), Act::kNone});
      rows.push_back({Tx("Back"), Act::kBack});
      return;
    }
    const bool spent = SeatsLeft(snap_) <= 0;
    int shown = 0;
    for (const online::InviteChoice& choice : online::InviteChoices(snap_.friends)) {
      if (Seated(choice.player)) continue;
      const bool invited = Invited(choice.player);
      friend_codes_.push_back(choice.player);
      const std::string status = invited ? Tx("Invited") : choice.player == sending_ ? Tx("Connecting") : choice.status;
      rows.push_back({choice.name + "    " + status, Act::kInviteFriend, shown++,
                      choice.selectable && !spent && !invited && !setup_missing_ && sending_.empty()});
    }
    if (shown == 0) rows.push_back({Tx("No friends to invite"), Act::kNone});
    rows.push_back({Tx("Back"), Act::kBack});
  }

  // The picker stays until the service has taken the invite, and tells why when it does not.
  void Invite(int index) {
    if (index < 0 || size_t(index) >= friend_codes_.size()) return;
    const std::string player = friend_codes_[size_t(index)];
    outcomes_seen_ = snap_.invite_outcome.generation;
    if (!api_.SendInvite(player, session_rules_)) {
      error_ = NameOf(player) + Tx(" cannot be invited");
      return;
    }
    error_.clear();
    sending_ = player;
    snap_ = api_.Snapshot();
    FollowSend();
  }

  void FollowSend() {
    if (sending_.empty()) return;
    if (Invited(sending_) || Seated(sending_)) {
      closed_ = true;
      return;
    }
    if (snap_.invite_outcome.generation != outcomes_seen_) error_ = snap_.invite_outcome.name + Tx(" cannot be invited");
    if (snap_.sending_invite != sending_) sending_.clear();
  }

  std::string NameOf(const std::string& player) const {
    for (const online::Friend& f : snap_.friends) {
      if (f.player == player) return f.name;
    }
    return player;
  }

  Api& api_;
  TextPackage package_;
  Page page_;
  std::optional<online::Invite> invite_;
  bool setup_missing_;
  std::optional<online::RoomRules> session_rules_;
  online::Snapshot snap_;
  std::string sending_;
  std::string error_;
  int outcomes_seen_ = 0;
  int cursor_ = 0;
  bool closed_ = false;
  mutable std::vector<std::string> friend_codes_;
};

}  // namespace eck::menu
