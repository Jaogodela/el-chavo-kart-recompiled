// The Unlock race tracks option (src/track_unlock.h) against a model of the game's own track lock and save, and the online
// path a track the local save has locked takes.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "character_unlock.h"
#include "guest_memory_model.h"
#include "online_lobby.h"
#include "online_race_start.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"
#include "online_test_support.h"
#include "track_unlock.h"

using online_test::Check;
using namespace eck;
using namespace eck::tracks;
namespace layout = eck::roster::layout;
using eck::roster::Watched;
using guest_model::Bytes;

namespace {

// KartDefaultData_Challenges.UnlockedTracks.
const std::vector<int> kDefaultSave = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
// KartDefaultData_Challenges.Definitions[challenge].bUnlockTrackID, for the challenges that have one.
const std::map<int, int> kTrackOfChallenge = {{3, 10}, {7, 11}, {11, 12}, {15, 13}, {19, 14},
                                              {23, 15}, {27, 16}, {31, 17}, {35, 18}, {39, 19}};
constexpr int kChallenges = 40;

// A checkout with CRLF line ends (Git for Windows' default) reads the same as an LF one: the checks are about the text.
std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out = text.str();
  std::erase(out, '\r');
  return out;
}

int Count(const std::string& text, const std::string& what) {
  int count = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
  return count;
}

// KartScreen_TrackSelectMenu.IsTrackLocked: UnlockedTracks[trackIndex] == 0, and an index outside the array reads 0.
bool GameLocked(const std::vector<int>& unlocked, int track) {
  return track < 0 || size_t(track) >= unlocked.size() || unlocked[size_t(track)] == 0;
}

// KartScoreDao.CreateChallengesJSON and HasUnblockedAllTracks.
std::string SavedChallenges(const std::vector<int>& unlocked, int challenge_count) {
  std::string json = "UnlockedTracksSize=" + std::to_string(unlocked.size());
  for (size_t i = 0; i < unlocked.size(); ++i) json += ";UnlockedTrack" + std::to_string(i) + "=" + std::to_string(unlocked[i]);
  return json + ";UnlockedChallengeCount=" + std::to_string(challenge_count);
}
bool AllTracksAchievement(const std::vector<int>& unlocked) {
  return std::none_of(unlocked.begin(), unlocked.end(), [](int value) { return value == 0; });
}

// KartProgressManager_Challenges.OnRaceEnded: only winning the newest challenge counts.
void ChallengeWon(std::vector<int>& unlocked, int& challenge_count, int challenge) {
  if (challenge != challenge_count - 1) return;
  const auto track = kTrackOfChallenge.find(challenge);
  if (track != kTrackOfChallenge.end()) unlocked[size_t(track->second)] = 1;
  ++challenge_count;
}

// KartScreen_TrackSelectMenu.OnInitComplete: LockItem for every entry the save has locked.
std::vector<int> LockedListItems(const std::vector<int>& unlocked, bool option) {
  std::vector<int> items;
  for (size_t i = 0; i < unlocked.size(); ++i) {
    if (unlocked[i] == 0 && LockedForTrack(true, int(i), option)) items.push_back(int(i));
  }
  return items;
}

void Roster() {
  Check(kRoster.size() == size_t(kTracks) && kTracks == race_telemetry::kTrackCount, "the roster is analytics'");
  Check(kDefaultSave.size() == size_t(kTracks), "one save entry per track");
  std::set<std::string> maps, names;
  std::set<int> challenges;
  for (int track = 0; track < kTracks; ++track) {
    const Track& entry = kRoster[size_t(track)];
    Check(race_telemetry::TrackMapName(track) == std::string(entry.map), "the map is the one analytics and the room use");
    Check(race_telemetry::TrackOfMap(entry.map) == track, "and the map leads back to its id");
    maps.insert(entry.map);
    names.insert(entry.name);
    Check(IsPlayable(track), "every roster id is playable");
    Check(IsLockedAtStart(track) == (kDefaultSave[size_t(track)] == 0), "locked at start is the game's default save");
    if (IsLockedAtStart(track)) {
      challenges.insert(entry.challenge);
      Check(kTrackOfChallenge.count(entry.challenge) == 1 && kTrackOfChallenge.at(entry.challenge) == track, "its challenge is the game's");
    }
  }
  Check(maps.size() == 20 && names.size() == 20, "twenty distinct tracks");
  Check(challenges.size() == 10 && LockedAtStartCount() == 10, "ten tracks are behind challenges");
  Check(IsLockedAtStart(10) && IsLockedAtStart(19) && !IsLockedAtStart(9) && !IsLockedAtStart(0), "ids 10 to 19 are the locked ones");
  Check(!IsPlayable(-1) && !IsPlayable(20) && !IsPlayable(255), "ids outside the roster are not tracks");
  Check(!race_telemetry::TrackOfMap("RET-Jump") && !race_telemetry::TrackOfMap("RETF-Abanderado") && !race_telemetry::TrackOfMap("StartMenu") &&
            !race_telemetry::TrackOfMap("TitleMap"),
        "challenge and menu maps are not race tracks");
}

