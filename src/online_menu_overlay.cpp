#include "online_menu_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "language_setting.h"
#include "launcher.h"
#include "modal_dialog.h"
#include "online_character_select.h"
#include "online_lobby.h"
#include "online_menu.h"
#include "online_runtime.h"
#include "online_probe.h"
#include "online_race_url.h"
#include "online_setup_capture.h"
#include "overlay_rules.h"
#include "settings.h"

REXCVAR_DECLARE(std::string, eck_language);

namespace eck {
namespace {

constexpr float kWidth = 560.0f;
constexpr double kInvitePollSeconds = 0.25;
constexpr double kToastHoldSeconds = 30.0;  // a menu that never shows does not keep the note up for good
constexpr ImVec4 kToastColor(0.98f, 0.8f, 0.16f, 1.0f);
constexpr ImVec4 kInfoColor(0.85f, 0.85f, 0.85f, 1.0f);
constexpr ImU32 kPanelColor = IM_COL32(14, 12, 4, 190);
constexpr ImU32 kBorderColor = IM_COL32(250, 204, 41, 255);
constexpr ImU32 kTextColor = IM_COL32(255, 255, 255, 255);
constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoTitleBar;

std::atomic<bool> g_menu_seen{false};
std::atomic<bool> g_race_launched{false};
std::mutex g_screens_mutex;
menu::ScreenTracker g_screens;  // guarded by g_screens_mutex
lobby::SetupCapture g_setup;    // guarded by g_screens_mutex
lobby::Bridge g_lobby;


void LogSetup(const std::string& note) {
  if (!note.empty()) REXLOG_INFO("[ONLINE-SETUP] {}", note);
}

TextPackage GamePackage() { return PackageForLanguage(REXCVAR_GET(eck_language)); }

bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat) {
  for (const ImGuiKey key : keys) {
    if (ImGui::IsKeyPressed(key, repeat)) return true;
  }
  return false;
}

class OnlineMenuOverlay final : public ModalDialog {
 public:
  OnlineMenuOverlay(rex::ui::ImGuiDrawer* drawer, menu::Page page, std::optional<online::Invite> invite, bool setup_missing,
                    std::optional<online::RoomRules> session_rules)
      : ModalDialog(drawer), api_(Online()), menu_(api_, GamePackage(), page, std::move(invite), setup_missing, std::move(session_rules)) {
    REXLOG_INFO("[ONLINE] {}", page == menu::Page::kInvite   ? "invite picker opened"
                                            : page == menu::Page::kLeave ? "leave menu opened"
                                                                         : "invite prompt opened");
  }

 protected:
  bool InterceptBack() override {
    menu_.Back();
    RequestClose();
    return true;
  }

  void DrawContent(ImGuiIO& io) override {
    PumpOnlineService();
    menu_.Update();
    Input();
    if (menu_.closed()) {
      RequestClose();
      return;
    }

    const ImVec2 viewport = io.DisplaySize;
    ImGui::PushFont(LauncherRegularFont());
    ImGui::SetNextWindowPos(ImVec2(viewport.x * 0.5f, viewport.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(FitWindowExtent(viewport.x, 320.0f, kWidth), 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!focused_) {
      focused_ = true;
      ImGui::SetNextWindowFocus();
    }
    if (ImGui::Begin("##eck_online_menu", nullptr, kWindowFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(kToastColor, "%s", menu_.Title().c_str());
      ImGui::Separator();
      Rows();
      ImGui::Spacing();
      ImGui::TextDisabled("A  OK    B  %s", menu_.Tx("Back"));
    }
    ImGui::End();
    ImGui::PopFont();
  }

 private:
  void Input() {
    if (Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}, true)) menu_.Move(-1);
    if (Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}, true)) menu_.Move(1);
    if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_GamepadFaceDown}, false)) menu_.Activate();
  }

  void Rows() {
    const std::vector<menu::Row> rows = menu_.Rows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const menu::Row& row = rows[i];
      if (row.act == menu::Act::kNone) {
        ImGui::TextColored(kInfoColor, "%s", row.text.c_str());
        continue;
      }
      ImGui::PushID(int(i));
      ImGui::BeginDisabled(!row.enabled);
      // Mouse only: Input() already took Enter and A, and ImGui's navigation would activate the row a second time.
      if (ImGui::Selectable(row.text.c_str(), int(i) == menu_.cursor()) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        menu_.SetCursor(int(i));
        menu_.Activate();
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }

  menu::ServiceApi api_;
  menu::OnlineMenu menu_;
  bool focused_ = false;
};

