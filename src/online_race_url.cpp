#include "online_race_url.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace eck::race_url {

namespace {

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

std::string Upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}

std::optional<int> ToInt(std::string_view text) {
  text = Trim(text);
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return value;
}

std::vector<int> ToList(std::string_view text) {
  std::vector<int> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t dash = text.find('-', start);
    const std::string_view piece = text.substr(start, dash == std::string_view::npos ? text.size() - start : dash - start);
    if (!piece.empty()) out.push_back(ToInt(piece).value_or(-1));
    if (dash == std::string_view::npos) break;
    start = dash + 1;
  }
  return out;
}

bool KeyIs(std::string_view key, std::string_view name) { return Lower(key) == Lower(name); }

std::string ListText(const std::vector<int>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += std::to_string(values[i]);
  }
  return out + "]";
}

}  // namespace

bool IsRaceStartCommand(std::string_view command) {
  command = Trim(command);
  const std::string lower = Lower(command);
  const bool verb = lower.rfind("open ", 0) == 0 || lower.rfind("start ", 0) == 0;
  if (!verb) return false;
  return lower.find("?numhumankarts=") != std::string::npos || lower.find("?playersarray=") != std::string::npos ||
         lower.find("?totalkarts=") != std::string::npos;
}

std::string GameModeOfPrefix(std::string_view prefix) {
  if (prefix.empty()) return "NormalRace";
  static constexpr std::pair<std::string_view, std::string_view> kModes[] = {
      {"TA", "TimeAttack"},     {"SU", "Survival"},          {"EN", "Endurance"},         {"CT", "CTF"},
      {"MA", "Matador"},        {"MAT", "MatadorTeams"},     {"CO", "Coins"},             {"FO", "FollowTheLeader"},
      {"TUTBRAWL", "TutorialItemsFight"}, {"TUTDRIVING", "TutorialDriving"}, {"RET", "Challenges"},
      {"RETF", "ChallengesCTF"}, {"PO", "Podium"}};
  const std::string upper = Upper(prefix);
  for (const auto& [name, mode] : kModes) {
    if (upper == name) return std::string(mode);
  }
  return "unknown";
}

std::optional<RaceUrl> ParseRaceUrl(std::string_view command) {
  if (!IsRaceStartCommand(command)) return std::nullopt;
  command = Trim(command);
  RaceUrl url;
  const size_t space = command.find(' ');
  url.command = std::string(command.substr(0, space));
  std::string_view rest = Trim(command.substr(space));

  const size_t first = rest.find('?');
  url.map = std::string(Trim(rest.substr(0, first)));
  const size_t dash = url.map.find('-');
  url.map_prefix = dash == std::string::npos ? std::string() : url.map.substr(0, dash);
  url.game_mode = GameModeOfPrefix(url.map_prefix);

  std::string_view options = first == std::string_view::npos ? std::string_view() : rest.substr(first + 1);
  size_t start = 0;
  while (start <= options.size() && !options.empty()) {
    const size_t next = options.find('?', start);
    const std::string_view option = options.substr(start, next == std::string_view::npos ? options.size() - start : next - start);
    if (!option.empty()) {
      const size_t equals = option.find('=');
      const std::string_view key = option.substr(0, equals);
      const std::string_view value = equals == std::string_view::npos ? std::string_view() : option.substr(equals + 1);
      if (KeyIs(key, "NumHumanKarts")) url.num_human_karts = ToInt(value);
      else if (KeyIs(key, "TotalKarts")) url.total_karts = ToInt(value);
      else if (KeyIs(key, "NumSelectedTrack")) url.num_selected_track = ToInt(value);
      else if (KeyIs(key, "KartAIDiffculty")) url.ai_difficulty = ToInt(value);
      else if (KeyIs(key, "GameType")) url.game_type = ToInt(value);
      else if (KeyIs(key, "PrimaryControllerID")) url.primary_controller = ToInt(value);
      else if (KeyIs(key, "LastTipBGKNumber")) url.last_tip_background = ToInt(value);
      else if (KeyIs(key, "GoBackType")) url.go_back_type = ToInt(value);
      else if (KeyIs(key, "NumGameMode")) url.num_game_mode = ToInt(value);
      else if (KeyIs(key, "LastTipTextField")) url.last_tip_text = std::string(value);
      else if (KeyIs(key, "PlayersArray")) url.players = ToList(value);
      else if (KeyIs(key, "CharArray")) url.characters = ToList(value);
      else if (KeyIs(key, "IsBotArray")) url.bots = ToList(value);
      else if (KeyIs(key, "TeamData")) url.teams = ToList(value);
      else url.other.emplace_back(std::string(key), std::string(value));
    }
    if (next == std::string_view::npos) break;
    start = next + 1;
  }
  return url;
}

