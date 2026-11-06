#pragma once

// The race between PCs. Seat 0 simulates it: guests send their pad, the host feeds it to that seat's kart through the same
// controller path a local player uses, and sends every kart back. Kart physics take a variable DeltaTime and read rand(), so
// the PCs cannot run in lockstep. Everything here is timed in seconds, never in frames. Pure: no guest memory, no sockets.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "online/game_socket.h"
#include "online/online_service.h"
#include "online_race.h"

namespace eck::race_sync {

using race::InputMsg;
using race::KartSample;
using race::PadState;
using race::SnapshotMsg;

inline constexpr double kInputHz = 30.0;
inline constexpr double kSnapshotHz = 20.0;
inline constexpr double kInterpolationDelay = 0.1;  // two snapshots behind, so one lost packet never shows
inline constexpr double kMaxExtrapolation = 0.15;
inline constexpr double kInputHoldSeconds = 0.25;
inline constexpr uint32_t kMaxSeqJump = 3000;
inline constexpr double kLoadWaitSeconds = 20.0;
inline constexpr double kStartPulseSeconds = 0.1;
inline constexpr float kSnapDistance = 900.0f;  // past this a blend would be seen as the kart sliding sideways
inline constexpr float kDeadZone = 64.0f;
inline constexpr float kTimingSlack = 0.03f;  // a frame on each PC: at speed, that much disagreement is only the clocks
inline constexpr float kBlendPerSecond = 5.0f;
inline constexpr double kRttWindowSeconds = 2.0;
inline constexpr int kRaceCinematic = 0;
inline constexpr int kRaceInProgress = 2;
inline constexpr int kRacePostRace = 4;
inline constexpr float kMinHeadingLength = 0.25f;

inline constexpr uint16_t kPadStart = 0x0010;
inline constexpr uint16_t kPadBack = 0x0020;
inline constexpr uint16_t kPadA = 0x1000;
inline constexpr uint16_t kPadB = 0x2000;

// A fixed rate on any frame rate: at most one send per frame, and a slow frame does not buy a burst afterwards.
class SendClock {
 public:
  explicit SendClock(double hz) : period_(1.0 / hz) {}
  bool Due(double now) {
    if (now < next_) return false;
    next_ = std::max(next_ + period_, now + period_ * 0.5);
    return true;
  }

 private:
  double period_;
  double next_ = 0;
};

// As SSF2T's session: a round trip from the sequence numbers the peer acknowledges, the lowest of the last two seconds.
class RoundTrip {
 public:
  void NoteSent(uint32_t seq, double now) { sent_[seq % sent_.size()] = {seq, now}; }
  void NoteAck(uint32_t seq, double now) {
    if (seq == 0 || seq == acked_) return;
    acked_ = seq;
    const Sent& sent = sent_[seq % sent_.size()];
    if (sent.seq != seq || now < sent.at) return;
    samples_[next_++ % samples_.size()] = {now, now - sent.at};
  }
  std::optional<int> Ms(double now) const {
    std::optional<double> lowest;
    for (const Sample& sample : samples_) {
      if (sample.at <= 0 || now - sample.at > kRttWindowSeconds) continue;
      if (!lowest || sample.seconds < *lowest) lowest = sample.seconds;
    }
    if (!lowest) return std::nullopt;
    return int(std::lround(*lowest * 1000.0));
  }

 private:
  struct Sent {
    uint32_t seq = 0;
    double at = 0;
  };
  struct Sample {
    double at = 0;
    double seconds = 0;
  };
  std::array<Sent, 128> sent_{};
  std::array<Sample, 64> samples_{};
  size_t next_ = 0;
  uint32_t acked_ = 0;
};

enum class Verdict { kAccepted, kWrongRace, kWrongSeat, kOld, kAbsurd };

// A guest speaks for its own seat only, and its datagrams can repeat, reorder or arrive from a race that is over.
class InputGate {
 public:
  Verdict Check(const InputMsg& input, int from_seat, uint32_t generation) {
    if (input.race_id != generation) return Verdict::kWrongRace;
    if (from_seat <= 0 || input.seat != from_seat) return Verdict::kWrongSeat;
    if (input.seq <= last_) return Verdict::kOld;
    if (input.seq - last_ > kMaxSeqJump) return Verdict::kAbsurd;
    last_ = input.seq;
    return Verdict::kAccepted;
  }
  uint32_t last() const { return last_; }