// Bottom left, over the game: nothing here takes input, so the character screen stays the game's.
// The game's own prompt stays underneath, so this has to be the first thing a waiting guest reads.
void DrawBanner(ImGuiIO& io, const char* text) {
  ImFont* font = ImGui::GetFont();
  const float size = ImGui::GetFontSize() * 1.5f;
  const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
  const ImVec2 pad(size * 0.8f, size * 0.4f);
  const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.82f);
  const ImVec2 p0(center.x - extent.x * 0.5f - pad.x, center.y - extent.y * 0.5f - pad.y);
  const ImVec2 p1(center.x + extent.x * 0.5f + pad.x, center.y + extent.y * 0.5f + pad.y);
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(p0, p1, kPanelColor, size * 0.2f);
  draw->AddRect(p0, p1, kBorderColor, size * 0.2f);
  draw->AddText(font, size, ImVec2(p0.x + pad.x, p0.y + pad.y), kTextColor, text);
}

void DrawPanel(ImGuiIO& io, const std::vector<std::string>& lines) {
  if (lines.empty()) return;
  ImFont* font = LauncherRegularFont() ? LauncherRegularFont() : ImGui::GetFont();  // the one with punctuation
  const float size = ImGui::GetFontSize();
  float width = 0;
  for (const std::string& line : lines) width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0, line.c_str()).x);
  const ImVec2 pad(size * 0.6f, size * 0.3f);
  const float height = float(lines.size()) * size + pad.y * 2;
  const ImVec2 p0(size, io.DisplaySize.y - height - size * 2);
  const ImVec2 p1(p0.x + width + pad.x * 2, p0.y + height);
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(p0, p1, kPanelColor, size * 0.2f);
  draw->AddRect(p0, p1, kBorderColor, size * 0.2f);
  for (size_t i = 0; i < lines.size(); ++i) {
    draw->AddText(font, size, ImVec2(p0.x + pad.x, p0.y + pad.y + float(i) * size), kTextColor, lines[i].c_str());
  }
}

// Always alive: the shortcut on the character screen, the session's status, and the prompt for an invite.
class OnlineMenuDriver final : public rex::ui::ImGuiDialog {
 public:
  OnlineMenuDriver(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer)
      : ImGuiDialog(drawer), drawer_(drawer), defer_(std::move(defer)), notes_(GamePackage()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!REXCVAR_GET(eck_online_enabled)) return;
    const double now = ImGui::GetTime();
    const online::Snapshot state = Online().snapshot();
    notes_.Update(state, now);
    const bool menu_press = g_lobby.TakeMenuPress();

    menu::GameScreen screen;
    menu::PanelObjects panels;
    bool x_press;
    {
      std::lock_guard lock(g_screens_mutex);
      screen = g_screens.current();
      panels = g_screens.panel_objects();
      x_press = g_screens.TakeInvitePress();
      results_open_ = g_screens.results_open();
    }
    const menu::InviteShortcut shortcut = menu::ShortcutFor(true, screen == menu::GameScreen::kCharacter, state);
    LogTransitions(screen, shortcut);

    if (g_race_launched.exchange(false)) LocalRaceLaunched(state);
    FollowSession(state, screen, panels, now);
    if (screen == menu::GameScreen::kMainMenu || now >= release_toast_at_) notes_.ReleaseToast(now);

