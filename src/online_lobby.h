#pragma once

// The game's character screen as the lobby: what the host publishes, how a guest travels into the same Free Race, and how
// each remote seat drives a real panel as a pad of that panel's controller. Pure: guest memory and hooks are the caller's.

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "online/online_service.h"
#include "online/protocol.h"
#include "online_character_select.h"
#include "online_race_start.h"
#include "online_race_sync.h"

namespace eck::lobby {

using online::PickState;

inline constexpr int kPanels = 4;
inline constexpr int kGameTypeExhibition = 0;
inline constexpr int kGameTypeStartMenu = 3;
inline constexpr int kFreeRaceModes = 7;  // EKartGameMode up to CaptureTheFlag
inline constexpr int kDifficulties = 3;   // KartDificultLevel
inline constexpr int kSetupVersion = 1;

struct GameSetup {
  int game_type = kGameTypeStartMenu;
  int mode = 0;
  int difficulty = 0;
};

constexpr bool ValidSetup(const GameSetup& setup) {
  return (setup.game_type == kGameTypeExhibition || setup.game_type == kGameTypeStartMenu) && setup.mode >= 0 &&
         setup.mode < kFreeRaceModes && setup.difficulty >= 0 && setup.difficulty < kDifficulties;
}

inline online::RoomRules SetupRules(const GameSetup& setup) {
  return {{"setup", kSetupVersion}, {"game_type", setup.game_type}, {"mode", setup.mode}, {"difficulty", setup.difficulty}};
}

inline std::optional<GameSetup> SetupOfRules(const online::RoomRules& rules) {
  std::optional<int> version, game_type, mode, difficulty;
  for (const auto& [key, value] : rules) {
    if (key == "setup") version = value;
    if (key == "game_type") game_type = value;
    if (key == "mode") mode = value;
    if (key == "difficulty") difficulty = value;
  }
  if (version != kSetupVersion || !game_type || !mode || !difficulty) return std::nullopt;
  const GameSetup setup{*game_type, *mode, *difficulty};
  if (!ValidSetup(setup)) return std::nullopt;
  return setup;
}

// The game's own KartScreen_EndRace.BackToCharacterSelect travel: KartGame_Menu walks main menu, competition and character
// screens by itself and sets the mode and difficulty on the way. No character is named, so every panel starts free.
inline std::string EntryCommand(const GameSetup& setup, int primary_controller) {
  return "start StartMenu?GoBackType=2?NumGameMode=" + std::to_string(setup.mode) + "?KartAIDiffculty=" +
         std::to_string(setup.difficulty) + "?GameType=" + std::to_string(setup.game_type) + "?PrimaryControllerID=" +
         std::to_string(primary_controller);
}

inline std::string MainMenuCommand(int primary_controller) {
  return "start StartMenu?GoBackType=0?PrimaryControllerID=" + std::to_string(primary_controller);
}

enum class TravelKind { kNone, kEntry, kMainMenu, kRace, kTrackSelect };

struct TravelRequest {
  TravelKind kind = TravelKind::kNone;
  GameSetup setup;
  CanonicalRace race;
  int local_seat = -1;
};

// What the game's own KartScreen_EndRace.BackToTrackSelect loads: the last race's players, so the menus can walk past the
// character screen by themselves.
inline std::string TrackSelectCommand(const CanonicalRace& race, int local_seat, const GameSetup& setup, int primary_controller) {
  const std::string command = RaceCommand(race, local_seat, primary_controller);
  return "start StartMenu" + command.substr(command.find('?')) + "?NumGameMode=" + std::to_string(setup.mode) + "?GoBackType=3";
}

inline std::string TravelCommand(const TravelRequest& travel, int primary_controller) {
  switch (travel.kind) {
    case TravelKind::kEntry: return EntryCommand(travel.setup, primary_controller);
    case TravelKind::kMainMenu: return MainMenuCommand(primary_controller);
    case TravelKind::kRace: return RaceCommand(travel.race, travel.local_seat, primary_controller);
    case TravelKind::kTrackSelect: return TrackSelectCommand(travel.race, travel.local_seat, travel.setup, primary_controller);
    default: return {};
  }
}

// ---- After a race -------------------------------------------------------------------------------------------------------

enum class AfterRace { kCharacterSelect = 2, kTrackSelect = 3, kNextRace = 9 };  // the first two are the game's GoBackType

struct AfterRaceStep {
  int generation = 0;
  AfterRace destination = AfterRace::kCharacterSelect;
};

inline online::RoomRules BaseRules(const online::RoomRules& rules) {
  online::RoomRules base;
  for (const auto& rule : rules) {
    if (rule.first.rfind("race", 0) != 0 && rule.first.rfind("after_", 0) != 0) base.push_back(rule);
  }
  return base;
}

inline online::RoomRules AfterRaceRules(const online::RoomRules& rules, const AfterRaceStep& step) {
  online::RoomRules next = BaseRules(rules);
  next.emplace_back("after_race", step.generation);
  next.emplace_back("after_dest", int(step.destination));
  return next;
}

inline std::optional<AfterRaceStep> AfterRaceOfRules(const online::RoomRules& rules) {
  std::optional<int> generation, destination;
  for (const auto& [key, value] : rules) {
    if (key == "after_race") generation = value;
    if (key == "after_dest") destination = value;
  }
  if (!generation || *generation < 1 || !destination) return std::nullopt;
  const int where = *destination;
  if (where != int(AfterRace::kCharacterSelect) && where != int(AfterRace::kTrackSelect) && where != int(AfterRace::kNextRace)) {
    return std::nullopt;
  }
  return AfterRaceStep{*generation, AfterRace(where)};
}

// The game's way back to the menus names where it is going. Anywhere before the character screen is out of this session's game.
inline std::optional<AfterRace> DestinationOfCommand(std::string_view command) {
  constexpr std::string_view kMenu = "start StartMenu", kBack = "?GoBackType=";
  if (command.rfind(kMenu, 0) != 0) return std::nullopt;
  const size_t at = command.find(kBack);
  if (at == std::string_view::npos || at + kBack.size() >= command.size()) return std::nullopt;
  const char type = command[at + kBack.size()];
  if (type == '2') return AfterRace::kCharacterSelect;
  if (type == '3') return AfterRace::kTrackSelect;
  return std::nullopt;
}

inline constexpr bool IsMenuCommand(std::string_view command) { return command.rfind("start StartMenu", 0) == 0; }

// ---- Panels -----------------------------------------------------------------------------------------------------------

inline constexpr uint16_t kPadDpadLeft = 0x0004;
inline constexpr uint16_t kPadDpadRight = 0x0008;
inline constexpr uint16_t kPadStart = 0x0010;
inline constexpr uint16_t kPadA = 0x1000;
inline constexpr uint16_t kPadB = 0x2000;

struct PanelView {
  PickState state = PickState::kNone;
  int character = 0;
};
using Panels = std::array<PanelView, kPanels>;  // index = ControlledById = the panel's pad

struct PanelTarget {
  PickState state = PickState::kNone;
  int character = -1;  // none chosen yet
};

// One press that brings a panel closer to its target, through the same screens a local pad uses.
inline uint16_t NextPress(const PanelView& panel, const PanelTarget& target, bool may_move) {
  if (target.state == PickState::kNone) return panel.state == PickState::kNone ? 0 : kPadB;
  if (panel.state == PickState::kNone) return kPadStart;
  const bool on_character = target.character < 0 || panel.character == target.character;
  if (panel.state == PickState::kConfirmed) return on_character && target.state == PickState::kConfirmed ? 0 : kPadB;
  if (!on_character) {
    if (!may_move) return 0;
    const int forward = (target.character - panel.character + online::kCharacters) % online::kCharacters;
    return forward <= online::kCharacters / 2 ? kPadDpadRight : kPadDpadLeft;
  }
  return target.state == PickState::kConfirmed ? kPadA : 0;
}

// A press long enough for the game's input to see it, with a release before the next one. The game skips characters
// another panel shows, so a target can be out of reach: the panel waits for it, and the moves toward one are bounded.
class PanelPad {
 public:
  static constexpr double kHoldSeconds = 0.07;
  static constexpr double kGapSeconds = 0.09;
  static constexpr int kMaxMoves = online::kCharacters * 2;

