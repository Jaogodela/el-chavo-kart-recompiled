// The race between PCs, headless: pads to the host, karts back, and the host's Start, over a scripted network or the real
// gameplay socket. Time is simulated, so 60 and 300 frames per second run side by side.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "online/game_socket.h"
#include "online/relay_frame.h"
#include "online_race_sync.h"

using namespace eck;
using namespace eck::race_sync;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAILED: %s\n", what);
  }
}

constexpr uint32_t kGeneration = 7;

SyncConfig ConfigFor(int seat, int humans = 2, int total = 8) {
  SyncConfig config;
  config.generation = kGeneration;
  config.seat = seat;
  config.humans = humans;
  config.total = total;
  for (int slot = 0; slot < humans; ++slot) config.seats[size_t(slot)] = slot;
  for (int who = 0; who < humans; ++who) {
    int pad = 0;
    for (int other = 0; other < who; ++other) {
      if (other != seat) ++pad;
    }
    config.pad_of_seat[size_t(who)] = who == seat ? 0 : pad + (seat == who ? 0 : 1);
  }
  return config;
}

// A wire with one-way delay and scripted loss, seat to seat.
class Net {
 public:
  struct Packet {
    double at;
    int from;
    int to;
    std::vector<uint8_t> bytes;
  };

  void Post(int from, int to, const std::vector<uint8_t>& bytes) {
    ++sent;
    if (lose_every > 0 && sent % lose_every == 0) return;
    packets.push_back({now + delay, from, to, bytes});
    if (duplicate) packets.push_back({now + delay, from, to, bytes});
  }

  double now = 0;
  double delay = 0.02;
  int lose_every = 0;
  bool duplicate = false;
  int sent = 0;
  std::deque<Packet> packets;
};

class NetLink final : public PeerLink {
 public:
  NetLink(Net& net, int me) : net_(net), me_(me) {}
  void Send(int seat, const std::vector<uint8_t>& datagram) override { net_.Post(me_, seat, datagram); }
  size_t Receive(int& seat, uint8_t* buffer, size_t capacity) override {
    for (auto it = net_.packets.begin(); it != net_.packets.end(); ++it) {
      if (it->to != me_ || it->at > net_.now) continue;
      const size_t size = std::min(capacity, it->bytes.size());
      std::memcpy(buffer, it->bytes.data(), size);
      seat = it->from;
      net_.packets.erase(it);
      return size;
    }
    return 0;
  }

 private:
  Net& net_;
  int me_;
};

KartFrames StillKarts(int total) {
  KartFrames karts;
  for (int slot = 0; slot < total; ++slot) {
    KartSample kart;
    kart.slot = uint8_t(slot);
    kart.position[1] = float(slot) * 200.0f;
    karts[size_t(slot)] = kart;
  }
  return karts;
}

// The host's kart in a slot drives straight along X at `speed`.
struct Pair {
  Net net;
  NetLink host_link{net, 0};
  NetLink guest_link{net, 1};
  RaceSync host{ConfigFor(0)};
  RaceSync guest{ConfigFor(1)};
  int host_state = 0;
  int guest_state = 0;
  double host_dt = 1.0 / 60;
  double guest_dt = 1.0 / 60;
  double host_next = 0;
  double guest_next = 0;
  KartFrames host_karts = StillKarts(8);
  KartFrames guest_karts = StillKarts(8);
  PadState guest_pad;
  float host_kart_speed = 0;
  FrameOut host_out;
  FrameOut guest_out;
  int host_frames = 0;
  int guest_frames = 0;

  void Run(double seconds) {
    const double end = net.now + seconds;
    while (net.now < end) {
      if (net.now >= host_next) {
        host_next += host_dt;
        host_karts[0]->position[0] += host_kart_speed * float(host_dt);
        host_karts[0]->velocity[0] = host_kart_speed;
        FrameIn in;
        in.now = net.now;
        in.dt = host_dt;
        in.race_state = host_state;
        in.karts = host_karts;
        host_out = host.Tick(in, host_link);
        ++host_frames;
      }
      if (net.now >= guest_next) {
        guest_next += guest_dt;
        FrameIn in;
        in.now = net.now;
        in.dt = guest_dt;
        in.race_state = guest_state;
        in.karts = guest_karts;
        in.own_pad = guest_pad;
        guest_out = guest.Tick(in, guest_link);
        ++guest_frames;
        for (int i = 0; i < guest_out.write_count; ++i) {
          const KartWrite& write = guest_out.writes[size_t(i)];
          KartSample& kart = *guest_karts[size_t(write.slot)];
          std::copy(write.state.position, write.state.position + 3, kart.position);
          if (!write.position_only) {
            std::copy(write.state.velocity, write.state.velocity + 3, kart.velocity);
            kart.speed = write.state.speed;
          }
        }
      }
      net.now = std::min(host_next, guest_next);
    }
  }
};

bool Noted(RaceSync& sync, const std::string& note) {
  for (const std::string& seen : sync.TakeNotes()) {
    if (seen == note) return true;
  }
  return false;
}

template <typename T>
bool RoundTrips(const T& message) {
  const std::vector<uint8_t> bytes = race::Encode(message);
  const std::optional<race::Message> decoded = race::Decode(bytes.data(), bytes.size());
  return decoded && std::holds_alternative<T>(*decoded) && std::get<T>(*decoded) == message;
}

