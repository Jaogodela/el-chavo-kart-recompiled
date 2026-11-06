// Pure tests: the race URL parser, the six registered probes, the dry-run seat mapping, the UE3 summary line and the
// guest fingerprint tables (synthetic buffers, and the real image when its path is given as the first argument).

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "online_game_guest.h"
#include "online_probe.h"
#include "online_probe_layout.h"
#include "online_race_url.h"

namespace guest = eck::online_guest;
namespace probe = eck::online_probe;
namespace url = eck::race_url;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                       \
  do {                                                    \
    ++g_checks;                                           \
    if (!(cond)) {                                        \
      ++g_failures;                                       \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
    }                                                     \
  } while (0)

const char* kExhibition =
    "open TA-PistaMuseo_Circuito0?NumSelectedTrack=3?NumHumanKarts=2?TotalKarts=8?KartAIDiffculty=1?GameType=0"
    "?PlayersArray=1-0-0-0-0-0-0-0?CharArray=2-1-0-3-4-5-6-7?IsBotArray=0-0-1-1-1-1-1-1?TeamData=0-1-0-0-1-0-0-0"
    "?LastTipTextField=tip_3?LastTipBGKNumber=2?PrimaryControllerID=0";

void ParserReadsEveryField() {
  const auto parsed = url::ParseRaceUrl(kExhibition);
  CHECK(parsed.has_value());
  const url::RaceUrl& u = *parsed;
  CHECK(u.command == "open");
  CHECK(u.map == "TA-PistaMuseo_Circuito0");
  CHECK(u.map_prefix == "TA");
  CHECK(u.game_mode == "TimeAttack");
  CHECK(u.num_human_karts == 2);
  CHECK(u.total_karts == 8);
  CHECK(u.num_selected_track == 3);
  CHECK(u.ai_difficulty == 1);
  CHECK(u.game_type == 0);
  CHECK(u.primary_controller == 0);
  CHECK(u.last_tip_background == 2);
  CHECK(u.last_tip_text == "tip_3");
  CHECK((u.players == std::vector<int>{1, 0, 0, 0, 0, 0, 0, 0}));
  CHECK((u.characters == std::vector<int>{2, 1, 0, 3, 4, 5, 6, 7}));
  CHECK((u.bots == std::vector<int>{0, 0, 1, 1, 1, 1, 1, 1}));
  CHECK((u.teams == std::vector<int>{0, 1, 0, 0, 1, 0, 0, 0}));
  CHECK(u.other.empty());
  CHECK(url::RaceUrlProblems(u).empty());
  const auto lines = url::DescribeRaceUrl(u);
  bool humans = false, players = false;
  for (const auto& line : lines) {
    humans = humans || line == "NumHumanKarts=2";
    players = players || line == "PlayersArray=[1,0,0,0,0,0,0,0]";
  }
  CHECK(humans && players);
}

void ParserRejectsOtherCommands() {
  CHECK(!url::ParseRaceUrl("getAll LocalPlayer ControllerId").has_value());
  CHECK(!url::ParseRaceUrl("DISABLEALLSCREENMESSAGES").has_value());
  CHECK(!url::ParseRaceUrl("open StartMenu").has_value());
  CHECK(!url::ParseRaceUrl("start TA_Cancun?Foo=1").has_value());
  CHECK(!url::ParseRaceUrl("").has_value());
  CHECK(!url::ParseRaceUrl("stat fps?NumHumanKarts=2").has_value());
  CHECK(url::ParseRaceUrl("start EN-PistaTanga_Circuito0?NumHumanKarts=1?TotalKarts=5").has_value());
  CHECK(url::ParseRaceUrl("START EN-PistaTanga_Circuito0?numhumankarts=1").has_value());
}

