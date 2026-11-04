// Pure tests: race datagrams, seat to kart mapping, the host-authoritative phase machine and the guest fingerprint
// check on a synthetic code buffer.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "online_game_guest.h"
#include "online_race.h"

using namespace eck::race;
namespace guest = eck::online_guest;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);              \
    }                                                                  \
  } while (0)

RaceConfig MakeConfig(std::initializer_list<int> seats, uint32_t id = 7) {
  RaceConfig c;
  c.race_id = id;
  c.track = 5;
  c.laps = 3;
  c.total_karts = 8;
  c.seed = 0xDEADBEEF;
  for (int s : seats) c.participants.push_back({uint8_t(s), uint8_t(10 + s), uint8_t(s)});
  return c;
}

KartState MakeKart() {
  KartState k;
  k.race_id = 7;
  k.seat = 2;
  k.tick = 123456;
  k.position[0] = 1.5f;
  k.position[1] = -2000.25f;
  k.position[2] = 33;
  k.rotation[0] = -65536;
  k.rotation[1] = 12345;
  k.rotation[2] = 0;
  k.velocity[0] = 9.5f;
  k.velocity[1] = 0;
  k.velocity[2] = -1;
  k.steering = -0.5f;
  k.throttle = 1.0f;
  k.lap = 2;
  k.checkpoint = 300;
  k.place = 3;
  k.flags = 5;
  return k;
}

void TestCodec() {
  const RaceConfig config = MakeConfig({0, 1, 3});
  const KartState kart = MakeKart();
  RaceResult result;
  result.race_id = 7;
  result.outcome = Outcome::kPlayerLeft;
  result.entries = {{0, SeatResult::kFinished, 1, 90000}, {1, SeatResult::kLeft, 0, 0}};
  const std::vector<Message> messages = {config,
                                         PhaseMsg{7, Phase::kCountdown, 2},
                                         kart,
                                         Finish{7, 1, 91234, 2},
                                         result,
                                         Leave{7, 3},
                                         Loaded{7, 1}};
  for (const Message& m : messages) {
    const std::vector<uint8_t> bytes = Encode(m);
    CHECK(bytes.size() >= kHeaderSize && bytes.size() <= kMaxDatagram);
    const auto back = Decode(bytes.data(), bytes.size());
    CHECK(back.has_value());
    CHECK(back && *back == m);
    CHECK(back && back->index() == m.index());
    // Every truncation and every extra byte is refused.
    for (size_t cut = 0; cut < bytes.size(); ++cut) CHECK(!Decode(bytes.data(), cut).has_value());
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size()).has_value());
  }

  std::vector<uint8_t> bytes = Encode(kart);
  std::vector<uint8_t> bad = bytes;
  bad[0] = 'X';
  CHECK(!Decode(bad.data(), bad.size()));
  bad = bytes;
  bad[2] = kVersion + 1;
  CHECK(!Decode(bad.data(), bad.size()));
  bad = bytes;
  bad[3] = 99;
  CHECK(!Decode(bad.data(), bad.size()));
  bad = bytes;
  bad[8] = 9;  // seat out of range
  CHECK(!Decode(bad.data(), bad.size()));
  CHECK(!Decode(nullptr, 10));

  // Non-finite and absurd coordinates, and out-of-range phase / outcome / count.
  KartState nan = kart;
  nan.position[0] = NAN;
  std::vector<uint8_t> n = Encode(nan);
  CHECK(!Decode(n.data(), n.size()));
  KartState huge = kart;
  huge.velocity[2] = 1e12f;
  n = Encode(huge);
  CHECK(!Decode(n.data(), n.size()));
  KartState steer = kart;
  steer.steering = 40.0f;
  n = Encode(steer);
  const auto clamped = Decode(n.data(), n.size());
  CHECK(clamped && std::get<KartState>(*clamped).steering == 1.0f);
  n = Encode(PhaseMsg{7, Phase::kRacing, 0});
  n[8] = 9;
  CHECK(!Decode(n.data(), n.size()));
  n = Encode(config);
  n[18] = 5;  // participant count (4 header + 4 id + 2 track + 4 bytes + 4 seed)
  CHECK(!Decode(n.data(), n.size()));
  std::vector<uint8_t> oversized(kMaxDatagram + 1, 0);
  oversized[0] = kMagic0;
  oversized[1] = kMagic1;
  oversized[2] = kVersion;
  CHECK(!Decode(oversized.data(), oversized.size()));
}