void TestInputPacket() {
  InputMsg input;
  input.race_id = kGeneration;
  input.seat = 1;
  input.seq = 9;
  input.snapshot_ack = 4;
  input.loaded = true;
  input.pad = {0xF3FF, 255, 12, -32768, 32767, 5, -5};
  Check(RoundTrips(input), "an input datagram survives the wire");
  Check(race::Encode(input).size() <= 32, "and stays compact");

  std::vector<uint8_t> bad = race::Encode(input);
  bad[2] = race::kVersion + 1;
  Check(!race::Decode(bad.data(), bad.size()), "another protocol version is refused");
  input.pad.buttons = 0x0400;
  bad = race::Encode(input);
  Check(!race::Decode(bad.data(), bad.size()), "a reserved button is malformed");
  input.pad.buttons = 0;
  bad = race::Encode(input);
  bad.push_back(0);
  Check(!race::Decode(bad.data(), bad.size()), "so is a trailing byte");
  bad = race::Encode(input);
  bad.pop_back();
  Check(!race::Decode(bad.data(), bad.size()), "or a short one");
  input.seat = race::kMaxHumans;
  bad = race::Encode(input);
  Check(!race::Decode(bad.data(), bad.size()), "or a seat that does not exist");
}

void TestInputGate() {
  InputMsg input;
  input.race_id = kGeneration;
  input.seat = 1;
  input.seq = 1;
  InputGate gate;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kAccepted, "a guest's own input is taken");
  Check(gate.Check(input, 1, kGeneration) == Verdict::kOld, "a duplicate is ignored");
  input.seq = 5;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kAccepted, "a gap is fine: only the newest pad matters");
  input.seq = 3;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kOld, "a late one is stale");
  input.seq = 6;
  input.race_id = kGeneration - 1;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kWrongRace, "the last race's input is dropped");
  input.race_id = kGeneration;
  input.seat = 2;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kWrongSeat, "seat 1 cannot speak for seat 2");
  input.seat = 0;
  Check(gate.Check(input, 0, kGeneration) == Verdict::kWrongSeat, "nobody sends the host's own seat");
  input.seat = 1;
  input.seq = 5 + kMaxSeqJump + 1;
  Check(gate.Check(input, 1, kGeneration) == Verdict::kAbsurd, "an absurd future sequence is refused");
  Check(gate.last() == 5, "and none of those moved the stream");
}

void TestSnapshotPacket() {
  SnapshotMsg snapshot;
  snapshot.race_id = kGeneration;
  snapshot.seq = 3;
  snapshot.host_ms = 1234;
  snapshot.race_state = 2;
  snapshot.begun = true;
  snapshot.input_ack = {0, 77, 0, 0};
  for (int slot = 0; slot < race::kMaxKarts; ++slot) {
    KartSample kart;
    kart.slot = uint8_t(slot);
    kart.position[0] = 100.5f * float(slot);
    kart.velocity[1] = -300;
    kart.speed = 5860;
    snapshot.karts.push_back(kart);
  }
  Check(RoundTrips(snapshot), "a full race fits one datagram");
  Check(race::Encode(snapshot).size() <= race::kMaxDatagram && race::Encode(snapshot).size() < 300, "well under the relay's frame");

  std::vector<uint8_t> bad = race::Encode(snapshot);
  const size_t position = race::Encode(SnapshotMsg{}).size() + 1;
  const float nan = std::nanf("");
  std::memcpy(bad.data() + position, &nan, sizeof nan);
  Check(!race::Decode(bad.data(), bad.size()), "a position that is not a number is malformed");
  SnapshotMsg twice = snapshot;
  twice.karts[1].slot = 0;
  bad = race::Encode(twice);
  Check(!race::Decode(bad.data(), bad.size()), "so is one slot sent twice");
  SnapshotMsg flat = snapshot;
  std::fill(flat.karts[2].quat, flat.karts[2].quat + 4, 0.0f);
  bad = race::Encode(flat);
  const std::optional<race::Message> unset = race::Decode(bad.data(), bad.size());
  Check(unset.has_value(), "a kart with no heading yet does not cost the other seven their snapshot");
  Check(!HasHeading(std::get<SnapshotMsg>(*unset).karts[2]) && HasHeading(std::get<SnapshotMsg>(*unset).karts[3]), "and is not given one");

  SnapshotGate gate;
  Check(gate.Check(snapshot, 0, kGeneration, 8) == Verdict::kAccepted, "the host's snapshot is taken");
  Check(gate.Check(snapshot, 0, kGeneration, 8) == Verdict::kOld, "a duplicate is ignored");
  snapshot.seq = 2;
  Check(gate.Check(snapshot, 0, kGeneration, 8) == Verdict::kOld, "a reordered one is ignored");
  snapshot.seq = 4;
  Check(gate.Check(snapshot, 1, kGeneration, 8) == Verdict::kWrongSeat, "only seat 0 speaks for the race");
  Check(gate.Check(snapshot, 0, kGeneration + 1, 8) == Verdict::kWrongRace, "and only for this race");
  Check(gate.Check(snapshot, 0, kGeneration, 4) == Verdict::kAbsurd, "and only for karts in it");
}

