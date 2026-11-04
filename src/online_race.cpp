#include "online_race.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace eck::race {
namespace {

enum Type : uint8_t { kConfig = 1, kPhaseMsg = 2, kKart = 3, kFinish = 4, kResult = 5, kLeave = 6, kLoadedMsg = 7 };

constexpr float kMaxCoordinate = 1.0e7f;

struct Writer {
  std::vector<uint8_t> out;
  void U8(uint32_t v) { out.push_back(uint8_t(v)); }
  void U16(uint32_t v) {
    U8(v & 0xFF);
    U8(v >> 8);
  }
  void U32(uint32_t v) {
    U16(v & 0xFFFF);
    U16(v >> 16);
  }
  void I32(int32_t v) { U32(uint32_t(v)); }
  void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
};

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t at = 0;
  bool ok = true;
  bool Has(size_t n) {
    if (!ok || size - at < n) ok = false;
    return ok;
  }
  uint8_t U8() { return Has(1) ? data[at++] : 0; }
  uint16_t U16() {
    const uint16_t lo = U8();
    return uint16_t(lo | (uint16_t(U8()) << 8));
  }
  uint32_t U32() {
    const uint32_t lo = U16();
    return lo | (uint32_t(U16()) << 16);
  }
  int32_t I32() { return int32_t(U32()); }
  float F32() { return std::bit_cast<float>(U32()); }
  // A coordinate, which must be finite and bounded.
  float Coord() {
    const float v = F32();
    if (!std::isfinite(v) || std::fabs(v) > kMaxCoordinate) ok = false;
    return v;
  }
  // An axis input, clamped to -1..1.
  float Axis() {
    const float v = F32();
    if (!std::isfinite(v)) {
      ok = false;
      return 0;
    }
    return std::clamp(v, -1.0f, 1.0f);
  }
};

bool SameFloats(const float* a, const float* b, int n) {
  for (int i = 0; i < n; ++i) {
    if (std::bit_cast<uint32_t>(a[i]) != std::bit_cast<uint32_t>(b[i])) return false;
  }
  return true;
}

struct Encoder {
  Writer& w;
  void operator()(const RaceConfig& m) const {
    w.U8(kConfig);
    w.U32(m.race_id);
    w.U16(m.track);
    w.U8(m.laps);
    w.U8(m.total_karts);
    w.U8(m.bot_difficulty);
    w.U8(m.rules);
    w.U32(m.seed);
    w.U8(uint32_t(std::min<size_t>(m.participants.size(), kMaxHumans)));
    for (size_t i = 0; i < m.participants.size() && i < kMaxHumans; ++i) {
      w.U8(m.participants[i].seat);
      w.U8(m.participants[i].character);
      w.U8(m.participants[i].kart);
    }
  }
  void operator()(const PhaseMsg& m) const {
    w.U8(kPhaseMsg);
    w.U32(m.race_id);
    w.U8(uint8_t(m.phase));
    w.U8(m.countdown);
  }
  void operator()(const KartState& m) const {
    w.U8(kKart);
    w.U32(m.race_id);
    w.U8(m.seat);
    w.U32(m.tick);
    for (float v : m.position) w.F32(v);
    for (int32_t v : m.rotation) w.I32(v);
    for (float v : m.velocity) w.F32(v);
    w.F32(m.steering);
    w.F32(m.throttle);
    w.U8(m.lap);
    w.U16(m.checkpoint);
    w.U8(m.place);
    w.U8(m.flags);
  }
  void operator()(const Finish& m) const {
    w.U8(kFinish);
    w.U32(m.race_id);
    w.U8(m.seat);
    w.U32(m.time_ms);
    w.U8(m.place);
  }
  void operator()(const RaceResult& m) const {
    w.U8(kResult);
    w.U32(m.race_id);
    w.U8(uint8_t(m.outcome));
    w.U8(uint32_t(std::min<size_t>(m.entries.size(), kMaxHumans)));
    for (size_t i = 0; i < m.entries.size() && i < kMaxHumans; ++i) {
      w.U8(m.entries[i].seat);
      w.U8(uint8_t(m.entries[i].result));
      w.U8(m.entries[i].place);
      w.U32(m.entries[i].time_ms);
    }
  }
  void operator()(const Leave& m) const {
    w.U8(kLeave);
    w.U32(m.race_id);
    w.U8(m.seat);
  }
  void operator()(const Loaded& m) const {
    w.U8(kLoadedMsg);
    w.U32(m.race_id);
    w.U8(m.seat);
  }
};

}  // namespace