void UnknownKeysFallThrough() {
  const auto parsed = url::ParseRaceUrl("open SU-PistaHeroes_Circuito0?NumHumanKarts=1?TotalKarts=4?Brand=New?Flag?numselectedtrack=9");
  CHECK(parsed.has_value());
  CHECK(parsed->game_mode == "Survival");
  CHECK(parsed->num_human_karts == 1);
  CHECK(parsed->num_selected_track == 9);  // keys are case-insensitive
  CHECK(parsed->other.size() == 2);
  CHECK(parsed->other[0].first == "Brand" && parsed->other[0].second == "New");
  CHECK(parsed->other[1].first == "Flag" && parsed->other[1].second.empty());
  bool described = false;
  for (const auto& line : url::DescribeRaceUrl(*parsed)) described = described || line == "unknown_option Brand=New";
  CHECK(described);
  CHECK(url::ParseRaceUrl("open ZZ-Odd?NumHumanKarts=1")->game_mode == "unknown");
  CHECK(url::ParseRaceUrl("open MAT-Odd?NumHumanKarts=1")->game_mode == "MatadorTeams");
  CHECK(url::ParseRaceUrl("open PistaCiudad_Circuito0?NumHumanKarts=1")->game_mode == "NormalRace");
  CHECK(url::ParseRaceUrl("open PistaCiudad_Circuito0?NumHumanKarts=1")->map_prefix.empty());
}

void ProblemsAreReported() {
  auto bad = url::ParseRaceUrl("open TA_X?NumHumanKarts=3?TotalKarts=2?PlayersArray=0-1?IsBotArray=1-0");
  CHECK(bad.has_value());
  const auto problems = url::RaceUrlProblems(*bad);
  CHECK(problems.size() >= 2);  // NumHumanKarts above TotalKarts, humans after bots
  auto garbled = url::ParseRaceUrl("open TA_X?NumHumanKarts=1?TotalKarts=2?CharArray=a-1");
  CHECK(garbled.has_value());
  CHECK(garbled->characters[0] == -1);
  CHECK(!url::RaceUrlProblems(*garbled).empty());
  auto missing = url::ParseRaceUrl("open TA_X?TotalKarts=2");
  CHECK(missing.has_value());
  bool noted = false;
  for (const auto& p : url::RaceUrlProblems(*missing)) noted = noted || p == "NumHumanKarts missing";
  CHECK(noted);
}

void SixProbesAreRegistered() {
  const auto& probes = probe::RegisteredProbes();
  CHECK(probes.size() == 6);
  std::set<int> numbers;
  std::set<std::string> names;
  for (const probe::ProbeInfo& p : probes) {
    numbers.insert(p.number);
    names.insert(p.name);
    CHECK(p.unknown && p.unknown[0]);
    CHECK(p.probe && p.probe[0]);
    CHECK(p.log_prefix && p.log_prefix[0]);
    CHECK(p.unlocks && p.unlocks[0]);
  }
  CHECK(numbers == (std::set<int>{1, 2, 3, 4, 5, 6}));
  CHECK(names.size() == 6);
  CHECK(std::string(probe::kRaceTag) == "[DEBUG_SAULO][ONLINE-RACE]");
  CHECK(std::string(probe::kXboxTag) == "[DEBUG_SAULO][ONLINE-XBOX]");
}

probe::RaceStartPlan MakePlan(std::vector<int> seats, int local) {
  probe::RaceStartPlan plan;
  plan.local_seat = local;
  plan.host_seat = 0;
  plan.track = 4;
  plan.bot_difficulty = 2;
  for (int s : seats) plan.participants.push_back({uint8_t(s), uint8_t(10 + s), uint8_t(s)});
  return plan;
}

