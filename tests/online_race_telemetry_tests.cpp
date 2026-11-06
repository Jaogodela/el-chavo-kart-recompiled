// Race telemetry from what the game shows to what reaches the wire. Pure: scripted game views and a scripted service.

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "online/json.h"
#include "online/online_service.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"
#include "online_test_support.h"

using online_test::Check;
namespace online = eck::online;
namespace rt = eck::race_telemetry;
namespace url = eck::race_url;

namespace {

constexpr uint32_t kMenuGame = 0x1000;
constexpr uint32_t kRaceGame = 0x2000;

const char* kTwoHumans =
    "open CO-PistaMuseo_Circuito0?NumSelectedTrack=3?NumHumanKarts=2?TotalKarts=8?KartAIDiffculty=1?GameType0"
    "?CharArray=6-1-0-2-3-4-5-7?IsBotArray=0-0-1-1-1-1-1-1?PlayersArray=0-1-99-99-99-99-99-99?TeamData=0-1-0-0-0-0-0-0-"
    "?LastTipTextField=tip_3?LastTipBGKNumber=2?PrimaryControllerID=0";
const char* kSolo = "open PistaCiudad_Circuito0?NumSelectedTrack=4?NumHumanKarts=1?TotalKarts=8?GameType0?CharArray=9-1-0-2-3-4-5-6"
                    "?IsBotArray=0-1-1-1-1-1-1-1?PlayersArray=0-99-99-99-99-99-99-99";

rt::RaceSetup SetupOf(const char* command) { return *rt::SetupOfUrl(*url::ParseRaceUrl(command)); }

rt::ServiceView Online() {
  rt::ServiceView view;
  view.ready = true;
  return view;
}

rt::GameView View(uint32_t game, int state, std::vector<rt::KartReading> humans = {}) {
  rt::GameView view;
  view.game = game;
  view.race_state = state;
  view.humans = std::move(humans);
  return view;
}

std::vector<rt::KartReading> Finished(int first_place, int second_place, float time = 100.0f) {
  return {{0, 6, first_place, time}, {1, 1, second_place, time}};
}

// Armed, loaded and through the countdown, so the next in-progress view starts the race.
void Load(rt::RaceTracker& tracker, const char* command, double& now) {
  tracker.Arm(SetupOf(command));
  tracker.Step(View(kMenuGame, 3), Online(), now += 0.25);
  tracker.Step(View(kRaceGame, rt::kCinematic), Online(), now += 0.25);
  tracker.Step(View(kRaceGame, rt::kCountdown), Online(), now += 0.25);
}

void MapsAndIdsComeFromTheScriptTables() {
  Check(rt::TrackOfMap("PistaBrasilM_Circuito0") == 0, "first track");
  Check(rt::TrackOfMap("PistaElEspacio_Circuito1") == 19, "last track");
  Check(rt::TrackOfMap("TA-PistaTanga_Circuito1") == 2, "a mode prefix is not part of the track");
  Check(rt::TrackOfMap("pistaciudad_circuito0") == 4, "map names are not case sensitive");
  Check(!rt::TrackOfMap("RET-Drift2"), "a challenge map is no track");
  Check(!rt::TrackOfMap("StartMenu"), "the menu is no track");
  Check(std::string(rt::CharacterClassName(0)) == "KartPawnChavo", "first character");
  Check(std::string(rt::CharacterClassName(11)) == "KartPawnGodinez", "last character");
  Check(rt::CharacterClassName(12) == nullptr && rt::CharacterClassName(-1) == nullptr, "out of range");
  Check(rt::TrackMapName(20) == nullptr, "track out of range");
  Check(std::string(rt::CompetitionOfPrefix("")) == "normal_race", "no prefix is a normal race");
  Check(std::string(rt::CompetitionOfPrefix("CT")) == "ctf", "CT");
  Check(std::string(rt::CompetitionOfPrefix("RETF")) == "challenges_ctf", "RETF");
  Check(rt::CompetitionOfPrefix("PO") == nullptr, "the podium is not a race");
  Check(rt::CompetitionOfPrefix("TUTDRIVING") == nullptr, "a tutorial is not a race");
}

void SetupReadsARealUrl() {
  const rt::RaceSetup setup = SetupOf(kTwoHumans);
  Check(setup.competition == "coins", "competition from the prefix");
  Check(setup.track == 3, "track from the map");
  Check(setup.humans == 2, "humans");
  Check((setup.characters == std::vector<int>{6, 1}), "only the humans' characters, never the bots'");
  Check((setup.controllers == std::vector<int>{0, 1}), "controllers");
}

void SetupFailsClosed() {
  const auto setup = [](const std::string& command) { return rt::SetupOfUrl(*url::ParseRaceUrl(command)); };
  Check(!setup("open PO-PistaCiudad_Circuito0?NumHumanKarts=1?TotalKarts=8"), "cup podium");
  Check(!setup("open TUTBRAWL-X?NumHumanKarts=1"), "tutorial");
  Check(!setup("open StartMenu?NumHumanKarts=1"), "a menu is no race");
  Check(!setup("open TA-PistaCiudad_Circuito0?NumHumanKarts=5"), "five humans");
  Check(!setup("open TA-PistaCiudad_Circuito0?NumHumanKarts=0"), "no humans");

  const auto wrong_track = setup("open PistaCiudad_Circuito0?NumSelectedTrack=9?NumHumanKarts=1?CharArray=0");
  Check(wrong_track && !wrong_track->track, "a selected track that disagrees with the map is not sent");
  const auto cup = setup("open CO-PistaCiudad_Circuito0?NumHumanKarts=1?NumSelectedTrack=2?GameType=2?CharArray=0");
  Check(cup && cup->track == 4, "a cup counts its own tracks: the map decides");
  const auto challenge = setup("open RET-Drift2?NumHumanKarts=1?NumSelectedTrack=9?GameType=1?CharArray=9");
  Check(challenge && challenge->competition == "challenges" && !challenge->track, "a challenge has no track");
  Check((challenge->characters == std::vector<int>{9}), "challenge character");
  const auto unknown = setup("open TA-PistaCiudad_Circuito0?NumHumanKarts=1?CharArray=12");
  Check(unknown && unknown->characters.empty(), "an id out of range is not sent");
  const auto bots_first = setup("open TA-PistaCiudad_Circuito0?NumHumanKarts=1?CharArray=1-2?IsBotArray=1-0");
  Check(bots_first && bots_first->characters.empty(), "a human slot marked as a bot is not sent");
}

void StartsOnceWhenTheRealRaceBegins() {
  rt::RaceTracker tracker;
  double now = 10;
  Load(tracker, kTwoHumans, now);
  const rt::Output start = tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now += 0.25);
  Check(start.start.has_value(), "race.start at InProgress");
  Check(start.start->competition == "coins" && start.start->players == 2 && start.start->track == 3, "fields");
  Check((start.start->characters == std::vector<int>{6, 1}), "characters");
  Check(start.start->room.empty(), "local race");
  for (int i = 0; i < 20; ++i) {
    Check(!tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now += 0.25).start.has_value(), "no second start");
  }
}