bool KartState::operator==(const KartState& o) const {
  return race_id == o.race_id && seat == o.seat && tick == o.tick && SameFloats(position, o.position, 3) &&
         std::memcmp(rotation, o.rotation, sizeof rotation) == 0 && SameFloats(velocity, o.velocity, 3) &&
         SameFloats(&steering, &o.steering, 1) && SameFloats(&throttle, &o.throttle, 1) && lap == o.lap &&
         checkpoint == o.checkpoint && place == o.place && flags == o.flags;
}

std::vector<uint8_t> Encode(const Message& message) {
  Writer w;
  w.U8(kMagic0);
  w.U8(kMagic1);
  w.U8(kVersion);
  std::visit(Encoder{w}, message);
  return std::move(w.out);
}

std::optional<Message> Decode(const uint8_t* data, size_t size) {
  if (data == nullptr || size < kHeaderSize || size > kMaxDatagram) return std::nullopt;
  if (data[0] != kMagic0 || data[1] != kMagic1 || data[2] != kVersion) return std::nullopt;
  Reader r{data, size, 3};
  const uint8_t type = r.U8();
  std::optional<Message> out;
  switch (type) {
    case kConfig: {
      RaceConfig m;
      m.race_id = r.U32();
      m.track = r.U16();
      m.laps = r.U8();
      m.total_karts = r.U8();
      m.bot_difficulty = r.U8();
      m.rules = r.U8();
      m.seed = r.U32();
      const uint8_t count = r.U8();
      if (count > kMaxHumans) return std::nullopt;
      for (uint8_t i = 0; i < count; ++i) {
        Participant p;
        p.seat = r.U8();
        p.character = r.U8();
        p.kart = r.U8();
        m.participants.push_back(p);
      }
      out = std::move(m);
      break;
    }
    case kPhaseMsg: {
      PhaseMsg m;
      m.race_id = r.U32();
      const uint8_t phase = r.U8();
      if (phase > uint8_t(Phase::kFinished)) return std::nullopt;
      m.phase = Phase(phase);
      m.countdown = r.U8();
      out = m;
      break;
    }
    case kKart: {
      KartState m;
      m.race_id = r.U32();
      m.seat = r.U8();
      m.tick = r.U32();
      for (float& v : m.position) v = r.Coord();
      for (int32_t& v : m.rotation) v = r.I32();
      for (float& v : m.velocity) v = r.Coord();
      m.steering = r.Axis();
      m.throttle = r.Axis();
      m.lap = r.U8();
      m.checkpoint = r.U16();
      m.place = r.U8();
      m.flags = r.U8();
      if (m.seat >= kMaxHumans) return std::nullopt;
      out = m;
      break;
    }
    case kFinish: {
      Finish m;
      m.race_id = r.U32();
      m.seat = r.U8();
      m.time_ms = r.U32();
      m.place = r.U8();
      if (m.seat >= kMaxHumans) return std::nullopt;
      out = m;
      break;
    }
    case kResult: {
      RaceResult m;
      m.race_id = r.U32();
      const uint8_t outcome = r.U8();
      if (outcome > uint8_t(Outcome::kHostConnectionLost)) return std::nullopt;
      m.outcome = Outcome(outcome);
      const uint8_t count = r.U8();
      if (count > kMaxHumans) return std::nullopt;
      for (uint8_t i = 0; i < count; ++i) {
        ResultEntry e;
        e.seat = r.U8();
        const uint8_t result = r.U8();
        if (e.seat >= kMaxHumans || result > uint8_t(SeatResult::kLost)) return std::nullopt;
        e.result = SeatResult(result);
        e.place = r.U8();
        e.time_ms = r.U32();
        m.entries.push_back(e);
      }
      out = std::move(m);
      break;
    }
    case kLeave: {
      Leave m;
      m.race_id = r.U32();
      m.seat = r.U8();
      if (m.seat >= kMaxHumans) return std::nullopt;
      out = m;
      break;
    }
    case kLoadedMsg: {
      Loaded m;
      m.race_id = r.U32();
      m.seat = r.U8();
      if (m.seat >= kMaxHumans) return std::nullopt;
      out = m;
      break;
    }
    default:
      return std::nullopt;
  }
  if (!r.ok || r.at != size) return std::nullopt;
  return out;
}

// ---- Seat mapping ---------------------------------------------------------------------------------------------------

bool ValidConfig(const RaceConfig& config) {
  const size_t n = config.participants.size();
  if (n < 2 || n > kMaxHumans) return false;
  if (config.total_karts < n || config.total_karts > kMaxKarts) return false;
  if (config.laps < 1 || config.laps > 20) return false;
  bool seen[kMaxHumans]{};
  for (const Participant& p : config.participants) {
    if (p.seat >= kMaxHumans || seen[p.seat]) return false;
    seen[p.seat] = true;
  }
  return true;
}