void TestStartOwnership() {
  StartGate host(true);
  Check(!host.NoteRaceState(-1) && !host.NoteRaceState(0), "the cinematic is not a start");
  Check(host.NoteRaceState(1), "the host's Start is the one begin event");
  Check(!host.NoteRaceState(1) && !host.NoteRaceState(2), "and there is only one");
  Check(!host.NoteBegin(true), "a host takes no begin from anyone");

  StartGate guest(false);
  Check(!guest.NoteRaceState(1) && !guest.begun(), "a guest's own game cannot start the race");
  Check(!guest.NoteBegin(false), "no begin yet");
  Check(guest.NoteBegin(true), "the host's begin arrives");
  Check(!guest.NoteBegin(true), "a repeat is ignored");

  const GatePad waiting = GuestGatePad(false, 0, 1.0);
  Check((waiting.block & kPadStart) && (waiting.block & kPadA) && waiting.press == 0, "before the host, a guest's Start does nothing");
  Check(GuestGatePad(true, 0, 1.0).press == 0 || GuestGatePad(true, 0, 1.1).press == 0, "the press has a release");
  Check((GuestGatePad(true, 0, 1.0).press | GuestGatePad(true, 0, 1.1).press) == (kPadStart | kPadA), "and reaches the gate by itself");
  Check(GuestGatePad(true, 1, 1.0).press == 0 && GuestGatePad(true, 1, 1.1).press == 0, "it stops once the game moved");
  Check((GuestGatePad(true, 2, 1.0).block & kPadA) == 0, "driving needs A");
  Check(GuestGatePad(true, 2, 1.0).block & kPadStart, "a guest cannot pause its side of the race");
  Check((GuestGatePad(true, 4, 1.0).block & (kPadA | kPadB)) == (kPadA | kPadB), "the results are the host's");
  Check(GuestGatePad(true, 4, 1.0).block & kPadStart, "and Start stays the way out");

  Check(HostGatePad(false, false, 1).block & kPadStart, "the host waits for the guests' karts");
  Check(HostGatePad(false, true, 1).block == 0, "then Start is the host's");
  Check(HostGatePad(false, false, kLoadWaitSeconds + 1).block == 0, "a guest that never loads does not hold the host forever");
  Check(HostSidePad({0xFFFF, 9, 9, 9, 9, 9, 9}, false) == PadState{}, "a guest drives nothing before the begin");
  const PadState driving = HostSidePad({uint16_t(kPadA | kPadStart | kPadBack), 1, 2, 3, 4, 5, 6}, true);
  Check(driving.buttons == kPadA && driving.right_trigger == 2 && driving.left_x == 3, "then its pad arrives without Start or Back");
}

void TestBeginEvent() {
  Pair race;
  race.Run(0.5);
  Check(race.guest_out.waiting_for_host, "the guest waits for the host");
  Check(!race.guest.begun(), "nobody has begun");
  Check(Noted(race.guest, "waiting_for_host"), "and says so once");
  race.guest_state = 1;  // the guest's game moves on its own
  race.Run(0.2);
  Check(!race.guest.begun(), "which begins nothing");
  race.guest_state = 0;

  race.host_state = 1;
  race.Run(0.02);
  Check(race.host.begun(), "the host's Start is confirmed on the host");
  const std::vector<std::string> notes = race.host.TakeNotes();
  int begins = 0;
  for (const std::string& note : notes) begins += note == "race_begin host_confirmed";
  Check(begins == 1, "once");
  race.net.lose_every = 2;
  race.Run(0.5);
  Check(race.guest.begun(), "the guest has it, packet loss or not");
  Check(!race.guest_out.waiting_for_host, "and stops waiting");
  bool pressed = false;
  for (int i = 0; i < 40; ++i) {
    race.Run(0.02);
    pressed = pressed || race.guest_out.own.press != 0;
  }
  Check(pressed, "its gate is pressed for it");
  race.guest_state = 1;
  race.Run(0.2);
  Check(race.guest_out.own.press == 0, "until its game has moved");

  // A race the same PCs ran before says nothing about this one.
  SyncConfig next = ConfigFor(1);
  next.generation = kGeneration + 1;
  Net net;
  NetLink link(net, 1);
  RaceSync fresh(next);
  SnapshotMsg stale;
  stale.race_id = kGeneration;
  stale.seq = 1;
  stale.begun = true;
  net.Post(0, 1, race::Encode(stale));
  net.now = 1;
  FrameIn in;
  in.now = 1;
  in.race_state = 0;
  Check(fresh.Tick(in, link).waiting_for_host, "a stale begin is ignored");
  Check(!fresh.begun(), "the new race still waits");
  Check(fresh.traffic().rejected == 1, "and it is counted");
}

void TestInputReachesTheRightKart() {
  Pair race;
  race.host_state = 1;
  race.guest_pad = {kPadA, 0, 255, -9000, 0, 0, 0};
  race.Run(0.5);
  const int pad = race.host.config().pad_of_seat[1];
  Check(pad == 1, "seat 1's kart answers to pad 1 on the host");
  Check(race.host_out.pads[1] && race.host_out.pads[1]->right_trigger == 255 && race.host_out.pads[1]->left_x == -9000,
        "the guest's throttle and steering reach it");
  Check(race.host_out.pads[0] == std::nullopt, "seat 0 stays on the host's own pad");
  Check(race.guest_out.pads[0] == std::nullopt, "seat 1 stays on the guest's own pad");
  Check(race.guest_out.pads[1] && *race.guest_out.pads[1] == PadState{}, "and the host's seat is no pad on the guest");
  Check(race.host.last_input_seq(1) > 0, "the host counts it");

  // Pressed and released between two sends: the press still arrives.
  Pair quick;
  quick.host_state = 1;
  quick.Run(0.3);
  quick.guest_pad.buttons = kPadA;
  quick.Run(1.0 / 300);
  quick.guest_pad.buttons = 0;
  bool seen = false;
  for (int i = 0; i < 12; ++i) {
    quick.Run(0.01);
    seen = seen || (quick.host_out.pads[1] && quick.host_out.pads[1]->buttons == kPadA);
  }
  Check(seen, "a press shorter than the send interval is not lost");

  // The stream stops.
  race.net.lose_every = 1;
  race.Run(kInputHoldSeconds + 0.1);
  Check(race.host_out.pads[1] && *race.host_out.pads[1] == PadState{}, "a silent guest lets go of the controls");
  race.net.lose_every = 0;
  race.Run(0.2);
  Check(race.host_out.pads[1] && race.host_out.pads[1]->right_trigger == 255, "and has them back when it returns");
}

