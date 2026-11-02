#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "achievement_list.h"
#include "overlay_rules.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

// Stands in for the SDK's AchievementManager: one catalog and one unlock set, shared by every reader.
class FakeManager {
 public:
  explicit FakeManager(std::vector<rex::system::AchievementInfo> catalog) : catalog_(std::move(catalog)) {}
  std::vector<rex::system::AchievementInfo> ListAchievements() const { return catalog_; }
  bool IsUnlocked(uint32_t id) const { return unlocked_.count(id) != 0; }
  void Unlock(uint32_t id) { unlocked_.insert(id); }

 private:
  std::vector<rex::system::AchievementInfo> catalog_;
  std::set<uint32_t> unlocked_;
};

FakeManager ShippedCatalog() {
  std::vector<rex::system::AchievementInfo> catalog;
  for (uint32_t id = 46; id >= 1; --id) {
    rex::system::AchievementInfo info;
    info.id = id;
    info.label = "Achievement " + std::to_string(id);
    info.description = "Earned " + std::to_string(id);
    info.unachieved_description = "Locked " + std::to_string(id);
    info.gamerscore = id <= 44 ? 20 : 60;
    catalog.push_back(std::move(info));
  }
  return FakeManager(std::move(catalog));
}

void TestList() {
  FakeManager manager = ShippedCatalog();
  auto entries = eck::ListAchievements(manager);
  Check(entries.size() == 46, "all 46 achievements are listed");
  Check(entries.front().info.id == 1 && entries.back().info.id == 46, "listed in ID order");
  eck::AchievementSummary locked = eck::Summarize(entries);
  Check(locked.total == 46 && locked.total_gamerscore == 1000, "46 achievements, 1000 gamerscore");
  Check(locked.unlocked == 0 && locked.earned_gamerscore == 0, "a clean profile starts locked");
  Check(eck::EntryDescription(entries[0]) == "Locked 1", "locked entries show the locked description");

  manager.Unlock(1);
  manager.Unlock(46);
  auto after = eck::ListAchievements(manager);
  eck::AchievementSummary summary = eck::Summarize(after);
  Check(summary.unlocked == 2 && summary.earned_gamerscore == 80, "unlock state and earned score follow the store");
  Check(eck::EntryDescription(after[0]) == "Earned 1", "unlocked entries show the earned description");
  Check(eck::Summarize(eck::ListAchievements(manager)).unlocked == summary.unlocked,
        "a second reader of the same store sees the same state");
}

void TestWindowSize() {
  Check(eck::FitWindowExtent(1280.0f, 420.0f, 560.0f) == 560.0f, "1280 wide is capped at the maximum");
  Check(eck::FitWindowExtent(720.0f, 320.0f, 460.0f) == 460.0f, "720 high is capped at the maximum");
  Check(eck::FitWindowExtent(720.0f, 320.0f, 460.0f) < 720.0f * 0.7f + 1.0f, "never above about 70% of the viewport");
  Check(eck::FitWindowExtent(300.0f, 420.0f, 560.0f) == 300.0f - 2.0f * eck::kOverlayViewportMargin,
        "a small viewport keeps the margin");
}

void TestCloseGate() {
  eck::CloseGate gate;
  Check(!gate.Step(true, 0.0), "no close requested: the dialog stays");
  gate.Request();
  Check(gate.requested(), "a request is remembered");
  Check(!gate.Step(true, 1.0), "B still held: the game stays blocked");
  Check(!gate.Step(true, 1.5), "still held within the timeout");
  Check(gate.Step(false, 1.6), "released: the game gets its input back");

  eck::CloseGate stuck;
  stuck.Request();
  Check(!stuck.Step(true, 10.0), "the wait starts at the first step");
  Check(stuck.Step(true, 10.0 + eck::kReleaseTimeoutSeconds), "a button never reported released times out");
}

void TestScroll() {
  Check(eck::ScrollDirection(true, false, 0.0f) == -1.0f, "D-pad up scrolls up");
  Check(eck::ScrollDirection(false, true, 0.0f) == 1.0f, "D-pad down scrolls down");
  Check(eck::ScrollDirection(true, false, 1.0f) == -1.0f, "the D-pad wins over the stick");
  Check(eck::ScrollDirection(false, false, 0.1f) == 0.0f, "the stick has a dead zone");
  Check(eck::ScrollDirection(false, false, -1.0f) == 1.0f, "stick down scrolls down");
}

}  // namespace

int main() {
  TestList();
  TestWindowSize();
  TestCloseGate();
  TestScroll();
  if (g_failures) {
    std::printf("%d achievement check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("achievement checks passed\n");
  return 0;
}