  uint16_t Step(double now, const PanelView& panel, const PanelTarget& target, bool reachable) {
    if (now < until_) return held_;
    if (held_ != 0) {
      held_ = 0;
      until_ = now + kGapSeconds;
      return 0;
    }
    if (target.character != last_character_) {
      last_character_ = target.character;
      moves_ = 0;
    }
    held_ = NextPress(panel, target, reachable && moves_ < kMaxMoves);
    if (held_ & (kPadDpadLeft | kPadDpadRight)) ++moves_;
    if (held_ != 0) until_ = now + kHoldSeconds;
    return held_;
  }

 private:
  uint16_t held_ = 0;
  double until_ = 0;
  int moves_ = 0;
  int last_character_ = -1;
};

// Player N is seat N on every PC, whichever pad each PC happens to hold, so the host is the first panel everywhere.
inline constexpr std::array<int, kPanels> kPanelOfSeat = {0, 1, 2, 3};
inline constexpr std::array<int, kPanels> kSeatOfPanel = {0, 1, 2, 3};

constexpr bool ValidSeat(int seat) { return seat >= 0 && seat < kPanels; }

// What the game reads from one controller index while the character screen is a session.
struct PadRule {
  bool virtual_pad = false;  // answer with `buttons` instead of the real pad, focused or not
  uint16_t buttons = 0;
  int source_pad = -1;       // the physical pad behind this index, when it is not the index itself
  uint16_t allow = 0xFFFF;   // real buttons that reach the game
  bool own_pad = false;
  bool back_leaves = false;  // the panel is closed, so B on this pad means leaving the screen
  uint16_t menu_button = 0;  // opens the multiplayer overlay instead of reaching the game
  uint16_t echo_buttons = 0;  // of the local player's, pressed on this virtual pad as well
  uint16_t press = 0;        // added to what the game reads, whatever the real pad says
  race::PadState analog;     // triggers and sticks of a virtual pad
};
using PadRules = std::array<PadRule, kPanels>;

// Published by the UI thread, read by the game thread.
struct Plan {
  bool on_screen = false;
  bool in_race = false;
  int session = 0;  // the service's session_generation: whatever belongs to another one is void
  bool results_open = false;  // past the finish HUD
  bool locked = false;  // a guest in screens that are the host's alone
  std::optional<CanonicalRace> race;
  race_sync::PeerRoutes routes;
  menu::PanelObjects panels{};
  int local_seat = -1;
  std::array<bool, kPanels> remote{};
  std::array<PanelTarget, kPanels> seats{};
};

constexpr bool HasRemotePanels(const Plan& plan) {
  for (const bool remote : plan.remote) {
    if (remote) return true;
  }
  return false;
}

constexpr bool IsRemotePanel(const Plan& plan, uint32_t panel) {
  for (size_t i = 0; i < kPanels; ++i) {
    if (panel != 0 && plan.panels[i] == panel) return plan.remote[size_t(kSeatOfPanel[i])];
  }
  return false;
}

inline Plan MakePlan(const online::Snapshot& state, bool on_screen, const menu::PanelObjects& panels) {
  Plan plan;
  plan.on_screen = on_screen;
  plan.panels = panels;
  plan.session = state.session_generation;
  if (!state.room) return plan;
  plan.local_seat = state.seat;
  const std::optional<AfterRaceStep> after = AfterRaceOfRules(state.room->rules);
  plan.locked = state.seat > 0 && state.room->state == online::RoomState::kOpen && after &&
                after->destination == AfterRace::kTrackSelect;
  for (const online::Seat& seat : state.room->seats) {
    if (seat.seat == state.seat || seat.seat < 0 || seat.seat >= kPanels || seat.state == online::SeatState::kEmpty) continue;
    plan.remote[size_t(seat.seat)] = true;
    const bool chosen = seat.pick != PickState::kNone;
    // The game asks "all characters selected?" once no panel is selecting, and only the host can answer it: on a guest the
    // other seats stop one step short of confirmed.
    const PickState pick = chosen && state.seat == 0 ? seat.pick : PickState::kSelecting;
    plan.seats[size_t(seat.seat)] = {pick, chosen ? seat.character : -1};
  }
  return plan;
}

// In a session's race the other human karts belong to their seats, so no pad on this PC may reach them: they read what the
// session says, and the local player's pad is read for the session.
inline PadRules RacePadRules(int own_pad, const race_sync::FrameOut& frame, bool guest = false, int race_state = -1,
                             bool results_open = false) {
  PadRules rules;
  for (int pad = 0; pad < kPanels; ++pad) {
    PadRule& rule = rules[size_t(pad)];
    if (pad == own_pad) {
      rule.own_pad = true;
      rule.allow = uint16_t(~frame.own.block);
      rule.press = frame.own.press;
      // In a race Start is the host's pause, which stops everybody, so its way out is the other button. Past the finish Start
      // pauses nothing.
      if (guest) rule.menu_button = kPadStart;
      else if (frame.post_race) rule.menu_button = kPadStart | race_sync::kPadBack;
      else if (race_state <= race_sync::kRaceInProgress) rule.menu_button = race_sync::kPadBack;
      rule.allow = uint16_t(rule.allow & ~rule.menu_button);
    } else if (const std::optional<race::PadState>& seat = frame.pads[size_t(pad)]) {
      rule.virtual_pad = true;
      rule.buttons = seat->buttons;
      rule.analog = *seat;
      // Every human has a finish HUD on this PC and each takes only its own pad, so on the host a remote seat's follows the
      // host's. It stops at the results, where a press would reach whatever screen is next.
      if (!guest && frame.post_race && !results_open) rule.echo_buttons = kPadA | kPadB;
    } else {
      rule.allow = 0;
    }
  }
  return rules;
}

// A guest on screens where nothing is theirs to press. No panel is driven either: the game is walking them by itself.
inline PadRules LockedPadRules(int physical_pad) {
  PadRules rules;
  for (int pad = 0; pad < kPanels; ++pad) {
    PadRule& rule = rules[size_t(pad)];
    if (pad != physical_pad) {
      rule.virtual_pad = true;
      continue;
    }
    rule.own_pad = true;
    rule.allow = 0;
    rule.menu_button = kPadStart;
  }
  return rules;
}

// What a guest shows on the game's own screens while the next move is the host's.
constexpr bool GuestWaitsOnCharacters(bool guest, bool on_character_screen, const std::optional<PanelView>& own) {
  return guest && on_character_screen && own && own->state == PickState::kConfirmed;
}

// The panel the local player acts on: their seat's in a session, and without one the game's own primary panel.
constexpr int LocalPanel(const Plan& plan, int physical_pad) {
  return ValidSeat(plan.local_seat) ? kPanelOfSeat[size_t(plan.local_seat)] : physical_pad;
}

inline PadSeats CanonicalPadSeats(const Plan& plan) {
  PadSeats seats = {-1, -1, -1, -1};
  if (!ValidSeat(plan.local_seat)) return seats;
  for (int seat = 0; seat < kPanels; ++seat) {
    if (seat == plan.local_seat || plan.remote[size_t(seat)]) seats[size_t(kPanelOfSeat[size_t(seat)])] = seat;
  }
  return seats;
}

// In a session the game reads every controller index from here: the physical pad answers on the local seat's index, whatever
// its own number is, and the other indices are the other seats. The game's primary controller stays what it was, as the
// profile, the saves and the race URL hang off it, and it opens its panel by itself: on a guest that is the host's panel.
class LobbyPads {
 public:
  void Reset() {
    NewSession();
    left_open_ = {};
  }