void TestSpoofedSeat() {
  Net net;
  NetLink host_link(net, 0);
  RaceSync host(ConfigFor(0));
  InputMsg input;
  input.race_id = kGeneration;
  input.seat = 1;
  input.seq = 1;
  input.pad.right_trigger = 255;
  net.Post(-1, 0, race::Encode(input));  // nobody we know
  input.seat = 2;
  net.Post(1, 0, race::Encode(input));  // seat 1 pretending
  net.now = 1;
  FrameIn in;
  in.now = 1;
  in.race_state = 2;
  FrameOut out = host.Tick(in, host_link);
  Check(host.traffic().rejected == 2, "neither reaches a kart");
  Check(out.pads[1] && *out.pads[1] == PadState{}, "seat 1's kart did not move");
  Check(host.last_input_seq(1) == 0 && host.last_input_seq(2) == 0, "and no stream advanced");
}

void TestOldRoomTraffic() {
  // A packet of a room this PC has left: its race is another, whoever it seems to come from.
  Net net;
  NetLink host_link(net, 0);
  RaceSync host(ConfigFor(0));
  InputMsg input;
  input.race_id = kGeneration - 1;
  input.seat = 1;
  input.seq = 1;
  input.pad.right_trigger = 255;
  net.Post(1, 0, race::Encode(input));
  net.now = 1;
  FrameIn in;
  in.now = 1;
  in.race_state = 2;
  const FrameOut out = host.Tick(in, host_link);
  Check(host.traffic().rejected == 1 && host.last_input_seq(1) == 0, "an old room's gameplay packet is refused");
  Check(out.pads[1] && *out.pads[1] == PadState{}, "and reaches no kart");
}

void TestLeftSeatIsNeutral() {
  Net net;
  NetLink host_link(net, 0);
  RaceSync host(ConfigFor(0));
  FrameIn in;
  in.now = 1;
  in.race_state = 2;
  host.Tick(in, host_link);
  InputMsg input;
  input.race_id = kGeneration;
  input.seat = 1;
  input.seq = 1;
  input.pad.right_trigger = 255;
  net.Post(1, 0, race::Encode(input));
  net.now = in.now = 1.1;
  Check(host.Tick(in, host_link).pads[1]->right_trigger == 255, "a guest's kart drives");
  net.now = in.now = 1.1 + kInputHoldSeconds + 0.1;
  Check(*host.Tick(in, host_link).pads[1] == PadState{}, "and with nothing more from it the kart lets go");
}

void TestPostRace() {
  Net net;
  NetLink host_link(net, 0);
  NetLink guest_link(net, 1);
  RaceSync host(ConfigFor(0));
  RaceSync guest(ConfigFor(1));
  FrameIn in;
  in.now = net.now = 1;
  in.race_state = 2;
  host.Tick(in, host_link);

  InputMsg input;
  input.race_id = kGeneration;
  input.seat = 1;
  input.seq = 1;
  input.pad.buttons = kPadA;
  net.Post(1, 0, race::Encode(input));
  in.now = net.now = 1.2;
  in.race_state = 4;
  const FrameOut host_out = host.Tick(in, host_link);
  Check(host_out.post_race, "a host past the finish is post-race");
  Check(*host_out.pads[1] == PadState{}, "and a guest's A reaches none of its screens");
  Check(!host_out.waiting_for_host, "nobody waits on the host's own PC");

  // The guest's own game has not finished: the host's word is enough.
  in.now = net.now = 2;
  host.Tick(in, host_link);
  FrameIn guest_in;
  guest_in.now = 2;
  guest_in.race_state = 2;
  const FrameOut guest_out = guest.Tick(guest_in, guest_link);
  Check(guest_out.post_race && guest_out.waiting_for_host, "the guest waits for the host on the results");
  Check(guest_out.own.block & kPadA, "with its A closed");
  guest_in.race_state = 4;
  guest_in.now = 2.1;
  Check(guest.Tick(guest_in, guest_link).post_race, "and on its own finish too");
}

void TestOldSnapshot() {
  Net net;
  NetLink guest_link(net, 1);
  RaceSync guest(ConfigFor(1));
  SnapshotMsg old;
  old.race_id = kGeneration - 1;
  old.seq = 9;
  old.race_state = 2;
  old.begun = true;
  net.Post(0, 1, race::Encode(old));
  net.now = 1;
  FrameIn in;
  in.now = 1;
  in.race_state = 2;
  guest.Tick(in, guest_link);
  Check(guest.traffic().rejected == 1 && guest.last_snapshot_seq() == 0, "a snapshot of an old race is refused");
  Check(!guest.begun(), "and starts nothing");
}

void TestInterpolation() {
  KartTrack track;
  KartSample a, b;
  a.position[0] = 0;
  a.velocity[0] = 1000;
  b.position[0] = 100;
  b.velocity[0] = 1000;
  track.Push(1.0, a);
  track.Push(1.1, b);
  Check(std::fabs(track.Sample(1.05)->position[0] - 50) < 0.01f, "between two snapshots the kart is between them");
  Check(track.Sample(0.5)->position[0] == 0, "before the first it waits at the first");
  Check(std::fabs(track.Sample(1.2)->position[0] - 200) < 0.01f, "past the last it carries on");
  Check(std::fabs(track.Sample(5.0)->position[0] - 250) < 0.01f, "for a moment only");
  track.Push(1.05, a);
  Check(std::fabs(track.Sample(1.05)->position[0] - 50) < 0.01f, "a late snapshot changes nothing");

  KartSample left, right;
  left.quat[2] = std::sin(0.5f);
  left.quat[3] = std::cos(0.5f);
  right.quat[2] = -std::sin(0.5f);
  right.quat[3] = -std::cos(0.5f);  // the same heading, opposite sign
  const KartSample middle = Blend(left, right, 0.5f);
  Check(std::fabs(middle.quat[2] - left.quat[2]) < 0.001f, "a heading blends the short way round");
}