void TheOldWorldNeverStartsARace() {
  rt::RaceTracker tracker;
  tracker.Arm(SetupOf(kTwoHumans));
  double now = 0;
  for (int i = 0; i < 8; ++i) {
    Check(!tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now += 0.25).start.has_value(), "restart from a running race");
  }
  tracker.Step(View(kRaceGame + 8, rt::kCinematic), Online(), now += 0.25);
  Check(tracker.Step(View(kRaceGame + 8, rt::kInProgress), Online(), now += 0.25).start.has_value(), "the new world starts it");
}

void CompletesOnceWithPlacesAndDuration() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now);
  now += 120;
  const rt::Output done = tracker.Step(View(kRaceGame, rt::kFinished, Finished(5, 2, 110.5f)), Online(), now);
  Check(done.complete.has_value(), "race.complete at Finished");
  Check(done.complete->duration_ms == 110500, "the race clock, not the pauses");
  Check((done.complete->places == std::vector<int>{5, 2}), "places by URL order");
  for (int i = 0; i < 20; ++i) {
    Check(!tracker.Step(View(kRaceGame, rt::kFinished, Finished(5, 2)), Online(), now += 0.25).complete.has_value(), "no second complete");
  }
  Check(!tracker.Active(), "done");
}

void PlacesRefuseAnythingUnproven() {
  std::vector<std::string> lines;
  const rt::RaceSetup setup = SetupOf(kTwoHumans);
  Check(rt::PlacesOf(setup, Finished(1, 1), &lines).empty(), "team places tie");
  Check(rt::PlacesOf(setup, Finished(0, 2), &lines).empty(), "a kart without a place");
  Check(rt::PlacesOf(setup, {{0, 6, 1, 0}}, &lines).empty(), "a human with no local kart");
  Check(rt::PlacesOf(setup, {{0, 2, 1, 0}, {1, 1, 2, 0}}, &lines).empty(), "pawn and URL disagree: the layout is wrong");
  Check((rt::PlacesOf(setup, {{1, 1, 4, 0}, {0, 6, 3, 0}}, &lines) == std::vector<int>{3, 4}), "matched by controller, not by order");
  bool logged = false;
  for (const std::string& line : lines) logged = logged || line.find("match=no") != std::string::npos;
  Check(logged, "a layout mismatch is logged");
}