std::vector<KartAssignment> MapSeatsToKarts(const RaceConfig& config, int local_seat) {
  std::vector<KartAssignment> karts;
  if (!ValidConfig(config)) return karts;
  std::vector<Participant> humans = config.participants;
  std::sort(humans.begin(), humans.end(), [](const Participant& a, const Participant& b) { return a.seat < b.seat; });
  if (std::none_of(humans.begin(), humans.end(), [&](const Participant& p) { return p.seat == local_seat; })) {
    return karts;
  }
  for (const Participant& p : humans) {
    KartAssignment k;
    k.slot = int(karts.size());
    k.seat = p.seat;
    k.human = true;
    k.character = p.character;
    k.kart = p.kart;
    k.local = p.seat == local_seat;
    k.remote_human = !k.local;
    k.controller_id = k.local ? 0 : -1;
    karts.push_back(k);
  }
  while (karts.size() < config.total_karts) {
    KartAssignment k;
    k.slot = int(karts.size());
    k.bot = true;
    karts.push_back(k);
  }
  return karts;
}

RaceCounts CountKarts(const std::vector<KartAssignment>& karts) {
  RaceCounts c;
  for (const KartAssignment& k : karts) {
    ++c.total;
    if (k.human) ++c.humans;
    if (k.local) ++c.local_humans;
    if (k.remote_human) ++c.remote_humans;
    if (k.bot) ++c.bots;
  }
  return c;
}

int SlotOfSeat(const std::vector<KartAssignment>& karts, int seat) {
  for (const KartAssignment& k : karts) {
    if (k.human && k.seat == seat) return k.slot;
  }
  return -1;
}

// ---- Phase machine --------------------------------------------------------------------------------------------------

RaceSession::Entry* RaceSession::Find(int seat) {
  for (Entry& e : entries_) {
    if (e.who.seat == seat) return &e;
  }
  return nullptr;
}

SeatResult RaceSession::SeatStatus(int seat) const {
  for (const Entry& e : entries_) {
    if (e.who.seat == seat) return e.status;
  }
  return SeatResult::kLeft;
}

int RaceSession::ActiveHumans() const {
  return int(std::count_if(entries_.begin(), entries_.end(), [](const Entry& e) {
    return e.status == SeatResult::kRacing || e.status == SeatResult::kFinished;
  }));
}

void RaceSession::Enter(Phase phase, double now) {
  phase_ = phase;
  phase_since_ = now;
  if (!host_) return;
  last_countdown_ = phase == Phase::kCountdown ? uint8_t(kCountdownSeconds) : 0;
  if (phase != Phase::kStarting) outbox_.push_back(PhaseMsg{race_id_, phase, last_countdown_});
}

bool RaceSession::HostBegin(const RaceConfig& config, double now) {
  if (!host_ || (phase_ != Phase::kLobby && phase_ != Phase::kFinished) || !ValidConfig(config)) return false;
  const auto& ps = config.participants;
  if (std::none_of(ps.begin(), ps.end(), [&](const Participant& p) { return p.seat == my_seat_; })) return false;
  config_ = config;
  race_id_ = config.race_id;
  outcome_ = Outcome::kNone;
  entries_.clear();
  for (const Participant& p : ps) entries_.push_back({p});
  outbox_.clear();
  Enter(Phase::kStarting, now);
  outbox_.push_back(config_);
  return true;
}

bool RaceSession::OnConfig(int from_seat, const RaceConfig& config, double now) {
  if (host_ || from_seat != 0 || !ValidConfig(config)) return false;
  if (phase_ != Phase::kLobby && phase_ != Phase::kFinished) return false;
  if (phase_ == Phase::kFinished && config.race_id == race_id_) return false;
  const auto& ps = config.participants;
  if (std::none_of(ps.begin(), ps.end(), [&](const Participant& p) { return p.seat == my_seat_; })) return false;
  config_ = config;
  race_id_ = config.race_id;
  outcome_ = Outcome::kNone;
  entries_.clear();
  for (const Participant& p : ps) entries_.push_back({p});
  Enter(Phase::kStarting, now);
  return true;
}

void RaceSession::OnLoaded(int seat, double now) {
  if (!host_ || phase_ != Phase::kStarting) return;
  if (Entry* e = Find(seat)) e->loaded = true;
  const bool all = std::all_of(entries_.begin(), entries_.end(), [](const Entry& e) {
    return e.loaded || (e.status != SeatResult::kRacing && e.status != SeatResult::kFinished);
  });
  if (all) Enter(Phase::kCountdown, now);
}