 private:
  uint32_t last_ = 0;
};

class SnapshotGate {
 public:
  Verdict Check(const SnapshotMsg& snapshot, int from_seat, uint32_t generation, int total_karts) {
    if (snapshot.race_id != generation) return Verdict::kWrongRace;
    if (from_seat != 0) return Verdict::kWrongSeat;
    if (snapshot.seq <= last_) return Verdict::kOld;
    if (snapshot.seq - last_ > kMaxSeqJump * 2) return Verdict::kAbsurd;
    for (const KartSample& kart : snapshot.karts) {
      if (kart.slot >= total_karts) return Verdict::kAbsurd;
    }
    last_ = snapshot.seq;
    return Verdict::kAccepted;
  }
  uint32_t last() const { return last_; }

 private:
  uint32_t last_ = 0;
};

// The host's Start begins the race once, on every PC. `Note` reports the moment; a repeat, or a guest's own game, is not one.
class StartGate {
 public:
  explicit StartGate(bool host) : host_(host) {}

  bool NoteRaceState(int state) {
    if (!host_ || begun_ || state <= kRaceCinematic) return false;
    begun_ = true;
    return true;
  }
  bool NoteBegin(bool begun) {
    if (host_ || begun_ || !begun) return false;
    begun_ = true;
    return true;
  }
  bool begun() const { return begun_; }

 private:
  bool host_;
  bool begun_ = false;
};

struct GatePad {
  uint16_t block = 0;
  uint16_t press = 0;
};

// A guest holds no key to the start: its own Start and A do nothing until the host begins, then the same press reaches its
// game through its own pad. A pause would stop only this PC, so Start stays closed for the whole race. So do the result
// screens' buttons: what comes next is the host's.
inline GatePad GuestGatePad(bool begun, int race_state, double now) {
  GatePad pad;
  pad.block = kPadStart | kPadBack;
  if (race_state > kRaceInProgress) pad.block |= kPadA | kPadB;
  if (race_state > kRaceCinematic) return pad;
  pad.block |= kPadA;
  if (begun && race_state == kRaceCinematic && int(now / kStartPulseSeconds) % 2 == 0) pad.press = kPadStart | kPadA;
  return pad;
}

// The host's Start waits for every guest's karts, so nobody loads into a race already running.
inline GatePad HostGatePad(bool begun, bool guests_loaded, double waited) {
  GatePad pad;
  if (!begun && !guests_loaded && waited < kLoadWaitSeconds) pad.block = kPadStart | kPadA;
  return pad;
}

// Driving only: a guest cannot pause the host, and nothing it holds counts before the host has begun.
inline PadState HostSidePad(PadState pad, bool begun) {
  if (!begun) return {};
  pad.buttons = uint16_t(pad.buttons & ~(kPadStart | kPadBack));
  return pad;
}

// The host's clock as this PC sees it: the fastest datagram of the last samples is the closest to the true offset.
class HostClock {
 public:
  void Note(double host_seconds, double now) {
    offsets_[next_++ % offsets_.size()] = now - host_seconds;
    count_ = std::min(count_ + 1, offsets_.size());
  }
  std::optional<double> HostNow(double now) const {
    if (count_ == 0) return std::nullopt;
    double lowest = offsets_[0];
    for (size_t i = 1; i < count_; ++i) lowest = std::min(lowest, offsets_[i]);
    return now - lowest;
  }

