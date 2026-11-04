#pragma once

// The seam between the Online room menu and the race entry. Pure: no guest memory, no sockets. The race-entry probes
// install a hook with SetStartRaceHook; without one the decision is the dry run below.

#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "online/protocol.h"
#include "online_race.h"

namespace eck::online_start {

inline constexpr char kDebugEnv[] = "ECK_ONLINE_DEBUG";
inline constexpr char kLogTag[] = "[DEBUG_SAULO][ONLINE-RACE]";
inline constexpr uint8_t kUnknownSelection = 0xFF;
inline constexpr uint16_t kUnknownTrack = 0xFFFF;

struct Plan {
  bool host = false;
  int local_seat = -1;
  int capacity = 0;
  race::RaceConfig config;
  std::vector<race::KartAssignment> karts;
  online::RoomRules rules;
  bool selection_known = false;  // false: no character or kart came from the players
};

struct Verdict {
  bool real_entry = false;  // a hook proved the guest side and takes over
  bool dry_run = false;     // the mapping was printed and nothing was injected
  std::string reason;       // why there is no real entry
};

using StartRaceHook = std::function<std::optional<Verdict>(const Plan&)>;

inline StartRaceHook& HookSlot() {
  static StartRaceHook hook;
  return hook;
}
inline void SetStartRaceHook(StartRaceHook hook) { HookSlot() = std::move(hook); }

inline bool DebugEnabled() {
  const char* value = std::getenv(kDebugEnv);
  return value && value[0] == '1' && value[1] == 0;
}

inline int RuleValue(const online::RoomRules& rules, const std::string& key, int fallback) {
  for (const auto& rule : rules) {
    if (rule.first == key) return rule.second;
  }
  return fallback;
}

inline uint32_t Fnv1a(const std::string& text, uint32_t seed) {
  uint32_t hash = 2166136261u ^ seed;
  for (const unsigned char c : text) hash = (hash ^ c) * 16777619u;
  return hash;
}

// The same room gives the same plan on every PC: ids and the seed come from the room, never from a local clock.
inline Plan MakePlan(const online::RoomView& room, int local_seat) {
  Plan plan;
  plan.host = local_seat == 0;
  plan.local_seat = local_seat;
  plan.capacity = room.capacity;
  plan.rules = room.rules;
  plan.config.race_id = Fnv1a(room.room, 1);
  plan.config.seed = Fnv1a(room.room, 2);
  plan.config.track = uint16_t(RuleValue(room.rules, "track", kUnknownTrack));
  plan.config.laps = uint8_t(RuleValue(room.rules, "laps", 3));
  plan.config.total_karts = uint8_t(RuleValue(room.rules, "karts", race::kMaxKarts));
  plan.config.rules = uint8_t(RuleValue(room.rules, "items", 1) ? 1 : 0);
  for (const online::Seat& seat : room.seats) {
    if (seat.state == online::SeatState::kEmpty) continue;
    plan.config.participants.push_back({uint8_t(seat.seat), kUnknownSelection, kUnknownSelection});
  }
  plan.karts = race::MapSeatsToKarts(plan.config, local_seat);
  return plan;
}

inline std::string Selection(int value) { return value < 0 || value == kUnknownSelection ? "unknown" : std::to_string(value); }

inline std::vector<std::string> DryRunLines(const Plan& plan) {
  std::vector<std::string> lines;
  const race::RaceCounts counts = race::CountKarts(plan.karts);
  const std::string tag = kLogTag;
  lines.push_back(tag + " dry-run room players=" + std::to_string(plan.config.participants.size()) + "/" +
                  std::to_string(plan.capacity) + " host_seat=0 local_seat=" + std::to_string(plan.local_seat) +
                  " local_player_index=0 role=" + (plan.host ? "host" : "guest") +
                  " karts=" + std::to_string(counts.total) + " humans=" + std::to_string(counts.humans) +
                  " bots=" + std::to_string(counts.bots));
  for (const race::KartAssignment& kart : plan.karts) {
    if (!kart.human) continue;
    lines.push_back(tag + " dry-run seat=" + std::to_string(kart.seat) + " " + (kart.local ? "local" : "remote") +
                    " kart_slot=" + std::to_string(kart.slot) + " player_controller_slot=" +
                    (kart.local ? std::to_string(kart.controller_id) : std::string("none")) +
                    " character=" + Selection(kart.character) + " kart=" + Selection(kart.kart));
  }
  const std::string track = plan.config.track == kUnknownTrack ? "unknown" : std::to_string(plan.config.track);
  lines.push_back(tag + " dry-run url expected ?NumHumanKarts=" + std::to_string(counts.humans) +
                  "?TotalKarts=" + std::to_string(counts.total) + "?NumSelectedTrack=" + track +
                  "?PrimaryControllerID=0 laps=" + std::to_string(plan.config.laps) +
                  " items=" + std::to_string(plan.config.rules) + " race_id=" + std::to_string(plan.config.race_id) +
                  " seed=" + std::to_string(plan.config.seed) +
                  " selection=" + (plan.selection_known ? "known" : "unknown"));
  return lines;
}

inline std::string BlockedLine(const std::string& reason) {
  return std::string(kLogTag) + " start blocked reason=" + reason;
}

inline constexpr char kUnprovenReason[] = "final guest hook unproven";
inline constexpr char kNoDebugReason[] = "ECK_ONLINE_DEBUG is not 1 and the final guest hook is unproven";

// What Start Race does on this PC. `lines` is what the caller must log.
struct Decision {
  Verdict verdict;
  std::vector<std::string> lines;
};

inline Decision Decide(const Plan& plan, bool debug_env) {
  Decision decision;
  if (plan.karts.empty()) {
    decision.verdict.reason = "invalid room plan";
    decision.lines.push_back(BlockedLine(decision.verdict.reason));
    return decision;
  }
  if (const StartRaceHook& hook = HookSlot()) {
    if (const std::optional<Verdict> verdict = hook(plan)) {
      decision.verdict = *verdict;
      if (!verdict->real_entry && !verdict->reason.empty()) decision.lines.push_back(BlockedLine(verdict->reason));
      return decision;
    }
  }
  decision.verdict.reason = debug_env ? kUnprovenReason : kNoDebugReason;
  if (debug_env) {
    decision.verdict.dry_run = true;
    decision.lines = DryRunLines(plan);
  }
  decision.lines.push_back(BlockedLine(decision.verdict.reason));
  return decision;
}

}  // namespace eck::online_start