  // What a session opened outlives it until it is closed: nobody else holds those panels.
  void NewSession() {
    pads_ = {};
    own_opened_ = false;
  }

  PadRules Step(double now, const Plan& plan, const Panels& panels, int physical_pad) {
    if (plan.locked) return LockedPadRules(physical_pad);
    PadRules rules;
    if (!ValidSeat(plan.local_seat)) {
      rules[size_t(physical_pad)].own_pad = true;
      CloseLeftOpen(now, panels, rules);
      return rules;
    }
    for (int panel = 0; panel < kPanels; ++panel) {
      left_open_[size_t(panel)] = panel != physical_pad && panels[size_t(panel)].state != PickState::kNone;
    }
    const int own_panel = kPanelOfSeat[size_t(plan.local_seat)];
    const bool guest = plan.local_seat > 0;
    for (int panel = 0; panel < kPanels; ++panel) {
      const size_t index = size_t(panel);
      PadRule& rule = rules[index];
      if (panel != own_panel) {
        const int seat = kSeatOfPanel[index];
        const PanelTarget target = plan.remote[size_t(seat)] ? plan.seats[size_t(seat)] : PanelTarget{};
        rule.virtual_pad = true;
        rule.buttons = pads_[index].Step(now, panels[index], target, !HeldElsewhere(plan, panels, panel, target.character));
        continue;
      }
      rule.own_pad = true;
      rule.source_pad = physical_pad;
      own_opened_ = own_opened_ || panels[index].state != PickState::kNone;
      if (!own_opened_) {
        rule.press = pads_[index].Step(now, panels[index], {PickState::kSelecting, -1}, false);
        continue;
      }
      rule.back_leaves = panels[index].state == PickState::kNone;
      // The track belongs to the host: a confirmed guest must not walk on to the track screen. Nor may it confirm while no
      // other panel is open yet, as the game would take that for everybody being done.
      const bool last_open = panels[index].state == PickState::kSelecting && !RemotePanelSelecting(panels, own_panel);
      if (guest && (panels[index].state == PickState::kConfirmed || last_open)) rule.allow = uint16_t(~(kPadA | kPadStart));
      if (guest && panels[index].state == PickState::kConfirmed) rule.menu_button = kPadStart;
    }
    return rules;
  }