void ASoloRaceSendsNoPlaces() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kSolo, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now);
  const rt::Output done = tracker.Step(View(kRaceGame, rt::kFinished, {{0, 9, 3, 90.0f}}), Online(), now += 100);
  Check(done.complete && done.complete->places.empty(), "nobody to rank against");
}

void DurationStaysInTheServiceRange() {
  Check(rt::DurationMs({}, 0.2) == rt::kMinDurationMs, "floor");
  Check(rt::DurationMs({}, 99999) == rt::kMaxDurationMs, "ceiling");
  Check(rt::DurationMs({{0, 0, 1, 500.0f}}, 90) == 90000, "an estimated clock past the wall clock is ignored");
  Check(rt::DurationMs({{0, 0, 1, std::numeric_limits<float>::quiet_NaN()}}, 90) == 90000, "a bad clock is ignored");
}

void LeavingSendsNothing() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now);
  const rt::Output left = tracker.Step(View(kMenuGame, rt::kMenu), Online(), now += 0.25);
  Check(!left.start && !left.complete, "the service derives the abandon");
  Check(!tracker.Active(), "tracking ends");

  rt::RaceTracker paused;
  Load(paused, kTwoHumans, now);
  paused.Step(View(kRaceGame, rt::kInProgress), Online(), now);
  Check(!paused.Step(View(kRaceGame, rt::kMenu), Online(), now += 1).complete.has_value(), "a pause ends nothing");
  Check(paused.Step(View(kRaceGame, rt::kFinished), Online(), now += 1).complete.has_value(), "the race goes on after a pause");
}

void AFreshRaceSupersedesAnUnfinishedOne() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  const int first = tracker.Step(View(kRaceGame, rt::kInProgress), Online(), now).start->race;
  tracker.Arm(SetupOf(kSolo));
  tracker.Step(View(kRaceGame + 8, rt::kCinematic), Online(), now += 1);
  const rt::Output next = tracker.Step(View(kRaceGame + 8, rt::kInProgress), Online(), now += 1);
  Check(next.start && next.start->race == first + 1, "a new number, which the service reads as superseding");
}