void Decision() {
  for (int track = 0; track < kTracks; ++track) {
    const bool locked = GameLocked(kDefaultSave, track);
    Check(LockedForTrack(locked, track, false) == locked, "off keeps the game's answer");
    Check(!LockedForTrack(locked, track, true), "on leaves nothing locked");
    Check(!LockedForTrack(false, track, false) && !LockedForTrack(false, track, true), "a track already available stays available");
  }
  for (const int invalid : {-1, 20, 21, 99, 255}) {
    Check(LockedForTrack(true, invalid, true), "an id outside the roster is never opened");
    Check(LockedForTrack(true, invalid, false), "and off leaves it locked too");
    Check(GameLocked(kDefaultSave, invalid), "the model agrees");
  }

  std::vector<int> save = kDefaultSave;
  int challenge_count = 1;
  const std::string saved = SavedChallenges(save, challenge_count);
  for (const bool option : {false, true, false}) {
    for (int track = 0; track < kTracks; ++track) {
      Check(LockedForTrack(GameLocked(save, track), track, option) == (!option && IsLockedAtStart(track)),
            "turning the option off brings the locks back");
    }
    Check(SavedChallenges(save, challenge_count) == saved, "the save stays the same");
    Check(!AllTracksAchievement(save), "no achievement comes from the option");
  }
  Check(LockedListItems(save, false).size() == 10 && LockedListItems(save, true).empty(), "the list locks follow the option");
}

void Campaign() {
  for (const bool option : {false, true}) {
    std::vector<int> save = kDefaultSave;
    int challenge_count = 1;
    for (int challenge = 0; challenge < kChallenges; ++challenge) {
      const std::vector<int> before = save;
      ChallengeWon(save, challenge_count, challenge + 1);
      Check(save == before && challenge_count == challenge + 1, "a challenge that is not the newest changes nothing");
      (void)LockedListItems(save, option);
      ChallengeWon(save, challenge_count, challenge);
      const auto track = kTrackOfChallenge.find(challenge);
      if (track != kTrackOfChallenge.end()) {
        Check(save[size_t(track->second)] == 1, "winning its challenge unlocks a track");
        Check(!LockedForTrack(GameLocked(save, track->second), track->second, option), "and it is open either way");
      } else {
        Check(save == before, "a challenge with no track unlocks none");
      }
      Check(AllTracksAchievement(save) == (challenge == kChallenges - 1), "the achievement follows only the last real unlock");
    }
    Check(AllTracksAchievement(save) && challenge_count == kChallenges + 1, "the whole campaign unlocks all ten, with or without the option");
  }
}