 private:
  void CloseLeftOpen(double now, const Panels& panels, PadRules& rules) {
    for (int panel = 0; panel < kPanels; ++panel) {
      const size_t index = size_t(panel);
      left_open_[index] = left_open_[index] && panels[index].state != PickState::kNone;
      if (!left_open_[index]) continue;
      rules[index].virtual_pad = true;
      rules[index].buttons = pads_[index].Step(now, panels[index], {}, true);
    }
  }

  static bool RemotePanelSelecting(const Panels& panels, int own_panel) {
    for (int panel = 0; panel < kPanels; ++panel) {
      if (panel != own_panel && panels[size_t(panel)].state == PickState::kSelecting) return true;
    }
    return false;
  }

  // A character on a panel that is staying there. Two remote panels swapping characters each show what the other wants, and
  // have to move through the free ones to get there.
  static bool HeldElsewhere(const Plan& plan, const Panels& panels, int panel, int character) {
    for (int other = 0; other < kPanels; ++other) {
      const PanelView& view = panels[size_t(other)];
      if (other == panel || view.state == PickState::kNone || view.character != character) continue;
      const int seat = kSeatOfPanel[size_t(other)];
      if (seat == plan.local_seat || plan.seats[size_t(seat)].character == character) return true;
    }
    return false;
  }