void OnlyTheRoomHostReports() {
  double now = 0;
  rt::ServiceView guest = Online();
  guest.room = "abcd";
  guest.seat = 1;
  rt::RaceTracker seated;
  Load(seated, kTwoHumans, now);
  const rt::Output none = seated.Step(View(kRaceGame, rt::kInProgress), guest, now);
  Check(!none.start.has_value(), "a guest never reports");
  Check(!seated.Active(), "and stops watching");

  rt::ServiceView host = guest;
  host.seat = 0;
  rt::RaceTracker hosting;
  for (int race = 0; race < 2; ++race) {
    Load(hosting, kTwoHumans, now);
    const rt::Output started = hosting.Step(View(kRaceGame, rt::kInProgress), host, now);
    Check(started.start && started.start->room == "abcd", "the host reports with the room");
    Check(started.start && started.start->race == race, "rooms number their own races from 0");
    Check(hosting.Step(View(kRaceGame, rt::kFinished, Finished(1, 2)), host, now += 100).complete->room == "abcd", "complete");
  }
  host.room = "efgh";
  Load(hosting, kTwoHumans, now);
  Check(hosting.Step(View(kRaceGame, rt::kInProgress), host, now).start->race == 0, "a new room starts over");
}

void NotConnectedDropsTheRaceWithoutWaiting() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  const rt::Output none = tracker.Step(View(kRaceGame, rt::kInProgress), rt::ServiceView{}, now);
  Check(!none.start.has_value(), "no start while not connected");
  Check(!tracker.Active(), "and no retry later");

  rt::RaceTracker lost;
  Load(lost, kTwoHumans, now);
  lost.Step(View(kRaceGame, rt::kInProgress), Online(), now);
  Check(!lost.Step(View(kRaceGame, rt::kFinished), rt::ServiceView{}, now += 1).complete.has_value(), "no complete after losing the service");
}

void ARaceThatNeverLoadsIsForgotten() {
  rt::RaceTracker tracker;
  tracker.Arm(SetupOf(kTwoHumans));
  tracker.Step(View(kMenuGame, rt::kMenu), Online(), 1);
  tracker.Step(View(kMenuGame, rt::kMenu), Online(), 1 + rt::kLoadTimeoutSeconds + 1);
  Check(!tracker.Active(), "timed out");
}

void DiscoveryLogsNewIdsOnce() {
  rt::Discovery discovery;
  const std::vector<std::string> first = discovery.Note(SetupOf(kTwoHumans));
  Check(first[0] == "character_slot=0 char_id=6 class=KartPawnNono", first[0]);
  Check(first[1] == "character_slot=1 char_id=1 class=KartPawnQuico", first[1]);
  Check(first[2] == "track_id=3 map=CO-PistaMuseo_Circuito0 name=track_name_museum", first[2]);
  Check(first[3] == "discovery characters=2/12 tracks=1/20", first[3]);
  Check(discovery.Note(SetupOf(kTwoHumans)).size() == 3, "nothing new, no summary");
}

// ---- Finished or left --------------------------------------------------------------------------------------------------

const char* kTimeTrial =
    "open TA-PistaBrasilM_Circuito0?NumSelectedTrack=0?NumHumanKarts=2?TotalKarts=8?GameType0?CharArray=6-1-0-2-3-4-5-7"
    "?IsBotArray=0-0-1-1-1-1-1-1?PlayersArray=0-1-99-99-99-99-99-99";

rt::ServiceView Hosting(std::string room = "abcd") {
  rt::ServiceView view = Online();
  view.room = std::move(room);
  view.seat = 0;
  return view;
}