 private:
  std::array<double, 40> offsets_{};
  size_t next_ = 0;
  size_t count_ = 0;
};

// A kart that has not been given a heading yet holds zeros.
inline bool HasHeading(const KartSample& kart) {
  float length = 0;
  for (float v : kart.quat) length += v * v;
  return length >= kMinHeadingLength;
}

inline void NormalizeQuat(float (&quat)[4]) {
  float length = 0;
  for (float v : quat) length += v * v;
  length = std::sqrt(length);
  if (length < 1e-6f) return;
  for (float& v : quat) v /= length;
}

inline KartSample Blend(const KartSample& a, const KartSample& b, float t) {
  KartSample out = a;
  for (int i = 0; i < 3; ++i) {
    out.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
    out.velocity[i] = a.velocity[i] + (b.velocity[i] - a.velocity[i]) * t;
  }
  out.speed = a.speed + (b.speed - a.speed) * t;
  float dot = 0;
  for (int i = 0; i < 4; ++i) dot += a.quat[i] * b.quat[i];
  const float sign = dot < 0 ? -1.0f : 1.0f;
  for (int i = 0; i < 4; ++i) out.quat[i] = a.quat[i] + (b.quat[i] * sign - a.quat[i]) * t;
  NormalizeQuat(out.quat);
  return out;
}

// One kart's recent snapshots, sampled at any time: what the receiver's frame rate decides is how often it asks.
class KartTrack {
 public:
  void Push(double host_time, const KartSample& kart) {
    if (count_ > 0 && host_time <= At(count_ - 1).time) return;
    entries_[(first_ + count_) % entries_.size()] = {host_time, kart};
    if (count_ == entries_.size()) {
      first_ = (first_ + 1) % entries_.size();
    } else {
      ++count_;
    }
  }

  std::optional<KartSample> Sample(double time) const {
    if (count_ == 0) return std::nullopt;
    if (time <= At(0).time) return At(0).kart;
    for (size_t i = 0; i + 1 < count_; ++i) {
      const Entry& a = At(i);
      const Entry& b = At(i + 1);
      if (time <= b.time) return Blend(a.kart, b.kart, float((time - a.time) / (b.time - a.time)));
    }
    const Entry& last = At(count_ - 1);
    KartSample out = last.kart;
    const float ahead = float(std::min(time - last.time, kMaxExtrapolation));
    for (int i = 0; i < 3; ++i) out.position[i] += out.velocity[i] * ahead;
    return out;
  }

 private:
  struct Entry {
    double time = 0;
    KartSample kart;
  };
  const Entry& At(size_t index) const { return entries_[(first_ + index) % entries_.size()]; }

  std::array<Entry, 16> entries_{};
  size_t first_ = 0;
  size_t count_ = 0;
};

enum class Correction { kNone, kNudge, kSnap };

struct OwnCorrection {
  Correction kind = Correction::kNone;
  float offset[3]{};  // kNudge
};

// A guest drives its own kart at once and the host's answer arrives a round trip later, so that answer is compared with
// where this kart was when the host had that input, not with where it is now.
class Reconciler {
 public:
  void NoteOwn(uint32_t seq, const float (&position)[3]) {
    Past& past = past_[seq % past_.size()];
    past.seq = seq;
    std::copy(position, position + 3, past.position);
  }

  // Returns true when the difference is too large to blend and `host` should replace the kart.
  bool OnHost(uint32_t seq, const KartSample& host, const float (&current)[3]) {
    const Past& past = past_[seq % past_.size()];
    const float* then = seq != 0 && past.seq == seq ? past.position : current;
    float length = 0;
    for (int i = 0; i < 3; ++i) {
      error_[i] = host.position[i] - then[i];
      length += error_[i] * error_[i];
    }
    length = std::sqrt(length);
    float speed = 0;
    for (float v : host.velocity) speed += v * v;
    if (length < kDeadZone + std::sqrt(speed) * kTimingSlack) {
      std::fill(error_, error_ + 3, 0.0f);
      return false;
    }
    if (length < kSnapDistance) return false;
    std::fill(error_, error_ + 3, 0.0f);
    return true;
  }

  OwnCorrection Step(double dt) {
    OwnCorrection out;
    float length = 0;
    for (float v : error_) length += v * v;
    if (length <= 0) return out;
    const float share = 1.0f - std::exp(-kBlendPerSecond * float(dt));
    out.kind = Correction::kNudge;
    for (int i = 0; i < 3; ++i) {
      out.offset[i] = error_[i] * share;
      error_[i] -= out.offset[i];
    }
    for (Past& past : past_) {
      for (int i = 0; i < 3; ++i) past.position[i] += out.offset[i];
    }
    return out;
  }