void GuestReads() {
  Bytes memory;
  memory.Put(layout::kGNames, guest_model::kNames);
  guest_model::PutFunction(memory, 0x42000000, 1, "IsTrackLocked", 0x42001000, 2, "KartScreen_TrackSelectMenu");
  guest_model::PutFunction(memory, 0x42002000, 3, "LockItem", 0x42001000, 2, "KartScreen_TrackSelectMenu");
  guest_model::PutFunction(memory, 0x42003000, 3, "LockItem", 0x42004000, 4, "KartScreen_Pause");
  guest_model::PutFunction(memory, 0x42005000, 3, "LockItem", 0x42006000, 5, "KartScreen_HUDIndividual");
  guest_model::PutFunction(memory, 0x42007000, 6, "SetLockVisible", 0x42008000, 7, "KartScreen_CupsSelectMenu");
  guest_model::PutFunction(memory, 0x42009000, 8, "IsTrackLocked", 0x4200A000, 9, "KartScreen_CupsSelectMenu");

  Check(roster::MayBeWatched(97) && roster::MayBeWatched(23) && !roster::MayBeWatched(96) && !roster::MayBeWatched(24), "the size is the first filter");
  Check(roster::Identify(memory, 0x42000000, 97) == Watched::kTrackLockCheck, "the track lock check is found");
  Check(roster::Identify(memory, 0x42000000, 98) == Watched::kNone, "another build's check is left alone");
  Check(roster::Identify(memory, 0x42002000, 23) == Watched::kTrackItemLock, "the list lock is found");
  Check(roster::Identify(memory, 0x42003000, 23) == Watched::kNone, "the pause menu's LockItem is not the track list's");
  Check(roster::Identify(memory, 0x42005000, 23) == Watched::kNone, "nor the HUD's");
  Check(roster::Identify(memory, 0x42007000, 29) == Watched::kNone, "the cups screen's lock is not watched");
  Check(roster::Identify(memory, 0x42009000, 97) == Watched::kNone, "and a check of the same name on another screen is left alone");

  for (const roster::WatchedFunction& function : roster::kWatched) {
    const bool track = function.watched == Watched::kTrackLockCheck || function.watched == Watched::kTrackItemLock;
    if (track) Check(function.owner == "KartScreen_TrackSelectMenu", "a track function belongs to the free race's screen only");
    Check(function.owner.find("Cup") == std::string_view::npos && function.owner.find("Competition") == std::string_view::npos &&
              function.owner.find("Challenge") == std::string_view::npos,
          "no cup, competition or challenge function is watched");
  }

  constexpr uint32_t kFrame = 0x43000000, kScreen = 0x43001000, kLocals = 0x43002000, kCode = 0x43003000, kProperty = 0x43004000;
  memory.Put(kFrame + layout::kFrameObject, kScreen);
  memory.Put(kFrame + layout::kFrameLocals, kLocals);
  memory.Put(kFrame + layout::kFrameCode, kCode);
  for (uint32_t at = 0; at < layout::kFrameObject; at += 4) memory.Put(kFrame + at, 0);
  memory.Put(kCode + 1, 0);
  memory.Put(kCode + 1 + layout::kScriptPointerLowWord, kProperty);
  memory.Put(kProperty + layout::kPropertyOffset, 0x4A8);
  memory.Put(kLocals + 0x4A8, 14);
  memory.Put(kScreen + 0x4A8, 19);
  memory.PutByte(kCode, layout::kTokenLocalVariable);
  Check(roster::IntArgument(memory, kFrame) == 14, "the track of a local (SelectTrack, OnInitComplete)");
  memory.PutByte(kCode, layout::kTokenInstanceVariable);
  Check(roster::IntArgument(memory, kFrame) == 19, "the track of a field (OnPressLeftButton, OnPressRightButton)");
  memory.PutByte(kCode, 0x1D);
  Check(!roster::IntArgument(memory, kFrame), "any other argument is not guessed at");
  Check(LockedForTrack(true, roster::IntArgument(memory, kFrame).value_or(-1), true), "and keeps the game's answer");
}

// KartScreen_TrackSelectMenu: OnPressLeftButton and OnPressRightButton show the lock from IsTrackLocked, SelectTrack starts
// the race only when it is false, and the movie's A calls SelectTrack with no check of its own.
void TrackScreen() {
  for (const bool option : {false, true}) {
    for (int track = 0; track < kTracks; ++track) {
      const bool locked = LockedForTrack(GameLocked(kDefaultSave, track), track, option);
      const bool lock_visible = locked;
      const bool name_hidden = lock_visible;  // CKTrackSelect.SetLockVisible hides txtTab with lockedMC
      const bool race_starts = !locked;
      if (option) {
        Check(!lock_visible && !name_hidden, "the padlock and Bloqueada are gone and the name is shown");
        Check(race_starts, "A starts the race");
      } else {
        Check(lock_visible == IsLockedAtStart(track) && race_starts == !IsLockedAtStart(track), "off is the original screen");
      }
    }
  }
}