bool Says(const std::vector<std::string>& lines, const std::string& text) {
  for (const std::string& line : lines) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

// What the host's menu command leaves behind: no game at all, then the menu world or the next race's.
void LeaveTheFinishedWorld(rt::RaceTracker& tracker, const rt::ServiceView& service, double& now) {
  for (const rt::GameView& view : {View(kRaceGame, rt::kFinished, Finished(5, 2)), rt::GameView{}, View(kMenuGame, rt::kCinematic),
                                   View(kMenuGame, rt::kMenu), rt::GameView{}}) {
    const rt::Output out = tracker.Step(view, service, now += 0.25);
    Check(!out.start && !out.complete, "a finished race reports nothing more");
    Check(!Says(out.lines, "race left before the end"), "a finished race was not left");
    Check(!Says(out.lines, "race_state="), "and is no longer watched");
  }
}

int RunToTheFinish(rt::RaceTracker& tracker, const char* command, const rt::ServiceView& service, double& now, float time) {
  Load(tracker, command, now);
  const rt::Output start = tracker.Step(View(kRaceGame, rt::kInProgress), service, now += 0.25);
  Check(start.start.has_value(), "the race started");
  now += time + 1;
  const rt::Output done = tracker.Step(View(kRaceGame, rt::kFinished, Finished(5, 2, time)), service, now);
  Check(done.complete.has_value() && done.complete->race == start.start->race, "race.complete names the race it finishes");
  Check(Says(done.lines, "race.complete race="), "and says so");
  Check(done.complete->duration_ms == int(time * 1000), "one duration");
  Check((done.complete->places == std::vector<int>{5, 2}), "one set of places");
  return start.start->race;
}

void AFinishedRaceStaysCompleteWhateverTheHostDoesNext() {
  for (const char* command : {kTwoHumans, kTimeTrial}) {
    rt::RaceTracker tracker;
    double now = 0;
    RunToTheFinish(tracker, command, Hosting(), now, 29.5f);
    LeaveTheFinishedWorld(tracker, Hosting(), now);
    Check(!tracker.Active(), "tracking ended at the finish");
    LeaveTheFinishedWorld(tracker, Hosting(""), now);
  }
}

void RestartAfterTheFinishStartsANewRaceAndLeavesNothing() {
  rt::RaceTracker tracker;
  double now = 0;
  const int first = RunToTheFinish(tracker, kTwoHumans, Hosting(), now, 60);
  const std::vector<std::string> lines = tracker.Arm(SetupOf(kTwoHumans));
  Check(lines.empty(), "the restart is no departure from a finished race");
  Check(RunToTheFinish(tracker, kTwoHumans, Hosting(), now, 60) == first + 1, "the next race has its own number");
}

void EachRaceOfAReopenedRoomCountsOnItsOwn() {
  rt::RaceTracker tracker;
  double now = 0;
  for (int race = 0; race < 3; ++race) {
    Check(RunToTheFinish(tracker, kTimeTrial, Hosting(), now, 45) == race, "the room goes on counting");
    // An open room gives no telemetry context until the next start.
    LeaveTheFinishedWorld(tracker, Hosting(""), now);
    tracker.Step(View(kMenuGame, rt::kMenu), Hosting(""), now += 30);
  }
}

// Log el_chavo_kart_012: Back to Character Select from the pause menu 0.7 s in, RaceState still 2 and no lap done.
void TheHostMenuCommandIsNoFinish() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.25);
  tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.5);
  const rt::Output left = tracker.Step(rt::GameView{}, Hosting(), now += 0.25);
  Check(!left.complete.has_value(), "leaving a running race never completes it");
  Check(Says(left.lines, "race left before the end race=0 last_state=2 seconds=0 reason=world_gone"), "the evidence is logged");
  Check(!tracker.Active(), "tracking ends");
}

void AQuitFromThePauseMenuIsNoFinish() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTimeTrial, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.25);
  tracker.Step(View(kRaceGame, rt::kMenu), Hosting(), now += 15);
  const rt::Output left = tracker.Step(View(kMenuGame, rt::kCinematic), Hosting(), now += 1);
  Check(!left.complete.has_value(), "a Time Trial left early is not complete");
  Check(Says(left.lines, "last_state=3 seconds=15"), "the last state seen in the race");
}

void RestartDuringARaceLeavesThatRace() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  const int first = tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.25).start->race;
  tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 23);
  Check(Says(tracker.Arm(SetupOf(kTwoHumans)), "race left before the end race=0 last_state=2 seconds=23 reason=new_race"), "said once");
  tracker.Step(View(kRaceGame + 8, rt::kCinematic), Hosting(), now += 1);
  const rt::Output next = tracker.Step(View(kRaceGame + 8, rt::kInProgress), Hosting(), now += 1);
  Check(next.start && next.start->race == first + 1, "the new race has its own number");
  Check(!next.complete.has_value(), "and the old one stays unfinished");
}