    if (ModalDialog::AnyOpen() || opening_) return;
    if (menu_press && state.room) {
      Open(menu::Page::kLeave, std::nullopt);
      return;
    }
    x_press = x_press || (shortcut.visible && ImGui::IsKeyPressed(ImGuiKey_X, false));
    if (x_press && shortcut.visible) {
      OpenPicker(state, now);
    } else if (g_menu_seen.load() && now - last_poll_ >= kInvitePollSeconds) {
      PromptInvite(state, now);
    }
    DrawPanel(io, PanelLines(state, shortcut, screen, now));
    const lobby::Bridge::RaceStatus race = g_lobby.race_status();
    if (race.waiting_for_host) DrawBanner(io, menu::MenuText("Waiting for host", GamePackage()));
  }

 private:

  void LogTransitions(menu::GameScreen screen, const menu::InviteShortcut& shortcut) {
    if (screen != last_screen_) {
      if (screen == menu::GameScreen::kCharacter) REXLOG_INFO("[ONLINE] character_select entered online_play=on");
      last_screen_ = screen;
    }
    if (shortcut.visible && shortcut.remaining != last_remaining_) {
      REXLOG_INFO("[ONLINE] invite shortcut available remaining={}", shortcut.remaining);
    }
    const std::string hidden = screen == menu::GameScreen::kCharacter ? shortcut.hidden_because : "none";
    if (hidden != last_hidden_ && hidden != "none") REXLOG_INFO("[ONLINE] invite shortcut hidden reason={}", hidden);
    last_hidden_ = hidden;
    last_remaining_ = shortcut.remaining;
  }

  // Every session ends here, whatever ended it. A start in progress is the one thing left to run: it still owes the host the
  // way back to the character screen.
  void NoteSession(const online::Snapshot& state) {
    if (state.session_generation == session_) return;
    session_ = state.session_generation;
    pacer_.Reset();
    active_race_.reset();
    g_lobby.SetRaceStatus({});
    if (!state.room && host_race_.active()) return;
    g_lobby.DropRace();
    host_race_ = {};
  }

  // The silence is only ever acted on in a race, and only together with what the service says.
  void WatchSession(const online::Snapshot& state) {
    if (g_menu_seen.load() || !state.room || state.room->state != online::RoomState::kRacing) return;
    // A race this PC is already in is not given up because the host's heartbeat looks late: the karts run on the players' own
    // path, and abandoning the session costs a race that had started. Only the game's own results screen ends it.
    if (active_race_.has_value() && !results_open_) {
      const lobby::RaceWatch watch = lobby::WatchRace(state, g_lobby.race_status().host_silence);
      if (watch != lobby::RaceWatch::kFine) {
        static bool told = false;
        if (!told) {
          told = true;
          REXLOG_INFO("[ONLINE] race kept: watch says {}", int(watch));
        }
      }
      return;
    }
    const lobby::RaceWatch watch = lobby::WatchRace(state, g_lobby.race_status().host_silence);
    if (watch == lobby::RaceWatch::kFine) return;
    Online().AbandonSession(watch == lobby::RaceWatch::kHostLeft ? "host_left" : "connection_lost");
  }

  bool HasGuests(const online::Snapshot& state) const { return state.room && state.room->players > 1; }

  bool Reopen(const online::Snapshot& state, lobby::AfterRace destination) {
    const lobby::AfterRaceStep step{++after_generation_, destination};
    const bool sent = Online().ReopenRoom(lobby::AfterRaceRules(state.room->rules, step));
    REXLOG_INFO("[ONLINE-LIFECYCLE] after_race host destination={} generation={} sent={}", int(destination), step.generation,
                sent);
    return sent;
  }

  // The host's own game named where it goes. The room follows it there; with nowhere to go together, LobbyFlow ends the session.
  void HostReturning(const online::Snapshot& state, const std::string& command) {
    REXLOG_INFO("[ONLINE-LIFECYCLE] host_return command={}", command);
    const std::optional<lobby::AfterRace> destination = lobby::DestinationOfCommand(command);
    if (destination && HasGuests(state) && Reopen(state, *destination)) flow_.NoteContinuing();
  }

  void ReturnToMenu(const online::Snapshot& state, double now) {
    const std::string reason = state.closed_reason.empty() ? "local_leave" : state.closed_reason;
    if (reason == "host_left" || reason == "disconnect" || reason == "cancelled") {
      REXLOG_INFO("[ONLINE-LIFECYCLE] host_left seat=0 action=return_to_menu");
    }
    REXLOG_INFO("[ONLINE-LIFECYCLE] return_to_menu reason={}", reason);
    notes_.HoldToast(now);
    release_toast_at_ = now + kToastHoldSeconds;
  }

  void FollowSession(const online::Snapshot& state, menu::GameScreen screen, const menu::PanelObjects& panels, double now) {
    NoteSession(state);
    WatchSession(state);
    if (const std::optional<std::string> command = g_lobby.TakeHostReturn()) HostReturning(state, *command);
    const lobby::FlowResult flow = flow_.Update(state, screen, g_menu_seen.load(), g_lobby.TakeBackPress(), host_race_.active());
    if (!flow.note.empty()) REXLOG_INFO("[ONLINE] {}", flow.note);
    // A guest that is seated in a race the room already says is running, and that is not travelling to it, is the one state
    // that leaves it racing alone. Say why, once per distinct answer, so the log names the gate instead of the symptom.
    if (state.room && state.seat > 0 && state.room->state == online::RoomState::kRacing &&
        flow.travel.kind == lobby::TravelKind::kNone) {
      const std::string why = "in_menus=" + std::to_string(g_menu_seen.load()) + " screen=" + std::to_string(int(screen)) +
                              " has_race=" + std::to_string(lobby::RaceOfRules(state.room->rules) ? 1 : 0) +
                              " seat=" + std::to_string(state.seat);
      static std::string told;
      if (told != why) {
        told = why;
        REXLOG_INFO("[ONLINE-RACE] guest holding: {}", why);
      }
    }
    if (flow.leave) Leave(flow.source);
    if (flow.travel.kind != lobby::TravelKind::kNone) {
      // A session that ends under a race that is still running does not take the race with it: the race is the players' own
      // UDP path, and only the game's own results screen ends it.
      const bool keep_racing = flow.travel.kind == lobby::TravelKind::kMainMenu && active_race_.has_value() && !results_open_;
      if (flow.travel.kind == lobby::TravelKind::kRace) EnterRace(flow.travel.race);
      if (!keep_racing) {
        if (flow.travel.kind == lobby::TravelKind::kMainMenu) ReturnToMenu(state, now);
        g_lobby.RequestTravel(flow.travel);
      } else {
        REXLOG_INFO("[ONLINE] race kept: the session ended while it was running");
      }
    }
    StartHeldRace(state, now);
    lobby::Plan plan = lobby::MakePlan(state, screen == menu::GameScreen::kCharacter, panels);
    plan.results_open = results_open_;
    plan.in_race = lobby::RaceIsOn(state.room && state.room->state == online::RoomState::kRacing, g_menu_seen.load(),
                                   active_race_.has_value());
    // The room says how long after the host published the start is; the race's own sync turns it into a deadline on the
    // clock its frames use.
    plan.start_in_ms = state.room ? lobby::StartInOfRules(state.room->rules).value_or(0) : 0;
    if (plan.start_in_ms > 0 && plan.start_in_ms != start_in_ms_) {
      start_in_ms_ = plan.start_in_ms;
      REXLOG_INFO("[ONLINE-RACE] start in {} ms", start_in_ms_);
    }
    // A session that drops mid-race leaves the plan without a seat, and the seat is what tells the sync which kart on this
    // PC is the player's: the one the race started with stands until the race is over.
    if (plan.in_race && lobby::ValidSeat(plan.local_seat)) race_seat_ = plan.local_seat;
    if (plan.in_race && !lobby::ValidSeat(plan.local_seat) && race_seat_ >= 0) plan.local_seat = race_seat_;
    if (plan.in_race) {
      plan.race = active_race_;
      // The race runs on the players' own UDP path, so the routes it already has outlive a session that drops under it: a
      // sign-in that reconnects mid-race comes back with a new generation and no room, and losing the routes there is what
      // freezes every kart but the host's.
      const race_sync::PeerRoutes routes = race_sync::RoutesOf(Online().GamePeers());
      bool any = false;
      for (const race_sync::PeerRoute& route : routes) {
        if (route.seat >= 0) any = true;
      }
      if (any) race_routes_ = routes;
      plan.routes = race_routes_;
      const lobby::Bridge::RaceStatus race = g_lobby.race_status();
      for (const online::PeerSnapshot& peer : state.peers) Online().SetPeerRtt(peer.seat, race.rtt_ms[size_t(peer.seat)]);
    }
    g_lobby.Publish(plan);
    SendOwnPick(state, now);
  }

  void SendOwnPick(const online::Snapshot& state, double now) {
    const std::optional<lobby::PanelView> own = g_lobby.own(state.session_generation);
    if (!state.room) pacer_.Reset();
    if (!state.room || !own) return;
    if (const std::optional<lobby::PanelView> due = pacer_.Due(now, *own)) {
      // Entering the session reloads this PC's map, and the panels come up empty for a moment. That is not the player clearing
      // the pick it already confirmed, and reporting it costs the host its race start: the host waits for every panel.
      bool confirmed_here = false;
      for (const online::Seat& seat : state.room->seats) {
        if (seat.seat == state.seat && seat.pick == online::PickState::kConfirmed) confirmed_here = true;
      }
      if (confirmed_here && due->state != online::PickState::kConfirmed) return;
      REXLOG_INFO("[ONLINE] pick seat={} state={} character={}", state.seat, int(due->state), due->character);
      Online().SendPick(due->state, due->character);
    }
  }

  std::vector<std::string> PanelLines(const online::Snapshot& state, const menu::InviteShortcut& shortcut, menu::GameScreen screen,
                                      double now) {
    const TextPackage package = GamePackage();
    std::vector<std::string> lines;
    if (shortcut.visible) lines.push_back(std::string("X  ") + menu::MenuText("Invite Player", package));
    const bool host_owns_screen = state.seat > 0 && screen == menu::GameScreen::kTrack;
    if (host_owns_screen ||
        lobby::GuestWaitsOnCharacters(state.seat > 0, screen == menu::GameScreen::kCharacter, g_lobby.own(state.session_generation))) {
      lines.push_back(menu::MenuText("Waiting for host", package));
    }
    for (std::string& line : menu::StatusLines(state, package)) lines.push_back(std::move(line));
    if (const std::string toast = notes_.Toast(now); !toast.empty()) lines.push_back(toast);
    return lines;
  }

  void Leave(const char* source = "menu") {
    notes_.NoteLeaving();
    Online().LeaveSession(source);
  }

  // Nobody joined: the race is simply the game's own, and the empty session has no reason to stay.
  void LocalRaceLaunched(const online::Snapshot& state) {
    g_menu_seen.store(false);
    flow_.Reset();
    if (state.room) Leave();
  }

  void EnterRace(const lobby::CanonicalRace& race) {
    g_menu_seen.store(false);
    active_race_ = race;
    online_probe::SetRaceSeats(race.seats);
  }

  // Seat 0 owns the start: it holds the game's command until the service says every seat is ready, then all of them travel.
  void StartHeldRace(const online::Snapshot& state, double now) {
    if (const std::optional<lobby::Bridge::HeldRace> held = g_lobby.TakeHeldRace()) BeginRace(*held, state, now);
    const lobby::RaceStep step = host_race_.Update(state, now);
    if (step.publish && Online().PublishRace(PublishingRules(*state.room))) {
      REXLOG_INFO("[ONLINE-RACE] race published generation={}", published_race_.generation);
      host_race_.NotePublished();
    }
    if (step.commit) {
      EnterRace(published_race_);
      g_lobby.ReleaseRace(lobby::GameCommandMatchesPads(published_race_, race_pad_)
                              ? std::string()
                              : lobby::RaceCommand(published_race_, 0, race_pad_));
    }
    if (step.abort) AbortRace(step.abort);
  }

  // What the room is told when the host publishes: the race, plus the instant every PC presses Start. The lead is the time
  // the rules take to reach the other PCs and be converted, not the countdown: the game runs its own 3-2-1 from the press.
  static constexpr int kStartDelayMs = 2000;

  online::RoomRules PublishingRules(const online::RoomView& room) {
    const online::RoomRules base = lobby::RaceRules(lobby::BaseRules(room.rules), published_race_);
    return lobby::StartInRules(base, kStartDelayMs);
  }

  void BeginRace(const lobby::Bridge::HeldRace& held, const online::Snapshot& state, double now) {
    const std::optional<race_url::RaceUrl> url = race_url::ParseRaceUrl(held.command);
    const lobby::RaceParse parse =
        url ? lobby::CanonicalOfUrl(*url, held.seats, ++race_generation_) : lobby::RaceParse{std::nullopt, "url"};
    if (!parse.race) {
      REXLOG_INFO("[ONLINE-RACE] race refused field={}", parse.failed);
      AbortRace("race_payload_invalid");
      return;
    }
    published_race_ = *parse.race;
    race_pad_ = held.physical_pad;
    REXLOG_INFO("[ONLINE-RACE] race canonical generation={} {}", published_race_.generation,
                lobby::RaceCommand(published_race_, 0, 0));
    // Restart and Next Race, from a pause or the results: the room has to come back from the last race first.
    if (state.room && state.room->state == online::RoomState::kRacing && !Reopen(state, lobby::AfterRace::kNextRace)) {
      AbortRace("reopen_unavailable");
      return;
    }
    host_race_.Begin(now);
  }

  // Nobody races alone by accident: the session ends and the host goes back to the character screen it came from.
  void AbortRace(const char* reason) {
    REXLOG_INFO("[ONLINE-RACE] race aborted reason={}", reason);
    g_lobby.DropRace();
    Leave();
    lobby::TravelRequest back;
    {
      std::lock_guard lock(g_screens_mutex);
      if (const std::optional<lobby::GameSetup> setup = g_setup.setup()) {
        back.kind = lobby::TravelKind::kEntry;
        back.setup = *setup;
      } else {
        back.kind = lobby::TravelKind::kMainMenu;
      }
    }
    g_lobby.RequestTravel(back);
  }

  // X only shows the friends. The session starts when one of them is chosen, carrying the game the host has actually set up.
  void OpenPicker(const online::Snapshot& state, double now) {
    std::optional<lobby::GameSetup> setup;
    std::string missing;
    {
      std::lock_guard lock(g_screens_mutex);
      setup = g_setup.setup();
      missing = g_setup.missing();
    }
    switch (menu::OnInvitePress(state, setup.has_value())) {
      case menu::InvitePress::kConnecting:
        REXLOG_INFO("[ONLINE] invite picker waiting reason=service_not_ready");
        notes_.NoteStatus("Connecting to online service", now);
        return;
      case menu::InvitePress::kPickerWithoutSetup:
        REXLOG_INFO("[ONLINE-SETUP] incomplete missing={}", missing);
        Open(menu::Page::kInvite, std::nullopt, true);
        return;
      case menu::InvitePress::kPicker: break;
    }
    REXLOG_INFO("[ONLINE-INVITE] picker_open room={} seat={}", state.room ? state.room->room : "none", state.seat);
    Open(menu::Page::kInvite, std::nullopt, false, setup ? std::optional(lobby::SetupRules(*setup)) : std::nullopt);
  }

  void PromptInvite(const online::Snapshot& state, double now) {
    last_poll_ = now;
    if (!menu::CanTakeInvite(state)) return;
    for (const online::Invite& invite : state.invites) {
      if (prompted_.insert(invite.id).second) {
        Open(menu::Page::kIncoming, invite);
        return;
      }
    }
  }

  void Open(menu::Page page, std::optional<online::Invite> invite, bool setup_missing = false,
            std::optional<online::RoomRules> session_rules = std::nullopt) {
    opening_ = true;
    defer_([this, page, invite = std::move(invite), setup_missing, session_rules = std::move(session_rules)]() mutable {
      opening_ = false;
      // Retained by the drawer until it closes.
      if (!ModalDialog::AnyOpen()) new OnlineMenuOverlay(drawer_, page, std::move(invite), setup_missing, std::move(session_rules));
    });
  }

  rex::ui::ImGuiDrawer* drawer_;
  std::function<void(std::function<void()>)> defer_;
  menu::SessionNotes notes_;
  lobby::LobbyFlow flow_;
  lobby::PickPacer pacer_;
  lobby::HostRaceStart host_race_;
  lobby::CanonicalRace published_race_;
  std::optional<lobby::CanonicalRace> active_race_;
  int race_generation_ = 0;
  int race_pad_ = 0;
  int session_ = 0;
  int after_generation_ = 0;
  bool results_open_ = false;
  double release_toast_at_ = 0;
  std::set<std::string> prompted_;
  menu::GameScreen last_screen_ = menu::GameScreen::kOther;
  race_sync::PeerRoutes race_routes_{};  // the race's own path, kept across a session that drops
  int race_seat_ = -1;                   // the seat this PC races as, kept across a session that drops
  int start_in_ms_ = 0;                  // the room's start delay last logged
  int last_remaining_ = -1;
  std::string last_hidden_ = "none";
  double last_poll_ = 0;
  bool opening_ = false;
};

}  // namespace