void Diagnostics() {
  UnlockLog log;
  const std::vector<std::string> first = log.Note(Surface::kPreview, 14, true, false);
  Check(first.size() == 3, "the roster, the option and the track");
  Check(first[0] == "roster total=20 playable=20 default=10 locked=10", first[0]);
  Check(first[1] == "enabled=yes source=availability_hook", first[1]);
  Check(first[2] == "id=14 name=The Gallery original=locked override=available context=free_race", first[2]);
  for (int i = 0; i < 1000; ++i) Check(log.Note(Surface::kPreview, 14, true, false).empty(), "a repeated check logs nothing");
  const std::vector<std::string> list = log.Note(Surface::kList, 14, true, false);
  Check(list.size() == 1 && list[0] == "id=14 name=The Gallery original=locked override=available context=free_race_list", "the list is its own state");
  Check(log.Note(Surface::kPreview, 3, false, false).back() == "id=3 name=The Museum original=available override=available context=free_race",
        "an open track is noted once");
  Check(log.Note(Surface::kPreview, 77, true, true).back() == "id=77 name=unknown original=locked override=locked context=free_race",
        "an unknown id is not named");
}

void Settings() {
  const std::string root = ECK_REPO_DIR;
  const std::string settings = ReadFile(root + "/src/settings.cpp");
  const std::string launcher = ReadFile(root + "/src/launcher.cpp");
  Check(Count(settings, "REXCVAR_DEFINE_BOOL(eck_unlock_race_tracks, false,") == 1, "the option defaults to off");
  Check(Count(settings, "      \"eck_unlock_race_tracks\",\n") == 1, "the option is one saved setting");
  Check(Count(settings, "GetBool(\"eck_unlock_race_tracks\", false)") == 1, "and the log's default is off");
  Check(Count(launcher, "\"eck_unlock_race_tracks\", false, \"Off\", \"On\"") == 1, "the Gameplay row defaults to off");
  Check(launcher.find("\"eck_unlock_secret_characters\", false, \"Off\", \"On\");\n    ui.Toggle(\"Unlock race tracks\"") != std::string::npos,
        "and sits under Unlock secret characters");
  const std::string table = ReadFile(root + "/src/pt_br_ui_text.cpp");
  Check(Count(table, "Unlock race tracks\tDesbloquear pistas") == 1, "pt-BR label");

  std::set<std::string> settings_named;
  const std::regex name("\"[a-z_]*unlock_race_tracks[a-z_]*\"");
  for (const auto& file : std::filesystem::recursive_directory_iterator(root + "/src")) {
    if (!file.is_regular_file()) continue;
    const std::string text = ReadFile(file.path().string());
    for (std::sregex_iterator it(text.begin(), text.end(), name), end; it != end; ++it) settings_named.insert(it->str());
  }
  Check(settings_named == std::set<std::string>{"\"eck_unlock_race_tracks\""}, "the option is the only name that persists");

  const std::string hook = ReadFile(root + "/src/unlock_hook.cpp");
  const std::string logic = ReadFile(root + "/src/track_unlock.h");
  for (const char* forbidden : {"PlayerData", "UnlockedChallenge", "SavePlayerProfile", "ProfileSettings", "UnlockAchievement",
                                "CheckAllTracksAchievement", "ChallengeDefinitions", "bUnlockTrackID", "fopen", "ofstream", "SaveNow"}) {
    Check(Count(hook, forbidden) == 0, std::string("the hook has nothing to do with ") + forbidden);
    Check(Count(logic, forbidden) == 0, std::string("nor has the logic with ") + forbidden);
  }
  Check(Count(hook, "UnlockedTracks") == 0, "the hook never names the save's array");
  Check(Count(hook, "StoreBE") == 3, "the hook writes a call's result and restores a lock function's script, nothing else");
  Check(hook.find("__imp__sub_82313048(ctx, base);\n  eck::StoreBE<uint16_t>(base, script, first);") != std::string::npos,
        "the script is back before the hook returns");
  Check(Count(hook, "if (!tracks) break;") == 2, "off runs the original call untouched");
  Check(Count(hook, "ClearResult(base, result)") == 2, "only the lock checks' results are cleared");
}