void DryRunMapsTwoThreeFour() {
  {
    const probe::DryRun dry = probe::PlanDryRun(MakePlan({0, 1}, 1));
    CHECK(dry.valid && dry.num_human_karts == 2 && dry.bots == 6 && dry.total_karts == 8);
    CHECK(dry.humans.size() == 2);
    CHECK(dry.humans[0].seat == 0 && !dry.humans[0].local && dry.humans[0].slot == 0 && dry.humans[0].controller_id == 1);
    CHECK(dry.humans[1].seat == 1 && dry.humans[1].local && dry.humans[1].slot == 1 && dry.humans[1].controller_id == 0);
    CHECK(dry.humans[1].local_player == 0 && dry.humans[0].local_player == -1);
    CHECK((dry.players_array == std::vector<int>{1, 0, 0, 0, 0, 0, 0, 0}));
    CHECK((dry.bot_array == std::vector<int>{0, 0, 1, 1, 1, 1, 1, 1}));
    CHECK((dry.char_array == std::vector<int>{10, 11, 0, 0, 0, 0, 0, 0}));
    // The expected URL parses back to the same counts.
    std::string expected;
    for (const std::string& line : probe::DryRunLines(dry)) {
      if (line.rfind("expected_url ", 0) == 0) expected = line;
    }
    CHECK(!expected.empty());
    CHECK(expected.find("NumHumanKarts=2") != std::string::npos);
    CHECK(expected.find("PlayersArray=1-0-0-0-0-0-0-0") != std::string::npos);
    CHECK(expected.find("IsBotArray=0-0-1-1-1-1-1-1") != std::string::npos);
    CHECK(expected.find("KartAIDiffculty=2") != std::string::npos);
    CHECK(expected.find("NumSelectedTrack=4") != std::string::npos);
  }
  {
    const probe::DryRun dry = probe::PlanDryRun(MakePlan({0, 1, 2}, 0));
    CHECK(dry.valid && dry.num_human_karts == 3 && dry.bots == 5);
    CHECK(dry.humans[0].local && dry.humans[0].controller_id == 0);
    CHECK(dry.humans[1].controller_id == 1 && dry.humans[2].controller_id == 2);
    CHECK((dry.players_array == std::vector<int>{0, 1, 2, 0, 0, 0, 0, 0}));
  }
  {
    const probe::DryRun dry = probe::PlanDryRun(MakePlan({0, 1, 2, 3}, 2));
    CHECK(dry.valid && dry.num_human_karts == 4 && dry.bots == 4);
    CHECK((dry.players_array == std::vector<int>{1, 2, 0, 3, 0, 0, 0, 0}));
    int locals = 0;
    for (const probe::SeatLine& seat : dry.humans) {
      locals += seat.local ? 1 : 0;
      CHECK(!seat.cpu);  // remote seats are never a CPU
    }
    CHECK(locals == 1);
    // Every human in the URL is flagged human (0) in IsBotArray, bots after them.
    for (int i = 0; i < 4; ++i) CHECK(dry.bot_array[size_t(i)] == 0);
    for (int i = 4; i < 8; ++i) CHECK(dry.bot_array[size_t(i)] == 1);
    const auto parsed = url::ParseRaceUrl(
        "open TA_X?NumHumanKarts=4?TotalKarts=8?PlayersArray=1-2-0-3-0-0-0-0?CharArray=10-11-12-13-0-0-0-0"
        "?IsBotArray=0-0-0-0-1-1-1-1");
    CHECK(parsed && url::RaceUrlProblems(*parsed).empty());
  }
  CHECK(!probe::PlanDryRun(MakePlan({0, 1}, 3)).valid);    // local seat not in the race
  CHECK(!probe::PlanDryRun(MakePlan({0}, 0)).valid);       // a race needs two humans
  CHECK(probe::DryRunLines(probe::PlanDryRun(MakePlan({0}, 0)))[0].find("invalid") != std::string::npos);
  const auto lines = probe::DryRunLines(probe::PlanDryRun(MakePlan({0, 1, 2}, 1)));
  bool remote = false, local = false;
  for (const std::string& line : lines) {
    remote = remote || (line.find("seat=0 role=remote kart_slot=0") != std::string::npos && line.find("cpu=no") != std::string::npos);
    local = local || (line.find("seat=1 role=local kart_slot=1") != std::string::npos && line.find("local_player=0") != std::string::npos);
  }
  CHECK(remote && local);
}

void BlockedLineIsExact() {
  CHECK(probe::BlockedLine("peer 2 has no path") == "start blocked reason=peer 2 has no path");
  CHECK(probe::BlockedLine("") == "start blocked reason=");
}

void Ue3SummaryNamesWhatIsActive() {
  std::array<uint64_t, size_t(probe::Ue3Api::kCount)> counts{};
  std::string none = probe::Ue3NetSummaryLine(counts, "race_start_requested");
  CHECK(none.find("verdict=none_active") != std::string::npos);
  CHECK(none.find("reason=race_start_requested") != std::string::npos);
  CHECK(none.find("CreateNamedNetDriver=0") != std::string::npos);
  CHECK(none.find("XSessionJoinRemote=0") != std::string::npos);
  counts[size_t(probe::Ue3Api::kSendTo)] = 12;
  counts[size_t(probe::Ue3Api::kSocket)] = 1;
  const std::string some = probe::Ue3NetSummaryLine(counts, "x");
  CHECK(some.find("verdict=active:socket,sendto") != std::string::npos);
}

