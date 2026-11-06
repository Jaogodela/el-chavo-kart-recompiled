// The Unlock secret characters option (src/character_unlock.h) against a model of the game's own lock check and save, and
// the online path a secret character takes.
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "character_unlock.h"
#include "online_character_select.h"
#include "online_lobby.h"
#include "online_race_start.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"
#include "online_test_support.h"

using online_test::Check;
using namespace eck;
using namespace eck::roster;

namespace {

const std::vector<int> kDefaults = {0, 1, 2, 5, 6, 7, 11};  // KartDefaultData_Cups.UnlockedCharacters
const std::map<int, int> kCupOfSecret = {{3, 1}, {4, 1}, {8, 3}, {9, 5}, {10, 7}};  // Definitions[cup].UnlocksCharacters

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

int Count(const std::string& text, const std::string& what) {
  int count = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
  return count;
}

// KartScreen_PlayerSelectManager.GetListOfLockedCharacters and isCharacterLocked.
bool GameLocked(const std::vector<int>& unlocked, int character) {
  if (!IsPlayable(character)) return false;
  return std::find(unlocked.begin(), unlocked.end(), character) == unlocked.end();
}

// KartScoreDao.CreateCupsJSON and HasUnblockedAllCharacters.
std::string SavedCups(const std::vector<int>& unlocked) {
  std::string json = "UnlockedCharactersSize=" + std::to_string(unlocked.size());
  for (size_t i = 0; i < unlocked.size(); ++i) json += ";UnlockedCharacter" + std::to_string(i) + "=" + std::to_string(unlocked[i]);
  return json;
}
bool AllCharactersAchievement(const std::vector<int>& unlocked) { return unlocked.size() == 12; }

class Bytes {
 public:
  void Put(uint32_t address, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes_[address + uint32_t(i)] = uint8_t(value >> (24 - 8 * i));
  }
  void PutByte(uint32_t address, uint8_t value) { bytes_[address] = value; }
  void PutText(uint32_t address, const std::string& text) {
    for (size_t i = 0; i <= text.size(); ++i) bytes_[address + uint32_t(i)] = i < text.size() ? uint8_t(text[i]) : 0;
  }
  bool Readable(uint32_t address, uint32_t length) const {
    for (uint32_t i = 0; i < length; ++i) {
      if (bytes_.count(address + i) == 0) return false;
    }
    return true;
  }
  uint32_t U8(uint32_t address) const { return bytes_.at(address); }
  uint32_t U32(uint32_t address) const {
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; ++i) value = (value << 8) | bytes_.at(address + i);
    return value;
  }

 private:
  std::map<uint32_t, uint8_t> bytes_;
};

constexpr uint32_t kNames = 0x40000000;

void PutName(Bytes& memory, uint32_t object, uint32_t index, const std::string& name) {
  const uint32_t entry = 0x41000000 + index * 0x100;
  memory.Put(kNames + index * 4, entry);
  memory.Put(entry + layout::kNameFlags, 0);
  memory.PutText(entry + layout::kNameText, name);
  memory.Put(object + layout::kObjectName, index);
}

Bytes GameObjects() {
  Bytes memory;
  memory.Put(layout::kGNames, kNames);
  PutName(memory, 0x42000000, 1, "isCharacterLocked");
  memory.Put(0x42000000 + layout::kObjectOuter, 0x42001000);
  PutName(memory, 0x42001000, 2, "KartScreen_PlayerSelectManager");
  PutName(memory, 0x42002000, 3, "SetLockVisible");
  memory.Put(0x42002000 + layout::kObjectOuter, 0x42003000);
  PutName(memory, 0x42003000, 4, "KartSceneCapture2DActor");
  PutName(memory, 0x42004000, 3, "SetLockVisible");
  memory.Put(0x42004000 + layout::kObjectOuter, 0x42005000);
  PutName(memory, 0x42005000, 5, "KartScreen_PlayerSelectIndividual");
  return memory;
}