float RemoteError(Pair& race) {
  // The guest shows the host's kart a little behind, by design: its interpolation delay plus the wire.
  const float behind = race.host_kart_speed * float(kInterpolationDelay + race.net.delay);
  return std::fabs(race.host_karts[0]->position[0] - race.guest_karts[0]->position[0] - behind);
}

float WorstStep(Pair& race, double seconds, double sample) {
  float worst = 0;
  float last = race.guest_karts[0]->position[0];
  for (double t = 0; t < seconds; t += sample) {
    race.Run(sample);
    worst = std::max(worst, std::fabs(race.guest_karts[0]->position[0] - last));
    last = race.guest_karts[0]->position[0];
  }
  return worst;
}

void TestFrameRates() {
  // 60 sending, Unlimited receiving.
  Pair fast_receiver;
  fast_receiver.guest_dt = 1.0 / 300;
  fast_receiver.host_state = fast_receiver.guest_state = 2;
  fast_receiver.host_kart_speed = 3000;
  fast_receiver.Run(1.0);
  Check(RemoteError(fast_receiver) < 120, "60 FPS host, Unlimited guest: the remote kart keeps up");
  Check(WorstStep(fast_receiver, 1.0, 1.0 / 300) < 3000.0f / 300 * 4, "and glides instead of stepping per snapshot");
  const uint64_t fast_sent = fast_receiver.guest.traffic().tx_packets;

  // Unlimited sending, 60 receiving.
  Pair fast_sender;
  fast_sender.host_dt = 1.0 / 300;
  fast_sender.host_state = fast_sender.guest_state = 2;
  fast_sender.host_kart_speed = 3000;
  fast_sender.Run(1.0);
  Check(RemoteError(fast_sender) < 120, "Unlimited host, 60 FPS guest: the same");
  Check(WorstStep(fast_sender, 1.0, 1.0 / 60) < 3000.0f / 60 * 2, "and just as smooth");

  // What goes on the wire does not depend on either.
  const uint64_t snapshots_at_300 = fast_sender.host.traffic().tx_packets;
  const uint64_t snapshots_at_60 = fast_receiver.host.traffic().tx_packets;
  Check(std::llabs(int64_t(snapshots_at_300) - int64_t(snapshots_at_60)) <= 2, "the snapshot rate ignores the host's frame rate");
  Check(snapshots_at_300 >= 36 && snapshots_at_300 <= 42, "about 20 per second");
  Pair slow_guest;
  slow_guest.host_state = slow_guest.guest_state = 2;
  slow_guest.Run(2.0);
  Check(std::llabs(int64_t(fast_sent) - int64_t(slow_guest.guest.traffic().tx_packets)) <= 3, "the input rate ignores the guest's");
  Check(fast_sent >= 54 && fast_sent <= 64, "about 30 per second");

  // A slow PC cannot catch up in a burst.
  SendClock clock(kSnapshotHz);
  int sends = 0;
  for (double now = 0; now < 2; now += 0.25) sends += clock.Due(now);
  Check(sends == 8, "one send per slow frame, and no burst after");
}

void TestPacketLoss() {
  Pair race;
  race.host_state = race.guest_state = 2;
  race.host_kart_speed = 3000;
  race.net.lose_every = 4;
  race.Run(1.0);
  const float drop = WorstStep(race, 1.0, 1.0 / 60);
  Check(drop < 3000.0f / 60 * 3, "one packet in four lost: the remote kart still moves smoothly");
  Check(RemoteError(race) < 200, "and stays with the host");

  Pair doubled;
  doubled.host_state = doubled.guest_state = 2;
  doubled.net.duplicate = true;
  doubled.Run(1.0);
  Check(doubled.host.traffic().dropped_old > 0, "a duplicated input is dropped as old");
  Check(doubled.guest.traffic().dropped_old > 0, "and so is a duplicated snapshot");

  Pair blackout;
  blackout.host_state = blackout.guest_state = 2;
  blackout.host_kart_speed = 3000;
  blackout.Run(1.0);
  blackout.net.lose_every = 1;
  blackout.Run(1.0);
  const float frozen = blackout.guest_karts[0]->position[0];
  blackout.Run(0.5);
  Check(blackout.guest_karts[0]->position[0] == frozen, "with nothing arriving the kart stops, not flies off");
  blackout.net.lose_every = 0;
  blackout.Run(1.0);
  Check(RemoteError(blackout) < 120, "and joins the host again");

  Pair reordered;
  reordered.host_state = reordered.guest_state = 2;
  reordered.host_kart_speed = 3000;
  reordered.Run(0.5);
  reordered.net.delay = 0.08;
  reordered.Run(0.1);
  reordered.net.delay = 0.0;  // newer packets overtake the delayed ones
  float worst = 0;
  float last = reordered.guest_karts[0]->position[0];
  for (int i = 0; i < 40; ++i) {
    reordered.Run(1.0 / 60);
    worst = std::max(worst, last - reordered.guest_karts[0]->position[0]);
    last = reordered.guest_karts[0]->position[0];
  }
  Check(worst < 1, "out of order packets never pull a kart backwards");
}

