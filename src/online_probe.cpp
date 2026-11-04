#include "online_probe.h"

#include <algorithm>

namespace eck::online_probe {

namespace {

std::string List(const std::vector<int>& values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += "-";
    out += std::to_string(values[i]);
  }
  return out;
}

}  // namespace

DryRun PlanDryRun(const RaceStartPlan& plan) {
  DryRun dry;
  dry.host_seat = plan.host_seat;
  dry.local_seat = plan.local_seat;
  dry.track = plan.track;
  dry.bot_difficulty = plan.bot_difficulty;
  race::RaceConfig config;
  config.track = uint16_t(std::clamp(plan.track, 0, 0xFFFF));
  config.laps = uint8_t(std::clamp(plan.laps, 0, 255));
  config.total_karts = uint8_t(std::clamp(plan.total_karts, 0, 255));
  config.participants = plan.participants;
  const std::vector<race::KartAssignment> karts = race::MapSeatsToKarts(config, plan.local_seat);
  if (karts.empty()) return dry;
  dry.valid = true;
  dry.total_karts = int(karts.size());

  // Virtual pads for remote humans: the first free ids after the local pad 0.
  int next_virtual = 1;
  int local_player = 0;
  for (const race::KartAssignment& kart : karts) {
    if (kart.bot) {
      dry.players_array.push_back(0);
      dry.char_array.push_back(0);
      dry.bot_array.push_back(1);
      ++dry.bots;
      continue;
    }
    SeatLine line;
    line.seat = kart.seat;
    line.slot = kart.slot;
    line.local = kart.local;
    line.character = kart.character;
    line.controller_id = kart.local ? 0 : next_virtual++;
    line.local_player = kart.local ? local_player++ : -1;
    dry.players_array.push_back(line.controller_id);
    dry.char_array.push_back(kart.character);
    dry.bot_array.push_back(0);
    dry.humans.push_back(line);
  }
  dry.num_human_karts = int(dry.humans.size());
  return dry;
}

std::vector<std::string> DryRunLines(const DryRun& dry) {
  std::vector<std::string> lines;
  if (!dry.valid) {
    lines.push_back("dry_run invalid (local seat not in the race or participants/total karts out of range)");
    return lines;
  }
  lines.push_back("dry_run host_seat=" + std::to_string(dry.host_seat) + " local_seat=" + std::to_string(dry.local_seat) +
                  " humans=" + std::to_string(dry.num_human_karts) + " bots=" + std::to_string(dry.bots) +
                  " total_karts=" + std::to_string(dry.total_karts));
  for (const SeatLine& seat : dry.humans) {
    std::string line = "seat=" + std::to_string(seat.seat) + " role=" + (seat.local ? "local" : "remote") +
                       " kart_slot=" + std::to_string(seat.slot) + " character=" + std::to_string(seat.character) +
                       " cpu=no";
    if (seat.local) {
      line += " local_player=" + std::to_string(seat.local_player) + " controller_id=" + std::to_string(seat.controller_id) +
              " pc_slot=" + std::to_string(seat.local_player);
    } else {
      line += " local_player=virtual controller_id=" + std::to_string(seat.controller_id) + " pc_slot=virtual";
    }
    lines.push_back(line);
  }
  lines.push_back("expected_url NumHumanKarts=" + std::to_string(dry.num_human_karts) +
                  " TotalKarts=" + std::to_string(dry.total_karts) + " NumSelectedTrack=" + std::to_string(dry.track) +
                  " KartAIDiffculty=" + std::to_string(dry.bot_difficulty) + " PlayersArray=" + List(dry.players_array) +
                  " CharArray=" + List(dry.char_array) + " IsBotArray=" + List(dry.bot_array) +
                  " PrimaryControllerID=0");
  return lines;
}

std::string BlockedLine(std::string_view reason) { return "start blocked reason=" + std::string(reason); }

const char* Ue3ApiName(Ue3Api api) {
  switch (api) {
    case Ue3Api::kSocket: return "socket";
    case Ue3Api::kSendTo: return "sendto";
    case Ue3Api::kRecvFrom: return "recvfrom";
    case Ue3Api::kCreateNamedNetDriver: return "CreateNamedNetDriver";
    case Ue3Api::kClientTravel: return "ClientTravel";
    case Ue3Api::kLiveCreateOnlineGame: return "CreateOnlineGame";
    case Ue3Api::kLiveJoinOnlineGame: return "JoinOnlineGame";
    case Ue3Api::kXSessionCreate: return "XSessionCreate";
    case Ue3Api::kXSessionJoinRemote: return "XSessionJoinRemote";
    default: return "?";
  }
}

std::string Ue3NetSummaryLine(const std::array<uint64_t, size_t(Ue3Api::kCount)>& counts, std::string_view reason) {
  std::string line = "ue3_net_summary reason=" + std::string(reason);
  std::string active;
  for (size_t i = 0; i < counts.size(); ++i) {
    line += std::string(" ") + Ue3ApiName(Ue3Api(i)) + "=" + std::to_string(counts[i]);
    if (counts[i] != 0) active += (active.empty() ? "" : ",") + std::string(Ue3ApiName(Ue3Api(i)));
  }
  line += active.empty() ? " verdict=none_active" : " verdict=active:" + active;
  return line;
}

}  // namespace eck::online_probe