void Roster() {
  Check(kCharacters == online::kCharacters, "the roster is the session's");
  std::set<int> secret;
  for (int character = 0; character < kCharacters; ++character) {
    if (IsSecret(character)) secret.insert(character);
    const bool is_default = std::find(kDefaults.begin(), kDefaults.end(), character) != kDefaults.end();
    Check(is_default != IsSecret(character), "a character is either default or secret");
    if (IsSecret(character)) Check(kCupOfSecret.at(character) == kRoster[size_t(character)].cup, "its cup is the game's");
  }
  Check(secret.size() == 5 && SecretCount() == 5, "five characters are behind cups");
}

void Decision() {
  for (int character = 0; character < kCharacters; ++character) {
    Check(LockedForPanel(GameLocked(kDefaults, character), character, false, false) == GameLocked(kDefaults, character),
          "off keeps the game's answer");
    Check(!LockedForPanel(GameLocked(kDefaults, character), character, true, false), "on leaves nothing locked");
    Check(!LockedForPanel(false, character, false, false) && !LockedForPanel(false, character, true, true),
          "a character already available stays available");
  }
  for (const int invalid : {-1, 12, 13, 99, 255}) {
    Check(!IsPlayable(invalid) && !IsSecret(invalid), "not a character");
    Check(LockedForPanel(true, invalid, true, true), "an id outside the roster is never opened");
  }

  std::vector<int> save = kDefaults;
  const std::string saved = SavedCups(save);
  for (const bool option : {false, true, false}) {
    for (int character = 0; character < kCharacters; ++character) {
      const bool locked = LockedForPanel(GameLocked(save, character), character, option, false);
      Check(locked == (!option && IsSecret(character)), "turning the option off brings the locks back");
    }
    Check(SavedCups(save) == saved, "the save stays the same");
    Check(!AllCharactersAchievement(save), "no achievement comes from the option");
  }

  save.push_back(9);
  Check(!LockedForPanel(GameLocked(save, 9), 9, false, false), "a cup won with the option on is really won");
  Check(LockedForPanel(GameLocked(save, 10), 10, false, false), "and leaves the others locked");
}

void GuestReads() {
  Bytes memory = GameObjects();
  Check(ObjectName(memory, 0x42000000) == "isCharacterLocked", "names come from the name table");
  Check(ObjectName(memory, 0x42009000).empty(), "an object that cannot be read has no name");
  Check(MayBeWatched(117) && MayBeWatched(279) && !MayBeWatched(0) && !MayBeWatched(116), "the size is the first filter");
  Check(Identify(memory, 0x42000000, 117) == Watched::kLockCheck, "the lock check is found");
  Check(Identify(memory, 0x42000000, 118) == Watched::kNone, "another build's lock check is left alone");
  Check(Identify(memory, 0x42002000, 279) == Watched::kPreviewLock, "the preview's lock is found");
  Check(Identify(memory, 0x42004000, 279) == Watched::kNone, "the panel's own SetLockVisible is not the preview's");

  constexpr uint32_t kFrame = 0x43000000, kPanel = 0x43001000, kLocals = 0x43002000, kCode = 0x43003000, kProperty = 0x43004000;
  memory.Put(kFrame + layout::kFrameObject, kPanel);
  memory.Put(kFrame + layout::kFrameLocals, kLocals);
  memory.Put(kFrame + layout::kFrameCode, kCode);
  for (uint32_t at = 0; at < layout::kFrameObject; at += 4) memory.Put(kFrame + at, 0);
  memory.Put(kCode + 1, 0);
  memory.Put(kCode + 1 + layout::kScriptPointerLowWord, kProperty);
  memory.Put(kProperty + layout::kPropertyOffset, 0x4A8);
  memory.Put(kLocals + 0x4A8, 9);
  memory.Put(kPanel + 0x4A8, 10);
  memory.PutByte(kCode, layout::kTokenLocalVariable);
  Check(CharacterArgument(memory, kFrame) == 9, "the character of a local");
  memory.PutByte(kCode, layout::kTokenInstanceVariable);
  Check(CharacterArgument(memory, kFrame) == 10, "the character of a field");
  memory.PutByte(kCode, 0x1D);
  Check(!CharacterArgument(memory, kFrame), "any other argument is not guessed at");
  Check(LockedForPanel(true, CharacterArgument(memory, kFrame).value_or(-1), true, false), "and keeps the game's answer");
}