void TestMapping() {
  for (int humans = 2; humans <= 4; ++humans) {
    RaceConfig config;
    config.race_id = 1;
    config.total_karts = 8;
    // The host is seat 0; a joiner can sit anywhere, so use a gap too.
    const int seats[4] = {0, 1, 2, 3};
    for (int i = 0; i < humans; ++i) config.participants.push_back({uint8_t(seats[i]), uint8_t(i), uint8_t(i)});
    CHECK(ValidConfig(config));
    for (int local = 0; local < humans; ++local) {
      const auto karts = MapSeatsToKarts(config, local);
      const RaceCounts counts = CountKarts(karts);
      CHECK(counts.total == 8);
      CHECK(counts.humans == humans);
      CHECK(counts.local_humans == 1);
      CHECK(counts.remote_humans == humans - 1);
      CHECK(counts.bots == 8 - humans);
      for (const KartAssignment& k : karts) {
        CHECK(k.bot != k.human);
        CHECK(!(k.remote_human && k.bot));
        CHECK(k.slot == int(&k - karts.data()));
        if (k.local) CHECK(k.controller_id == 0 && k.seat == local);
        if (k.remote_human) CHECK(k.controller_id == -1 && k.seat != local);
      }
      // Slot order is the seat order, the same on every PC.
      for (int s = 0; s < humans; ++s) CHECK(SlotOfSeat(karts, s) == s);
    }
  }

  RaceConfig gap = MakeConfig({3, 0, 2});
  const auto karts = MapSeatsToKarts(gap, 2);
  CHECK(SlotOfSeat(karts, 0) == 0 && SlotOfSeat(karts, 2) == 1 && SlotOfSeat(karts, 3) == 2);
  CHECK(SlotOfSeat(karts, 1) == -1);
  CHECK(MapSeatsToKarts(gap, 1).empty());  // that seat does not take part

  RaceConfig solo = MakeConfig({0});
  CHECK(!ValidConfig(solo));
  RaceConfig dup = MakeConfig({0, 0});
  CHECK(!ValidConfig(dup));
  RaceConfig few = MakeConfig({0, 1, 2});
  few.total_karts = 2;
  CHECK(!ValidConfig(few));
  RaceConfig many = MakeConfig({0, 1});
  many.total_karts = 9;
  CHECK(!ValidConfig(many));
  RaceConfig seat9 = MakeConfig({0, 9});
  CHECK(!ValidConfig(seat9));
  RaceConfig nolap = MakeConfig({0, 1});
  nolap.laps = 0;
  CHECK(!ValidConfig(nolap));
}

// Runs the host through a start to a racing phase, with the clients following its outbox.
struct Room {
  RaceSession host{0, true};
  std::vector<RaceSession> clients;
  explicit Room(int players) {
    for (int s = 1; s < players; ++s) clients.emplace_back(s, false);
  }
  void Deliver(double now) {
    for (const Message& m : host.TakeOutbox()) {
      for (size_t i = 0; i < clients.size(); ++i) {
        if (const auto* c = std::get_if<RaceConfig>(&m)) clients[i].OnConfig(0, *c, now);
        if (const auto* p = std::get_if<PhaseMsg>(&m)) clients[i].OnPhase(0, *p);
        if (const auto* r = std::get_if<RaceResult>(&m)) clients[i].OnResult(0, *r);
      }
    }
  }
  void StartRacing(const RaceConfig& config) {
    CHECK(host.HostBegin(config, 0));
    Deliver(0);
    for (const RaceSession& c : clients) CHECK(c.phase() == Phase::kStarting);
    host.OnLoaded(0, 1);
    for (size_t i = 0; i < clients.size(); ++i) host.OnLoaded(int(i) + 1, 1.5);
    CHECK(host.phase() == Phase::kCountdown);
    Deliver(1.5);
    host.Tick(1.5 + kCountdownSeconds + 0.01);
    CHECK(host.phase() == Phase::kRacing);
    Deliver(5);
    for (const RaceSession& c : clients) CHECK(c.phase() == Phase::kRacing);
  }
};