std::vector<std::string> DescribeRaceUrl(const RaceUrl& url) {
  std::vector<std::string> lines;
  lines.push_back("command=" + url.command);
  lines.push_back("map=" + url.map);
  lines.push_back("game_mode=" + url.game_mode + "(" + url.map_prefix + ")");
  const auto number = [&](const char* name, const std::optional<int>& value) {
    if (value) lines.push_back(std::string(name) + "=" + std::to_string(*value));
  };
  number("NumHumanKarts", url.num_human_karts);
  number("TotalKarts", url.total_karts);
  number("NumSelectedTrack", url.num_selected_track);
  number("KartAIDiffculty", url.ai_difficulty);
  number("GameType", url.game_type);
  number("PrimaryControllerID", url.primary_controller);
  number("LastTipBGKNumber", url.last_tip_background);
  number("GoBackType", url.go_back_type);
  number("NumGameMode", url.num_game_mode);
  if (!url.last_tip_text.empty()) lines.push_back("LastTipTextField=" + url.last_tip_text);
  if (!url.players.empty()) lines.push_back("PlayersArray=" + ListText(url.players));
  if (!url.characters.empty()) lines.push_back("CharArray=" + ListText(url.characters));
  if (!url.bots.empty()) lines.push_back("IsBotArray=" + ListText(url.bots));
  if (!url.teams.empty()) lines.push_back("TeamData=" + ListText(url.teams));
  for (const auto& [key, value] : url.other) lines.push_back("unknown_option " + key + "=" + value);
  return lines;
}

std::vector<std::string> RaceUrlProblems(const RaceUrl& url) {
  std::vector<std::string> problems;
  const int total = url.total_karts.value_or(url.num_human_karts.value_or(0));
  const auto length = [&](const char* name, const std::vector<int>& values) {
    if (!values.empty() && int(values.size()) != total) {
      problems.push_back(std::string(name) + " has " + std::to_string(values.size()) + " entries for TotalKarts=" +
                         std::to_string(total));
    }
    if (std::find(values.begin(), values.end(), -1) != values.end()) problems.push_back(std::string(name) + " has a non-numeric entry");
  };
  length("PlayersArray", url.players);
  length("CharArray", url.characters);
  length("IsBotArray", url.bots);
  length("TeamData", url.teams);
  if (url.num_human_karts && url.total_karts && *url.num_human_karts > *url.total_karts) {
    problems.push_back("NumHumanKarts exceeds TotalKarts");
  }
  if (url.num_human_karts && !url.bots.empty()) {
    const auto humans = std::count(url.bots.begin(), url.bots.end(), 0);
    if (humans != *url.num_human_karts) {
      problems.push_back("IsBotArray has " + std::to_string(humans) + " humans for NumHumanKarts=" +
                         std::to_string(*url.num_human_karts));
    }
    for (int i = 0; i < *url.num_human_karts && i < int(url.bots.size()); ++i) {
      if (url.bots[i] != 0) {
        problems.push_back("human karts must come first in IsBotArray");
        break;
      }
    }
  }
  if (!url.num_human_karts) problems.push_back("NumHumanKarts missing");
  return problems;
}

}  // namespace eck::race_url