void TestOwnKart() {
  Reconciler small;
  float mine[3] = {1000, 0, 0};
  small.NoteOwn(5, mine);
  KartSample host;
  host.position[0] = 1020;
  float now[3] = {1500, 0, 0};
  Check(!small.OnHost(5, host, now), "a small difference is not a snap");
  Check(small.Step(1.0 / 60).kind == Correction::kNone, "inside the dead zone the guest's kart is left alone");

  Reconciler lagging;
  lagging.NoteOwn(5, mine);
  host.position[0] = 1000;
  Check(!lagging.OnHost(5, host, now), "the host agreeing with where this kart was is no error, however far it has driven since");
  Check(lagging.Step(1.0 / 60).kind == Correction::kNone, "so nothing is corrected");

  Reconciler blending;
  blending.NoteOwn(5, mine);
  host.position[0] = 1300;
  Check(!blending.OnHost(5, host, now), "a medium difference blends");
  float total = 0;
  double dt = 1.0 / 300;
  for (int i = 0; i < 900; ++i) {
    const OwnCorrection correction = blending.Step(dt);
    if (correction.kind == Correction::kNudge) total += correction.offset[0];
  }
  Check(std::fabs(total - 300) < 1, "and over time closes exactly the difference");
  blending.NoteOwn(6, mine);
  Reconciler blending60;
  blending60.NoteOwn(5, mine);
  blending60.OnHost(5, host, now);
  float total60 = 0;
  dt = 1.0 / 60;
  for (int i = 0; i < 60; ++i) total60 += blending60.Step(dt).offset[0];
  float total300 = 0;
  Reconciler blending300;
  blending300.NoteOwn(5, mine);
  blending300.OnHost(5, host, now);
  dt = 1.0 / 300;
  for (int i = 0; i < 300; ++i) total300 += blending300.Step(dt).offset[0];
  Check(std::fabs(total60 - total300) < 1, "at the same speed at any frame rate");

  Reconciler distant;
  distant.NoteOwn(5, mine);
  host.position[0] = 1000 + kSnapDistance + 1;
  Check(distant.OnHost(5, host, now), "a large divergence is replaced");

  // Both karts drive the same line at the same speed, the host's one wire delay later: nothing is wrong with that.
  Pair same;
  same.host_state = same.guest_state = 2;
  same.Run(0.5);
  const double speed = 5800;
  int corrected = 0;
  float guest_x = same.guest_karts[1]->position[0];
  float host_x = same.host_karts[1]->position[0];
  for (int i = 0; i < 240; ++i) {
    const double dt = 1.0 / 240;
    guest_x += float(speed * dt);
    host_x = guest_x - float(speed * same.net.delay);
    same.guest_karts[1]->position[0] = guest_x;
    same.guest_karts[1]->velocity[0] = float(speed);
    same.host_karts[1]->position[0] = host_x;
    same.host_karts[1]->velocity[0] = float(speed);
    same.Run(dt);
    for (int w = 0; w < same.guest_out.write_count; ++w) corrected += same.guest_out.writes[size_t(w)].slot == 1;
    guest_x = same.guest_karts[1]->position[0];
  }
  Check(corrected == 0, "a guest in step with the host at full speed is not pulled about");

  Reconciler unknown;
  host.position[0] = 1500;
  Check(!unknown.OnHost(99, host, now) && unknown.Step(1.0 / 60).kind == Correction::kNone, "a sequence it no longer has compares with now");

  // On the grid each PC's game may place the humans differently: the guest ends up where the host has it.
  Pair grid;
  grid.host_karts[1]->position[1] = -14000;
  grid.guest_karts[1]->position[1] = -12700;
  grid.Run(0.5);
  Check(std::fabs(grid.guest_karts[1]->position[1] + 14000) < 1, "the guest's own kart takes the host's grid slot");
  Check(std::fabs(grid.guest_karts[0]->position[1] - grid.host_karts[0]->position[1]) < 1, "and sees the host's where the host has it");
}

void TestCanonicalSeats() {
  Pair race;
  race.host_karts[5]->position[0] = 4321;
  race.Run(0.5);
  Check(std::fabs(race.guest_karts[5]->position[0] - 4321) < 0.5f, "a bot follows the host");
  Check(race.host_out.write_count == 0, "the host writes no kart: it is the simulation");
  bool own_written = false;
  race.Run(0.2);
  for (int i = 0; i < race.guest_out.write_count; ++i) own_written = own_written || race.guest_out.writes[size_t(i)].slot == 1;
  Check(!own_written, "a guest in agreement with the host leaves its own kart to its own controls");

  RaceSync four(ConfigFor(3, 4));
  Check(four.config().seats[3] == 3 && four.config().total == 8, "seat 3 is slot 3 on every PC");
}

void TestFourPlayers() {
  Net net;
  NetLink host_link(net, 0);
  std::vector<std::unique_ptr<NetLink>> links;
  std::vector<std::unique_ptr<RaceSync>> guests;
  for (int seat = 1; seat < 4; ++seat) {
    links.push_back(std::make_unique<NetLink>(net, seat));
    guests.push_back(std::make_unique<RaceSync>(ConfigFor(seat, 4)));
  }
  RaceSync host(ConfigFor(0, 4));
  KartFrames karts = StillKarts(8);
  FrameOut host_out;
  for (int frame = 0; frame < 120; ++frame) {
    net.now = frame / 60.0;
    FrameIn in;
    in.now = net.now;
    in.dt = 1.0 / 60;
    in.race_state = frame > 20 ? 1 : 0;
    in.karts = karts;
    host_out = host.Tick(in, host_link);
    for (int i = 0; i < 3; ++i) {
      FrameIn side = in;
      side.own_pad.right_trigger = uint8_t(100 + i);
      guests[size_t(i)]->Tick(side, *links[size_t(i)]);
    }
  }
  Check(host_out.pads[1] && host_out.pads[1]->right_trigger == 100, "seat 1 drives its own kart");
  Check(host_out.pads[2] && host_out.pads[2]->right_trigger == 101, "seat 2 its own");
  Check(host_out.pads[3] && host_out.pads[3]->right_trigger == 102, "seat 3 its own");
  bool all = true;
  for (const auto& guest : guests) all = all && guest->begun() && guest->last_snapshot_seq() > 0;
  Check(all, "the star reaches every guest");
  for (const auto& guest : guests) Check(guest->traffic().tx_bytes < 3000, "and each guest only ever sends to the host");
}