  std::array<PanelPad, kPanels> pads_{};
  std::array<bool, kPanels> left_open_{};
  bool own_opened_ = false;
};


inline std::string CanonicalPanelsLine(int local_seat) {
  std::string line = "canonical_panels";
  for (int seat = 0; seat < kPanels; ++seat) {
    line += " seat" + std::to_string(seat) + "=panel" + std::to_string(kPanelOfSeat[size_t(seat)]);
  }
  return line + " local_seat=" + std::to_string(local_seat);
}

// One panel as this PC holds it.
inline std::string PanelLine(const Plan& plan, const Panels& panels, int panel, int physical_pad) {
  const int seat = kSeatOfPanel[size_t(panel)];
  const std::string name = "panel" + std::to_string(panel);
  const bool local = seat == plan.local_seat;
  if (!local && !plan.remote[size_t(seat)]) return name + " empty";
  const PanelView& view = panels[size_t(panel)];
  return name + " seat=" + std::to_string(seat) + " owner=" + (local ? "local physical=" + std::to_string(physical_pad) : "remote virtual=" + std::to_string(panel)) +
         " character=" + std::to_string(view.character) + " state=" + std::to_string(int(view.state));
}

// ---- Between the threads ------------------------------------------------------------------------------------------------

class Bridge {
 public:
  void Publish(const Plan& plan) {
    std::lock_guard lock(mutex_);
    plan_ = plan;
  }
  Plan plan() const {
    std::lock_guard lock(mutex_);
    return plan_;
  }

  void SetRules(const PadRules& rules) {
    std::lock_guard lock(mutex_);
    rules_ = rules;
  }
  PadRule RuleFor(uint32_t user) const {
    std::lock_guard lock(mutex_);
    return user < rules_.size() ? rules_[user] : PadRule{};
  }

  void NoteOwnBack(bool down, bool leaves) {
    std::lock_guard lock(mutex_);
    back_press_ = back_press_ || (down && !back_down_ && leaves);
    back_down_ = down;
  }
  bool TakeBackPress() {
    std::lock_guard lock(mutex_);
    const bool pressed = back_press_;
    back_press_ = false;
    return pressed;
  }