lobby::CanonicalRace RaceOn(int track, int mode) {
  lobby::CanonicalRace race;
  race.generation = 1;
  race.mode = mode;
  race.track = track;
  race.humans = 2;
  race.total = 8;
  for (size_t i = 0; i < race.characters.size(); ++i) race.characters[i] = int(i);
  race.bots = 0xFC;
  race.seats = {0, 1, -1, -1};
  return race;
}

void Online() {
  const std::string worker = ReadFile(std::string(ECK_REPO_DIR) + "/server/eck-lobby/src/game_data.js");
  for (int track = 0; track < kTracks; ++track) {
    for (int mode = 0; mode < int(lobby::kRacePrefixes.size()); ++mode) {
      const lobby::CanonicalRace race = RaceOn(track, mode);
      for (int seat = 0; seat < 2; ++seat) {
        // A guest builds its command from the host's room rules, never from its own save.
        const std::string command = lobby::RaceCommand(race, seat, 0);
        const std::optional<race_url::RaceUrl> url = race_url::ParseRaceUrl(command);
        Check(url.has_value() && race_url::RaceUrlProblems(*url).empty(), "the URL is coherent: " + command);
        Check(url.has_value() && race_telemetry::TrackOfMap(url->map) == track, "and names the host's track on every PC");
      }
      const std::optional<race_url::RaceUrl> host_url = race_url::ParseRaceUrl(lobby::RaceCommand(race, 0, 0));
      const lobby::RaceParse parse = host_url ? lobby::CanonicalOfUrl(*host_url, {0, 1, -1, -1}, 1) : lobby::RaceParse{};
      Check(parse.race.has_value() && parse.race->track == track && parse.race->mode == mode, std::string("the room takes the track: ") + parse.failed);
      const std::optional<lobby::CanonicalRace> again = lobby::RaceOfRules(lobby::RaceRules({}, race));
      Check(again.has_value() && again->track == track && again->mode == mode, "and it survives the room rules");
    }
    Check(Count(worker, "{ id: " + std::to_string(track) + ", map: '" + kRoster[size_t(track)].map + "'") == 1, "the Worker accepts the id and names its map");
  }
  Check(lobby::RaceCommand(RaceOn(10, 0), 1, 0).find("start PistaHeroes_Circuito0?") == 0, "a locked-at-start track starts like any other");
  Check(lobby::RaceCommand(RaceOn(19, 1), 1, 0).find("start TA-PistaElEspacio_Circuito1?") == 0, "with its mode prefix");
  Check(lobby::RaceCommand(RaceOn(14, 0), 0, 0).find("?NumSelectedTrack=14?") != std::string::npos, "and its id");

  const std::filesystem::path src = std::filesystem::path(ECK_REPO_DIR) / "src";
  int online_files = 0;
  for (const auto& file : std::filesystem::recursive_directory_iterator(src)) {
    const std::string name = file.path().filename().string();
    const bool online = name.rfind("online", 0) == 0 || file.path().parent_path().filename() == "online";
    if (!file.is_regular_file() || !online) continue;
    ++online_files;
    const std::string text = ReadFile(file.path().string());
    for (const char* lock : {"UnlockedTracks", "IsTrackLocked", "LockItem", "eck_unlock_race_tracks", "track_unlock.h", "TrackSelectMenu"}) {
      Check(Count(text, lock) == 0, name + " does not know the track lock: " + lock);
    }
  }
  Check(online_files > 20, "the whole online client was checked");
  const std::string hub = ReadFile(std::string(ECK_REPO_DIR) + "/server/eck-lobby/src/hub.js");
  Check(Count(hub, "UnlockedTracks") == 0 && Count(hub, "unlock_race_tracks") == 0, "the Worker knows nothing of the option");
}

}  // namespace

int main() {
  Roster();
  Decision();
  Campaign();
  GuestReads();
  TrackScreen();
  Diagnostics();
  Settings();
  Online();
  return online_test::Finish();
}
