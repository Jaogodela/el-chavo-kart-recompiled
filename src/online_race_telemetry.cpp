#include "online_race_telemetry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace eck::race_telemetry {

namespace {

struct Track {
  const char* map;
  const char* locale;
};

// KartGame_MainGame.PlayerCharacterClassNames, indexed by EKartCharacterType.
constexpr std::array<const char*, kCharacterCount> kCharacters = {
    "KartPawnChavo",    "KartPawnQuico", "KartPawnDonRamon",  "KartPawnFlorinda", "KartPawnProfJirafales", "KartPawnPopis",
    "KartPawnNono",     "KartPawnSrBarriga", "KartPawnCleotilde", "KartPawnPaty",     "KartPawnRufino",        "KartPawnGodinez"};

// KartProgressManager_Base.normalTracks and normalTrackLocaleIDs, indexed by EKartTracks.
constexpr std::array<Track, kTrackCount> kTracks = {{
    {"PistaBrasilM_Circuito0", "track_name_brasil_maracana"},
    {"PistaCastillo_Circuito0", "track_name_castle"},
    {"PistaTanga_Circuito1", "track_name_tangamandapio_inv"},
    {"PistaMuseo_Circuito0", "track_name_museum"},
    {"PistaCiudad_Circuito0", "track_name_city"},
    {"PistaBrasilC_Circuito0", "track_name_brasil_sky"},
    {"PistaAcapulco_Circuito0", "track_name_acapulco"},
    {"PistaCastillo_Circuito1", "track_name_castle_inv"},
    {"PistaElEspacio_Circuito0", "track_name_space"},
    {"PistaLaFeria_Circuito0", "track_name_fair"},
    {"PistaHeroes_Circuito0", "track_name_heroes"},
    {"PistaTanga_Circuito0", "track_name_tangamandapio"},
    {"PistaCiudad_Circuito1", "track_name_city_inv"},
    {"PistaBrasilM_Circuito1", "track_name_brasil_maracana_inv"},
    {"PistaMuseo_Circuito1", "track_name_museum_inv"},
    {"PistaBrasilC_Circuito1", "track_name_brasil_sky_inv"},
    {"PistaAcapulco_Circuito1", "track_name_acapulco_inv"},
    {"PistaLaFeria_Circuito1", "track_name_fair_inv"},
    {"PistaHeroes_Circuito1", "track_name_heroes_inv"},
    {"PistaElEspacio_Circuito1", "track_name_space_inv"},
}};

bool SameText(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
           return std::tolower(x) == std::tolower(y);
         });
}

std::string Yes(bool value) { return value ? "yes" : "no"; }

}  // namespace

const char* CompetitionOfPrefix(std::string_view prefix) {
  static constexpr std::pair<std::string_view, const char*> kCompetitions[] = {
      {"", "normal_race"}, {"TA", "time_attack"}, {"SU", "survival"}, {"EN", "endurance"}, {"CO", "coins"},
      {"FO", "follow_the_leader"}, {"CT", "ctf"}, {"RET", "challenges"}, {"RETF", "challenges_ctf"}};
  for (const auto& [name, competition] : kCompetitions) {
    if (SameText(prefix, name)) return competition;
  }
  return nullptr;
}

std::optional<int> TrackOfMap(std::string_view map) {
  const size_t dash = map.find('-');
  if (dash != std::string_view::npos) map.remove_prefix(dash + 1);
  for (int id = 0; id < kTrackCount; ++id) {
    if (SameText(map, kTracks[size_t(id)].map)) return id;
  }
  return std::nullopt;
}

const char* CharacterClassName(int id) { return id >= 0 && id < kCharacterCount ? kCharacters[size_t(id)] : nullptr; }
const char* TrackMapName(int id) { return id >= 0 && id < kTrackCount ? kTracks[size_t(id)].map : nullptr; }
const char* TrackLocaleId(int id) { return id >= 0 && id < kTrackCount ? kTracks[size_t(id)].locale : nullptr; }

