// Frame rate choices, the frame limiter schedule and the wait, plus the settings and hook wiring.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

#include "frame_limiter.h"

using namespace eck;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

std::string ReadSource(const char* name) {
  std::ifstream in(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

LimiterClock::time_point At(double seconds) {
  return LimiterClock::time_point{} +
         std::chrono::duration_cast<LimiterClock::duration>(std::chrono::duration<double>(seconds));
}

double Seconds(LimiterClock::duration d) { return std::chrono::duration<double>(d).count(); }

void TestLocalInstancePacing() {
  std::printf("local instance pacing\n");
  CHECK(!LocalInstancePacing(nullptr, 1));
  CHECK(!LocalInstancePacing("", 1));
  CHECK(!LocalInstancePacing("b", 0));
  CHECK(LocalInstancePacing("a", 1));
  CHECK(LocalInstancePacing("b", 3));

  CHECK(LocalInstanceLimiterRate(kFrameRateUnlimited) == 60);
  CHECK(LocalInstanceLimiterRate(240) == 60);
  CHECK(LocalInstanceLimiterRate(60) == 60);
  CHECK(LocalInstanceLimiterRate(30) == 30);
  CHECK(LimiterRate(kFrameRateUnlimited) == kUnlimitedFrameRate);
  CHECK(LimiterRate(240) == 240);

  CHECK(LocalInstancePacingLine(true, kFrameRateUnlimited) ==
        "[DEBUG_SAULO][ONLINE-PACING] local_multi_instance=yes requested=unlimited effective=60");
  CHECK(LocalInstancePacingLine(true, 60) == "[DEBUG_SAULO][ONLINE-PACING] local_multi_instance=yes requested=60 effective=60");
  CHECK(LocalInstancePacingLine(false, kFrameRateUnlimited) ==
        "[DEBUG_SAULO][ONLINE-PACING] local_multi_instance=no requested=unlimited effective=unlimited");
}

void TestChoices() {
  std::printf("frame rate choices\n");
  const int expected[] = {30, 60, 90, 120, 144, 165, 240, 0};
  CHECK(std::size(kFrameRates) == std::size(expected));
  for (size_t i = 0; i < std::size(expected); ++i) {
    CHECK(kFrameRates[i] == expected[i]);
    CHECK(IsFrameRateChoice(expected[i]));
    CHECK(FrameRateOrDefault(expected[i]) == expected[i]);
  }
  CHECK(kFrameRateDefault == 60 && kFrameRateOriginal == 30 && kFrameRateUnlimited == 0);
  for (const int other : {-1, 1, 45, 59, 61, 100, 200, 360, 1000}) {
    CHECK(!IsFrameRateChoice(other));
    CHECK(FrameRateOrDefault(other) == kFrameRateDefault);
  }
  const auto choices = FrameRateChoices();
  CHECK(choices.size() == std::size(expected));
  CHECK(choices[0] == std::make_pair(std::string("30"), std::string("30 Original")));
  CHECK(choices[1] == std::make_pair(std::string("60"), std::string("60")));
  CHECK(choices[4] == std::make_pair(std::string("144"), std::string("144")));
  CHECK(choices[7] == std::make_pair(std::string("0"), std::string("Unlimited")));
  CHECK(FrameRateFromText("30") == 30 && FrameRateFromText("144") == 144 && FrameRateFromText("0") == 0);
  for (const char* bad : {"", " ", "abc", "-1", "61", "30x", "1.5", "99999999999999999999", "0x1E"}) {
    CHECK(FrameRateFromText(bad) == kFrameRateDefault);
  }
  for (const int rate : {30, 60, 90, 120, 144, 165, 240}) {
    CHECK(LimiterRate(rate) == rate);
  }
  CHECK(LimiterRate(kFrameRateUnlimited) == kUnlimitedFrameRate);
  CHECK(kUnlimitedFrameRate == 300);
  CHECK(std::fabs(Seconds(FramePeriod(kUnlimitedFrameRate)) - 1.0 / 300.0) < 1e-8);
  CHECK(FramePeriod(kUnlimitedFrameRate) > std::chrono::microseconds(3300));
}

void TestLimiterSchedule() {
  std::printf("limiter schedule\n");
  CHECK(FramePeriod(0) == LimiterClock::duration::zero());
  for (const int rate : {30, 60, 90, 120, 144, 165, 240}) {
    CHECK(std::fabs(Seconds(FramePeriod(rate)) - 1.0 / rate) < 1e-8);
  }
  FrameLimiter limiter;
  const double period = 1.0 / 60.0;
  CHECK(!limiter.Next(60, At(10.0)));
  const auto deadline = limiter.Next(60, At(10.0 + 0.2 * period));
  CHECK(deadline && std::fabs(Seconds(*deadline - At(10.0)) - period) < 1e-8);
  double now = 10.0 + period;
  double target = 10.0 + 2 * period;
  for (int i = 0; i < 1000; ++i) {
    const auto wait = limiter.Next(60, At(now));
    CHECK(wait && std::fabs(Seconds(*wait - At(target))) < 1e-5);
    now = target;
    target += period;
  }
}

void TestLateFramesAndRateChange() {
  std::printf("late frames and rate changes\n");
  FrameLimiter limiter;
  const double period = 1.0 / 30.0;
  limiter.Next(30, At(5.0));
  CHECK(!limiter.Next(30, At(5.0 + 3 * period)));
  const auto after = limiter.Next(30, At(5.0 + 3 * period + 0.1 * period));
  CHECK(after && Seconds(*after - At(5.0 + 3 * period)) <= period + 1e-8);
  CHECK(!limiter.Next(30, At(8.0)));
  const auto resumed = limiter.Next(30, At(8.0 + 0.1 * period));
  CHECK(resumed && Seconds(*resumed - At(8.0)) <= 2 * period);
  CHECK(!limiter.Next(60, At(9.0)));
  CHECK(limiter.Next(60, At(9.0 + 0.1 * period)).has_value());
  CHECK(!limiter.Next(kFrameRateUnlimited, At(9.5)));
  CHECK(!limiter.Next(kFrameRateUnlimited, At(9.5001)));
  CHECK(!limiter.Next(144, At(10.0)));
}

void TestUnlimitedStillHoldsTheFloor() {
  std::printf("unlimited floor\n");
  FrameLimiter limiter;
  const int rate = LimiterRate(kFrameRateUnlimited);
  limiter.Next(rate, At(1.0));
  const auto deadline = limiter.Next(rate, At(1.0 + 0.0001));
  CHECK(deadline && Seconds(*deadline - At(1.0)) >= 0.0033);
}

void TestWait() {
  std::printf("high-resolution wait\n");
  for (const int rate : {30, 60, 144, 240}) {
    const auto start = LimiterClock::now();
    const auto deadline = start + FramePeriod(rate);
    WaitUntil(deadline);
    const auto end = LimiterClock::now();
    CHECK(end >= deadline);
    CHECK(end - deadline < 100ms);
  }
  const auto before = LimiterClock::now();
  WaitUntil(before - 1s);
  CHECK(LimiterClock::now() - before < 5ms);
  const auto start = LimiterClock::now();
  FrameLimiter limiter;
  int frames = 0;
  while (LimiterClock::now() - start < 500ms) {
    if (const auto deadline = limiter.Next(60, LimiterClock::now())) {
      WaitUntil(*deadline);
    }
    ++frames;
  }
  CHECK(frames >= 27 && frames <= 33);
}

void TestWiring() {
  std::printf("frame rate wiring\n");
  const std::string limiter = ReadSource("frame_limiter.cpp");
  CHECK(Has(limiter, "REX_HOOK_RAW(sub_82B583B0)"));
  CHECK(Has(limiter, "ctx.r3.u32 &= ~eck::kPresentIntervalMask"));
  CHECK(Has(limiter, "REX_HOOK_RAW(sub_825A4D70)"));
  CHECK(Has(limiter, "REXCVAR_DEFINE_INT32(eck_frame_rate"));
  CHECK(kPresentIntervalMask == 0xF00);

  const std::string host = ReadSource("window_host.cpp");
  CHECK(Has(host, "REX_HOOK_RAW(__imp__VdSwap)"));
  CHECK(Has(host, "eck::PaceGuestFrame()"));

  const std::string settings = ReadSource("settings.cpp");
  CHECK(Has(settings, "\"eck_frame_rate\","));
  CHECK(!Has(settings, "eck_frame_pacing"));

  const std::string launcher = ReadSource("launcher.cpp");
  CHECK(Has(launcher, "\"eck_frame_rate\", \"60\", frame_rates"));
}

}  // namespace

int main() {
  TestChoices();
  TestLocalInstancePacing();
  TestLimiterSchedule();
  TestLateFramesAndRateChange();
  TestUnlimitedStillHoldsTheFloor();
  TestWait();
  TestWiring();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
