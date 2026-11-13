#pragma once

// The requests a test may make of a running game, and their text. Pure: no sockets, no window, no guest memory, so a test
// can check what a line means without a game at all.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

#include "online_race.h"

namespace eck::test_control {

enum class Command {
  kNone,
  kHelp,
  kState,    // what the game is doing and every kart in it
  kConsole,  // any console command the game understands
  kRace,     // start a race on a map, the way the game's own menus do
  kPad,      // the pad this PC reports, so a test can drive
  kShot,     // a picture of the game's window
  kReset,    // release the pad and forget everything held
  kMatch,    // the online side: host a session and invite, accept, start, or leave
  kFriend,   // send a friend request to a friend code, which is what the launcher's Friends page does
  kPick,     // this seat's character, which is what the character screen would say
};

struct Request {
  Command command = Command::kNone;
  std::string text;      // the console command, the map, the picture's path, or the friend code
  race::PadState pad;    // kPad
  int match = 0;         // kMatch: 0 host, 1 accept, 2 leave
  int pick = 0;          // kPick: 0 none, 1 selecting, 2 confirmed
  int character = 0;     // kPick: 0..11
};

inline std::string TrimLeft(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t");
  return first == std::string::npos ? std::string() : text.substr(first);
}

inline int Clamp(int value, int low, int high) { return std::max(low, std::min(high, value)); }

// One line, one request:
//   state
//   console <any console command>
//   race <map>                      the game's own "open <map>" with no options
//   pad <buttons> <lx> <ly> <rt> <lt>   XInput values: buttons 0..65535, sticks -32768..32767, triggers 0..255
//   shot <path.bmp>
//   reset
//   help
// Unknown verbs and malformed numbers are refused, so a typo in a test fails loudly instead of driving the game oddly.
inline std::optional<Request> Parse(const std::string& line) {
  std::istringstream in(line);
  std::string verb;
  if (!(in >> verb)) return std::nullopt;
  Request request;
  // A verb that takes nothing refuses a stray word, so a test with a typo fails instead of half running.
  const auto alone = [&in, &request](Command command) -> std::optional<Request> {
    std::string extra;
    if (in >> extra) return std::nullopt;
    request.command = command;
    return request;
  };
  if (verb == "help") return alone(Command::kHelp);
  if (verb == "state") return alone(Command::kState);
  if (verb == "reset") return alone(Command::kReset);
  if (verb == "console" || verb == "race" || verb == "shot") {
    std::string rest;
    std::getline(in, rest);
    rest = TrimLeft(rest);
    if (rest.empty()) return std::nullopt;
    request.text = rest;
    request.command = verb == "console" ? Command::kConsole : verb == "race" ? Command::kRace : Command::kShot;
    return request;
  }
  if (verb == "pad") {
    int buttons = 0;
    int lx = 0;
    int ly = 0;
    int rt = 0;
    int lt = 0;
    if (!(in >> buttons >> lx >> ly >> rt >> lt)) return std::nullopt;
    if (buttons < 0 || buttons > 0xFFFF) return std::nullopt;
    race::PadState pad;
    pad.buttons = uint16_t(buttons);
    pad.left_trigger = uint8_t(Clamp(lt, 0, 255));
    pad.right_trigger = uint8_t(Clamp(rt, 0, 255));
    pad.left_x = int16_t(Clamp(lx, -32768, 32767));
    pad.left_y = int16_t(Clamp(ly, -32768, 32767));
    request.pad = pad;
    request.command = Command::kPad;
    return request;
  }
  if (verb == "match") {
    std::string what;
    if (!(in >> what)) return std::nullopt;
    if (what == "host") {
      std::string code;
      in >> code;
      if (code.empty()) return std::nullopt;
      request.text = code;
      request.match = 0;
    } else if (what == "accept") {
      request.match = 1;
    } else if (what == "leave") {
      request.match = 2;
    } else if (what == "start") {
      request.match = 3;
    } else {
      return std::nullopt;
    }
    request.command = Command::kMatch;
    return request;
  }
  if (verb == "friend") {
    std::string what;
    std::string code;
    if (!(in >> what >> code) || what != "add") return std::nullopt;
    request.text = code;
    request.command = Command::kFriend;
    return request;
  }
  if (verb == "pick") {
    int state = 0;
    int character = 0;
    if (!(in >> state)) return std::nullopt;
    if (state < 0 || state > 2) return std::nullopt;
    if (state != 0 && !(in >> character)) return std::nullopt;
    if (state != 0 && (character < 0 || character > 11)) return std::nullopt;
    request.pick = state;
    request.character = character;
    request.command = Command::kPick;
    return request;
  }
  return std::nullopt;
}

inline std::string HelpText() {
  return "state | console <text> | race <map> | pad <buttons> <lx> <ly> <rt> <lt> | shot <path.bmp> | "
         "match host <friend-code> | match accept | match start | match leave | friend add <code> | "
         "pick <0|1|2> [character] | reset | help";
}

}  // namespace eck::test_control
