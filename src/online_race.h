#pragma once

// Pure model of an online race: how a room's seats become kart slots, the datagrams the PCs exchange during the race,
// and the host-authoritative phase machine. No guest memory and no sockets.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace eck::race {

inline constexpr int kMaxHumans = 4;
inline constexpr int kMaxKarts = 8;  // "Race against 7 competitors"
inline constexpr uint8_t kMagic0 = 'E';
inline constexpr uint8_t kMagic1 = 'R';
inline constexpr uint8_t kVersion = 3;
inline constexpr size_t kHeaderSize = 4;
inline constexpr size_t kMaxDatagram = 512;

inline constexpr double kLoadTimeoutSeconds = 45.0;
inline constexpr double kCountdownSeconds = 3.0;

struct Participant {
  uint8_t seat = 0;
  uint8_t character = 0;
  uint8_t kart = 0;
  bool operator==(const Participant&) const = default;
};

// Sent by the host once, then again until every seat reports loaded.
struct RaceConfig {
  uint32_t race_id = 0;
  uint16_t track = 0;
  uint8_t laps = 3;
  uint8_t total_karts = kMaxKarts;
  uint8_t bot_difficulty = 0;
  uint8_t rules = 0;  // bit 0: items on
  uint32_t seed = 0;
  std::vector<Participant> participants;
  bool operator==(const RaceConfig&) const = default;
};

enum class Phase : uint8_t { kLobby, kStarting, kCountdown, kRacing, kFinished };

struct PhaseMsg {
  uint32_t race_id = 0;
  Phase phase = Phase::kLobby;
  uint8_t countdown = 0;  // whole seconds left, while counting down
  bool operator==(const PhaseMsg&) const = default;
};

// Every PC sends its own kart; a receiver drives the matching remote kart. Higher `tick` wins, so reordering is safe.
struct KartState {
  uint32_t race_id = 0;
  uint8_t seat = 0;
  uint32_t tick = 0;
  float position[3]{};
  int32_t rotation[3]{};  // UE3 rotator units (65536 per turn)
  float velocity[3]{};
  float steering = 0;  // -1..1
  float throttle = 0;  // -1..1
  uint8_t lap = 0;
  uint16_t checkpoint = 0;
  uint8_t place = 0;
  uint8_t flags = 0;  // bit 0 drifting, bit 1 braking, bit 2 boosted
  bool operator==(const KartState& o) const;
};

struct Finish {
  uint32_t race_id = 0;
  uint8_t seat = 0;
  uint32_t time_ms = 0;
  uint8_t place = 0;
  bool operator==(const Finish&) const = default;
};

enum class Outcome : uint8_t { kNone, kCompleted, kPlayerLeft, kConnectionLost, kHostLeft, kHostConnectionLost };
enum class SeatResult : uint8_t { kRacing, kFinished, kLeft, kLost };

struct ResultEntry {
  uint8_t seat = 0;
  SeatResult result = SeatResult::kRacing;
  uint8_t place = 0;
  uint32_t time_ms = 0;
  bool operator==(const ResultEntry&) const = default;
};

// Host's final word.
struct RaceResult {
  uint32_t race_id = 0;
  Outcome outcome = Outcome::kNone;
  std::vector<ResultEntry> entries;
  bool operator==(const RaceResult&) const = default;
};

struct Leave {
  uint32_t race_id = 0;
  uint8_t seat = 0;
  bool operator==(const Leave&) const = default;
};

// A client tells the host its race scene is loaded.
struct Loaded {
  uint32_t race_id = 0;
  uint8_t seat = 0;
  bool operator==(const Loaded&) const = default;
};

// What a controller reports, native XInput values: everything the game derives driving from.
struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t left_x = 0;
  int16_t left_y = 0;
  int16_t right_x = 0;
  int16_t right_y = 0;
  bool operator==(const PadState&) const = default;
};

// A guest's pad, to the host only.
struct InputMsg {
  uint32_t race_id = 0;
  uint8_t seat = 0;
  uint32_t seq = 0;
  uint32_t snapshot_ack = 0;  // the newest snapshot this guest has
  bool loaded = false;        // its karts exist
  PadState pad;
  bool operator==(const InputMsg&) const = default;
};

struct KartSample {
  uint8_t slot = 0;
  float position[3]{};
  float quat[4]{0, 0, 0, 1};
  float velocity[3]{};
  float speed = 0;
  // The race the host says this kart is having: the game's own counters, so a guest's HUD, ordering and results are the
  // host's. A guest writes them onto a remote kart and never computes them itself.
  uint8_t lap = 0;             // CompletedLaps
  float distance = 0;          // DistanceInRace: what the game orders karts by
  uint8_t place = 0;           // RacePosition
  uint8_t finish_place = 0;    // RaceFinishPosition, 0 while the kart has not finished
  float racing_time = 0;       // racingTime
  bool finished = false;       // bIsRaceFinished
  bool operator==(const KartSample&) const = default;
};