void Diagnostics() {
  UnlockLog log;
  const std::vector<std::string> first = log.Note(true, false, 9, true, false);
  Check(first.size() == 3, "the roster, the option and the character");
  Check(first[0] == "roster total=12 playable=12 default=7 secret=5", first[0]);
  Check(first[1] == "enabled=yes source=call_function_hook", first[1]);
  Check(first[2] == "id=9 name=Paty original=locked override=available panel=local", first[2]);
  for (int i = 0; i < 1000; ++i) Check(log.Note(true, false, 9, true, false).empty(), "a repeated check logs nothing");
  Check(log.Note(false, false, 9, true, true).size() == 2, "a change of option logs once");
  Check(log.Note(false, true, 9, true, false).back() == "id=9 name=Paty original=locked override=available panel=remote", "a remote panel");
}

void Settings() {
  const std::string root = ECK_REPO_DIR;
  const std::string settings = ReadFile(root + "/src/settings.cpp");
  const std::string launcher = ReadFile(root + "/src/launcher.cpp");
  Check(Count(settings, "REXCVAR_DEFINE_BOOL(eck_unlock_secret_characters, false,") == 1, "the option defaults to off");
  Check(Count(settings, "      \"eck_unlock_secret_characters\",\n") == 1, "the option is one saved setting");
  Check(Count(settings, "GetBool(\"eck_unlock_secret_characters\", false)") == 1, "and the log's default is off");
  Check(Count(launcher, "\"eck_unlock_secret_characters\", false, \"Off\", \"On\"") == 1, "the Gameplay row defaults to off");
  const std::string table = ReadFile(root + "/src/pt_br_ui_text.cpp");
  Check(Count(table, "Unlock secret characters\tDesbloquear personagens secretos") == 1, "pt-BR label");

  const std::string hook = ReadFile(root + "/src/character_unlock_hook.cpp");
  for (const char* forbidden : {"PlayerData", "UnlockedCharacters", "SavePlayerProfile", "ProfileSettings", "UnlockAchievement", "AddUnlockedCharacter",
                                "fopen", "ofstream", "SaveNow"}) {
    Check(Count(hook, forbidden) == 0, std::string("the hook has nothing to do with ") + forbidden);
  }
  Check(Count(hook, "StoreBE") == 3, "the hook writes a call's result and restores the preview's script, nothing else");
  Check(hook.find("__imp__sub_82313048(ctx, base);\n  eck::StoreBE<uint16_t>(base, script, first);") != std::string::npos,
        "the preview's script is back before the hook returns");
}

online::Snapshot SessionState(int local_seat, int remote_seat, int character) {
  online::RoomView room;
  room.room = "0123456789abcdef";
  room.capacity = 4;
  for (int i = 0; i < 4; ++i) {
    online::Seat seat;
    seat.seat = i;
    if (i == local_seat || i == remote_seat) {
      seat.state = online::SeatState::kConnected;
      seat.player = "player" + std::to_string(i);
    }
    if (i == remote_seat) {
      seat.pick = online::PickState::kConfirmed;
      seat.character = character;
    }
    room.seats.push_back(seat);
  }
  online::Snapshot state;
  state.room = room;
  state.seat = local_seat;
  return state;
}