void ALostServiceLeavesOnlyARunningRace() {
  rt::RaceTracker racing;
  double now = 0;
  Load(racing, kTwoHumans, now);
  racing.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.25);
  Check(Says(racing.Stop("service_lost"), "reason=service_lost"), "lost before the finish");
  Check(!racing.Active(), "tracking ends");
  Check(racing.Stop("service_lost").empty(), "and only once");

  rt::RaceTracker finished;
  RunToTheFinish(finished, kTwoHumans, Hosting(), now, 60);
  Check(finished.Stop("service_lost").empty(), "a finished race is not undone by a later loss");
}

void AGuestNeverCompletesOrAbandonsARace() {
  rt::ServiceView guest = Hosting();
  guest.seat = 1;
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  const rt::Output begun = tracker.Step(View(kRaceGame, rt::kInProgress), guest, now += 0.25);
  Check(!begun.start.has_value() && Says(begun.lines, "reason=guest_seat"), "a guest stops watching at the start");
  const rt::Output gone = tracker.Step(View(kRaceGame, rt::kFinished, Finished(5, 2)), guest, now += 60);
  Check(!gone.complete.has_value() && gone.lines.empty(), "its own finish reports nothing");
  Check(!Says(tracker.Stop("service_lost"), "race left"), "and its leaving says nothing");
}

void TheHostGoesOnWhenAGuestLeaves() {
  rt::RaceTracker tracker;
  double now = 0;
  Load(tracker, kTwoHumans, now);
  tracker.Step(View(kRaceGame, rt::kInProgress), Hosting(), now += 0.25);
  for (int i = 0; i < 8; ++i) {
    const rt::Output out = tracker.Step(View(kRaceGame, rt::kInProgress, {{0, 6, 0, 0}}), Hosting(), now += 1);
    Check(!out.complete.has_value() && !Says(out.lines, "race left"), "the race is still running");
  }
  Check(tracker.Step(View(kRaceGame, rt::kFinished, Finished(5, 2, 8)), Hosting(), now += 1).complete.has_value(), "until it really ends");
}

// ---- The wire ----------------------------------------------------------------------------------------------------------

struct Wire {
  std::vector<std::string> sent;
  std::vector<online::ChannelEvent> incoming;
  size_t read = 0;
};

class FakeChannel final : public online::LobbyChannel {
 public:
  explicit FakeChannel(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
  bool Poll(online::ChannelEvent& out) override {
    if (wire_->read >= wire_->incoming.size()) return false;
    out = wire_->incoming[wire_->read++];
    return true;
  }
  void Send(const std::string& text) override { wire_->sent.push_back(text); }
  void Close() override {}

 private:
  std::shared_ptr<Wire> wire_;
};

struct Rig {
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  std::unique_ptr<online::OnlineService> service;
  int opened = 0;

  explicit Rig(bool telemetry) {
    online::ServiceConfig config;
    config.url = "wss://test";
    config.identity = [] { return online::Identity{std::string(64, 'a'), "ABCD-2345"}; };
    config.open_channel = [this](const std::string&) -> std::unique_ptr<online::LobbyChannel> {
      ++opened;
      return std::make_unique<FakeChannel>(wire);
    };
    service = std::make_unique<online::OnlineService>(std::move(config));
    capabilities = telemetry ? R"({"protocol_version":1,"race_telemetry":true})" : R"({"protocol_version":1})";
  }
  std::string capabilities;

