#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "graphics_adapters.h"
#include "online_test_support.h"

using eck::GraphicsAdapter;
using eck::GraphicsAdapterOptions;
using eck::GraphicsAdapterSettingIsStale;
using eck::ResolveGraphicsAdapter;
using online_test::Check;

namespace {

const std::vector<GraphicsAdapter> kHybrid = {{0, "Intel UHD Graphics"}, {1, "NVIDIA GeForce RTX 4060"}};
const std::vector<GraphicsAdapter> kSingle = {{0, "NVIDIA GeForce RTX 4070"}};

std::string Source(const std::string& name) {
  std::ifstream file(std::string(ECK_SRC_DIR) + "/" + name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

void TestOptionsAreRealCards() {
  const auto single = GraphicsAdapterOptions(kSingle);
  Check(single.size() == 1, "one GPU, one option");
  Check(single[0].first == "0" && single[0].second == "NVIDIA GeForce RTX 4070", "the option is the card's name");

  const auto hybrid = GraphicsAdapterOptions(kHybrid);
  Check(hybrid.size() == 2, "every card is listed, nothing else");
  Check(hybrid[0].second == "Intel UHD Graphics" && hybrid[1].second == "NVIDIA GeForce RTX 4060", "in list order");
  Check(GraphicsAdapterOptions({}).empty(), "no cards, no options");

  for (const auto& [value, label] : hybrid) {
    Check(value != "auto" && value != "-1", "no sentinel value is offered");
    Check(!Contains(label, "Automatic") && !Contains(label, "Auto"), "no Automatic label is offered");
  }

  const std::vector<GraphicsAdapter> twins = {{0, "Radeon"}, {1, "Radeon"}};
  const auto same = GraphicsAdapterOptions(twins);
  Check(same[0].second == "Radeon (0)" && same[1].second == "Radeon (1)", "identical cards stay distinguishable");
}

void TestSavedChoice() {
  Check(ResolveGraphicsAdapter("0", kHybrid) == 0, "a saved first card stays selected");
  Check(ResolveGraphicsAdapter("1", kHybrid) == 1, "a saved second card stays selected");

  const std::vector<GraphicsAdapter> gap = {{0, "Intel UHD Graphics"}, {2, "NVIDIA GeForce RTX 4060"}};
  Check(ResolveGraphicsAdapter("2", gap) == 2, "the value is the DXGI index, not the list position");
  Check(ResolveGraphicsAdapter("1", gap) == 0, "and a skipped index is not a card");
}

void TestFallbackToFirstCard() {
  Check(ResolveGraphicsAdapter("", kHybrid) == 0, "the default selects the first card");
  Check(ResolveGraphicsAdapter("7", kHybrid) == 0, "a card that is gone falls back to the first");
  for (const std::string_view old : {"auto", "-1", "Automatic", "1x"}) {
    Check(ResolveGraphicsAdapter(old, kHybrid) == 0, "an old value selects the first card: " + std::string(old));
  }
  Check(ResolveGraphicsAdapter("7", kSingle) == 0, "with one GPU that GPU is used");
  Check(!ResolveGraphicsAdapter("0", {}), "no cards resolves to nothing");
}

void TestStaleValueIsReplacedOnce() {
  for (const std::string_view old : {"7", "auto", "-1", "Automatic"}) {
    Check(GraphicsAdapterSettingIsStale(old, kHybrid), "replaced in the file: " + std::string(old));
  }
  Check(!GraphicsAdapterSettingIsStale("1", kHybrid), "a valid choice is left alone");
  Check(!GraphicsAdapterSettingIsStale("", kHybrid), "the default is not written out");
  Check(!GraphicsAdapterSettingIsStale("7", {}), "with no cards the saved choice is kept");

  const std::string settings = Source("settings.cpp");
  Check(Contains(settings, "SaveNow({{key, TomlValue(key, saved_[key])}})"), "the corrected value reaches settings.toml");
  const size_t capture = settings.find("void GameSettings::Capture");
  const size_t reset = settings.find("g_graphics_adapter_stale = false;", capture);
  Check(reset != std::string::npos && reset < settings.find("void GameSettings::CaptureLateRegistered"),
        "once, when the settings are captured");
}

void TestChoiceReachesTheRuntime() {
  const std::string settings = Source("settings.cpp");
  const size_t apply = settings.find("void ApplyGraphicsAdapter()");
  Check(apply != std::string::npos, "the choice is applied at start");
  Check(settings.find("rex::cvar::SetFlagByName(\"d3d12_adapter\", std::to_string(*index));", apply) !=
            std::string::npos,
        "the resolved index reaches d3d12_adapter");
  Check(Contains(Source("el_chavo_kart_app.h"), "eck::ApplyGraphicsAdapter();"), "the app applies it");
  Check(Contains(settings, "\"eck_graphics_adapter\",\n") || Contains(settings, "\"eck_graphics_adapter\",\r\n"),
        "the choice is saved with the window settings");
}

void TestNoAutomaticIsLeft() {
  const std::string settings = Source("settings.cpp");
  Check(Contains(settings, "REXCVAR_DEFINE_STRING(eck_graphics_adapter, \"\","), "the default is empty, not a sentinel");
  Check(!Contains(Source("graphics_adapters.h"), "\"auto\""), "the header knows no auto value");
  Check(!Contains(settings, "Automatic"), "settings know no Automatic label");

  const std::string launcher = Source("launcher.cpp");
  Check(Contains(launcher, "GraphicsAdapterOptions(graphics_adapters_)"), "the Graphics page lists the cards");
  Check(Contains(launcher, "\"Graphics card\""), "the row keeps its name");
  Check(Contains(launcher, "If the selected GPU is no longer available, the first available"), "the help says so");
  Check(!Contains(launcher, "Tr(\"Automatic\")") && !Contains(launcher, "automatic_adapter_"), "no Automatic row");
  Check(!Contains(Source("pt_br_ui_text.cpp"), "Automático usa a GPU"), "the translation follows");

  const std::string diagnostics = Source("diagnostics.cpp");
  Check(Contains(diagnostics, "\"Runtime GPU: \""), "the log names the GPU");
  Check(!Contains(diagnostics, "selected=") && !Contains(diagnostics, "resolved="), "without a selected/resolved pair");
}

void TestPreferenceExportsRemain() {
  const std::string platform = Source("platform.cpp");
  Check(Contains(platform, "__declspec(dllexport) DWORD NvOptimusEnablement = 1;"), "the NVIDIA export is there");
  Check(Contains(platform, "__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;"),
        "the AMD export is there");
  Check(Contains(Source("../CMakeLists.txt"), "src/platform.cpp"), "and is linked into the game");
}

}  // namespace

int main() {
  TestOptionsAreRealCards();
  TestSavedChoice();
  TestFallbackToFirstCard();
  TestStaleValueIsReplacedOnce();
  TestChoiceReachesTheRuntime();
  TestNoAutomaticIsLeft();
  TestPreferenceExportsRemain();
  return online_test::Finish();
}
