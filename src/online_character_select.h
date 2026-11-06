#pragma once

// Which of the game's Free Race screens is up, from the movies it opens and the literal ActionScriptVoid calls of its
// screens. The character screen is the online lobby, so Invite Player lives there. Pure.

#include <array>
#include <cstdint>
#include <string_view>

namespace eck::menu {

enum class GameScreen { kOther, kMainMenu, kCompetition, kCharacter, kTrack };

inline constexpr size_t kCharacterPanels = 4;
using PanelObjects = std::array<uint32_t, kCharacterPanels>;

constexpr GameScreen ScreenOfMovie(std::string_view url) {
  constexpr std::array<std::pair<std::string_view, GameScreen>, 4> kMovies{{
      {"ChavoKart_MainMenu/", GameScreen::kMainMenu},
      {"ChavoKart_CompetitionSelect/", GameScreen::kCompetition},
      {"ChavoKart_PlayerSelectManager/", GameScreen::kCharacter},
      {"ChavoKart_TrackSelect/", GameScreen::kTrack},
  }};
  for (const auto& [movie, screen] : kMovies) {
    if (url.find(movie) != std::string_view::npos) return screen;
  }
  return GameScreen::kOther;
}

// The movies past the finish HUD. The game opens them when it first shows them, so one being asked for means that HUD is over.
constexpr bool IsResultMovie(std::string_view url) {
  return url.find("ChavoKart_ResultsScreen/") != std::string_view::npos || url.find("ChavoKart_EndRace/") != std::string_view::npos ||
         url.find("ChavoKart_WinnersPodium/") != std::string_view::npos;
}

class ScreenTracker {
 public:
  void NoteMovie(std::string_view url) {
    results_open_ = results_open_ || IsResultMovie(url);
    if (const GameScreen screen = ScreenOfMovie(url); screen != GameScreen::kOther) pending_ = screen;
  }

  // `screen_class` is the calling screen's UClass. A movie is opened only the first time, but the class stays for the
  // whole run, so one sighting is enough for every later visit.
  void NoteCall(uint32_t screen_class, uint32_t screen, std::string_view method) {
    if (method == "ShowScreen") {
      Learn(screen_class);
      if (const GameScreen found = Find(screen_class); found != GameScreen::kOther) current_ = found;
      if (current_ == GameScreen::kCharacter) Remember(screen_class, screen);
    } else if (method == "HideScreen") {
      if (Find(screen_class) == current_) Leave();
    } else if (method == "OnPressXButton") {
      invite_press_ = invite_press_ || current_ == GameScreen::kCharacter;
    }
  }

  // A map change throws every screen away without hiding it.
  void NoteMapChange() {
    Leave();
    pending_ = GameScreen::kOther;
    results_open_ = false;
  }

  GameScreen current() const { return current_; }
  bool results_open() const { return results_open_; }

  // The manager shows its panels in pad order, one object each: the first class with four objects, or zeros until it shows.
  PanelObjects panel_objects() const {
    for (size_t first = 0; first < shown_count_; ++first) {
      PanelObjects panels{};
      size_t found = 0;
      for (size_t i = first; i < shown_count_ && found < panels.size(); ++i) {
        if (shown_[i].screen_class == shown_[first].screen_class) panels[found++] = shown_[i].object;
      }
      if (found == panels.size()) return panels;
    }
    return {};
  }

  bool TakeInvitePress() {
    const bool pressed = invite_press_;
    invite_press_ = false;
    return pressed;
  }

 private:
  struct Shown {
    uint32_t screen_class = 0;
    uint32_t object = 0;
  };

  void Leave() {
    current_ = GameScreen::kOther;
    shown_count_ = 0;
  }

  void Remember(uint32_t screen_class, uint32_t object) {
    for (size_t i = 0; i < shown_count_; ++i) {
      if (shown_[i].object == object) return;
    }
    if (shown_count_ < shown_.size()) shown_[shown_count_++] = {screen_class, object};
  }

  GameScreen Find(uint32_t screen_class) const {
    for (size_t i = 0; i < classes_.size(); ++i) {
      if (classes_[i] != 0 && classes_[i] == screen_class) return GameScreen(i);
    }
    return GameScreen::kOther;
  }

  void Learn(uint32_t screen_class) {
    if (pending_ == GameScreen::kOther || screen_class == 0) return;
    if (Find(screen_class) == GameScreen::kOther) classes_[size_t(pending_)] = screen_class;
    pending_ = GameScreen::kOther;
  }

  std::array<uint32_t, 5> classes_{};
  GameScreen pending_ = GameScreen::kOther;
  GameScreen current_ = GameScreen::kOther;
  std::array<Shown, 8> shown_{};
  size_t shown_count_ = 0;
  bool invite_press_ = false;
  bool results_open_ = false;
};

}  // namespace eck::menu