void Online() {
  constexpr menu::PanelObjects kObjects = {0x1000, 0x2000, 0x3000, 0x4000};
  Check(lobby::kPanelOfSeat == std::array<int, 4>{0, 1, 2, 3} && lobby::kSeatOfPanel == std::array<int, 4>{0, 1, 2, 3}, "seat N is panel N");

  for (const auto& [character, cup] : kCupOfSecret) {
    const lobby::Plan host = lobby::MakePlan(SessionState(0, 1, character), true, kObjects);
    Check(lobby::HasRemotePanels(host), "a session has remote panels");
    Check(lobby::IsRemotePanel(host, kObjects[1]) && !lobby::IsRemotePanel(host, kObjects[0]), "the panels are told apart");
    Check(!lobby::IsRemotePanel(host, kObjects[2]) && !lobby::IsRemotePanel(host, 0) && !lobby::IsRemotePanel(host, 0x9000), "and nothing else is remote");
    const bool remote = lobby::IsRemotePanel(host, kObjects[1]);
    Check(!LockedForPanel(GameLocked(kDefaults, character), character, false, remote), "a remote pick is not refused by this save");
    Check(LockedForPanel(GameLocked(kDefaults, character), character, false, lobby::IsRemotePanel(host, kObjects[0])),
          "this player still needs the option");
    Check(host.seats[1].character == character && host.seats[1].state == online::PickState::kConfirmed, "the pick reaches its panel");

    const lobby::PanelTarget target = host.seats[1];
    lobby::PanelView panel{online::PickState::kSelecting, 0};
    int presses = 0;
    while (panel.character != character && presses++ < online::kCharacters) {
      const uint16_t press = lobby::NextPress(panel, target, true);
      panel.character = (panel.character + (press == lobby::kPadDpadRight ? 1 : online::kCharacters - 1)) % online::kCharacters;
    }
    Check(panel.character == character && lobby::NextPress(panel, target, true) == lobby::kPadA, "the panel walks to it and confirms");

    const lobby::Plan guest = lobby::MakePlan(SessionState(1, 0, character), true, kObjects);
    Check(lobby::IsRemotePanel(guest, kObjects[0]) && !lobby::IsRemotePanel(guest, kObjects[1]), "the host's panel is remote on a guest");
  }
  const lobby::Plan alone = lobby::MakePlan(online::Snapshot{}, true, kObjects);
  Check(!lobby::HasRemotePanels(alone) && !lobby::IsRemotePanel(alone, kObjects[1]), "without a session every panel is local");

  const std::string command =
      "start PistaBrasilM_Circuito0?CharArray=10-9-1-5-0-2-11-7?IsBotArray=0-0-1-1-1-1-1-1?PlayersArray=0-1-99-99-99-99-99-99"
      "?TeamData=0-1-0-1-0-1-0-1-?LastTipTextField=normal_race_tip_5?LastTipBGKNumber=0?PrimaryControllerID=0?NumSelectedTrack=0"
      "?NumHumanKarts=2?TotalKarts=8?KartAIDiffculty=0?GameType0";
  const std::optional<race_url::RaceUrl> url = race_url::ParseRaceUrl(command);
  Check(url.has_value() && url->characters == std::vector<int>({10, 9, 1, 5, 0, 2, 11, 7}), "CharArray carries secret ids");
  Check(url.has_value() && race_url::RaceUrlProblems(*url).empty(), "the URL is coherent");
  if (url) {
    const lobby::RaceParse parse = lobby::CanonicalOfUrl(*url, {0, 1, -1, -1}, 1);
    Check(parse.race.has_value(), std::string("the canonical race takes secret ids: ") + parse.failed);
    if (parse.race) {
      const std::optional<lobby::CanonicalRace> again = lobby::RaceOfRules(lobby::RaceRules({}, *parse.race));
      Check(again.has_value() && again->characters == parse.race->characters, "and they survive the room rules");
    }
  }

  std::set<std::string> classes;
  for (int character = 0; character < kCharacters; ++character) classes.insert(race_telemetry::CharacterClassName(character));
  Check(classes.size() == 12, "analytics has a class for every id");
  const std::string worker = ReadFile(std::string(ECK_REPO_DIR) + "/server/eck-lobby/src/game_data.js");
  for (int character = 0; character < kCharacters; ++character) {
    Check(Count(worker, "{ id: " + std::to_string(character) + ", key: ") >= 1, "the Worker accepts the id");
  }
  Check(Count(worker, "{ id: 9, key: 'paty', name: 'Paty' }") == 1, "and names it the same");
}

}  // namespace

int main() {
  Roster();
  Decision();
  GuestReads();
  Diagnostics();
  Settings();
  Online();
  return online_test::Finish();
}
