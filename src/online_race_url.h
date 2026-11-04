#pragma once

// Pure parser of the console command KartProgressManager_Base.LoadRace hands to ConsoleCommand
// ("open|start <map>?Key=Value?Key=Value..."), see "Race URL" in docs/online-analysis.md.

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eck::race_url {

struct RaceUrl {
  std::string command;      // "open" or "start", as written
  std::string map;          // text before the first '?'
  std::string map_prefix;   // letters before the '-' of the map, empty for a normal race
  std::string game_mode;    // from [Engine.GameInfo] DefaultMapPrefixes, or "unknown"

  std::optional<int> num_human_karts;
  std::optional<int> total_karts;
  std::optional<int> num_selected_track;
  std::optional<int> ai_difficulty;  // spelled KartAIDiffculty in the URL
  std::optional<int> game_type;
  std::optional<int> primary_controller;
  std::optional<int> last_tip_background;
  std::optional<int> go_back_type;
  std::optional<int> num_game_mode;
  std::string last_tip_text;

  // One entry per kart, '-' separated in the URL. -1 marks an entry that is not a number.
  std::vector<int> players;     // PlayersArray: ControllerId of each kart
  std::vector<int> characters;  // CharArray
  std::vector<int> bots;        // IsBotArray: 1 = CPU
  std::vector<int> teams;       // TeamData

  std::vector<std::pair<std::string, std::string>> other;  // keys this parser does not know, in order
};

// True for "open ..." / "start ..." commands that carry race options. Everything else ConsoleCommand sees is not a race
// start.
bool IsRaceStartCommand(std::string_view command);

// nullopt when the text is not a race start command.
std::optional<RaceUrl> ParseRaceUrl(std::string_view command);

// "TA" -> "TimeAttack", "" -> "NormalRace"; "unknown" for a prefix the game does not list.
std::string GameModeOfPrefix(std::string_view prefix);

// One "name=value" line per field that was present, arrays as "[a,b,c]".
std::vector<std::string> DescribeRaceUrl(const RaceUrl& url);

// Inconsistencies a race needs resolved (array lengths against TotalKarts, human count against IsBotArray, ...). Empty
// when the URL is coherent.
std::vector<std::string> RaceUrlProblems(const RaceUrl& url);

}  // namespace eck::race_url