  void NoteHostReturn(const std::string& command) {
    std::lock_guard lock(mutex_);
    if (plan_.in_race && plan_.local_seat == 0) host_return_ = command;
  }
  std::optional<std::string> TakeHostReturn() {
    std::lock_guard lock(mutex_);
    std::optional<std::string> command;
    command.swap(host_return_);
    return command;
  }

  void NoteOwnMenuButton(bool down) {
    std::lock_guard lock(mutex_);
    menu_press_ = menu_press_ || (down && !menu_down_);
    menu_down_ = down;
  }
  bool TakeMenuPress() {
    std::lock_guard lock(mutex_);
    const bool pressed = menu_press_;
    menu_press_ = false;
    return pressed;
  }

  void ReportOwn(const PanelView& panel, int session) {
    std::lock_guard lock(mutex_);
    own_ = panel;
    own_session_ = session;
  }
  std::optional<PanelView> own(int session) const {
    std::lock_guard lock(mutex_);
    return own_session_ == session ? own_ : std::nullopt;
  }
  void ClearOwn() {
    std::lock_guard lock(mutex_);
    own_.reset();
  }

  // Who holds each pad, as the character screen last saw it: the final URL names pads, and seats are what the session knows.
  void SetPadSeats(const PadSeats& seats, int physical_pad = 0) {
    std::lock_guard lock(mutex_);
    pad_seats_ = seats;
    physical_pad_ = physical_pad;
  }

  // The host's race, kept from the game until every seat can go with it.
  bool HoldRace(const std::string& command) {
    std::lock_guard lock(mutex_);
    const bool guests = plan_.local_seat == 0 &&
                        std::any_of(plan_.remote.begin(), plan_.remote.end(), [](bool remote) { return remote; });
    if (guests) held_ = command;
    return guests;
  }
  struct HeldRace {
    std::string command;
    PadSeats seats;
    int physical_pad = 0;
  };
  std::optional<HeldRace> TakeHeldRace() {
    std::lock_guard lock(mutex_);
    if (held_.empty() || taken_) return std::nullopt;
    taken_ = true;
    return HeldRace{held_, pad_seats_, physical_pad_};
  }
  // `command` replaces the game's own when the race has to be told apart from it.
  void ReleaseRace(const std::string& command = {}) {
    std::lock_guard lock(mutex_);
    if (!command.empty()) held_ = command;
    released_ = true;
  }
  void DropRace() {
    std::lock_guard lock(mutex_);
    held_.clear();
    taken_ = released_ = false;
  }
  std::string TakeReleasedRace() {
    std::lock_guard lock(mutex_);
    if (!released_) return {};
    std::string command = std::move(held_);
    held_.clear();
    taken_ = released_ = false;
    return command;
  }

  void SetOwnPad(const race::PadState& pad) {
    std::lock_guard lock(mutex_);
    own_pad_ = pad;
  }
  race::PadState own_pad() const {
    std::lock_guard lock(mutex_);
    return own_pad_;
  }

  struct RaceStatus {
    bool waiting_for_host = false;
    std::array<std::optional<int>, kPanels> rtt_ms{};
    std::optional<double> host_silence;  // on a guest in a race: seconds since the host's last snapshot
  };
  void SetRaceStatus(const RaceStatus& status) {
    std::lock_guard lock(mutex_);
    race_status_ = status;
  }
  RaceStatus race_status() const {
    std::lock_guard lock(mutex_);
    return race_status_;
  }

  void RequestTravel(const TravelRequest& travel) {
    std::lock_guard lock(mutex_);
    travel_ = travel;
  }
  TravelRequest TakeTravel() {
    std::lock_guard lock(mutex_);
    const TravelRequest travel = travel_;
    travel_ = {};
    return travel;
  }
  // A travel the game could not take yet, unless a newer one came in.
  void RetryTravel(const TravelRequest& travel) {
    std::lock_guard lock(mutex_);
    if (travel_.kind == TravelKind::kNone) travel_ = travel;
  }