  void SignIn() {
    online::UserSettings settings;
    settings.enabled = true;
    service->Apply(settings);
    service->Update(1);
    wire->incoming.push_back({online::ChannelEvent::Kind::kOpen, ""});
    service->Update(1.1);
    wire->incoming.push_back({online::ChannelEvent::Kind::kText, R"({"re":1,"code":"ok","player":"ABCD-2345","name":"x","friend_code":"ABCD-2345","capabilities":)" + capabilities + "}"});
    service->Update(1.2);
  }
};

void EncodesOnlyBoundedRaceSummaries() {
  online::RaceStartFields start;
  start.race = 2;
  start.competition = "coins";
  start.players = 2;
  start.track = 3;
  start.characters = {6, 1};
  Check(online::EncodeRaceStart(9, start) ==
            R"({"op":"race.start","seq":9,"race":2,"competition":"coins","players":2,"track":3,"characters":[6,1]})",
        online::EncodeRaceStart(9, start));
  online::RaceStartFields bare;
  bare.competition = "challenges";
  const std::string text = online::EncodeRaceStart(1, bare);
  Check(text.find("track") == std::string::npos && text.find("characters") == std::string::npos, "unproven fields are left out");
  online::RaceCompleteFields complete;
  complete.race = 2;
  complete.room = "abcd";
  complete.duration_ms = 90000;
  complete.places = {2, 1};
  Check(online::EncodeRaceComplete(4, complete) ==
            R"({"op":"race.complete","seq":4,"room":"abcd","race":2,"duration_ms":90000,"places":[2,1]})",
        online::EncodeRaceComplete(4, complete));
}

void ReportsOnlyToASignedInServiceThatTakesThem() {
  Rig off(true);
  online::RaceStartFields start;
  start.competition = "coins";
  Check(!off.service->RaceContext().ready, "not ready before sign-in");
  Check(!off.service->ReportRaceStart(start), "nothing is sent while off");
  Check(off.opened == 0, "reporting never opens a connection");

  Rig older(false);
  older.SignIn();
  Check(older.opened == 1, "signed in");
  Check(!older.service->RaceContext().ready, "a Worker without race telemetry is left alone");
  Check(!older.service->ReportRaceStart(start), "and gets no unknown op");

  Rig current(true);
  current.SignIn();
  Check(current.service->RaceContext().ready, "ready");
  Check(current.service->ReportRaceStart(start), "sent");
  bool on_wire = false;
  for (const std::string& text : current.wire->sent) on_wire = on_wire || text.find("\"race.start\"") != std::string::npos;
  Check(on_wire, "on the wire");
}

void ARefusedReportChangesNothing() {
  Rig rig(true);
  rig.SignIn();
  online::RaceStartFields start;
  start.competition = "coins";
  rig.service->ReportRaceStart(start);
  const std::optional<online::json::Value> sent = online::json::Parse(rig.wire->sent.back());
  const std::string reply = R"({"re":)" + std::to_string(sent->Find("seq")->AsInt64().value_or(0)) + R"(,"code":"bad_request"})";
  rig.wire->incoming.push_back({online::ChannelEvent::Kind::kText, reply});
  rig.service->Update(2);
  Check(rig.service->snapshot().connection == online::Connection::kOnline, "still online");
  Check(rig.service->snapshot().notice.empty(), "the player sees nothing");
  Check(rig.opened == 1, "no reconnect");
}

}  // namespace

int main() {
  MapsAndIdsComeFromTheScriptTables();
  SetupReadsARealUrl();
  SetupFailsClosed();
  StartsOnceWhenTheRealRaceBegins();
  TheOldWorldNeverStartsARace();
  CompletesOnceWithPlacesAndDuration();
  PlacesRefuseAnythingUnproven();
  ASoloRaceSendsNoPlaces();
  DurationStaysInTheServiceRange();
  LeavingSendsNothing();
  AFreshRaceSupersedesAnUnfinishedOne();
  OnlyTheRoomHostReports();
  NotConnectedDropsTheRaceWithoutWaiting();
  ARaceThatNeverLoadsIsForgotten();
  DiscoveryLogsNewIdsOnce();
  AFinishedRaceStaysCompleteWhateverTheHostDoesNext();
  RestartAfterTheFinishStartsANewRaceAndLeavesNothing();
  EachRaceOfAReopenedRoomCountsOnItsOwn();
  TheHostMenuCommandIsNoFinish();
  AQuitFromThePauseMenuIsNoFinish();
  RestartDuringARaceLeavesThatRace();
  ALostServiceLeavesOnlyARunningRace();
  AGuestNeverCompletesOrAbandonsARace();
  TheHostGoesOnWhenAGuestLeaves();
  EncodesOnlyBoundedRaceSummaries();
  ReportsOnlyToASignedInServiceThatTakesThem();
  ARefusedReportChangesNothing();
  return online_test::Finish();
}