 private:
  struct Past {
    uint32_t seq = 0;
    float position[3]{};
  };
  std::array<Past, 128> past_{};
  float error_[3]{};
};

struct SyncConfig {
  uint32_t generation = 0;
  int seat = 0;
  int humans = 0;
  int total = 0;
  std::array<int, race::kMaxHumans> seats{};        // kart slot of a human -> seat
  std::array<int, race::kMaxHumans> pad_of_seat{};  // the pad each seat's kart answers to on this PC
};

using KartFrames = std::array<std::optional<KartSample>, race::kMaxKarts>;

struct FrameIn {
  double now = 0;
  double dt = 0;
  int race_state = -1;  // -1 until the race's game exists
  KartFrames karts;     // this PC's karts by slot, empty while loading
  PadState own_pad;
};

struct KartWrite {
  int slot = 0;
  bool position_only = false;
  KartSample state;
};

struct FrameOut {
  std::array<std::optional<PadState>, race::kMaxHumans> pads;  // by pad index: what this PC's remote karts read
  GatePad own;
  std::array<KartWrite, race::kMaxKarts> writes;
  int write_count = 0;
  bool waiting_for_host = false;
  bool post_race = false;
};

struct Traffic {
  uint64_t tx_packets = 0;
  uint64_t rx_packets = 0;
  uint64_t tx_bytes = 0;
  uint64_t rx_bytes = 0;
  uint64_t dropped_old = 0;
  uint64_t rejected = 0;
  uint64_t own_snaps = 0;  // DEBUG_SAULO
};

// The established Direct or Relay path of each seat. Nothing else carries a kart.
class PeerLink {
 public:
  virtual ~PeerLink() = default;
  virtual void Send(int seat, const std::vector<uint8_t>& datagram) = 0;
  // Never blocks. The seat is -1 for a datagram of nobody in this session.
  virtual size_t Receive(int& seat, uint8_t* buffer, size_t capacity) = 0;
};

class RaceSync {
 public:
  explicit RaceSync(const SyncConfig& config) : config_(config), gate_(config.seat == 0) {}

  bool host() const { return config_.seat == 0; }
  const SyncConfig& config() const { return config_; }
  const Traffic& traffic() const { return traffic_; }
  bool begun() const { return gate_.begun(); }

  FrameOut Tick(const FrameIn& in, PeerLink& link) {
    started_ = started_ > 0 ? started_ : in.now;
    Receive(in, link);
    FrameOut out;
    if (host()) {
      HostFrame(in, link, out);
    } else {
      GuestFrame(in, link, out);
    }
    return out;
  }

  std::vector<std::string> TakeNotes() {
    std::vector<std::string> notes;
    notes.swap(notes_);
    return notes;
  }
  std::optional<int> RttMs(int seat, double now) const {
    return size_t(seat) < rtt_.size() ? rtt_[size_t(seat)].Ms(now) : std::nullopt;
  }
  // DEBUG_SAULO
  uint32_t last_input_seq(int seat) const { return size_t(seat) < inputs_.size() ? inputs_[size_t(seat)].gate.last() : 0; }
  uint32_t last_snapshot_seq() const { return snapshots_.last(); }
  double LastHeardAge(int seat, double now) const {
    const double heard = host() ? inputs_[size_t(seat)].heard : last_snapshot_at_;
    return heard > 0 ? now - heard : -1;
  }

 private:
  struct SeatInput {
    InputGate gate;
    PadState pad;
    double heard = 0;
    bool loaded = false;
  };

  bool IsGuestSeat(int seat) const {
    for (int slot = 1; slot < config_.humans; ++slot) {
      if (config_.seats[size_t(slot)] == seat) return true;
    }
    return false;
  }

  int OwnSlot() const {
    for (int slot = 0; slot < config_.humans; ++slot) {
      if (config_.seats[size_t(slot)] == config_.seat) return slot;
    }
    return -1;
  }

  void NoteOnce(bool& said, const std::string& note) {
    if (said) return;
    said = true;
    notes_.push_back(note);
  }

  void Receive(const FrameIn& in, PeerLink& link) {
    uint8_t buffer[race::kMaxDatagram + 1];
    int seat = -1;
    while (const size_t size = link.Receive(seat, buffer, sizeof buffer)) {
      const std::optional<race::Message> message = race::Decode(buffer, size);
      if (!message) {
        ++traffic_.rejected;
        continue;
      }
      ++traffic_.rx_packets;
      traffic_.rx_bytes += size;
      if (const InputMsg* input = std::get_if<InputMsg>(&*message)) {
        if (host()) TakeInput(*input, seat, in);
      } else if (const SnapshotMsg* snapshot = std::get_if<SnapshotMsg>(&*message)) {
        if (!host()) TakeSnapshot(*snapshot, seat, in);
      }
    }
  }