bool RaceSession::OnPhase(int from_seat, const PhaseMsg& msg) {
  if (host_ || from_seat != 0 || msg.race_id != race_id_ || phase_ == Phase::kLobby) return false;
  if (msg.phase == Phase::kLobby || msg.phase == Phase::kStarting) return false;
  if (msg.phase < phase_ || phase_ == Phase::kFinished) return false;
  phase_ = msg.phase;
  return true;
}

void RaceSession::OnFinish(int seat, uint8_t place, uint32_t time_ms) {
  if (phase_ != Phase::kRacing) return;
  Entry* e = Find(seat);
  if (e == nullptr || e->status != SeatResult::kRacing) return;
  e->status = SeatResult::kFinished;
  e->place = place;
  e->time_ms = time_ms;
  CheckEnd();
}

void RaceSession::CheckEnd() {
  if (!host_ || phase_ == Phase::kFinished || phase_ == Phase::kLobby) return;
  const bool racing = std::any_of(entries_.begin(), entries_.end(),
                                  [](const Entry& e) { return e.status == SeatResult::kRacing; });
  if (!racing) Conclude(Outcome::kCompleted);
}

void RaceSession::OnSeatGone(int seat, bool lost) {
  if (phase_ == Phase::kLobby || phase_ == Phase::kFinished) return;
  Entry* e = Find(seat);
  if (e == nullptr || seat == my_seat_) return;
  if (e->status == SeatResult::kLeft || e->status == SeatResult::kLost) return;
  const bool was_racing = e->status == SeatResult::kRacing;
  e->status = lost ? SeatResult::kLost : SeatResult::kLeft;
  if (seat == 0 && !host_) {
    const Outcome outcome = lost ? Outcome::kHostConnectionLost : Outcome::kHostLeft;
    notices_.push_back({outcome, seat});
    Conclude(outcome);
    return;
  }
  notices_.push_back({lost ? Outcome::kConnectionLost : Outcome::kPlayerLeft, seat});
  if (!host_) return;
  if (ActiveHumans() < 2 && was_racing) {
    Conclude(lost ? Outcome::kConnectionLost : Outcome::kPlayerLeft);
  } else {
    CheckEnd();
  }
}

void RaceSession::Conclude(Outcome outcome) {
  if (phase_ == Phase::kFinished) return;
  outcome_ = outcome;
  phase_ = Phase::kFinished;
  if (!host_) return;
  outbox_.push_back(PhaseMsg{race_id_, Phase::kFinished, 0});
  RaceResult result;
  result.race_id = race_id_;
  result.outcome = outcome;
  for (const Entry& e : entries_) result.entries.push_back({e.who.seat, e.status, e.place, e.time_ms});
  outbox_.push_back(std::move(result));
}

bool RaceSession::OnResult(int from_seat, const RaceResult& result) {
  if (host_ || from_seat != 0 || result.race_id != race_id_ || phase_ == Phase::kLobby) return false;
  // The host's PhaseMsg(kFinished) may arrive first; the result still fills in the outcome. A local conclusion stands.
  if (phase_ == Phase::kFinished && outcome_ != Outcome::kNone) return false;
  for (const ResultEntry& r : result.entries) {
    if (Entry* e = Find(r.seat)) {
      e->status = r.result;
      e->place = r.place;
      e->time_ms = r.time_ms;
    }
  }
  outcome_ = result.outcome;
  phase_ = Phase::kFinished;
  return true;
}

void RaceSession::Tick(double now) {
  if (!host_) return;
  if (phase_ == Phase::kStarting && now - phase_since_ >= kLoadTimeoutSeconds) {
    for (Entry& e : entries_) {
      if (e.loaded || e.status != SeatResult::kRacing) continue;
      e.status = SeatResult::kLost;
      notices_.push_back({Outcome::kConnectionLost, e.who.seat});
    }
    if (ActiveHumans() < 2) {
      Conclude(Outcome::kConnectionLost);
    } else {
      Enter(Phase::kCountdown, now);
    }
  } else if (phase_ == Phase::kCountdown) {
    const double elapsed = now - phase_since_;
    if (elapsed >= kCountdownSeconds) {
      Enter(Phase::kRacing, now);
      return;
    }
    const uint8_t left = uint8_t(std::ceil(kCountdownSeconds - elapsed));
    if (left != last_countdown_) {
      last_countdown_ = left;
      outbox_.push_back(PhaseMsg{race_id_, Phase::kCountdown, left});
    }
  }
}

std::vector<Message> RaceSession::TakeOutbox() { return std::exchange(outbox_, {}); }
std::vector<Notice> RaceSession::TakeNotices() { return std::exchange(notices_, {}); }

}  // namespace eck::race