void TestPhaseNormal() {
  Room room(3);
  const RaceConfig config = MakeConfig({0, 1, 2});
  RaceSession lobby(1, false);
  CHECK(lobby.phase() == Phase::kLobby);
  room.StartRacing(config);

  room.host.OnFinish(1, 1, 80000);
  room.host.OnFinish(0, 2, 82000);
  CHECK(room.host.phase() == Phase::kRacing);
  room.host.OnFinish(2, 3, 85000);
  CHECK(room.host.phase() == Phase::kFinished);
  CHECK(room.host.outcome() == Outcome::kCompleted);
  room.Deliver(100);
  for (const RaceSession& c : room.clients) {
    CHECK(c.phase() == Phase::kFinished);
    CHECK(c.outcome() == Outcome::kCompleted);
    CHECK(c.SeatStatus(1) == SeatResult::kFinished);
  }
  CHECK(room.host.TakeNotices().empty());

  // A new race after the finish.
  RaceConfig again = MakeConfig({0, 1, 2}, 8);
  CHECK(room.host.HostBegin(again, 200));
  room.Deliver(200);
  CHECK(room.clients[0].phase() == Phase::kStarting && room.clients[0].race_id() == 8);
}

void TestPhaseAuthority() {
  RaceSession client(1, false);
  const RaceConfig config = MakeConfig({0, 1});
  CHECK(!client.OnConfig(1, config, 0));  // not from the host
  CHECK(client.OnConfig(0, config, 0));
  CHECK(!client.OnConfig(0, config, 1));  // repeat of the running race
  CHECK(!client.OnPhase(1, PhaseMsg{7, Phase::kRacing, 0}));
  CHECK(!client.OnPhase(0, PhaseMsg{99, Phase::kRacing, 0}));  // another race
  CHECK(client.OnPhase(0, PhaseMsg{7, Phase::kCountdown, 3}));
  CHECK(client.OnPhase(0, PhaseMsg{7, Phase::kRacing, 0}));
  CHECK(!client.OnPhase(0, PhaseMsg{7, Phase::kCountdown, 1}));  // never backwards
  CHECK(client.phase() == Phase::kRacing);
  CHECK(!client.OnPhase(0, PhaseMsg{7, Phase::kStarting, 0}));
  CHECK(!client.OnResult(2, RaceResult{7, Outcome::kCompleted, {}}));
  CHECK(client.OnResult(0, RaceResult{7, Outcome::kCompleted, {{1, SeatResult::kFinished, 1, 5}}}));
  CHECK(client.phase() == Phase::kFinished);

  RaceSession host(0, true);
  CHECK(!host.HostBegin(MakeConfig({1, 2}), 0));  // the host must take part
  CHECK(!host.HostBegin(MakeConfig({0}), 0));
  CHECK(!client.HostBegin(config, 0));  // only the host starts
  // A host-only machine ignores phases and results from the network.
  CHECK(host.HostBegin(config, 0));
  CHECK(!host.OnPhase(0, PhaseMsg{7, Phase::kRacing, 0}));
  CHECK(!host.HostBegin(config, 0));  // already running
}

void TestPlayerLeaves() {
  Room room(3);
  room.StartRacing(MakeConfig({0, 1, 2}));
  room.host.OnSeatGone(2, false);
  CHECK(room.host.phase() == Phase::kRacing);  // two humans remain
  CHECK(room.host.SeatStatus(2) == SeatResult::kLeft);
  auto notices = room.host.TakeNotices();
  CHECK(notices.size() == 1 && notices[0].outcome == Outcome::kPlayerLeft && notices[0].seat == 2);
  room.clients[0].OnSeatGone(2, false);
  notices = room.clients[0].TakeNotices();
  CHECK(notices.size() == 1 && notices[0].outcome == Outcome::kPlayerLeft);
  CHECK(room.clients[0].phase() == Phase::kRacing);  // a client waits for the host

  // The rest finish: the race completes without the missing seat.
  room.host.OnFinish(0, 1, 70000);
  CHECK(room.host.phase() == Phase::kRacing);
  room.host.OnFinish(1, 2, 71000);
  CHECK(room.host.phase() == Phase::kFinished && room.host.outcome() == Outcome::kCompleted);
  room.Deliver(50);
  CHECK(room.clients[0].phase() == Phase::kFinished);
  CHECK(room.clients[0].SeatStatus(2) == SeatResult::kLeft);

  // Two players: the last one alone ends the race with "player left".
  Room duo(2);
  duo.StartRacing(MakeConfig({0, 1}));
  duo.host.OnSeatGone(1, false);
  CHECK(duo.host.phase() == Phase::kFinished && duo.host.outcome() == Outcome::kPlayerLeft);
  duo.Deliver(60);
  CHECK(duo.clients[0].phase() == Phase::kFinished);
  CHECK(duo.clients[0].outcome() == Outcome::kPlayerLeft);

  // Gone is final: a seat that left cannot finish.
  Room trio(3);
  trio.StartRacing(MakeConfig({0, 1, 2}));
  trio.host.OnSeatGone(1, true);
  trio.host.OnFinish(1, 1, 1);
  CHECK(trio.host.SeatStatus(1) == SeatResult::kLost);
  auto lost = trio.host.TakeNotices();
  CHECK(lost.size() == 1 && lost[0].outcome == Outcome::kConnectionLost);
  trio.host.OnSeatGone(1, false);  // already gone: no second notice
  CHECK(trio.host.TakeNotices().empty());
}