  void Count(Verdict verdict) {
    if (verdict == Verdict::kOld) {
      ++traffic_.dropped_old;
    } else {
      ++traffic_.rejected;
    }
  }

  void TakeInput(const InputMsg& input, int seat, const FrameIn& in) {
    if (seat < 0 || size_t(seat) >= inputs_.size() || !IsGuestSeat(seat)) {
      ++traffic_.rejected;
      return;
    }
    SeatInput& entry = inputs_[size_t(seat)];
    const Verdict verdict = entry.gate.Check(input, seat, config_.generation);
    if (verdict != Verdict::kAccepted) return Count(verdict);
    entry.pad = input.pad;
    entry.heard = in.now;
    entry.loaded = entry.loaded || input.loaded;
    rtt_[size_t(seat)].NoteAck(input.snapshot_ack, in.now);
    NoteOnce(said_input_rx_[size_t(seat)], "gameplay input_rx seat=" + std::to_string(seat));
  }

  // Where the host's kart was when that input reached it, the moment the guest's own kart is comparable with.
  static KartSample AtInputArrival(KartSample kart, uint8_t age_ms) {
    const float age = float(age_ms) / 1000.0f;
    for (int i = 0; i < 3; ++i) kart.position[i] -= kart.velocity[i] * age;
    return kart;
  }

  void TakeSnapshot(const SnapshotMsg& snapshot, int seat, const FrameIn& in) {
    const Verdict verdict = snapshots_.Check(snapshot, seat, config_.generation, config_.total);
    if (verdict != Verdict::kAccepted) return Count(verdict);
    last_snapshot_at_ = in.now;
    NoteOnce(said_snapshot_rx_, "gameplay snapshot_rx");
    if (gate_.NoteBegin(snapshot.begun)) notes_.push_back("race_begin received");
    host_state_ = snapshot.race_state;
    const double host_time = snapshot.host_ms / 1000.0;
    clock_.Note(host_time, in.now);
    const uint32_t acked = snapshot.input_ack[size_t(config_.seat)];
    rtt_[0].NoteAck(acked, in.now);
    const int own = OwnSlot();
    for (const KartSample& kart : snapshot.karts) {
      if (kart.slot != own) {
        tracks_[kart.slot].Push(host_time, kart);
      } else {
        own_host_ = kart;
        own_arrival_ = AtInputArrival(kart, snapshot.input_age_ms[size_t(config_.seat)]);
        own_ack_ = acked;
        own_fresh_ = true;
      }
    }
  }

  void HostFrame(const FrameIn& in, PeerLink& link, FrameOut& out) {
    out.post_race = in.race_state > kRaceInProgress;
    if (out.post_race) NoteOnce(said_post_race_, "post_race host");
    if (gate_.NoteRaceState(in.race_state)) {
      notes_.push_back("race_begin host_confirmed");
      notes_.push_back("race_begin broadcast");
    }
    bool all_loaded = true;
    for (int slot = 0; slot < config_.humans; ++slot) {
      const int seat = config_.seats[size_t(slot)];
      if (seat == config_.seat) continue;
      const SeatInput& entry = inputs_[size_t(seat)];
      all_loaded = all_loaded && entry.loaded;
      const int pad = config_.pad_of_seat[size_t(seat)];
      if (pad < 0 || pad >= race::kMaxHumans) continue;
      const bool fresh = entry.heard > 0 && in.now - entry.heard <= kInputHoldSeconds;
      out.pads[size_t(pad)] = fresh ? HostSidePad(entry.pad, gate_.begun() && !out.post_race) : PadState{};
    }
    out.own = HostGatePad(gate_.begun(), all_loaded, in.now - started_);
    if (snapshot_clock_.Due(in.now)) SendSnapshot(in, link);
  }