 private:
  mutable std::mutex mutex_;
  Plan plan_;
  PadRules rules_{};
  std::optional<PanelView> own_;
  int own_session_ = 0;
  TravelRequest travel_;
  PadSeats pad_seats_ = {-1, -1, -1, -1};
  int physical_pad_ = 0;
  std::string held_;
  bool taken_ = false;
  bool released_ = false;
  bool back_down_ = false;
  bool back_press_ = false;
  bool menu_down_ = false;
  bool menu_press_ = false;
  std::optional<std::string> host_return_;
  race::PadState own_pad_;
  RaceStatus race_status_;
};

// ---- The session around the screen --------------------------------------------------------------------------------------

struct FlowResult {
  TravelRequest travel;
  bool leave = false;
  const char* source = "menu";  // of a leave, for the log
  std::string note;  // for the log
};

inline constexpr double kPeerGoneSeconds = 15.0;  // SSF2T's establishment budget, so a relay can come back in it
inline constexpr double kDeadRaceSeconds = 45.0;

enum class RaceWatch { kFine, kHostLeft, kConnectionLost };

// The fallback for what the service cannot say: it needs both the silence and a reason for it. Membership comes first, so a
// `ready` flag, a lost packet or a stall never ends a race, and only a very long silence does with the host still seated.
inline RaceWatch WatchRace(const online::Snapshot& state, std::optional<double> silence) {
  if (!state.room || state.seat <= 0 || !silence) return RaceWatch::kFine;
  if (*silence >= kDeadRaceSeconds) return RaceWatch::kConnectionLost;
  if (*silence < kPeerGoneSeconds) return RaceWatch::kFine;
  if (state.reconnecting) return RaceWatch::kConnectionLost;
  for (const online::Seat& seat : state.room->seats) {
    if (seat.seat == 0 && seat.state == online::SeatState::kReconnecting) return RaceWatch::kHostLeft;
  }
  return RaceWatch::kFine;
}

// The one path into and out of the host's game: a guest travels once per session and leaves with B on a closed panel; whoever
// was in a session's race goes to the main menu when the session ends, and a game back in the menus ends its part of it.
class LobbyFlow {
 public:
  FlowResult Update(const online::Snapshot& state, menu::GameScreen screen, bool in_menus, bool back_press,
                    bool race_starting = false) {
    raced_session_ = raced_session_ || (state.room && state.room->state == online::RoomState::kRacing && !in_menus);
    if (state.room && state.seat > 0) return Guest(*state.room, state.seat, screen, in_menus, back_press);
    if (!room_.empty() || (raced_session_ && !state.room)) return SessionEnded();
    FlowResult result;
    if (state.room && state.room->state == online::RoomState::kOpen) raced_session_ = false;
    // The way back walks the main menu and the competition screen by itself.
    if (!state.room || screen == menu::GameScreen::kCharacter || screen == menu::GameScreen::kTrack) continuing_ = false;
    if (continuing_) return result;
    if (state.room && raced_session_ && in_menus && !race_starting) {
      // The game is already on its way to wherever the host sent it.
      raced_session_ = false;
      return LeftTheRace();
    }
    const bool left_screen = screen == menu::GameScreen::kMainMenu || screen == menu::GameScreen::kCompetition;
    if (state.room && state.seat == 0 && (back_press || left_screen) && !race_starting) {
      result.leave = true;
      result.note = "host left character select";
    }
    return result;
  }

  bool in_lobby() const { return step_ == Step::kInLobby; }

  // The host's game is going on with the session, and the room is on its way back to open.
  void NoteContinuing() { continuing_ = true; }
  // The race the game just launched is this PC's alone.
  void Reset() {
    room_.clear();
    step_ = Step::kJoined;
    raced_session_ = false;
    continuing_ = false;
    next_race_ = false;
  }

 private:
  enum class Step { kJoined, kTravelling, kInLobby };

  // The race is the host's: a guest follows the room, once per generation, and only ever from the menus.
  FlowResult GuestRace(const online::RoomView& room, int seat, bool in_menus) {
    FlowResult result;
    const std::optional<CanonicalRace> race = RaceOfRules(room.rules);
    if (!race || !SlotOfSeat(*race, seat)) return Forget("race aborted reason=race_payload_invalid");
    if (raced_session_ && in_menus && room.state == online::RoomState::kRacing) return LeftTheRace();
    if (race->generation <= raced_ || room.state != online::RoomState::kRacing || (!in_menus && !next_race_)) return result;
    raced_ = race->generation;
    last_race_ = *race;
    next_race_ = false;
    result.travel.kind = TravelKind::kRace;
    result.travel.race = *race;
    result.travel.local_seat = seat;
    result.note = "race start generation=" + std::to_string(race->generation) + " seat=" + std::to_string(seat);
    return result;
  }