void TestHostLeaves() {
  Room room(3);
  room.StartRacing(MakeConfig({0, 1, 2}));
  room.clients[0].OnSeatGone(0, false);
  CHECK(room.clients[0].phase() == Phase::kFinished && room.clients[0].outcome() == Outcome::kHostLeft);
  auto notices = room.clients[0].TakeNotices();
  CHECK(notices.size() == 1 && notices[0].outcome == Outcome::kHostLeft && notices[0].seat == 0);

  room.clients[1].OnSeatGone(0, true);
  CHECK(room.clients[1].outcome() == Outcome::kHostConnectionLost);
  // A late result from the vanished host changes nothing.
  CHECK(!room.clients[1].OnResult(0, RaceResult{7, Outcome::kCompleted, {}}));
  CHECK(room.clients[1].outcome() == Outcome::kHostConnectionLost);

  // In the lobby or after the end, a departure is not news.
  RaceSession idle(1, false);
  idle.OnSeatGone(0, false);
  CHECK(idle.phase() == Phase::kLobby && idle.TakeNotices().empty());
  room.clients[0].OnSeatGone(2, false);
  CHECK(room.clients[0].TakeNotices().empty());
}

void TestLoadAndCountdown() {
  Room room(3);
  CHECK(room.host.HostBegin(MakeConfig({0, 1, 2}), 0));
  room.Deliver(0);
  room.host.OnLoaded(0, 1);
  room.host.OnLoaded(1, 2);
  CHECK(room.host.phase() == Phase::kStarting);  // seat 2 has not loaded
  room.host.Tick(kLoadTimeoutSeconds - 1);
  CHECK(room.host.phase() == Phase::kStarting);
  room.host.Tick(kLoadTimeoutSeconds + 1);
  CHECK(room.host.phase() == Phase::kCountdown);  // the others go on without seat 2
  CHECK(room.host.SeatStatus(2) == SeatResult::kLost);
  auto notices = room.host.TakeNotices();
  CHECK(notices.size() == 1 && notices[0].outcome == Outcome::kConnectionLost && notices[0].seat == 2);

  // The countdown announces each whole second once.
  Room timed(2);
  CHECK(timed.host.HostBegin(MakeConfig({0, 1}), 0));
  timed.host.OnLoaded(0, 0);
  timed.host.OnLoaded(1, 0);
  timed.host.TakeOutbox();
  std::vector<int> seen;
  for (double t = 0.1; t < kCountdownSeconds; t += 0.1) {
    timed.host.Tick(t);
    for (const Message& m : timed.host.TakeOutbox()) seen.push_back(std::get<PhaseMsg>(m).countdown);
  }
  CHECK((seen == std::vector<int>{2, 1}));
  timed.host.Tick(kCountdownSeconds);
  CHECK(timed.host.phase() == Phase::kRacing);

  // Nobody else loads: the race cannot start.
  Room alone(2);
  CHECK(alone.host.HostBegin(MakeConfig({0, 1}), 0));
  alone.host.OnLoaded(0, 1);
  alone.host.Tick(kLoadTimeoutSeconds + 1);
  CHECK(alone.host.phase() == Phase::kFinished && alone.host.outcome() == Outcome::kConnectionLost);
}