  void SendSnapshot(const FrameIn& in, PeerLink& link) {
    SnapshotMsg snapshot;
    snapshot.race_id = config_.generation;
    snapshot.seq = ++snapshot_seq_;
    snapshot.host_ms = uint32_t(in.now * 1000.0);
    snapshot.race_state = uint8_t(std::clamp(in.race_state, 0, 4));
    snapshot.begun = gate_.begun();
    std::string seats;
    for (int slot = 0; slot < config_.total; ++slot) {
      if (!in.karts[size_t(slot)]) continue;
      snapshot.karts.push_back(*in.karts[size_t(slot)]);
      if (slot < config_.humans) seats += (seats.empty() ? "" : ",") + std::to_string(config_.seats[size_t(slot)]);
    }
    for (size_t seat = 0; seat < inputs_.size(); ++seat) {
      snapshot.input_ack[seat] = inputs_[seat].gate.last();
      snapshot.input_age_ms[seat] = uint8_t(std::clamp((in.now - inputs_[seat].heard) * 1000.0, 0.0, 255.0));
    }
    const std::vector<uint8_t> bytes = race::Encode(snapshot);
    for (int slot = 0; slot < config_.humans; ++slot) {
      const int seat = config_.seats[size_t(slot)];
      if (seat == config_.seat) continue;
      link.Send(seat, bytes);
      rtt_[size_t(seat)].NoteSent(snapshot.seq, in.now);
      ++traffic_.tx_packets;
      traffic_.tx_bytes += bytes.size();
    }
    if (!snapshot.karts.empty()) NoteOnce(said_snapshot_tx_, "gameplay snapshot_tx seats=" + seats);
  }

  void GuestFrame(const FrameIn& in, PeerLink& link, FrameOut& out) {
    NoteOnce(said_waiting_, "waiting_for_host");
    pending_buttons_ = uint16_t(pending_buttons_ | in.own_pad.buttons);
    out.post_race = std::max(in.race_state, host_state_) > kRaceInProgress;
    if (out.post_race) NoteOnce(said_post_race_, "post_race guest");
    out.own = GuestGatePad(gate_.begun(), out.post_race ? kRacePostRace : in.race_state, in.now);
    out.waiting_for_host = out.post_race || (!gate_.begun() && in.race_state >= 0);
    for (int slot = 0; slot < config_.humans; ++slot) {
      const int seat = config_.seats[size_t(slot)];
      const int pad = config_.pad_of_seat[size_t(seat)];
      if (seat != config_.seat && pad >= 0 && pad < race::kMaxHumans) out.pads[size_t(pad)] = PadState{};
    }
    if (input_clock_.Due(in.now)) SendInput(in, link);
    ApplyRemote(in, out);
    ApplyOwn(in, out);
  }

  void SendInput(const FrameIn& in, PeerLink& link) {
    InputMsg input;
    input.race_id = config_.generation;
    input.seat = uint8_t(config_.seat);
    input.seq = ++input_seq_;
    input.snapshot_ack = snapshots_.last();
    input.loaded = in.race_state >= 0;
    input.pad = in.own_pad;
    input.pad.buttons = uint16_t(pending_buttons_ & ~out_of_band_);
    pending_buttons_ = 0;
    const std::vector<uint8_t> bytes = race::Encode(input);
    link.Send(0, bytes);
    rtt_[0].NoteSent(input.seq, in.now);
    ++traffic_.tx_packets;
    traffic_.tx_bytes += bytes.size();
    if (const int own = OwnSlot(); own >= 0 && in.karts[size_t(own)]) reconciler_.NoteOwn(input.seq, in.karts[size_t(own)]->position);
    NoteOnce(said_input_tx_, "gameplay input_tx seat=" + std::to_string(config_.seat));
  }

  void ApplyRemote(const FrameIn& in, FrameOut& out) {
    const std::optional<double> host_now = clock_.HostNow(in.now);
    if (!host_now) return;
    const double render_time = *host_now - kInterpolationDelay;
    const int own = OwnSlot();
    for (int slot = 0; slot < config_.total; ++slot) {
      if (slot == own || !in.karts[size_t(slot)]) continue;
      const std::optional<KartSample> kart = tracks_[size_t(slot)].Sample(render_time);
      if (!kart) continue;
      KartWrite& write = out.writes[size_t(out.write_count++)];
      write.slot = slot;
      write.state = *kart;
      if (slot < config_.humans) NoteOnce(said_applied_[size_t(slot)], "remote seat=" + std::to_string(config_.seats[size_t(slot)]) + " state_applied");
    }
  }