void TestRtt() {
  Pair race;
  race.net.delay = 0.04;
  race.Run(1.5);
  const std::optional<int> guest = race.guest.RttMs(0, race.net.now);
  const std::optional<int> host = race.host.RttMs(1, race.net.now);
  Check(guest && *guest >= 80 && *guest <= 130, "the guest measures a real round trip");
  Check(host && *host >= 80 && *host <= 130, "so does the host");
  race.net.lose_every = 1;
  race.Run(kRttWindowSeconds + 0.5);
  Check(!race.guest.RttMs(0, race.net.now), "a silent peer has no ping, rather than an old one");
}

// A process whose GPU work is starved runs one frame every 400 ms. The session has to live through it and recover, and
// the wire shows why it may not happen: the guest hears the authoritative host only that often.
void TestStarvedHostFrames() {
  Pair race;
  race.guest_dt = 1.0 / 300;
  race.host_state = race.guest_state = kRaceInProgress;
  race.host_kart_speed = 3000;
  race.Run(2.0);
  Check(RemoteError(race) < 120, "healthy before");
  const double healthy_heard = race.guest.LastHeardAge(0, race.net.now);
  Check(healthy_heard >= 0 && healthy_heard < 0.1, "snapshots arrive about every 50 ms");
  const uint32_t generation = race.guest.config().generation;

  race.host_next = race.net.now;
  race.host_dt = 0.4;
  const uint64_t before = race.host.traffic().tx_packets;
  const int frames_before = race.host_frames;
  double worst_silence = 0;
  const double starved_until = race.net.now + 4.0;
  while (race.net.now < starved_until) {
    race.Run(0.01);
    worst_silence = std::max(worst_silence, race.guest.LastHeardAge(0, race.net.now));
  }
  const int frames = race.host_frames - frames_before;
  Check(frames >= 10 && frames <= 11, "the signature: about 2.5 host frames per second");
  Check(race.host.traffic().tx_packets - before <= uint64_t(frames + 1), "one snapshot per frame at most, no burst");
  Check(worst_silence > 0.35, "and the guest hears nothing for the whole gap");
  Check(race.guest.begun() && race.guest.config().generation == generation, "the race itself is not reset");
  Check(race.guest.traffic().rejected == 0, "nothing is refused");

  race.host_next = race.net.now;
  race.host_dt = 1.0 / 60;
  race.Run(1.0);
  Check(RemoteError(race) < 120, "normal frames again: the remote kart catches up at once");
  Check(race.guest.LastHeardAge(0, race.net.now) < 0.1, "and snapshots are back at their rate");
}

// Both PCs at the local pacing, or a 60 FPS host with an Unlimited guest: the guest's own kart is left to it.
void TestNoSnapsAtHealthyRates() {
  for (const double guest_dt : {1.0 / 60, 1.0 / 300}) {
    Pair race;
    race.guest_dt = guest_dt;
    race.host_state = race.guest_state = kRaceInProgress;
    race.Run(0.5);
    const double speed = 5800;
    float guest_x = race.guest_karts[1]->position[0];
    for (int i = 0; i < 1200; ++i) {
      const double dt = 1.0 / 240;
      guest_x += float(speed * dt);
      race.guest_karts[1]->position[0] = guest_x;
      race.guest_karts[1]->velocity[0] = float(speed);
      race.host_karts[1]->position[0] = guest_x - float(speed * race.net.delay);
      race.host_karts[1]->velocity[0] = float(speed);
      race.Run(dt);
      guest_x = race.guest_karts[1]->position[0];
    }
    Check(race.guest.traffic().own_snaps == 0, "a host in step never snaps the guest back, at either guest rate");
    Check(race.guest.LastHeardAge(0, race.net.now) < 0.1, "and its snapshots stay fresh");

    race.host_karts[1]->position[0] -= kSnapDistance * 3;
    race.Run(0.2);
    Check(race.guest.traffic().own_snaps >= 1, "a real divergence still snaps: the threshold hides nothing");
  }
  Check(kSnapDistance == 900.0f, "the threshold did not move to hide a late host");
}