void InstallOnlineMenu(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> defer) {
  new OnlineMenuDriver(drawer, std::move(defer));  // retained by the drawer for the session
}

void NoteOnlineMovie(std::string_view url) {
  if (!REXCVAR_GET(eck_online_enabled)) return;
  std::lock_guard lock(g_screens_mutex);
  g_screens.NoteMovie(url);
}

void NoteOnlineScreenCall(uint32_t screen_class, uint32_t screen, std::string_view method) {
  std::lock_guard lock(g_screens_mutex);
  const menu::GameScreen before = g_screens.current();
  g_screens.NoteCall(screen_class, screen, method);
  if (g_screens.current() != before) LogSetup(g_setup.NoteScreen(g_screens.current()));
}

void NoteOnlineMovieCall(std::string_view method, std::optional<int> number, std::string_view text) {
  std::lock_guard lock(g_screens_mutex);
  LogSetup(g_setup.NoteMovieCall(method, number, text));
}

void NoteOnlineRaceLaunch() {
  NoteOnlineTravel();
  g_race_launched.store(true);
}

void NoteOnlineTravel() {
  std::lock_guard lock(g_screens_mutex);
  g_screens.NoteMapChange();
}

lobby::Bridge& OnlineLobby() { return g_lobby; }

void NoteMainMenuShown() { g_menu_seen.store(true); }

}  // namespace eck