void TestFingerprints() {
  constexpr uint32_t kBase = 0x82000000;
  std::vector<uint8_t> image(0x2000, 0);
  const guest::CodeFingerprint table[] = {
      {"first", kBase + 0x100, {0x7D8802A6, 0x48000011, 0x9421FF60, 0x7C7F1B78}},
      {"second", kBase + 0x1FF0, {0x483FFA2C, 0, 0x483FFC44, 0}},
  };
  auto put = [&](uint32_t address, std::initializer_list<uint32_t> words) {
    size_t at = address - kBase;
    for (uint32_t w : words) {
      image[at++] = uint8_t(w >> 24);
      image[at++] = uint8_t(w >> 16);
      image[at++] = uint8_t(w >> 8);
      image[at++] = uint8_t(w);
    }
  };
  put(kBase + 0x100, {0x7D8802A6, 0x48000011, 0x9421FF60, 0x7C7F1B78});
  put(kBase + 0x1FF0, {0x483FFA2C, 0, 0x483FFC44, 0});
  CHECK(guest::FirstMismatch(table, image.data(), image.size(), kBase) == nullptr);
  image[0x103] ^= 1;  // the last byte of the first word
  const char* bad = guest::FirstMismatch(table, image.data(), image.size(), kBase);
  CHECK(bad != nullptr && std::string(bad) == "first");
  image[0x103] ^= 1;
  image[0x1FFF] = 1;  // the last byte of the last word
  bad = guest::FirstMismatch(table, image.data(), image.size(), kBase);
  CHECK(bad != nullptr && std::string(bad) == "second");
  image[0x1FFF] = 0;
  // An image too short for a function, a base above it, and no image at all never match.
  CHECK(guest::FirstMismatch(table, image.data(), 0x1FF8, kBase) != nullptr);
  CHECK(guest::FirstMismatch(table, image.data(), image.size(), kBase + 0x200) != nullptr);
  CHECK(guest::FirstMismatch(table, nullptr, 0, kBase) != nullptr);

  // The real tables are well formed: unique addresses, word-aligned, inside the 32 MB image.
  std::vector<uint32_t> addresses;
  const std::span<const guest::CodeFingerprint> tables[] = {guest::kIdentityFingerprints, guest::kNetFingerprints,
                                                           guest::kSessionFingerprints, guest::kLiveFingerprints,
                                                           guest::kEngineFingerprints};
  for (const auto& t : tables) {
    for (const guest::CodeFingerprint& f : t) {
      CHECK(f.address % 4 == 0 && f.address >= 0x82000000 && f.address < 0x84000000);
      addresses.push_back(f.address);
    }
  }
  std::sort(addresses.begin(), addresses.end());
  CHECK(std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end());
  CHECK(addresses.size() == 26);
}

void TestLiveProfile() {
  CHECK(!guest::ReportLiveProfile(false, true, true));  // developer switch off: the original profile
  CHECK(!guest::ReportLiveProfile(true, false, true));  // Online play off
  CHECK(!guest::ReportLiveProfile(true, true, false));  // not signed in
  CHECK(guest::ReportLiveProfile(true, true, true));
  CHECK(guest::EnvOn("1") && !guest::EnvOn("0") && !guest::EnvOn("") && !guest::EnvOn(nullptr) && !guest::EnvOn("11"));

  const uint64_t a = guest::VirtualXuid("ABCD-1234");
  CHECK(a == guest::VirtualXuid("ABCD-1234"));
  CHECK(a != guest::VirtualXuid("ABCD-1235"));
  CHECK((a >> 48) == 0x0009);
  CHECK((a & 0x0000FFFFFFFFFFFFull) != 0);
  CHECK(guest::GuestName("0123456789ABCDEFGHI").size() == 15);
  CHECK(guest::GuestName("a\tb") == "a?b");
}

void TestTransitionGate() {
  guest::TransitionGate gate(0.25, 3);
  CHECK(gate.ShouldLog("a", 1, 0));
  CHECK(!gate.ShouldLog("a", 1, 10));    // same value: no line
  CHECK(!gate.ShouldLog("a", 2, 0.1));   // changed, but too soon
  CHECK(gate.ShouldLog("a", 2, 0.5));
  CHECK(gate.ShouldLog("b", 9, 0.5));    // keys are independent
  CHECK(!gate.ShouldLog("c", 1, 5));     // the total cap of 3 lines
  CHECK(gate.lines() == 3);
  CHECK(gate.calls("a") == 4);
}

}  // namespace

int main() {
  TestCodec();
  TestMapping();
  TestPhaseNormal();
  TestPhaseAuthority();
  TestPlayerLeaves();
  TestHostLeaves();
  TestLoadAndCountdown();
  TestFingerprints();
  TestLiveProfile();
  TestTransitionGate();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
