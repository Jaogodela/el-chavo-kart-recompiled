#include "online_race_telemetry_guest.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "online_guest_reader.h"
#include "online_probe_layout.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"
#include "online_runtime.h"

namespace eck::race_telemetry {

namespace {

namespace lay = online_probe::layout;
using online_guest::Guest;

constexpr double kPollSeconds = 0.25;

std::mutex g_mutex;
std::optional<RaceSetup> g_armed;

GameView ReadGame() {
  const Guest g;
  GameView view;
  const uint32_t world = g.P(lay::kGWorld);
  const uint32_t level = world != 0 ? g.P(world + lay::kWorldLevel) : 0;
  const uint32_t actors = level != 0 ? g.P(level + lay::kLevelActors) : 0;
  const uint32_t info = actors != 0 ? g.P(actors) : 0;
  view.game = info != 0 ? g.P(info + lay::kWorldGame) : 0;
  if (view.game == 0) return view;
  const uint8_t state = g.U8(view.game + lay::kMainGameRaceState);
  if (state <= kFinished) view.race_state = state;

  const uint32_t engine = g.P(lay::kGEngine);
  const uint32_t players = engine != 0 ? g.P(engine + lay::kEngineGamePlayers) : 0;
  const uint32_t count = engine != 0 ? g.U32(engine + lay::kEngineGamePlayers + 4).value_or(0) : 0;
  for (uint32_t i = 0; i < count && i < uint32_t(kMaxHumans) && players != 0; ++i) {
    const uint32_t local = g.P(players + 4 * i);
    const uint32_t controller = local != 0 ? g.P(local + lay::kPlayerActor) : 0;
    const uint32_t pawn = controller != 0 ? g.P(controller + lay::kControllerPawn) : 0;
    const uint32_t character = pawn != 0 ? g.P(pawn + lay::kKartCharInfo) : 0;
    if (character == 0) continue;
    KartReading kart;
    kart.controller_id = g.I32(local + lay::kLocalPlayerControllerId);
    kart.character = g.U8(character + lay::kCharInfoType);
    kart.finish_position = g.I32(pawn + lay::kKartRaceFinishPosition);
    kart.racing_time = g.F32(pawn + lay::kKartRacingTime);
    view.humans.push_back(kart);
  }
  return view;
}

// DEBUG_SAULO
void LogLines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) REXLOG_INFO("{} {}", kLogTag, line);
}

}  // namespace

void OnConsoleCommand(const uint8_t* base, uint32_t fstring) {
  const std::string text = online_guest::ConsoleCommandText(base, fstring);
  if (text.empty()) return;
  const std::optional<race_url::RaceUrl> url = race_url::ParseRaceUrl(text);
  if (!url) return;
  std::optional<RaceSetup> setup = SetupOfUrl(*url);
  if (!setup) return;
  std::lock_guard lock(g_mutex);
  g_armed = std::move(setup);
}

void PumpRaceTelemetry(online::OnlineService& service, double now) {
  static RaceTracker tracker;
  static Discovery discovery;
  static double next = 0;

  std::optional<RaceSetup> armed;
  {
    std::lock_guard lock(g_mutex);
    armed.swap(g_armed);
  }
  if (armed) LogLines(discovery.Note(*armed));
  const online::RaceTelemetryContext context = service.RaceContext();
  // Nothing here opens a connection: with no signed-in service a race is simply not reported.
  if (!context.ready) {
    if (armed) LogLines({"race not reported reason=service_not_ready map=" + armed->map});
    tracker.Reset();
    return;
  }
  if (armed) tracker.Arm(std::move(*armed));
  if (!tracker.Active() || now < next) return;
  next = now + kPollSeconds;
  ServiceView view;
  view.ready = true;
  view.room = context.room;
  view.seat = context.seat;
  const Output out = tracker.Step(ReadGame(), view, now);
  LogLines(out.lines);
  if (out.start) service.ReportRaceStart(*out.start);
  if (out.complete) service.ReportRaceComplete(*out.complete);
}

}  // namespace eck::race_telemetry