void WaitFor(const std::function<bool()>& done) {
  for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void ExerciseSockets(PeerLink& host_link, PeerLink& guest_link, const char* label, double frame_seconds = 0.06) {
  RaceSync host(ConfigFor(0));
  RaceSync guest(ConfigFor(1));
  KartFrames karts = StillKarts(8);
  karts[0]->position[0] = 777;
  FrameOut host_out;
  double now = 1;
  WaitFor([&] {
    now += frame_seconds;
    FrameIn in;
    in.now = now;
    in.dt = frame_seconds;
    in.race_state = 1;
    in.karts = karts;
    host_out = host.Tick(in, host_link);
    in.own_pad.right_trigger = 200;
    guest.Tick(in, guest_link);
    return guest.begun() && host.last_input_seq(1) > 1;
  });
  std::printf("  %s: host rx %llu tx %llu, guest rx %llu tx %llu\n", label, (unsigned long long)host.traffic().rx_packets,
              (unsigned long long)host.traffic().tx_packets, (unsigned long long)guest.traffic().rx_packets,
              (unsigned long long)guest.traffic().tx_packets);
  Check(host.last_input_seq(1) > 1, "input crosses the real socket");
  Check(guest.begun() && guest.last_snapshot_seq() > 0, "and so do snapshots");
  Check(host_out.pads[1] && host_out.pads[1]->right_trigger == 200, "and the pad comes out of the host's side");
  Check(host.traffic().rejected == 0 && guest.traffic().rejected == 0, "with nothing refused");
}

void TestDirectTransport() {
  online::OnlineGameSocket host_socket, guest_socket;
  Check(host_socket.Open({0}, true) && guest_socket.Open({0}, true), "two gameplay sockets open");
  uint32_t loopback = 0;
  online::ParseIPv4("127.0.0.1", loopback);
  PeerRoutes host_routes, guest_routes;
  host_routes[0] = {1, {loopback, guest_socket.bound_port()}};
  guest_routes[0] = {0, {loopback, host_socket.bound_port()}};
  SocketLink host_link(host_socket, host_routes), guest_link(guest_socket, guest_routes);
  ExerciseSockets(host_link, guest_link, "direct");
  ExerciseSockets(host_link, guest_link, "direct, one frame every 400 ms", 0.4);
  Check(host_socket.traffic().relay_tx_packets == 0, "direct stays off the relay");
  Check(host_socket.traffic().udp_rx_packets > 0, "and on UDP");

  // A stranger on the socket is nobody's seat.
  online::OnlineGameSocket stranger;
  Check(stranger.Open({0}, true), "a third socket");
  const uint8_t noise[] = {'E', 'R', race::kVersion, 8, 1, 2, 3};
  stranger.Send({loopback, host_socket.bound_port()}, noise, sizeof noise);
  uint8_t buffer[64];
  int seat = 7;
  size_t size = 0;
  WaitFor([&] {
    size = host_link.Receive(seat, buffer, sizeof buffer);
    return size > 0 && seat != 1;
  });
  Check(size > 0 && seat == -1, "a datagram from outside the session has no seat");
}

void TestRelayTransport() {
  online::OnlineGameSocket host_socket, guest_socket;
  Check(host_socket.Open({0}, true) && guest_socket.Open({0}, true), "two gameplay sockets open");
  constexpr uint32_t kToken = 0x1234;
  // The service: routes by the destination and stamps the sender, as the Worker does.
  const auto route = [&](online::OnlineGameSocket& to, uint8_t from) {
    return [&to, from](std::vector<uint8_t>&& frame) {
      const std::optional<online::RelayFrame> parsed = online::DecodeRelayFrame(frame.data(), frame.size());
      if (!parsed) return false;
      frame[13] = from;
      return to.DeliverRelay(frame.data(), frame.size());
    };
  };
  host_socket.OpenRelay(kToken, route(guest_socket, 0));
  guest_socket.OpenRelay(kToken, route(host_socket, 1));
  host_socket.SelectRelay(1, true);
  guest_socket.SelectRelay(0, true);
  PeerRoutes host_routes, guest_routes;
  host_routes[0] = {1, online::RelayPeerEndpoint(1)};
  guest_routes[0] = {0, online::RelayPeerEndpoint(0)};
  SocketLink host_link(host_socket, host_routes), guest_link(guest_socket, guest_routes);
  ExerciseSockets(host_link, guest_link, "relay");
  ExerciseSockets(host_link, guest_link, "relay, one frame every 400 ms", 0.4);
  Check(host_socket.traffic().relay_tx_packets > 0 && guest_socket.traffic().relay_tx_packets > 0, "both directions went through the relay");
  Check(host_socket.traffic().udp_tx_packets == 0, "and none of it as UDP");

  // Inside the Worker's budget for one connection, at four players: three streams out of the host.
  const double packets_per_second = kSnapshotHz * 3;
  const double bytes_per_second = packets_per_second * (race::kMaxDatagram + 16);
  Check(packets_per_second < 400 && bytes_per_second < 128 * 1024, "a full race stays well inside the relay's rate limits");
}

std::string SourceOf(const char* name) {
  std::ifstream file(std::string(ECK_REPO_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

void TestNoMovementOnTheService() {
  const std::string guest = SourceOf("src/online_race_sync_guest.cpp");
  const std::string hub = SourceOf("server/eck-lobby/src/hub.js");
  Check(!guest.empty() && !hub.empty(), "the sources are here");
  Check(guest.find("Online()") == std::string::npos, "the game thread never waits on the service");
  Check(hub.find("velocity") == std::string::npos && hub.find("race_state") == std::string::npos, "the Worker has no gameplay message");
}

}  // namespace

int main() {
  TestInputPacket();
  TestInputGate();
  TestSnapshotPacket();
  TestStartOwnership();
  TestBeginEvent();
  TestInputReachesTheRightKart();
  TestSpoofedSeat();
  TestOldRoomTraffic();
  TestLeftSeatIsNeutral();
  TestOldSnapshot();
  TestPostRace();
  TestInterpolation();
  TestFrameRates();
  TestStarvedHostFrames();
  TestNoSnapsAtHealthyRates();
  TestPacketLoss();
  TestOwnKart();
  TestCanonicalSeats();
  TestFourPlayers();
  TestRtt();
  TestDirectTransport();
  TestRelayTransport();
  TestNoMovementOnTheService();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