void LayoutNames() {
  CHECK(std::string(probe::layout::RaceStateName(0)) == "cinematic");
  CHECK(std::string(probe::layout::RaceStateName(1)) == "countdown");
  CHECK(std::string(probe::layout::RaceStateName(2)) == "racing");
  CHECK(std::string(probe::layout::RaceStateName(4)) == "finished");
  CHECK(std::string(probe::layout::RaceStateName(9)) == "unknown");
}

// Synthetic image: the table's own words at the table's own addresses, 16 bytes each.
std::vector<uint8_t> ImageFor(std::span<const guest::CodeFingerprint> table, uint32_t base, uint32_t size) {
  std::vector<uint8_t> memory(size, 0);
  for (const guest::CodeFingerprint& f : table) {
    for (size_t i = 0; i < 4; ++i) {
      const uint32_t w = f.words[i];
      uint8_t* p = memory.data() + (f.address - base) + 4 * i;
      p[0] = uint8_t(w >> 24);
      p[1] = uint8_t(w >> 16);
      p[2] = uint8_t(w >> 8);
      p[3] = uint8_t(w);
    }
  }
  return memory;
}

void FingerprintMismatchFallsBack() {
  for (const auto table : {std::span<const guest::CodeFingerprint>(guest::kSocketFingerprints),
                           std::span<const guest::CodeFingerprint>(guest::kRaceFingerprints)}) {
    const uint32_t base = 0x82000000;
    const uint32_t size = 0x01400000;
    std::vector<uint8_t> memory = ImageFor(table, base, size);
    CHECK(guest::FirstMismatch(table, memory.data(), memory.size(), base) == nullptr);
    // A changed word names the function and makes the group a pass-through.
    for (const guest::CodeFingerprint& f : table) {
      std::vector<uint8_t> patched = memory;
      patched[f.address - base + 7] ^= 0x01;
      const char* bad = guest::FirstMismatch(table, patched.data(), patched.size(), base);
      CHECK(bad != nullptr && std::string(bad) == f.name);
    }
    CHECK(guest::FirstMismatch(table, nullptr, 0, base) != nullptr);
    CHECK(guest::FirstMismatch(table, memory.data(), 0x100, base) != nullptr);  // outside the buffer
  }
  CHECK(guest::kSocketFingerprints.size() == 3 && guest::kRaceFingerprints.size() == 7);
  CHECK(guest::kActorConsoleCommand == 0x826A1F30 && guest::kCrtSrand == 0x830B53B0);
  CHECK(guest::kNetSocket == 0x82B74930 && guest::kNetSendTo == 0x82B74AA0 && guest::kNetRecvFrom == 0x82B74A68);
}

// With the image path as the first argument: every table must match the real bytes.
void RealImageMatches(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("image: %s not readable, skipped\n", path);
    return;
  }
  const std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const uint32_t base = 0x82000000;
  const auto check = [&](const char* name, std::span<const guest::CodeFingerprint> table) {
    const char* bad = guest::FirstMismatch(table, image.data(), image.size(), base);
    if (bad) std::printf("image: %s mismatch at %s\n", name, bad);
    CHECK(bad == nullptr);
  };
  check("identity", guest::kIdentityFingerprints);
  check("net", guest::kNetFingerprints);
  check("session", guest::kSessionFingerprints);
  check("live", guest::kLiveFingerprints);
  check("engine", guest::kEngineFingerprints);
  check("socket", guest::kSocketFingerprints);
  check("race", guest::kRaceFingerprints);
  std::printf("image: %zu bytes checked\n", image.size());
}

}  // namespace

int main(int argc, char** argv) {
  ParserReadsEveryField();
  ParserRejectsOtherCommands();
  UnknownKeysFallThrough();
  ProblemsAreReported();
  SixProbesAreRegistered();
  DryRunMapsTwoThreeFour();
  BlockedLineIsExact();
  Ue3SummaryNamesWhatIsActive();
  LayoutNames();
  FingerprintMismatchFallsBack();
  if (argc > 1) RealImageMatches(argv[1]);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