  void ApplyOwn(const FrameIn& in, FrameOut& out) {
    const int own = OwnSlot();
    if (own < 0 || !in.karts[size_t(own)]) return;
    const KartSample& mine = *in.karts[size_t(own)];
    bool snap = false;
    if (own_fresh_) {
      own_fresh_ = false;
      // Before the green light nothing is predicted, so the kart simply stands where the host has it.
      const bool racing = in.race_state == kRaceInProgress && host_state_ == kRaceInProgress;
      snap = reconciler_.OnHost(racing ? own_ack_ : 0, racing ? own_arrival_ : own_host_, mine.position);
    }
    KartWrite& write = out.writes[size_t(out.write_count)];
    write.slot = own;
    if (snap) {
      if (in.race_state == kRaceInProgress) ++traffic_.own_snaps;  // DEBUG_SAULO
      write.state = own_host_;
      const float latency = float(rtt_[0].Ms(in.now).value_or(0)) / 2000.0f;
      for (int i = 0; i < 3; ++i) write.state.position[i] += write.state.velocity[i] * latency;
      ++out.write_count;
      return;
    }
    const OwnCorrection correction = reconciler_.Step(in.dt);
    if (correction.kind != Correction::kNudge) return;
    write.position_only = true;
    write.state = mine;
    for (int i = 0; i < 3; ++i) write.state.position[i] += correction.offset[i];
    ++out.write_count;
  }

  static constexpr uint16_t out_of_band_ = kPadStart | kPadBack;

  SyncConfig config_;
  StartGate gate_;
  Traffic traffic_;
  double started_ = 0;
  std::array<SeatInput, race::kMaxHumans> inputs_{};
  std::array<RoundTrip, race::kMaxHumans> rtt_{};
  SendClock snapshot_clock_{kSnapshotHz};
  SendClock input_clock_{kInputHz};
  uint32_t snapshot_seq_ = 0;
  uint32_t input_seq_ = 0;
  uint16_t pending_buttons_ = 0;
  SnapshotGate snapshots_;
  HostClock clock_;
  std::array<KartTrack, race::kMaxKarts> tracks_{};
  Reconciler reconciler_;
  KartSample own_host_;
  KartSample own_arrival_;
  uint32_t own_ack_ = 0;
  bool own_fresh_ = false;
  int host_state_ = 0;
  bool said_post_race_ = false;
  double last_snapshot_at_ = 0;
  std::vector<std::string> notes_;
  std::array<bool, race::kMaxHumans> said_input_rx_{};
  std::array<bool, race::kMaxHumans> said_applied_{};
  bool said_snapshot_rx_ = false;
  bool said_snapshot_tx_ = false;
  bool said_input_tx_ = false;
  bool said_waiting_ = false;
};

// Where each seat is, as the UI thread last published it: the game thread never waits on the service.
struct PeerRoute {
  int seat = -1;
  online::Endpoint endpoint;
};
using PeerRoutes = std::array<PeerRoute, race::kMaxHumans>;

inline PeerRoutes RoutesOf(const std::vector<online::GamePeer>& peers) {
  PeerRoutes routes;
  size_t count = 0;
  for (const online::GamePeer& peer : peers) {
    if (count < routes.size()) routes[count++] = {peer.seat, peer.endpoint};
  }
  return routes;
}

// Direct and Relay both look like an endpoint on the one gameplay socket, so nothing above this class tells them apart.
class SocketLink final : public PeerLink {
 public:
  SocketLink(online::Transport& socket, const PeerRoutes& routes) : socket_(socket), routes_(routes) {}

  void SetRoutes(const PeerRoutes& routes) { routes_ = routes; }

  void Send(int seat, const std::vector<uint8_t>& datagram) override {
    for (const PeerRoute& route : routes_) {
      if (route.seat == seat) socket_.Send(route.endpoint, datagram.data(), datagram.size());
    }
  }

  size_t Receive(int& seat, uint8_t* buffer, size_t capacity) override {
    online::Endpoint from;
    const size_t size = socket_.Receive(buffer, capacity, from);
    seat = -1;
    if (const int relayed = online::RelaySeatOf(from); relayed >= 0) seat = relayed;
    for (const PeerRoute& route : routes_) {
      if (route.seat >= 0 && route.endpoint == from) seat = route.seat;
    }
    return size;
  }

 private:
  online::Transport& socket_;
  PeerRoutes routes_;
};

}  // namespace eck::race_sync