  // Only the host's step moves a guest off its results, and each step once.
  FlowResult FollowHost(const online::RoomView& room, int seat) {
    FlowResult result;
    const std::optional<AfterRaceStep> step = AfterRaceOfRules(room.rules);
    if (!step || step->generation <= followed_) return result;
    followed_ = step->generation;
    if (step->destination == AfterRace::kNextRace) {
      next_race_ = true;
      result.note = "after_race host goes on to a race generation=" + std::to_string(step->generation);
      return result;
    }
    const std::optional<GameSetup> setup = SetupOfRules(room.rules);
    if (!setup) return Forget("guest setup rejected");
    raced_session_ = false;
    step_ = Step::kTravelling;
    result.travel.setup = *setup;
    result.travel.race = last_race_;
    result.travel.local_seat = seat;
    result.travel.kind = step->destination == AfterRace::kTrackSelect ? TravelKind::kTrackSelect : TravelKind::kEntry;
    result.note = "after_race follow destination=" + std::to_string(int(step->destination)) +
                  " generation=" + std::to_string(step->generation);
    return result;
  }

  FlowResult Guest(const online::RoomView& room, int seat, menu::GameScreen screen, bool in_menus, bool back_press) {
    FlowResult result;
    if (room.room != room_) {
      room_ = room.room;
      step_ = Step::kJoined;
      raced_ = 0;
      followed_ = 0;
    }
    if (room.state == online::RoomState::kOpen && raced_session_) return FollowHost(room, seat);
    if (room.state != online::RoomState::kOpen) return GuestRace(room, seat, in_menus);
    if (step_ == Step::kJoined && in_menus) {
      const std::optional<GameSetup> setup = SetupOfRules(room.rules);
      if (!setup) return Forget("guest setup rejected");
      step_ = Step::kTravelling;
      result.travel = {TravelKind::kEntry, *setup};
      result.note = "guest setup received mode=" + std::to_string(setup->mode) + " difficulty=" + std::to_string(setup->difficulty);
    } else if (step_ == Step::kTravelling && (screen == menu::GameScreen::kCharacter || screen == menu::GameScreen::kTrack)) {
      step_ = Step::kInLobby;
      result.note = "guest entered character select";
    } else if (step_ == Step::kInLobby && back_press) {
      FlowResult left = Forget("guest left character select");
      left.travel.kind = TravelKind::kMainMenu;
      return left;
    }
    return result;
  }

  // The service drops the room as soon as it is told to leave, so this is not seen again.
  FlowResult Forget(const std::string& note) {
    room_.clear();
    step_ = Step::kJoined;
    FlowResult result;
    result.leave = true;
    result.note = note;
    return result;
  }

  // The game's own menus are past the race, and there is no way back into the session from them.
  FlowResult LeftTheRace() {
    FlowResult left = Forget("left the race for the menus");
    left.source = "race_return";
    return left;
  }

  FlowResult SessionEnded() {
    room_.clear();
    step_ = Step::kJoined;
    raced_ = 0;
    followed_ = 0;
    raced_session_ = false;
    continuing_ = false;
    next_race_ = false;
    FlowResult result;
    result.note = "session ended";
    result.travel.kind = TravelKind::kMainMenu;
    return result;
  }

  std::string room_;
  Step step_ = Step::kJoined;
  int raced_ = 0;
  int followed_ = 0;
  bool raced_session_ = false;
  bool continuing_ = false;
  bool next_race_ = false;
  CanonicalRace last_race_;
};

// The local panel as the session sees it, at most `kMinGapSeconds` apart: the last state always goes out.
class PickPacer {
 public:
  static constexpr double kMinGapSeconds = 0.12;

  std::optional<PanelView> Due(double now, const PanelView& panel) {
    if (sent_ && sent_->state == panel.state && sent_->character == panel.character) return std::nullopt;
    if (sent_ && now - at_ < kMinGapSeconds) return std::nullopt;
    sent_ = panel;
    at_ = now;
    return panel;
  }
  void Reset() { sent_.reset(); }

 private:
  std::optional<PanelView> sent_;
  double at_ = 0;
};

}  // namespace eck::lobby