// The host's race, to every guest. `begun` repeats in every snapshot, so a lost one costs nothing.
struct SnapshotMsg {
  uint32_t race_id = 0;
  uint32_t seq = 0;
  uint32_t host_ms = 0;
  uint8_t race_state = 0;
  bool begun = false;
  std::array<uint32_t, kMaxHumans> input_ack{};  // per seat: the newest input the host has applied
  std::array<uint8_t, kMaxHumans> input_age_ms{};  // and how long the kart has been running on it, capped at 255
  std::vector<KartSample> karts;
  bool operator==(const SnapshotMsg&) const = default;
};

using Message = std::variant<RaceConfig, PhaseMsg, KartState, Finish, RaceResult, Leave, Loaded, InputMsg, SnapshotMsg>;

std::vector<uint8_t> Encode(const Message& message);
// Rejects a short, long, mistyped, wrong-version or non-finite datagram, and any trailing bytes.
std::optional<Message> Decode(const uint8_t* data, size_t size);

// ---- Seat to kart mapping -------------------------------------------------------------------------------------------

struct KartAssignment {
  int slot = 0;
  int seat = -1;           // -1 for a bot
  int controller_id = -1;  // 0 on this PC for its own seat; -1 for remote humans and bots
  int character = -1;
  int kart = -1;
  bool human = false;
  bool local = false;
  bool remote_human = false;  // driven by the network, never by the CPU
  bool bot = false;
};

struct RaceCounts {
  int humans = 0;
  int local_humans = 0;
  int remote_humans = 0;
  int bots = 0;
  int total = 0;
};

bool ValidConfig(const RaceConfig& config);
// Humans take slots 0.. in seat order (identical on every PC), bots fill the rest. Empty when the config is invalid or
// `local_seat` does not take part.
std::vector<KartAssignment> MapSeatsToKarts(const RaceConfig& config, int local_seat);
RaceCounts CountKarts(const std::vector<KartAssignment>& karts);
// The slot of a seat, or -1.
int SlotOfSeat(const std::vector<KartAssignment>& karts, int seat);

// ---- Phase machine --------------------------------------------------------------------------------------------------

struct Notice {
  Outcome outcome = Outcome::kNone;
  int seat = -1;
};

// One per PC. The host (seat 0) owns every transition and announces it; a client only follows PhaseMsg/RaceResult from
// seat 0 for the current race. Time is passed in, so it is deterministic.
class RaceSession {
 public:
  RaceSession(int my_seat, bool host) : my_seat_(my_seat), host_(host) {}

  Phase phase() const { return phase_; }
  Outcome outcome() const { return outcome_; }
  uint32_t race_id() const { return race_id_; }
  bool host() const { return host_; }
  const RaceConfig& config() const { return config_; }
  SeatResult SeatStatus(int seat) const;

  // Host: lobby -> starting. False when the config is invalid, a race is under way or this is not the host.
  bool HostBegin(const RaceConfig& config, double now);
  // Client: a config from `from_seat` (must be 0). Repeats of the current race are ignored.
  bool OnConfig(int from_seat, const RaceConfig& config, double now);
  // Host: a seat reports its scene loaded (the host calls it for itself too).
  void OnLoaded(int seat, double now);
  // From the host: the phase advanced. Honoured only from seat 0, for this race, and only forward.
  bool OnPhase(int from_seat, const PhaseMsg& msg);
  void OnFinish(int seat, uint8_t place, uint32_t time_ms);
  // The seat left on purpose, or its link dropped; `lost` picks the outcome.
  void OnSeatGone(int seat, bool lost);
  bool OnResult(int from_seat, const RaceResult& result);
  void Tick(double now);

  // Messages the host must broadcast to every other seat since the last call.
  std::vector<Message> TakeOutbox();
  // "Player left" / "connection lost" style outcomes since the last call, for the UI.
  std::vector<Notice> TakeNotices();

 private:
  struct Entry {
    Participant who;
    SeatResult status = SeatResult::kRacing;
    bool loaded = false;
    uint8_t place = 0;
    uint32_t time_ms = 0;
  };
  Entry* Find(int seat);
  void Enter(Phase phase, double now);
  void Conclude(Outcome outcome);
  void CheckEnd();
  int ActiveHumans() const;

  int my_seat_;
  bool host_;
  Phase phase_ = Phase::kLobby;
  Outcome outcome_ = Outcome::kNone;
  uint32_t race_id_ = 0;
  RaceConfig config_;
  std::vector<Entry> entries_;
  double phase_since_ = 0;
  uint8_t last_countdown_ = 0;
  std::vector<Message> outbox_;
  std::vector<Notice> notices_;
};

}  // namespace eck::race