std::optional<RaceSetup> SetupOfUrl(const race_url::RaceUrl& url) {
  const char* competition = CompetitionOfPrefix(url.map_prefix);
  const int humans = url.num_human_karts.value_or(0);
  if (competition == nullptr || humans < 1 || humans > kMaxHumans) return std::nullopt;

  RaceSetup setup;
  setup.map = url.map;
  setup.competition = competition;
  setup.humans = humans;
  setup.track = TrackOfMap(url.map);
  // Without a prefix only a known track is a race: the same URL shape could load any other map.
  if (url.map_prefix.empty() && !setup.track) return std::nullopt;
  // NumSelectedTrack is the global track only in Exhibition; a cup counts its own tracks from 0.
  const bool exhibition = !url.game_type || *url.game_type == 0;
  if (exhibition && url.num_selected_track && url.num_selected_track != setup.track) setup.track.reset();

  const auto human = [&](const std::vector<int>& values) {
    return values.size() >= size_t(humans) ? std::vector<int>(values.begin(), values.begin() + humans) : std::vector<int>{};
  };
  const std::vector<int> bots = human(url.bots);
  const bool humans_first = std::all_of(bots.begin(), bots.end(), [](int bot) { return bot == 0; });
  std::vector<int> characters = human(url.characters);
  const bool known = std::all_of(characters.begin(), characters.end(), [](int c) { return c >= 0 && c < kCharacterCount; });
  if (humans_first && known) setup.characters = std::move(characters);
  if (humans_first) setup.controllers = human(url.players);
  return setup;
}

std::vector<int> PlacesOf(const RaceSetup& setup, const std::vector<KartReading>& humans, std::vector<std::string>* lines) {
  if (setup.characters.empty() || setup.controllers.size() != setup.characters.size()) return {};
  std::vector<int> places;
  for (size_t i = 0; i < setup.controllers.size(); ++i) {
    const auto found = std::find_if(humans.begin(), humans.end(),
                                    [&](const KartReading& kart) { return kart.controller_id == setup.controllers[i]; });
    if (found == humans.end()) {
      lines->push_back("layout_check slot=" + std::to_string(i) + " no local kart for controller=" +
                       std::to_string(setup.controllers[i]));
      return {};
    }
    const bool match = found->character == setup.characters[i];
    lines->push_back("layout_check slot=" + std::to_string(i) + " url_char=" + std::to_string(setup.characters[i]) +
                     " pawn_char=" + std::to_string(found->character) + " match=" + Yes(match) +
                     " finish_position=" + std::to_string(found->finish_position));
    if (!match || found->finish_position < 1 || found->finish_position > kMaxPlace) return {};
    places.push_back(found->finish_position);
  }
  std::vector<int> distinct = places;
  std::sort(distinct.begin(), distinct.end());
  if (std::adjacent_find(distinct.begin(), distinct.end()) != distinct.end()) return {};
  return places;
}

int DurationMs(const std::vector<KartReading>& humans, double wall_seconds) {
  double seconds = wall_seconds;
  float best = 0;
  for (const KartReading& kart : humans) {
    if (std::isfinite(kart.racing_time)) best = std::max(best, kart.racing_time);
  }
  if (best > 0 && best <= wall_seconds + 1.0) seconds = best;
  return int(std::clamp(seconds * 1000.0, double(kMinDurationMs), double(kMaxDurationMs)));
}

void RaceTracker::Arm(RaceSetup setup) {
  setup_ = std::move(setup);
  phase_ = Phase::kArmed;
  armed_at_ = -1;
  game_ = 0;
  logged_state_ = -2;
}

void RaceTracker::Drop(std::string_view reason, Output& out) {
  out.lines.push_back("race not reported reason=" + std::string(reason) + " map=" + setup_.map);
  Reset();
}

void RaceTracker::Begin(const ServiceView& service, double now, Output& out) {
  if (!service.ready) return Drop("not_connected", out);
  if (!service.room.empty() && service.seat != 0) return Drop("guest_seat", out);
  room_ = service.room;
  if (!room_.empty() && room_ != numbered_room_) {
    numbered_room_ = room_;
    next_room_ = 0;
  }
  race_ = room_.empty() ? next_local_++ : next_room_++;
  online::RaceStartFields start;
  start.race = race_;
  start.room = room_;
  start.competition = setup_.competition;
  start.players = setup_.humans;
  start.track = setup_.track;
  start.characters = setup_.characters;
  out.lines.push_back("race.start race=" + std::to_string(race_) + " competition=" + start.competition +
                      " players=" + std::to_string(start.players) + " online=" + Yes(!room_.empty()));
  out.start = std::move(start);
  started_at_ = now;
  phase_ = Phase::kRacing;
}

void RaceTracker::Finish(const GameView& game, const ServiceView& service, double now, Output& out) {
  if (!service.ready) return Drop("not_connected_at_finish", out);
  online::RaceCompleteFields complete;
  complete.race = race_;
  complete.room = room_;
  complete.duration_ms = DurationMs(game.humans, now - started_at_);
  if (setup_.humans >= 2) complete.places = PlacesOf(setup_, game.humans, &out.lines);
  out.lines.push_back("race.complete race=" + std::to_string(race_) + " duration_ms=" + std::to_string(complete.duration_ms) +
                      " places=" + std::to_string(complete.places.size()));
  out.complete = std::move(complete);
  Reset();
}

Output RaceTracker::Step(const GameView& game, const ServiceView& service, double now) {
  Output out;
  const int state = game.race_state.value_or(-1);
  const bool opening = game.game != 0 && (state == kCinematic || state == kCountdown);
  if (Active() && state != logged_state_) {
    logged_state_ = state;
    out.lines.push_back("race_state=" + std::to_string(state) + " humans=" + std::to_string(game.humans.size()));
  }
  switch (phase_) {
    case Phase::kIdle:
      break;
    // The old world still shows its own state after the URL, so a race starts only from a world seen opening.
    case Phase::kArmed:
      if (armed_at_ < 0) armed_at_ = now;
      if (opening) {
        game_ = game.game;
        phase_ = Phase::kLoading;
      } else if (now - armed_at_ > kLoadTimeoutSeconds) {
        Drop("never_started", out);
      }
      break;
    case Phase::kLoading:
      if (game.game != game_) {
        if (opening) game_ = game.game;
        else Drop("left_before_start", out);
      } else if (state == kInProgress) {
        Begin(service, now, out);
      }
      break;
    // Leaving a race says nothing: the service counts it abandoned on its own.
    case Phase::kRacing:
      if (game.game != game_ || !game.race_state) {
        out.lines.push_back("race left before the end race=" + std::to_string(race_));
        Reset();
      } else if (state == kFinished) {
        Finish(game, service, now, out);
      }
      break;
  }
  return out;
}

std::vector<std::string> Discovery::Note(const RaceSetup& setup) {
  std::vector<std::string> lines;
  bool fresh = false;
  for (size_t i = 0; i < setup.characters.size(); ++i) {
    const int id = setup.characters[i];
    lines.push_back("character_slot=" + std::to_string(i) + " char_id=" + std::to_string(id) + " class=" + CharacterClassName(id));
    fresh = characters_.insert(id).second || fresh;
  }
  const std::string name = setup.track ? TrackLocaleId(*setup.track) : "unknown";
  lines.push_back("track_id=" + (setup.track ? std::to_string(*setup.track) : std::string("none")) + " map=" + setup.map + " name=" + name);
  if (setup.track) fresh = tracks_.insert(*setup.track).second || fresh;
  if (fresh) {
    lines.push_back("discovery characters=" + std::to_string(characters_.size()) + "/" + std::to_string(kCharacterCount) +
                    " tracks=" + std::to_string(tracks_.size()) + "/" + std::to_string(kTrackCount));
  }
  return lines;
}

}  // namespace eck::race_telemetry
