// The open-file crash, and the character screen as the lobby. Screen is a small model of KartScreen_PlayerSelectManager and
// its panels, so remote seats are proven against the same rules the game applies to a pad.
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "guest_region.h"
#include "input_focus.h"
#include "online_character_select.h"
#include "online_lobby.h"
#include "online_text.h"
#include "online_menu.h"
#include "online_panel_bridge.h"
#include "online_race_telemetry.h"
#include "online_race_url.h"
#include "online_setup_capture.h"
#include "online_test_support.h"
#include "open_file_policy.h"

using online_test::Check;
namespace lobby = eck::lobby;
namespace menu = eck::menu;
namespace online = eck::online;
namespace open_file = eck::open_file;
using lobby::PickState;

namespace {

constexpr uint32_t kPage = 0x10000;
constexpr menu::PanelObjects kPanelObjects = {0x41000000, 0x41001000, 0x41002000, 0x41003000};
constexpr uint32_t kMovieSize = 176575;

// Guest memory as the heap reports it, and a count of every read past what is mapped.
class FakeMemory {
 public:
  void Map(uint32_t address, uint32_t size) {
    for (uint32_t page = address & ~(kPage - 1); page < address + size; page += kPage) pages_[page].resize(kPage);
  }
  void Put(uint32_t address, uint32_t value) {
    uint8_t* at = &pages_.at(address & ~(kPage - 1))[address & (kPage - 1)];
    for (int i = 0; i < 4; ++i) at[i] = uint8_t(value >> (24 - 8 * i));
  }

  void PutByte(uint32_t address, uint8_t value) { pages_.at(address & ~(kPage - 1))[address & (kPage - 1)] = value; }
  uint32_t U8(uint32_t address) const {
    const auto page = pages_.find(address & ~(kPage - 1));
    if (page == pages_.end()) {
      ++faults;
      return 0;
    }
    return page->second[address & (kPage - 1)];
  }

  uint32_t RegionSize(uint32_t address) const {
    uint32_t size = 0;
    for (uint32_t page = address & ~(kPage - 1); pages_.count(page) != 0; page += kPage) size += kPage;
    return size;
  }
  bool Readable(uint32_t address, uint32_t length) const {
    if (address < 0x10000 || address + length < address) return false;
    const uint32_t region = RegionSize(address);
    return region != 0 && eck::GuestRangeInRegion(address, length, kPage, region);
  }
  uint32_t U32(uint32_t address) const {
    const uint32_t page = address & ~(kPage - 1);
    const uint32_t offset = address & (kPage - 1);
    if (pages_.count(page) == 0 || offset + 4 > kPage) {
      ++faults;
      return 0;
    }
    const uint8_t* at = &pages_.at(page)[offset];
    return (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | at[3];
  }

  mutable int faults = 0;

 private:
  std::map<uint32_t, std::vector<uint8_t>> pages_;
};

void TestRegionBounds() {
  // The crash: 0x7040FFxx passed the old check, and 0x7040FF80 + 0x100 runs into the unmapped 0x70410000.
  const uint32_t object = 0x7040FF80;
  const uint32_t region = kPage;
  Check(uint64_t(object) + 0x100 <= uint64_t(object) + region, "the sum of base_address and region_size accepted the scan");
  Check(!eck::GuestRangeInRegion(object, 0x100, kPage, region), "the page-based end refuses it");
  Check(eck::GuestRegionEnd(object, kPage, region) == 0x70410000, "the region ends at the page boundary");
  Check(eck::GuestRangeInRegion(object, 0x80, kPage, region), "the same object up to the last byte is fine");
  Check(!eck::GuestRangeInRegion(object, 0x81, kPage, region), "one byte more is not");
  Check(eck::GuestRangeInRegion(0x82000123, 16, 0x1000, 0x1000), "4 KiB pages");
  Check(!eck::GuestRangeInRegion(0x82000FF8, 16, 0x1000, 0x1000), "4 KiB pages end at their boundary too");
  Check(eck::GuestRangeInRegion(0x7040FFF0, 0x20, kPage, 2 * kPage), "a scan across two mapped pages of a region is fine");
}

void TestInspectionByPackage() {
  using open_file::Inspection;
  using open_file::InspectionOf;
  Check(InspectionOf("/ package/ChavoKart_MainMenu/ChavoKartMainMenu") == Inspection::kPatchMainMenu, "the main menu is searched");
  Check(InspectionOf("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager") == Inspection::kTrackScreen,
        "the character screen is only noted");
  Check(InspectionOf("/ package/ChavoKart_CompetitionSelect/ChavoKartCompetitionSelect") == Inspection::kTrackScreen,
        "CompetitionSelect is only noted");
  Check(InspectionOf("/ package/ChavoKart_TrackSelect/ChavoKartTrackSelect") == Inspection::kTrackScreen, "TrackSelect is only noted");
  Check(InspectionOf("/ package/ChavoKart_Popup/ChavoPopup") == Inspection::kPass, "ChavoPopup passes straight through");
  Check(InspectionOf("/ package/ChavoKart_PlayerSelect/ChavoKartPlayerSelect") == Inspection::kPass, "panel movies pass through");
  Check(InspectionOf("/ package/ChavoKart_LoadScreens/ChavoKartLoading") == Inspection::kPass, "unknown packages pass through");
  Check(InspectionOf("") == Inspection::kPass, "an unreadable URL passes through");
  Check(InspectionOf("garbage??") == Inspection::kPass, "a malformed URL passes through");
  Check(std::string(open_file::InspectionName(Inspection::kPatchMainMenu)) == "patch_main_menu" &&
            std::string(open_file::InspectionName(Inspection::kTrackScreen)) == "track_screen" &&
            std::string(open_file::InspectionName(Inspection::kPass)) == "pass",
        "every operation has a log name");
}

void TestMovieSearch() {
  FakeMemory memory;
  memory.Map(0x00CD0000, kPage);
  memory.Map(0x40000000, kPage);
  memory.Map(0x70400000, kPage);  // nothing mapped after it, as in the crash

  // A real movie, one level down.
  const uint32_t root = 0x00CD4110;
  const uint32_t holder = 0x40000100;
  const uint32_t bytes = 0x40000800;
  memory.Put(root + 0x10, holder);
  memory.Put(holder + 0x08, bytes);
  memory.Put(holder + 0x0C, kMovieSize);
  memory.Put(bytes, 0x47465800);
  auto fields = open_file::FindMovieFieldsInTree(memory, root, kMovieSize);
  Check(fields && fields->pointer == holder + 0x08 && fields->size == holder + 0x0C, "the main menu's movie is found");
  Check(memory.faults == 0, "without a read outside the mapped pages");

  // The crash: a pointer in the object tree sits less than a scan away from the end of the last mapped page.
  const uint32_t popup = 0x00CD8000;
  memory.Put(popup + 0x20, 0x7040FF80);
  memory.Put(0x7040FF80, 0x7040FFF0);
  Check(!open_file::FindMovieFieldsInTree(memory, popup, kMovieSize), "no movie there");
  Check(memory.faults == 0, "and no read of 0x70410000");

  memory.Put(popup + 0x24, 0x70500000);  // unmapped
  memory.Put(popup + 0x28, 0x7040FFFC);  // the last word of the page
  memory.Put(popup + 0x2C, 0x7FFFFFFC);
  Check(!open_file::FindMovieFieldsInTree(memory, popup, kMovieSize), "unmapped and edge pointers find nothing");
  Check(memory.faults == 0, "and read nothing they should not");

  Check(!open_file::FindMovieFieldsInTree(memory, 0, kMovieSize), "a null file object is refused");
  Check(!open_file::FindMovieFieldsInTree(memory, 0x70410000, kMovieSize), "an unmapped file object is refused");
  Check(!open_file::FindMovieFieldsInTree(memory, 0xFFFFFFF0, kMovieSize), "a malformed file object is refused");
  Check(!open_file::FindMovieFieldsInTree(memory, 0x7040FFF0, kMovieSize), "a file object at the end of a page is refused");
  Check(memory.faults == 0, "none of them was read");

  // A size word next to a pointer that leads nowhere is not a movie.
  const uint32_t decoy = 0x40001000;
  memory.Put(decoy + 0x08, 0x70410000);
  memory.Put(decoy + 0x0C, kMovieSize);
  Check(!open_file::FindMovieFieldsInTree(memory, decoy, kMovieSize), "a bad data pointer is not followed");
  Check(memory.faults == 0, "and is not read");

  Check(!open_file::LooksLikeHeapObject(0x7FFFFFFE) && !open_file::LooksLikeHeapObject(0x80000000) &&
            !open_file::LooksLikeHeapObject(0x3FFFFFFC) && open_file::LooksLikeHeapObject(0x40000000),
        "only aligned heap addresses are followed");
}

void TestTrackerAcrossReopenedMovies() {
  constexpr uint32_t kManager = 0x1000, kPanel = 0x2000, kObject = 0x7000;
  menu::ScreenTracker screens;
  screens.NoteMovie("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager");
  screens.NoteCall(kManager, kObject, "ShowScreen");
  Check(screens.current() == menu::GameScreen::kCharacter, "the screen is known");
  Check(screens.panel_objects()[0] == 0, "the manager is no panel");
  for (uint32_t panel = 0; panel < 4; ++panel) screens.NoteCall(kPanel, 0x8000 + panel, "ShowScreen");
  Check(screens.panel_objects() == menu::PanelObjects{0x8000, 0x8001, 0x8002, 0x8003}, "the panels, in the order they showed");
  screens.NoteCall(kPanel, 0x8001, "ShowScreen");
  screens.NoteCall(kManager, kObject, "ShowScreen");
  Check(screens.panel_objects() == menu::PanelObjects{0x8000, 0x8001, 0x8002, 0x8003}, "showing again changes nothing");

  screens.NoteMapChange();
  Check(screens.current() == menu::GameScreen::kOther && screens.panel_objects()[0] == 0, "a map change forgets the screen");

  // The map reloads its movies, while the classes stay.
  screens.NoteMovie("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager");
  screens.NoteCall(kManager, kObject + 4, "ShowScreen");
  for (int panel = 0; panel < 4; ++panel) screens.NoteCall(kPanel, 0x8000 + panel, "ShowScreen");
  Check(screens.current() == menu::GameScreen::kCharacter, "still the character screen after panels show");
  screens.NoteCall(kManager, kObject + 4, "OnPressXButton");
  Check(screens.TakeInvitePress() && !screens.TakeInvitePress(), "X still arrives, once");
  screens.NoteCall(kManager, kObject + 4, "HideScreen");
  Check(screens.panel_objects()[0] == 0, "hidden panels are not read any more");

  // If the panels show first, the screen is still the same screen.
  menu::ScreenTracker early;
  early.NoteMovie("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager");
  for (uint32_t panel = 0; panel < 4; ++panel) early.NoteCall(kPanel, 0x8000 + panel, "ShowScreen");
  early.NoteCall(kManager, kObject, "ShowScreen");
  Check(early.current() == menu::GameScreen::kCharacter && early.panel_objects()[3] == 0x8003, "the order does not matter");
  early.NoteCall(kManager, kObject, "OnPressXButton");
  Check(early.TakeInvitePress(), "and X from the manager still counts");
}

// A panel as the game built it, with its real base `pad_at`, which the layout header does not know.
void PutPanels(FakeMemory& memory, uint32_t pad_at, uint32_t game) {
  namespace layout = eck::online_probe::layout;
  const uint32_t shift = pad_at - layout::kPanelPad;
  memory.Map(kPanelObjects[0], 0x8000);
  memory.Map(0x42000000, kPage);
  memory.Put(0x42000000 + layout::kObjectClass, 0x900);  // the manager
  for (uint32_t pad = 0; pad < 4; ++pad) {
    const uint32_t panel = kPanelObjects[pad];
    memory.Put(panel + layout::kObjectClass, 0x800);
    memory.Put(panel + pad_at, pad);
    memory.Put(panel + pad_at + 4, game);
    memory.Put(panel + layout::kPanelParent + shift, 0x42000000);
    memory.Put(panel + layout::kPanelCharacter + shift, pad + 2);
  }
  memory.PutByte(kPanelObjects[0] + layout::kPanelState + shift, 2);
}

void TestPanelScan() {
  namespace layout = eck::online_probe::layout;
  constexpr uint32_t kGame = 0x43000000;

  // The header's distances hold, wherever the TMaps put the base.
  for (const uint32_t pad_at : {layout::kPanelPad, layout::kPanelPad + 0x30, layout::kPanelPad + 0x90, layout::kPanelPad + 0xB4}) {
    FakeMemory memory;
    PutPanels(memory, pad_at, kGame);
    const lobby::PanelScan scan = lobby::ScanPanels(memory, kPanelObjects, kGame);
    Check(scan.fields && scan.fields->pad == pad_at, "the panel base is found");
    Check(scan.manager == 0x42000000, "the relationship names the manager");
    Check(std::string(scan.failure) == "none" && scan.readable, "and nothing failed");
    if (!scan.fields) continue;
    const auto panels = lobby::ReadPanels(memory, kPanelObjects, *scan.fields);
    Check(panels && (*panels)[0].state == PickState::kConfirmed && (*panels)[0].character == 2, "panel 0 is read");
    Check(panels && (*panels)[3].state == PickState::kNone && (*panels)[3].character == 5, "panel 3 is read");
    Check(memory.faults == 0, "without a read outside the panels");
  }

  FakeMemory order;
  PutPanels(order, 0x448, kGame);
  order.Put(kPanelObjects[0] + 0x448, 2);
  order.Put(kPanelObjects[2] + 0x448, 0);
  const lobby::PanelScan swapped = lobby::ScanPanels(order, kPanelObjects, kGame);
  const auto ordered = swapped.fields ? lobby::ReadPanels(order, kPanelObjects, *swapped.fields) : std::nullopt;
  Check(ordered && (*ordered)[2].state == PickState::kConfirmed, "a panel is indexed by its pad, not by when it showed");

  Check(std::string(lobby::ScanPanels(FakeMemory{}, {}, kGame).failure) == "panels_not_seen", "no panels seen yet");
  Check(std::string(lobby::ScanPanels(FakeMemory{}, {0x41000000, 0, 0, 0}, kGame).failure) == "panels_not_seen", "one is not four");
  FakeMemory empty;
  Check(std::string(lobby::ScanPanels(empty, kPanelObjects, kGame).failure) == "panel_unreadable", "unmapped panels");
  Check(empty.faults == 0, "are not read");
  FakeMemory no_game;
  PutPanels(no_game, 0x448, kGame);
  Check(std::string(lobby::ScanPanels(no_game, kPanelObjects, 0).failure) == "game_unreadable", "no game to check against");

  // The runtime evidence: pad and game at 1100 and 1104, the layout's own numbers.
  Check(layout::kPanelPad == 1100, "ControlledById is where the real game has it");
  FakeMemory real;
  PutPanels(real, 1100, kGame);
  const lobby::PanelScan proven = lobby::ScanPanels(real, kPanelObjects, kGame);
  Check(proven.fields && proven.shift == 0 && proven.manager == 0x42000000, "the scan confirms it with no shift");
  Check(std::string(proven.candidate.failed) == "none" && proven.candidate.pad_at == 1100 && proven.candidate.game_at == 1104,
        "and the log names the candidate");

  // The real game's words decide: a look-alike is refused and the log has what it needs.
  FakeMemory wrong_game;
  PutPanels(wrong_game, 0x448, kGame + 4);
  const lobby::PanelScan missed = lobby::ScanPanels(wrong_game, kPanelObjects, kGame);
  Check(!missed.fields && std::string(missed.failure) == "layout_not_found", "no panel base, no bridge");
  Check(std::find(missed.pad_hits.begin(), missed.pad_hits.end(), 0x448u) != missed.pad_hits.end(), "the log shows where the pad was");

  FakeMemory two_parents;
  PutPanels(two_parents, 0x448, kGame);
  two_parents.Put(kPanelObjects[2] + layout::kPanelParent + (0x448 - layout::kPanelPad), 0x42000100);
  const lobby::PanelScan parents = lobby::ScanPanels(two_parents, kPanelObjects, kGame);
  Check(!parents.fields, "panels of two managers are no screen");
  Check(std::string(parents.candidate.failed) == "screen_parent" && parents.candidate.pad_at == 0x448, "the log says which check failed");

  FakeMemory bad_state;
  PutPanels(bad_state, 0x448, kGame);
  bad_state.PutByte(kPanelObjects[1] + layout::kPanelState + (0x448 - layout::kPanelPad), 3);
  const lobby::PanelScan state_scan = lobby::ScanPanels(bad_state, kPanelObjects, kGame);
  Check(!state_scan.fields && std::string(state_scan.candidate.failed) == "state_or_character", "a state the game does not have is refused");

  FakeMemory edge;
  edge.Map(0x7040F000, 0x1000);
  const menu::PanelObjects at_the_end = {0x7040FE00, 0x7040FE40, 0x7040FE80, 0x7040FEC0};
  for (const uint32_t panel : at_the_end) edge.Put(panel + layout::kObjectClass, 0x800);
  Check(!lobby::ScanPanels(edge, at_the_end, kGame).fields && edge.faults == 0, "a scan never leaves the mapped pages");
}

void CaptureFreeRace(lobby::SetupCapture& setup, int mode, int difficulty) {
  setup.NoteScreen(menu::GameScreen::kMainMenu);
  setup.NoteMovieCall(lobby::kSelectMenu, std::nullopt, lobby::kFreeRaceMenu);
  setup.NoteScreen(menu::GameScreen::kCompetition);
  setup.NoteMovieCall(lobby::kSelectCompetition, mode, "");
  setup.NoteMovieCall(lobby::kSetDifficulty, difficulty, "");
  setup.NoteScreen(menu::GameScreen::kCharacter);
}

void TestSetupCapture() {
  using menu::GameScreen;
  lobby::SetupCapture setup;
  Check(!setup.setup() && std::string(setup.missing()) == "free_race", "nothing before Free Race");
  Check(setup.NoteMovieCall(lobby::kSelectCompetition, 2, "").empty() && !setup.setup(), "no capture outside Free Race");

  Check(setup.NoteMovieCall(lobby::kSelectMenu, std::nullopt, "Exhibition") == "free_race generation=1", "Free Race starts a generation");
  Check(std::string(setup.missing()) == "mode", "Free Race resets the setup");
  Check(setup.NoteScreen(GameScreen::kCompetition).empty(), "CompetitionSelect keeps it");
  Check(setup.NoteMovieCall(lobby::kSelectCompetition, 4, "") == "mode committed value=4", "competition selection captures mode");
  Check(std::string(setup.missing()) == "difficulty", "the difficulty is still to come");
  Check(!setup.setup(), "half a setup is no setup");
  Check(setup.NoteMovieCall(lobby::kSetDifficulty, 2, "") == "difficulty committed value=2", "difficulty commit captures difficulty");
  Check(setup.NoteScreen(GameScreen::kCompetition).empty(), "the popup closing shows CompetitionSelect again");
  Check(setup.setup().has_value(), "cached setup survives from CompetitionSelect");
  Check(setup.NoteScreen(GameScreen::kCharacter) == "ready generation=1 game_type=3 mode=4 difficulty=2", "the character screen logs it once");
  const auto game = setup.setup();
  Check(game && game->mode == 4 && game->difficulty == 2 && game->game_type == lobby::kGameTypeStartMenu, "and has it, complete");
  Check(lobby::SetupOfRules(lobby::SetupRules(*game)).has_value(), "as a payload a guest accepts");
  Check(setup.NoteScreen(GameScreen::kOther).empty() && setup.setup(), "a popup over the screen changes nothing");
  Check(setup.NoteScreen(GameScreen::kTrack).empty() && setup.setup(), "nor does the track screen");

  Check(setup.NoteScreen(GameScreen::kCompetition) == "reset reason=back_to_competition", "backing out invalidates the choices");
  Check(!setup.setup() && std::string(setup.missing()) == "mode", "so a stale setup cannot be reused");
  setup.NoteMovieCall(lobby::kSelectCompetition, 1, "");
  Check(std::string(setup.missing()) == "difficulty", "a new competition asks for the difficulty again");
  setup.NoteMovieCall(lobby::kSetDifficulty, 0, "");
  Check(setup.setup() && setup.setup()->mode == 1 && setup.setup()->difficulty == 0, "the new choice is the setup");
  Check(setup.NoteScreen(GameScreen::kMainMenu) == "reset reason=main_menu" && !setup.setup(), "the main menu resets it");
  Check(setup.NoteScreen(GameScreen::kMainMenu).empty(), "once");

  // Choosing another competition before the character screen.
  lobby::SetupCapture changed;
  changed.NoteMovieCall(lobby::kSelectMenu, std::nullopt, "Exhibition");
  changed.NoteMovieCall(lobby::kSelectCompetition, 1, "");
  changed.NoteMovieCall(lobby::kSetDifficulty, 1, "");
  changed.NoteMovieCall(lobby::kSelectCompetition, 2, "");
  Check(!changed.setup(), "a difficulty does not carry over to another competition");

  for (const char* other : {"Cups", "Challenges"}) {
    lobby::SetupCapture next;
    CaptureFreeRace(next, 3, 1);
    Check(next.setup().has_value(), "a Free Race was set up");
    Check(next.NoteMovieCall(lobby::kSelectMenu, std::nullopt, other) == std::string("reset reason=menu=") + other, "another mode resets it");
    Check(!next.setup() && std::string(next.missing()) == "free_race", "and does not reuse the Free Race setup");
    next.NoteMovieCall(lobby::kSetDifficulty, 1, "");
    Check(next.NoteScreen(GameScreen::kCharacter) == "incomplete generation=1 missing=free_race", "the log says what is missing");
  }

  lobby::SetupCapture again;
  CaptureFreeRace(again, 3, 1);
  CaptureFreeRace(again, 5, 2);
  Check(again.generation() == 2 && again.setup()->mode == 5, "each Free Race is a new generation");

  lobby::SetupCapture bad;
  bad.NoteMovieCall(lobby::kSelectMenu, std::nullopt, "Exhibition");
  Check(bad.NoteMovieCall(lobby::kSelectCompetition, 7, "") == "mode rejected", "podium is no competition");
  Check(bad.NoteMovieCall(lobby::kSelectCompetition, std::nullopt, "") == "mode rejected", "a call without a number commits nothing");
  bad.NoteMovieCall(lobby::kSelectCompetition, 0, "");
  Check(bad.NoteMovieCall(lobby::kSetDifficulty, 3, "") == "difficulty rejected", "difficulty stops at hard");
  Check(bad.NoteMovieCall(lobby::kSetDifficulty, -1, "") == "difficulty rejected", "and at easy");
  Check(!bad.setup(), "nothing malformed is cached");
  Check(bad.NoteMovieCall("RequestTextFieldUpdate", 1, "").empty() && !lobby::IsSetupCall("RequestTextFieldUpdate"), "other calls are ignored");
}

void PutGfxValue(FakeMemory& memory, uint32_t at, uint32_t type, uint32_t high, uint32_t low = 0) {
  memory.Put(at + lobby::kGfxValueType, type);
  memory.Put(at + lobby::kGfxValueData, high);
  memory.Put(at + lobby::kGfxValueData + 4, low);
}

void PutText(FakeMemory& memory, uint32_t at, const char* text) {
  for (size_t i = 0; i <= std::strlen(text); ++i) memory.PutByte(at + uint32_t(i), uint8_t(text[i]));
}

void TestMovieArguments() {
  constexpr uint32_t kArgs = 0x45000000, kNode = 0x45000100, kText = 0x45000200;
  FakeMemory memory;
  memory.Map(kArgs, kPage);

  PutGfxValue(memory, kArgs, lobby::kGfxInt, 4);
  auto argument = lobby::FirstMovieArgument(memory, kArgs, 1);
  Check(argument.number == 4, "an int argument");
  PutGfxValue(memory, kArgs, lobby::kGfxUInt, 2);
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).number == 2, "a uint argument");
  PutGfxValue(memory, kArgs, lobby::kGfxNumber, 0x40180000);  // 6.0
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).number == 6, "an ActionScript number");
  PutGfxValue(memory, kArgs, lobby::kGfxNumber, 0x3FF80000);  // 1.5
  Check(!lobby::FirstMovieArgument(memory, kArgs, 1).number, "a fraction is no id");
  PutGfxValue(memory, kArgs, lobby::kGfxNumber, 0x7FF80000);  // NaN
  Check(!lobby::FirstMovieArgument(memory, kArgs, 1).number, "NaN is no id");

  PutText(memory, kText, "Exhibition");
  memory.Put(kNode, kText);
  PutGfxValue(memory, kArgs, lobby::kGfxString | lobby::kGfxManagedBit, kNode);
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).text == "Exhibition", "a managed string, as the conversion writes it");
  PutGfxValue(memory, kArgs, lobby::kGfxString, kText);
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).text == "Exhibition", "a plain string");

  PutGfxValue(memory, kArgs, lobby::kGfxString | lobby::kGfxManagedBit, 0x70410000);
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).text.empty(), "an unmapped string node is not followed");
  memory.Put(kNode, 0x70410000);
  PutGfxValue(memory, kArgs, lobby::kGfxString | lobby::kGfxManagedBit, kNode);
  Check(lobby::FirstMovieArgument(memory, kArgs, 1).text.empty(), "nor is an unmapped text");
  PutGfxValue(memory, kArgs, 2, 1);
  const auto boolean = lobby::FirstMovieArgument(memory, kArgs, 1);
  Check(!boolean.number && boolean.text.empty(), "another type is nothing");
  Check(!lobby::FirstMovieArgument(memory, kArgs, 0).number && !lobby::FirstMovieArgument(memory, 0, 1).number, "no arguments");
  Check(!lobby::FirstMovieArgument(memory, 0x70410000, 1).number, "unmapped arguments");

  PutText(memory, kText, "SelectCompetitionType");
  Check(lobby::MovieText(memory, kText) == lobby::kSelectCompetition, "a method name");
  Check(lobby::MovieText(memory, 0x70410000).empty(), "an unmapped method name");
  Check(lobby::MovieText(memory, 0).empty(), "a null method name");
  memory.Map(0x7040F000, 0x1000);
  PutText(memory, 0x7040FFFC, "Sel");
  memory.PutByte(0x7040FFFF, 'x');
  Check(lobby::MovieText(memory, 0x7040FFFC).empty(), "a name running off the mapped pages is refused");
  for (uint32_t i = 0; i < 40; ++i) memory.PutByte(kText + i, 'a');
  Check(lobby::MovieText(memory, kText).empty(), "a name without an end is refused");
  Check(memory.faults == 0, "nothing was read outside the mapped pages");
}

online::Snapshot Room(int seats, int mine);

// The three systems: the screen alone decides the hint, whatever happens to the panels and the setup.
void TestInviteDoesNotDependOnPanels() {
  using menu::ShortcutFor;
  online::Snapshot state;
  state.connection = online::Connection::kOnline;
  menu::ScreenTracker screens;
  screens.NoteMovie("/ package/ChavoKart_PlayerSelectManager/ChavoKartPlayerSelectManager");
  screens.NoteCall(0x1000, 0x7000, "ShowScreen");
  const bool on_screen = screens.current() == menu::GameScreen::kCharacter;
  Check(on_screen, "the character screen is active");
  Check(ShortcutFor(true, on_screen, state).visible, "Invite Player shows with no panel seen");

  FakeMemory nothing;
  Check(!lobby::ScanPanels(nothing, screens.panel_objects(), 0x43000000).fields, "the panel reader fails");
  Check(ShortcutFor(true, on_screen, state).visible, "panel reader failure does not hide Invite Player");
  lobby::SetupCapture nothing_captured;
  Check(!nothing_captured.setup(), "no setup was captured");
  Check(ShortcutFor(true, on_screen, state).visible, "a missing setup does not hide Invite Player");
  Check(menu::OnInvitePress(state, false) == menu::InvitePress::kPickerWithoutSetup, "and X still opens the picker");
  Check(!ShortcutFor(false, on_screen, state).visible, "Online play Off hides it");
  screens.NoteCall(0x1000, 0x7000, "OnPressXButton");
  Check(screens.TakeInvitePress(), "X reaches the picker with no panel seen");

  // The setup comes from the movie calls alone: no panel, no manager and no game object was needed.
  lobby::SetupCapture captured;
  CaptureFreeRace(captured, 2, 1);
  Check(!lobby::ScanPanels(nothing, screens.panel_objects(), 0).fields && captured.setup().has_value(),
        "a failed panel bridge leaves the setup complete");
  Check(menu::OnInvitePress(state, captured.setup().has_value()) == menu::InvitePress::kPicker, "so the picker can send");

  // Remote panels unavailable: the session and the guest's flow go on, and no pad is touched.
  lobby::LobbyFlow flow;
  const online::Snapshot host = Room(2, 0);
  Check(!flow.Update(host, menu::GameScreen::kCharacter, true, false).leave, "a seated guest keeps the session");
  const lobby::Bridge untouched;
  for (uint32_t pad = 0; pad < 4; ++pad) {
    const lobby::PadRule rule = untouched.RuleFor(pad);
    Check(!rule.virtual_pad && rule.allow == 0xFFFF, "without the bridge every pad stays the game's");
  }
}

void TestSetup() {
  const lobby::GameSetup setup{lobby::kGameTypeStartMenu, 4, 2};
  Check(lobby::ValidSetup(setup), "a Free Race setup is valid");
  const auto back = lobby::SetupOfRules(lobby::SetupRules(setup));
  Check(back && back->mode == 4 && back->difficulty == 2 && back->game_type == lobby::kGameTypeStartMenu, "rules round trip");
  Check(lobby::SetupRules(setup).size() <= 16, "within the service's rule limit");

  Check(!lobby::SetupOfRules({}), "a session without a setup has nothing to enter");
  Check(!lobby::SetupOfRules({{"setup", 2}, {"game_type", 3}, {"mode", 1}, {"difficulty", 1}}), "another version is refused");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 3}, {"difficulty", 1}}), "a missing field is refused");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 3}, {"mode", 7}, {"difficulty", 1}}), "podium is no competition");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 3}, {"mode", -1}, {"difficulty", 1}}), "a negative mode is refused");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 3}, {"mode", 1}, {"difficulty", 3}}), "difficulty stops at hard");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 2}, {"mode", 1}, {"difficulty", 1}}), "a cup is not a Free Race");
  Check(!lobby::SetupOfRules({{"setup", 1}, {"game_type", 1}, {"mode", 1}, {"difficulty", 1}}), "a challenge is not a Free Race");
  Check(lobby::ValidSetup({lobby::kGameTypeExhibition, 0, 0}), "back from a race the game type is Exhibition");
}

void TestTravelCommands() {
  const std::string entry = lobby::EntryCommand({lobby::kGameTypeStartMenu, 4, 2}, 1);
  Check(entry.rfind("start StartMenu?", 0) == 0, "the game's default command, to the menu map");
  const auto has = [&](const char* option) { return (entry + "?").find(std::string("?") + option + "?") != std::string::npos; };
  Check(has("GoBackType=2"), "GoBackType=SelectPlayers");
  Check(has("NumGameMode=4") && has("KartAIDiffculty=2") && has("GameType=3"), "the host's game");
  Check(has("PrimaryControllerID=1"), "the local pad stays the primary one");
  Check(entry.find("CharArray") == std::string::npos && entry.find("PlayersArray") == std::string::npos,
        "no character is forced on the guest");
  const std::string home = lobby::MainMenuCommand(0);
  Check(home == "start StartMenu?GoBackType=0?PrimaryControllerID=0", "back to the main menu");
  for (const std::string& command : {entry, home}) {
    const auto url = eck::race_url::ParseRaceUrl(command);
    Check(!url || !eck::race_telemetry::SetupOfUrl(*url), "and neither is taken for a race launch");
  }
  Check(lobby::TravelCommand({}, 0).empty(), "no travel, no command");
}

// KartScreen_PlayerSelectManager: the character skip of DoPlayerSelection, B through the states, and the walk to the track
// screen IsReadyToGo allows.
class Screen {
 public:
  explicit Screen(int primary) : primary_(primary) {
    panels_[size_t(primary)].state = PickState::kSelecting;  // Show() opens the primary pad's panel
  }

  void Pad(int pad, uint16_t buttons) {
    const uint16_t pressed = buttons & uint16_t(~last_[size_t(pad)]);
    last_[size_t(pad)] = buttons;
    lobby::PanelView& panel = panels_[size_t(pad)];
    if (pressed & (lobby::kPadA | lobby::kPadStart)) {
      if (ReadyToGo(pad)) {
        went_on = true;
      } else if (panel.state == PickState::kNone) {
        if (pressed & lobby::kPadStart) {
          panel.state = PickState::kSelecting;
          panel.character = 0;
          if (!SelectionOk(pad, 0)) Move(pad, 1);
        }
      } else if (panel.state == PickState::kSelecting) {
        panel.state = PickState::kConfirmed;
      }
    }
    if (pressed & lobby::kPadB) {
      if (panel.state == PickState::kConfirmed) {
        panel.state = PickState::kSelecting;
      } else if (panel.state == PickState::kSelecting) {
        panel.state = PickState::kNone;
        popped = popped || EveryoneClosed();
      } else if (EveryoneClosed()) {
        popped = true;
      }
    }
    if (panel.state == PickState::kSelecting) {
      if (pressed & lobby::kPadDpadRight) Move(pad, 1);
      if (pressed & lobby::kPadDpadLeft) Move(pad, -1);
    }
  }

  const lobby::Panels& panels() const { return panels_; }
  bool went_on = false;
  bool popped = false;

 private:
  bool SelectionOk(int pad, int character) const {
    for (int other = 0; other < lobby::kPanels; ++other) {
      const lobby::PanelView& view = panels_[size_t(other)];
      if (other != pad && view.state != PickState::kNone && view.character == character) return false;
    }
    return true;
  }
  void Move(int pad, int step) {
    int& character = panels_[size_t(pad)].character;
    do {
      character = (character + step + online::kCharacters) % online::kCharacters;
    } while (!SelectionOk(pad, character));
  }
  bool EveryoneClosed() const {
    return std::all_of(panels_.begin(), panels_.end(), [](const lobby::PanelView& p) { return p.state == PickState::kNone; });
  }
  bool ReadyToGo(int pad) const {
    bool selected = false;
    for (int other = 0; other < lobby::kPanels; ++other) {
      if (panels_[size_t(other)].state == PickState::kSelecting) return false;
      selected = selected || (other == pad && panels_[size_t(other)].state == PickState::kConfirmed);
    }
    return selected;
  }

  int primary_;
  lobby::Panels panels_{};
  std::array<uint16_t, lobby::kPanels> last_{};
};

online::Snapshot Room(int seats, int mine) {
  online::Snapshot state;
  online::RoomView room;
  room.room = "room1";
  room.capacity = 4;
  room.players = seats;
  room.rules = lobby::SetupRules({lobby::kGameTypeStartMenu, 3, 1});
  for (int i = 0; i < 4; ++i) {
    online::Seat seat;
    seat.seat = i;
    seat.state = i < seats ? online::SeatState::kConnected : online::SeatState::kEmpty;
    room.seats.push_back(seat);
  }
  state.room = room;
  state.seat = mine;
  return state;
}

// One PC: its game, its physical pad, and the rules that go between them.
struct Machine {
  explicit Machine(int seat_number, int primary = 0) : seat(seat_number), own_panel(seat_number), physical(primary), screen(primary) {}

  void Tick(double now, const online::Snapshot& state, uint16_t local_buttons) {
    rules = pads.Step(now, lobby::MakePlan(state, true, kPanelObjects), screen.panels(), physical);
    for (int pad = 0; pad < lobby::kPanels; ++pad) {
      const lobby::PadRule& rule = rules[size_t(pad)];
      const int source = rule.source_pad >= 0 ? rule.source_pad : pad;
      const uint16_t real = source == physical ? local_buttons : 0;
      screen.Pad(pad, rule.virtual_pad ? rule.buttons : uint16_t((real & rule.allow) | rule.press));
    }
  }

  int seat;
  int own_panel;
  int physical;
  Screen screen;
  lobby::LobbyPads pads;
  lobby::PadRules rules{};
};

// The service between the PCs: every seat's own panel, as a pick, in every snapshot.
online::Snapshot WithPicks(int seats, const std::vector<Machine*>& machines, int mine) {
  online::Snapshot state = Room(seats, mine);
  for (const Machine* machine : machines) {
    const lobby::PanelView& own = machine->screen.panels()[size_t(machine->own_panel)];
    online::Seat& seat = state.room->seats[size_t(machine->seat)];
    seat.pick = own.state;
    seat.character = own.character;
  }
  return state;
}

// A player's whole turn: walks to `character` and confirms, with the same pulses a thumb would give.
class Thumb {
 public:
  Thumb(int character, bool confirm) : character_(character), confirm_(confirm) {}

  uint16_t Next(int tick, const lobby::PanelView& panel) {
    if (tick % 8 != 0) return 0;
    if (panel.state == PickState::kSelecting && panel.character != character_) return lobby::kPadDpadRight;
    return panel.state == PickState::kSelecting && confirm_ ? lobby::kPadA : 0;
  }

 private:
  int character_;
  bool confirm_;
};

void Run(std::vector<Machine*> machines, const std::vector<int>& characters, int ticks = 1800) {
  std::vector<Thumb> thumbs;
  for (const int character : characters) thumbs.emplace_back(character, true);
  const int seats = int(machines.size());
  for (int tick = 0; tick < ticks; ++tick) {
    const double now = double(tick) / 60.0;
    for (size_t i = 0; i < machines.size(); ++i) {
      Machine& machine = *machines[i];
      const uint16_t press = thumbs[i].Next(tick, machine.screen.panels()[size_t(machine.own_panel)]);
      machine.Tick(now, WithPicks(seats, machines, machine.seat), press);
    }
  }
}

bool Shows(const Machine& machine, int panel, PickState state, int character) {
  const lobby::PanelView& view = machine.screen.panels()[size_t(panel)];
  return view.state == state && view.character == character;
}

void TestTwoPlayers() {
  Machine a(0);
  Machine b(1);
  Run({&a, &b}, {4, 7});

  Check(Shows(a, 0, PickState::kConfirmed, 4), "A keeps A's character on A's own panel");
  Check(Shows(a, 1, PickState::kConfirmed, 7), "A sees B's character on a real panel");
  Check(Shows(b, 1, PickState::kConfirmed, 7), "B chose B's own character on Player 2");
  Check(Shows(b, 0, PickState::kSelecting, 4), "B sees A's character on Player 1");
  Check(a.screen.panels()[2].state == PickState::kNone && a.screen.panels()[3].state == PickState::kNone, "A's other panels stay free");
  Check(b.screen.panels()[2].state == PickState::kNone && b.screen.panels()[3].state == PickState::kNone, "B's other panels stay free");
  Check(!a.screen.went_on && !b.screen.went_on, "nobody left the screen on their own");

  // B presses A again: the host owns the walk to the track screen.
  Check(!(b.rules[1].allow & lobby::kPadA) && !(b.rules[1].allow & lobby::kPadStart), "a confirmed guest cannot walk on");
  Check((b.rules[1].allow & lobby::kPadB) != 0, "but can still go back");
  Check((a.rules[0].allow & lobby::kPadA) != 0, "the host can");
  a.Tick(100.0, WithPicks(2, {&a, &b}, 0), lobby::kPadA);
  Check(a.screen.went_on, "with everybody confirmed the game's own rule lets the host go on");
}

void TestBackingOutOfAPick() {
  Machine a(0);
  Machine b(1);
  Run({&a, &b}, {2, 5});
  Check(Shows(a, 1, PickState::kConfirmed, 5), "B confirmed");

  // B changes their mind: back to selecting, then another character.
  double now = 200.0;
  auto settle = [&](uint16_t press) {
    for (int tick = 0; tick < 240; ++tick, now += 1.0 / 60.0) {
      a.Tick(now, WithPicks(2, {&a, &b}, 0), 0);
      b.Tick(now, WithPicks(2, {&a, &b}, 1), tick == 0 ? press : 0);
    }
  };
  settle(lobby::kPadB);
  Check(Shows(a, 1, PickState::kSelecting, 5), "A sees B selecting again");
  settle(lobby::kPadDpadRight);
  Check(Shows(a, 1, PickState::kSelecting, 6), "A follows the change");
  settle(lobby::kPadA);
  Check(Shows(a, 1, PickState::kConfirmed, 6), "and the new confirmation");
}

void TestUnreachableCharacterWaits() {
  // Both players end up on one character: the game refuses to show it twice, so the panel waits instead of spinning.
  Screen host(0);
  lobby::LobbyPads pads;
  host.Pad(0, lobby::kPadDpadRight);
  host.Pad(0, 0);
  const int taken = host.panels()[0].character;
  online::Snapshot state = Room(2, 0);
  state.room->seats[1].pick = PickState::kConfirmed;
  state.room->seats[1].character = taken;

  int moves = 0;
  for (int tick = 0; tick < 1800; ++tick) {
    const lobby::PadRules rules = pads.Step(double(tick) / 60.0, lobby::MakePlan(state, true, kPanelObjects), host.panels(), 0);
    if (rules[1].buttons & (lobby::kPadDpadLeft | lobby::kPadDpadRight)) ++moves;
    host.Pad(1, rules[1].buttons);
  }
  Check(moves == 0, "no press spent on a character another panel shows");
  Check(host.panels()[1].state == PickState::kSelecting, "and it is never confirmed on the wrong character");
  Check(host.panels()[1].character != taken, "the game's own rule holds");

  host.Pad(0, lobby::kPadDpadRight);
  host.Pad(0, 0);
  for (int tick = 1800; tick < 3600; ++tick) {
    host.Pad(1, pads.Step(double(tick) / 60.0, lobby::MakePlan(state, true, kPanelObjects), host.panels(), 0)[1].buttons);
  }
  Check(host.panels()[1].state == PickState::kConfirmed && host.panels()[1].character == taken, "once it is free the panel gets there");

  // A target that stays out of reach for another reason (a character the game cannot show) cannot spin forever.
  lobby::PanelPad pad;
  lobby::PanelView panel{PickState::kSelecting, 0};
  int presses = 0;
  uint16_t last = 0;
  for (int tick = 0; tick < 36000; ++tick) {
    const uint16_t buttons = pad.Step(double(tick) / 60.0, panel, {PickState::kConfirmed, 3}, true);
    if (buttons != 0 && last == 0) ++presses;
    last = buttons;
  }
  Check(presses == lobby::PanelPad::kMaxMoves, "the moves toward one target are bounded");
}

void TestThreePlayers() {
  Machine a(0);
  Machine b(1);
  Machine c(2);
  Run({&a, &b, &c}, {1, 2, 3});
  Check(Shows(a, 1, PickState::kConfirmed, 2) && Shows(a, 2, PickState::kConfirmed, 3), "A sees B and C in seat order");
  Check(Shows(b, 0, PickState::kSelecting, 1) && Shows(b, 2, PickState::kSelecting, 3), "B sees A and C");
  Check(Shows(c, 0, PickState::kSelecting, 1) && Shows(c, 1, PickState::kSelecting, 2), "C sees A and B");
  Check(Shows(c, 2, PickState::kConfirmed, 3), "C has their own panel");
}

void TestFourPlayers() {
  Machine a(0);
  Machine b(1);
  Machine c(2);
  Machine d(3);
  Run({&a, &b, &c, &d}, {8, 9, 10, 11});
  Check(Shows(a, 0, PickState::kConfirmed, 8) && Shows(a, 1, PickState::kConfirmed, 9) && Shows(a, 2, PickState::kConfirmed, 10) &&
            Shows(a, 3, PickState::kConfirmed, 11),
        "A sees all four, seat on the same panel");
  Check(Shows(d, 3, PickState::kConfirmed, 11) && Shows(d, 0, PickState::kSelecting, 8) && Shows(d, 1, PickState::kSelecting, 9) &&
            Shows(d, 2, PickState::kSelecting, 10),
        "D sees the same order as everybody, themself as Player 4");
  Check(!a.screen.went_on && !d.screen.went_on, "nobody walked on");
}

void TestRemotePanelsSwapCharacters() {
  // Each remote panel shows what the other wants: neither may wait for the other.
  Screen host(0);
  online::Snapshot state = Room(3, 0);
  lobby::LobbyPads pads;
  const auto settle = [&](double from) {
    for (int tick = 0; tick < 1200; ++tick) {
      const lobby::PadRules rules = pads.Step(from + double(tick) / 60.0, lobby::MakePlan(state, true, kPanelObjects), host.panels(), 0);
      for (const int pad : {1, 2}) host.Pad(pad, rules[size_t(pad)].buttons);
    }
  };
  state.room->seats[1].pick = PickState::kSelecting;
  state.room->seats[1].character = 10;
  state.room->seats[2].pick = PickState::kSelecting;
  state.room->seats[2].character = 11;
  settle(0);
  Check(host.panels()[1].character == 10 && host.panels()[2].character == 11, "both reach their characters");
  state.room->seats[1].character = 11;
  state.room->seats[2].character = 10;
  settle(30);
  Check(host.panels()[1].character == 11 && host.panels()[2].character == 10, "and can swap them");
}

void TestCanonicalPanels() {
  for (int seat = 0; seat < lobby::kPanels; ++seat) {
    Check(lobby::kPanelOfSeat[size_t(seat)] == seat && lobby::kSeatOfPanel[size_t(seat)] == seat, "seat N is panel N, on every PC");
  }

  // Physical pad 0 on every PC, and all of them see the same screen.
  Machine a(0);
  Machine b(1);
  Machine c(2);
  Machine d(3);
  Run({&a, &b, &c, &d}, {8, 9, 10, 11});
  for (const Machine* machine : {&a, &b, &c, &d}) {
    for (int panel = 0; panel < lobby::kPanels; ++panel) {
      Check(machine->screen.panels()[size_t(panel)].character == 8 + panel, "every PC shows each player on that player's panel");
    }
    const lobby::PadRule& own = machine->rules[size_t(machine->seat)];
    Check(own.own_pad && own.source_pad == 0, "physical pad 0 answers on the local seat's index");
    for (int panel = 0; panel < lobby::kPanels; ++panel) {
      if (panel != machine->seat) Check(machine->rules[size_t(panel)].virtual_pad, "and every other index is a seat's virtual pad");
    }
  }
  Check(b.rules[0].virtual_pad && !b.rules[0].own_pad, "a guest never holds Player 1");
  Check(!a.rules[0].virtual_pad && a.rules[1].virtual_pad, "the host holds it, and sees B on Player 2");

  // The game opens the primary panel by itself: on a guest that panel is the host's, and the guest's own opens by a press.
  Machine guest(1);
  online::Snapshot two = Room(2, 1);
  two.room->seats[0].pick = PickState::kSelecting;
  two.room->seats[0].character = 4;
  bool pulsed = false;
  for (int tick = 0; tick < 60; ++tick) {
    guest.Tick(double(tick) / 60.0, two, 0);
    pulsed = pulsed || (guest.rules[1].press & lobby::kPadStart) != 0;
  }
  Check(pulsed && guest.screen.panels()[1].state == PickState::kSelecting, "the guest's own panel opens without a press of theirs");
  Check(guest.screen.panels()[0].state == PickState::kSelecting && guest.screen.panels()[0].character == 4, "the host stays on Player 1");
  Check(guest.screen.panels()[2].state == PickState::kNone && guest.screen.panels()[3].state == PickState::kNone, "the rest stay Press Start");

  guest.Tick(2, two, lobby::kPadB);
  guest.Tick(2.2, two, 0);
  Check(guest.screen.panels()[1].state == PickState::kNone, "B closes the guest's own panel");
  Check(guest.screen.panels()[0].state == PickState::kSelecting, "and nothing of the host's");
  for (int tick = 0; tick < 30; ++tick) guest.Tick(3.0 + double(tick) / 60.0, two, 0);
  Check(guest.screen.panels()[1].state == PickState::kNone, "a panel closed on purpose is not opened again");
  Check(guest.rules[1].back_leaves, "and B now means leaving");
  guest.Tick(4, two, lobby::kPadDpadRight);
  Check(guest.screen.panels()[0].character == 4, "the guest's pad does not reach the host's panel");

  // Rejoining: nothing of the last session is kept.
  guest.pads.NewSession();
  guest.Tick(5, two, 0);
  Check(!guest.rules[1].back_leaves && guest.rules[1].press == lobby::kPadStart, "a new session opens the panel again");

  // A second pad opened a panel before anybody joined: the seat still goes on its own panel, never on the next free one.
  Screen crowded(0);
  crowded.Pad(1, lobby::kPadStart);
  crowded.Pad(1, 0);
  online::Snapshot joined = Room(3, 0);
  joined.room->seats[2].pick = PickState::kConfirmed;
  joined.room->seats[2].character = 6;
  lobby::LobbyPads pads;
  for (int tick = 0; tick < 240; ++tick) {
    const lobby::PadRules rules = pads.Step(double(tick) / 60.0, lobby::MakePlan(joined, true, kPanelObjects), crowded.panels(), 0);
    for (const int pad : {1, 2, 3}) crowded.Pad(pad, rules[size_t(pad)].buttons);
  }
  Check(crowded.panels()[2].state == PickState::kConfirmed && crowded.panels()[2].character == 6, "seat 2 is panel 2");
  Check(crowded.panels()[3].state == PickState::kNone, "and panel 3 stays free");

  // The pad a host happens to hold does not move them either.
  Machine host(0, 1);
  for (int tick = 0; tick < 120; ++tick) host.Tick(double(tick) / 60.0, Room(1, 0), 0);
  Check(host.screen.panels()[0].state == PickState::kSelecting, "a host on pad 1 is still Player 1");
  Check(host.screen.panels()[1].state == PickState::kNone, "and the game's primary panel gives way");
  Check(host.rules[0].source_pad == 1 && host.rules[1].virtual_pad, "pad 1 answers on index 0");

  const lobby::Plan plan = lobby::MakePlan(Room(3, 2), true, kPanelObjects);
  Check(lobby::LocalPanel(plan, 0) == 2, "C on pad 0 acts on panel 2");
  Check(lobby::CanonicalPadSeats(plan) == lobby::PadSeats{0, 1, 2, -1}, "the final URL reads panels as seats");
  const lobby::Plan offline = lobby::MakePlan({}, true, kPanelObjects);
  Check(lobby::LocalPanel(offline, 1) == 1, "without a session the panel is the game's primary one");

  Check(lobby::CanonicalPanelsLine(1) == "canonical_panels seat0=panel0 seat1=panel1 seat2=panel2 seat3=panel3 local_seat=1", "the log line");
  lobby::Panels view{};
  view[0] = {PickState::kSelecting, 4};
  view[1] = {PickState::kConfirmed, 7};
  const lobby::Plan b_plan = lobby::MakePlan(Room(2, 1), true, kPanelObjects);
  Check(lobby::PanelLine(b_plan, view, 0, 0) == "panel0 seat=0 owner=remote virtual=0 character=4 state=1", "the host's panel on B");
  Check(lobby::PanelLine(b_plan, view, 1, 0) == "panel1 seat=1 owner=local physical=0 character=7 state=2", "B's own panel");
  Check(lobby::PanelLine(b_plan, view, 2, 0) == "panel2 empty", "an empty panel");
}

void TestOwnPickBelongsToItsSession() {
  lobby::Bridge bridge;
  bridge.ReportOwn({PickState::kConfirmed, 5}, 3);
  Check(bridge.own(3).has_value(), "the panel of this session");
  Check(!bridge.own(4), "says nothing about the next one");
  online::Snapshot state = Room(2, 1);
  state.session_generation = 9;
  Check(lobby::MakePlan(state, true, kPanelObjects).session == 9, "the game side is told which session it serves");
}

void TestLocalGameIsUntouched() {
  // Online play Off, or On with no session: every pad and panel is the original game's.
  lobby::LobbyPads pads;
  Screen screen(0);
  const lobby::Plan none = lobby::MakePlan({}, true, kPanelObjects);
  screen.Pad(1, lobby::kPadStart);
  screen.Pad(1, 0);
  lobby::PadRules rules = pads.Step(0, none, screen.panels(), 0);
  for (const lobby::PadRule& rule : rules) {
    Check(!rule.virtual_pad && rule.allow == 0xFFFF && rule.source_pad == -1 && rule.press == 0 && !rule.back_leaves,
          "without a session every pad stays itself");
  }
  Check(rules[0].own_pad, "the primary pad is the local one");
  Check(screen.panels()[1].state == PickState::kSelecting, "and a second local player keeps their panel");
  const lobby::Bridge untouched;
  Check(untouched.RuleFor(0).source_pad == -1, "the hook's default leaves the pad alone");
}

void TestPadRules() {
  lobby::LobbyPads pads;
  lobby::Panels panels{};
  panels[0].state = PickState::kSelecting;

  lobby::LobbyPads host;
  lobby::PadRules rules = host.Step(0, lobby::MakePlan(Room(2, 0), true, kPanelObjects), panels, 0);
  Check(rules[1].virtual_pad, "B's panel is driven by a virtual pad");
  Check(rules[0].own_pad && rules[0].allow == 0xFFFF && !rules[0].back_leaves, "A's open panel is A's");
  Check(rules[2].virtual_pad && rules[2].buttons == 0, "in a session an empty seat's panel takes no local pad");

  panels[1].state = PickState::kSelecting;
  lobby::LobbyPads guest;
  rules = guest.Step(0, lobby::MakePlan(Room(2, 1), true, kPanelObjects), panels, 0);
  Check(rules[1].own_pad && rules[1].source_pad == 0, "the guest's own pad, on the guest's seat");
  Check(rules[0].virtual_pad, "the host's panel on the guest's PC");
  Check(rules[2].virtual_pad && rules[3].virtual_pad, "a guest's other pads cannot open a panel");

  panels[1].state = PickState::kNone;
  rules = guest.Step(1, lobby::MakePlan(Room(2, 1), true, kPanelObjects), panels, 0);
  Check(rules[1].back_leaves, "B on a closed panel means leaving");
  Check(!lobby::LobbyPads().Step(0, lobby::MakePlan({}, true, kPanelObjects), panels, 0)[0].back_leaves,
        "without a session B stays the game's own");

  // Input follows focus for real pads only: a remote seat has to keep moving on an unfocused window.
  Check(eck::GameReadsInput(false, false) == false, "an unfocused window reads no local input");
  Check(rules[0].virtual_pad, "and the virtual pad does not depend on it");
}

void TestPlan() {
  online::Snapshot state = Room(3, 1);
  state.room->seats[0].pick = PickState::kConfirmed;
  state.room->seats[0].character = 6;
  const lobby::Plan plan = lobby::MakePlan(state, true, kPanelObjects);
  Check(plan.on_screen && plan.panels == kPanelObjects && plan.local_seat == 1, "a seat in a room on the screen");
  Check(plan.remote[0] && !plan.remote[1] && plan.remote[2] && !plan.remote[3], "everybody seated but me");
  Check(plan.seats[0].state == PickState::kSelecting && plan.seats[0].character == 6, "the host's pick");
  Check(plan.seats[2].state == PickState::kSelecting && plan.seats[2].character == -1, "a seat that chose nothing yet still shows");
  Check(!lobby::MakePlan(state, false, kPanelObjects).on_screen, "off the screen nothing is driven");
}

void TestGuestFlow() {
  using menu::GameScreen;
  lobby::LobbyFlow flow;
  const online::Snapshot none;
  Check(flow.Update(none, GameScreen::kMainMenu, true, false).travel.kind == lobby::TravelKind::kNone, "nothing without a session");

  const online::Snapshot joined = Room(2, 1);
  const lobby::FlowResult first = flow.Update(joined, GameScreen::kMainMenu, true, false);
  Check(first.travel.kind == lobby::TravelKind::kEntry, "accepting from the main menu travels");
  Check(first.travel.setup.mode == 3 && first.travel.setup.difficulty == 1, "into the host's game");
  Check(!first.leave, "and stays in the session");
  Check(flow.Update(joined, GameScreen::kMainMenu, true, false).travel.kind == lobby::TravelKind::kNone, "only once");
  Check(flow.Update(joined, GameScreen::kCompetition, true, false).travel.kind == lobby::TravelKind::kNone,
        "the game's own walk through CompetitionSelect is not leaving");
  Check(!flow.Update(joined, GameScreen::kCompetition, true, false).leave, "for a guest either");
  Check(flow.Update(joined, GameScreen::kCharacter, true, false).note == "guest entered character select", "arrives");
  Check(flow.in_lobby(), "the character screen is the lobby");

  const lobby::FlowResult left = flow.Update(joined, GameScreen::kCharacter, true, true);
  Check(left.leave && left.travel.kind == lobby::TravelKind::kMainMenu, "backing out leaves and goes home");
  Check(flow.Update(none, GameScreen::kCharacter, true, false).travel.kind == lobby::TravelKind::kNone, "exactly once");

  // From any menu screen, and from the character screen of an earlier local game.
  for (const GameScreen screen : {GameScreen::kOther, GameScreen::kCompetition, GameScreen::kCharacter, GameScreen::kTrack}) {
    lobby::LobbyFlow other;
    Check(other.Update(joined, screen, true, false).travel.kind == lobby::TravelKind::kEntry, "accepting from any menu travels");
  }

  // Live gameplay is never torn down.
  lobby::LobbyFlow racing;
  Check(racing.Update(joined, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "nothing happens in a race");
  Check(racing.Update(joined, GameScreen::kMainMenu, true, false).travel.kind == lobby::TravelKind::kEntry, "it waits for the menus");

  lobby::LobbyFlow host_gone;
  host_gone.Update(joined, GameScreen::kMainMenu, true, false);
  host_gone.Update(joined, GameScreen::kCharacter, true, false);
  const lobby::FlowResult ended = host_gone.Update(none, GameScreen::kCharacter, true, false);
  Check(ended.travel.kind == lobby::TravelKind::kMainMenu && !ended.leave, "a guest whose host left is taken home");

  lobby::LobbyFlow race_ended;
  race_ended.Update(joined, GameScreen::kMainMenu, true, false);
  Check(race_ended.Update(none, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu,
        "from anywhere the session ended, a race included");

  lobby::LobbyFlow bad;
  online::Snapshot unset = joined;
  unset.room->rules.clear();
  const lobby::FlowResult refused = bad.Update(unset, GameScreen::kMainMenu, true, false);
  Check(refused.leave && refused.travel.kind == lobby::TravelKind::kNone, "a session without a setup is left, not entered blind");
}

void TestHostFlow() {
  using menu::GameScreen;
  lobby::LobbyFlow flow;
  const online::Snapshot host = Room(2, 0);
  const lobby::FlowResult stays = flow.Update(host, GameScreen::kCharacter, true, false);
  Check(!stays.leave && stays.travel.kind == lobby::TravelKind::kNone, "the host stays on the character screen when B joins");
  Check(!flow.Update(host, GameScreen::kTrack, true, false).leave, "and through the track screen");
  Check(flow.Update(host, GameScreen::kCharacter, true, true).leave, "B on a closed panel ends the session");
  Check(flow.Update(host, GameScreen::kCompetition, true, false).leave, "and so does the game going back");
  Check(flow.Update(host, GameScreen::kMainMenu, true, false).leave, "or the main menu");
  Check(flow.Update(host, GameScreen::kCharacter, true, false).travel.kind == lobby::TravelKind::kNone, "the host never travels");
}

void TestHostLeavesAndPanelsClose() {
  Machine a(0);
  Machine b(1);
  Run({&a, &b}, {3, 9});
  // The session is gone: what was B's panel closes itself through the game's own B.
  online::Snapshot none;
  for (int tick = 0; tick < 240; ++tick) a.Tick(300.0 + double(tick) / 60.0, none, 0);
  Check(a.screen.panels()[1].state == PickState::kNone, "the remote panel closed");
  Check(a.screen.panels()[0].state == PickState::kConfirmed, "A's own panel is untouched");
  Check(!a.screen.popped, "A is still on the screen, as a local game");

  // A host who closed their own panel first leaves the screen with the last remote panel.
  Machine host(0);
  Machine guest(1);
  Run({&host, &guest}, {3, 9});
  for (int tick = 0; tick < 120; ++tick) {
    host.Tick(300.0 + double(tick) / 60.0, WithPicks(2, {&host, &guest}, 0), tick % 40 == 0 ? lobby::kPadB : 0);
  }
  Check(host.screen.panels()[0].state == PickState::kNone && !host.screen.popped, "a guest's open panel holds the screen");
  for (int tick = 0; tick < 240; ++tick) host.Tick(400.0 + double(tick) / 60.0, none, 0);
  Check(host.screen.popped, "the screen goes back by itself once the session is gone");
}

// The command the real host produced: Quico and Popis on the Maracana.
constexpr char kRealUrl[] =
    "start PistaBrasilM_Circuito0?CharArray=1-5-9-10-0-4-11-7?IsBotArray=0-0-1-1-1-1-1-1?PlayersArray=0-1-99-99-99-99-99-99"
    "?TeamData=0-1-0-1-0-1-0-1-?LastTipTextField=normal_race_tip_5?LastTipBGKNumber=0?PrimaryControllerID=0?NumSelectedTrack=0"
    "?NumHumanKarts=2?TotalKarts=8?KartAIDiffculty=0?GameType0";
constexpr lobby::PadSeats kIdentityPads = {0, 1, 2, 3};

lobby::CanonicalRace RealRace() { return *lobby::CanonicalOfUrl(*eck::race_url::ParseRaceUrl(kRealUrl), kIdentityPads, 1).race; }

lobby::CanonicalRace RaceOf(int humans) {
  lobby::CanonicalRace race = RealRace();
  race.humans = humans;
  race.bots = 0xFF & ~((1u << humans) - 1);
  for (int seat = 0; seat < humans; ++seat) race.seats[size_t(seat)] = seat;
  return race;
}

// What KartGame_Base.SetupLocalPlayers and SpawnDefaultPawnFor make of a command on one PC.
struct LoadedRace {
  std::array<int, 8> characters{};
  int local_slot = -1;
  int primary = -1;
  std::string map;
  int difficulty = -1;
  int track = -1;
  std::vector<int> bots;
};

LoadedRace Load(const std::string& command) {
  const auto url = eck::race_url::ParseRaceUrl(command);
  LoadedRace loaded;
  if (!url) return loaded;
  loaded.primary = url->primary_controller.value_or(-1);
  loaded.map = url->map;
  loaded.difficulty = url->ai_difficulty.value_or(-1);
  loaded.track = url->num_selected_track.value_or(-1);
  loaded.bots = url->bots;
  for (int slot = 0; slot < url->num_human_karts.value_or(0); ++slot) {
    const int pad = url->players[size_t(slot)];
    const auto found = std::find(url->players.begin(), url->players.end(), pad);
    loaded.characters[size_t(slot)] = url->characters[size_t(found - url->players.begin())];
    if (pad == loaded.primary) loaded.local_slot = slot;
  }
  return loaded;
}

void TestFinalRaceUrl() {
  const auto url = eck::race_url::ParseRaceUrl(kRealUrl);
  Check(url.has_value(), "the real URL parses");
  const lobby::RaceParse parse = lobby::CanonicalOfUrl(*url, kIdentityPads, 7);
  Check(parse.race.has_value(), "and is a canonical race");
  if (!parse.race) return;
  const lobby::CanonicalRace& race = *parse.race;
  Check(race.generation == 7 && race.track == 0 && race.mode == 0 && race.difficulty == 0, "track, mode and difficulty");
  Check(race.humans == 2 && race.total == 8, "human and total counts");
  Check(race.characters == std::array<int, 8>{1, 5, 9, 10, 0, 4, 11, 7}, "every character, bots included, never 255");
  Check(race.bots == 0xFC, "bots stay in their slots");
  Check(race.teams == 0xAA, "teams");
  Check(race.seats[0] == 0 && race.seats[1] == 1, "seat 0 and 1 hold kart slots 0 and 1");

  const auto again = eck::race_url::ParseRaceUrl(lobby::RaceCommand(race, 0, 0));
  Check(again && again->map == url->map && again->players == url->players && again->characters == url->characters &&
            again->bots == url->bots && again->teams == url->teams && again->num_human_karts == url->num_human_karts &&
            again->total_karts == url->total_karts && again->num_selected_track == url->num_selected_track &&
            again->ai_difficulty == url->ai_difficulty && again->primary_controller == url->primary_controller,
        "the command rebuilt for seat 0 is the host's own race");
  Check(again && again->other == url->other, "with the game's own GameType0");
  Check(eck::race_telemetry::SetupOfUrl(*again).has_value(), "and telemetry knows it as a race");

  const auto failed = [&](std::string text, const std::string& from, const std::string& to) {
    text.replace(text.find(from), from.size(), to);
    const auto bad = eck::race_url::ParseRaceUrl(text);
    return std::string(lobby::CanonicalOfUrl(*bad, kIdentityPads, 1).failed);
  };
  Check(failed(kRealUrl, "PistaBrasilM_Circuito0", "SomeOtherMap") == "track", "an unknown map is refused");
  Check(failed(kRealUrl, "start Pista", "start RET-Pista") == "mode", "a challenge is no Free Race");
  Check(failed(kRealUrl, "KartAIDiffculty=0", "KartAIDiffculty=3") == "difficulty", "an unknown difficulty is refused");
  Check(failed(kRealUrl, "TotalKarts=8", "TotalKarts=9") == "karts", "too many karts are refused");
  Check(failed(kRealUrl, "NumHumanKarts=2", "NumHumanKarts=1") == "karts", "one human is no online race");
  Check(failed(kRealUrl, "CharArray=1-5", "CharArray=1-12") == "characters", "an unknown character is refused");
  Check(failed(kRealUrl, "CharArray=1-5-9", "CharArray=1-5") == "arrays", "a short array is refused");
  Check(failed(kRealUrl, "IsBotArray=0-0-1", "IsBotArray=0-1-1") == "bots", "a human cannot be a bot");
  Check(failed(kRealUrl, "PlayersArray=0-1-99", "PlayersArray=0-1-98") == "players", "a bot has the game's bot id");
  Check(failed(kRealUrl, "PlayersArray=0-1-99", "PlayersArray=0-0-99") == "seats", "a seat cannot drive two karts");
  Check(failed(kRealUrl, "?GameType0", "?Sneaky=1") == "unknown_option", "an option nobody knows is not carried");
  const lobby::RaceParse local = lobby::CanonicalOfUrl(*url, {0, -1, -1, -1}, 1);
  Check(std::string(local.failed) == "seats", "a local second pad is no session seat");
}

void TestKartOwnership() {
  const lobby::CanonicalRace two = RaceOf(2);
  const LoadedRace host = Load(lobby::RaceCommand(two, 0, 0));
  const LoadedRace guest = Load(lobby::RaceCommand(two, 1, 0));
  Check(host.local_slot == 0, "host local ownership: kart slot 0");
  Check(guest.local_slot == 1, "guest local ownership: kart slot 1");
  Check(guest.primary == 0, "the guest keeps their own pad as the primary one");
  Check(host.characters == guest.characters, "both PCs give every kart the same character");
  Check(host.characters[0] == 1 && host.characters[1] == 5, "Quico in slot 0, Popis in slot 1");
  Check(host.map == guest.map && host.track == guest.track && host.difficulty == guest.difficulty, "same map, track and difficulty");
  Check(host.bots == guest.bots, "the same bots");
  const std::string b = lobby::RaceCommand(two, 1, 0);
  Check(b.find("PlayersArray=1-0-99-99-99-99-99-99") != std::string::npos, "on B slot 0 is the remote pad and slot 1 the real one");
  Check(b.find("CharArray=1-5-9-10-0-4-11-7") != std::string::npos, "and the characters do not move");
  Check(lobby::RaceCommand(two, 1, 1).find("PlayersArray=0-1-99") != std::string::npos, "a guest on pad 1 needs no swap");

  for (const int humans : {2, 3, 4}) {
    const lobby::CanonicalRace race = RaceOf(humans);
    for (int seat = 0; seat < humans; ++seat) {
      for (int own_pad = 0; own_pad < 4; ++own_pad) {
        const auto url = eck::race_url::ParseRaceUrl(lobby::RaceCommand(race, seat, own_pad));
        const LoadedRace loaded = Load(lobby::RaceCommand(race, seat, own_pad));
        Check(loaded.local_slot == seat, "each seat owns its own kart slot, on any pad");
        std::set<int> pads(url->players.begin(), url->players.begin() + humans);
        Check(pads.size() == size_t(humans) && *pads.rbegin() < 4, "every human kart has its own pad");
        Check(lobby::SlotOfSeat(race, seat) == seat, "the seat is in the race");
      }
    }
  }
  const lobby::CanonicalRace four = RaceOf(4);
  Check(lobby::RaceCommand(four, 2, 0).find("PlayersArray=1-2-0-3") != std::string::npos, "C on pad 0");
  Check(lobby::RaceCommand(four, 3, 0).find("PlayersArray=1-2-3-0") != std::string::npos, "D on pad 0");

  Check(lobby::GameCommandMatchesPads(RaceOf(2), 0) && lobby::GameCommandMatchesPads(four, 0),
        "a host on pad 0 with every seat filled keeps the game's own command");
  Check(!lobby::GameCommandMatchesPads(four, 1), "a host on another pad needs the race told by pad");
  lobby::CanonicalRace gap = RaceOf(3);
  gap.seats[1] = 2;
  gap.seats[2] = 3;
  Check(!lobby::GameCommandMatchesPads(gap, 0), "an empty seat needs it too, or a kart answers to a pad nobody drives");
  Check(lobby::RaceCommand(gap, 0, 0).find("PlayersArray=0-1-2-") != std::string::npos, "the karts then take the pads in order");
  for (int slot = 0; slot < gap.humans; ++slot) {
    const int seat = gap.seats[size_t(slot)];
    Check(lobby::PadOfSeat(gap, seat, 0, 0) == slot, "each seat drives the pad its own kart reads on the host");
  }
}

void TestGuestConfirmation() {
  online::Snapshot guest;
  guest.seat = 1;
  guest.room = online::RoomView{};
  online::Seat host;
  host.seat = 0;
  host.state = online::SeatState::kConnected;
  host.pick = lobby::PickState::kConfirmed;
  host.character = 3;
  guest.room->seats.push_back(host);
  const lobby::Plan plan = lobby::MakePlan(guest, true, {});
  Check(plan.seats[0].state == lobby::PickState::kSelecting && plan.seats[0].character == 3,
        "on a guest the host's panel shows the character, one step short of confirmed");

  online::Snapshot hosting = guest;
  hosting.seat = 0;
  hosting.room->seats[0].seat = 1;
  const lobby::Plan host_plan = lobby::MakePlan(hosting, true, {});
  Check(host_plan.seats[1].state == lobby::PickState::kConfirmed, "on the host a guest's panel confirms: the game may go on");

  // Run the screen the way the game does: its popup needs one panel confirmed and none selecting.
  lobby::Panels panels{};
  panels[1] = {lobby::PickState::kConfirmed, 0};
  panels[0] = {lobby::PickState::kNone, 0};
  lobby::LobbyPads pads;
  double now = 1;
  for (int i = 0; i < 40; ++i, now += 0.05) {
    const lobby::PadRules rules = pads.Step(now, plan, panels, 0);
    if (rules[0].buttons == lobby::kPadStart) panels[0] = {lobby::PickState::kSelecting, 3};
    Check(!(rules[0].buttons == lobby::kPadA), "a guest never confirms the host's panel for it");
  }
  Check(panels[0].state == lobby::PickState::kSelecting, "so the popup's condition never holds on a guest");
  Check((pads.Step(now, plan, panels, 0)[1].allow & lobby::kPadA) == 0, "and a confirmed guest has no A to walk on with");

  lobby::Panels first{};
  first[1] = {lobby::PickState::kSelecting, 0};
  lobby::LobbyPads early;
  Check((early.Step(1, plan, first, 0)[1].allow & lobby::kPadA) == 0, "no confirming before the host's panel is open");
  first[0] = {lobby::PickState::kSelecting, 3};
  Check((early.Step(2, plan, first, 0)[1].allow & lobby::kPadA) != 0, "then the guest's A is its own again");
  first[1].state = lobby::PickState::kConfirmed;
  Check((early.Step(3, plan, first, 0)[1].allow & lobby::kPadB) != 0, "and B still takes the choice back");

  Check(lobby::GuestWaitsOnCharacters(true, true, lobby::PanelView{lobby::PickState::kConfirmed, 2}), "a confirmed guest waits for the host");
  Check(!lobby::GuestWaitsOnCharacters(true, true, lobby::PanelView{lobby::PickState::kSelecting, 2}), "not while still choosing");
  Check(!lobby::GuestWaitsOnCharacters(false, true, lobby::PanelView{lobby::PickState::kConfirmed, 2}), "and never the host");
  Check(std::string(eck::OnlineText("Waiting for host", eck::TextPackage::kInt)) == "Waiting for host", "English");
  Check(std::string(eck::OnlineText("Waiting for host", eck::TextPackage::kPtb)) == "Aguardando host", "Portuguese");
  Check(std::string(eck::OnlineText("Waiting for host", eck::TextPackage::kEsm)) == "Esperando al host", "Latin American Spanish");
  Check(std::string(eck::OnlineText("Waiting for host", eck::TextPackage::kEsn)) == "Esperando al host", "Spanish");
}

void TestCommandLength() {
  lobby::CanonicalRace race = RaceOf(4);
  race.mode = 1;
  race.track = 8;  // the longest map name
  for (int& character : race.characters) character = 11;
  Check(lobby::RaceCommand(race, 3, 3).size() <= lobby::kMaxCommandChars, "the longest race fits a console command");
  Check(std::strlen(kRealUrl) <= lobby::kMaxCommandChars, "as does the game's own, tip included");
}

void TestRacePads() {
  eck::race_sync::FrameOut frame;
  frame.pads[1] = eck::race::PadState{0x1000, 0, 255, -1200, 0, 0, 0};
  frame.own.block = lobby::kPadStart;
  frame.own.press = lobby::kPadA;
  const lobby::PadRules guest = lobby::RacePadRules(0, frame, true, 2);
  Check(guest[0].own_pad && !guest[0].virtual_pad, "the local kart stays on its own pad");
  Check(guest[0].allow == uint16_t(~lobby::kPadStart) && guest[0].press == lobby::kPadA, "whatever the session blocks or presses");
  Check(guest[1].virtual_pad && guest[1].buttons == 0x1000 && guest[1].analog.right_trigger == 255 && guest[1].analog.left_x == -1200,
        "a remote kart reads the session's pad, triggers and sticks too");
  Check(guest[2].allow == 0 && guest[3].allow == 0, "a stray pad cannot reach a kart");
  Check(lobby::RacePadRules(2, {})[0].allow == 0 && lobby::RacePadRules(2, {})[2].own_pad, "whichever pad is the local one");
}

online::Snapshot RaceRoom(int seats, int mine) {
  online::Snapshot state = Room(seats, mine);
  state.room->rules = lobby::RaceRules(state.room->rules, RaceOf(seats));
  state.room->state = online::RoomState::kRacing;
  return state;
}

void TestLeaveButtons() {
  eck::race_sync::FrameOut gate;
  gate.own.block = uint16_t(lobby::kPadStart | eck::race_sync::kPadBack);
  gate.own.press = lobby::kPadA;
  const auto frame_of = [](const eck::race_sync::GatePad& pad) {
    eck::race_sync::FrameOut frame;
    frame.own = pad;
    return frame;
  };
  const lobby::PadRule guest = lobby::RacePadRules(0, gate, true, 0)[0];
  Check(guest.menu_button == lobby::kPadStart, "a guest's Start opens the leave menu, at the gate too");
  Check((guest.allow & lobby::kPadStart) == 0, "and starts nothing");
  Check(guest.press == lobby::kPadA, "the host's begin still reaches the guest's gate");
  const uint16_t guest_result = lobby::RacePadRules(0, frame_of(eck::race_sync::GuestGatePad(true, 4, 1.0)), true, 4)[0].allow;
  Check((guest_result & lobby::kPadA) == 0, "on the results a guest's A cannot Continue");
  Check((guest_result & lobby::kPadB) == 0, "nor B go back");
  Check((guest_result & lobby::kPadStart) == 0 && lobby::RacePadRules(0, frame_of(eck::race_sync::GuestGatePad(true, 4, 1.0)), true, 4)[0].menu_button == lobby::kPadStart,
        "and Start opens the same menu there");

  const lobby::PadRule host = lobby::RacePadRules(0, {}, false, 2)[0];
  Check(host.menu_button == eck::race_sync::kPadBack, "the host's way out is Back");
  Check((host.allow & lobby::kPadStart) != 0, "as Start stays the host's own pause");
  Check((host.allow & eck::race_sync::kPadBack) == 0, "and Back goes no further");
  eck::race_sync::FrameOut finished;
  finished.post_race = true;
  finished.pads[1] = eck::race::PadState{};
  const lobby::PadRules post = lobby::RacePadRules(0, finished, false, 4);
  Check(post[0].menu_button == (lobby::kPadStart | eck::race_sync::kPadBack), "past the finish either button is the host's way out");
  Check((post[0].allow & lobby::kPadA) && (post[0].allow & lobby::kPadB), "and A and B stay the host's");
  Check(post[1].virtual_pad && post[1].echo_buttons == (lobby::kPadA | lobby::kPadB), "the remote seat's finish HUD follows the host's");
  Check(lobby::RacePadRules(0, finished, false, 4, true)[1].echo_buttons == 0, "up to the results, and not a press further");
  Check(lobby::RacePadRules(0, finished, true, 4)[1].echo_buttons == 0, "and never on a guest");

  lobby::Panels panels{};
  panels[0] = {PickState::kSelecting, 3};
  panels[1] = {PickState::kConfirmed, 5};
  lobby::LobbyPads pads;
  const lobby::Plan plan = lobby::MakePlan(Room(2, 1), true, kPanelObjects);
  Check(pads.Step(0, plan, panels, 0)[1].menu_button == lobby::kPadStart, "waiting for the host, Start offers the way out");
  panels[1].state = PickState::kSelecting;
  Check(pads.Step(1, plan, panels, 0)[1].menu_button == 0, "still choosing, Start is the screen's");

  lobby::Bridge bridge;
  bridge.NoteOwnMenuButton(true);
  bridge.NoteOwnMenuButton(true);
  Check(bridge.TakeMenuPress() && !bridge.TakeMenuPress(), "one press, one menu, however long it is held");
  bridge.NoteOwnMenuButton(false);
  Check(!bridge.TakeMenuPress(), "and letting go is not a press");

  lobby::TravelRequest home;
  home.kind = lobby::TravelKind::kMainMenu;
  bridge.RetryTravel(home);
  Check(bridge.TakeTravel().kind == lobby::TravelKind::kMainMenu, "a travel the game could not take yet comes back");
  lobby::TravelRequest newer;
  newer.kind = lobby::TravelKind::kEntry;
  bridge.RequestTravel(newer);
  bridge.RetryTravel(home);
  Check(bridge.TakeTravel().kind == lobby::TravelKind::kEntry, "but never over a newer one");
}

void TestRaceWatch() {
  using lobby::RaceWatch;
  using lobby::WatchRace;
  online::Snapshot state = RaceRoom(2, 1);
  Check(WatchRace(state, std::nullopt) == RaceWatch::kFine, "nothing to judge without a race");
  Check(WatchRace(state, 2.0) == RaceWatch::kFine, "lost packets are not a departure");
  Check(WatchRace(state, lobby::kPeerGoneSeconds + 1) == RaceWatch::kFine, "nor is silence with the host seated and the service up");
  state.room->seats[0].ready = false;
  Check(WatchRace(state, 2.0) == RaceWatch::kFine, "ready=no says nothing");
  Check(WatchRace(state, lobby::kPeerGoneSeconds + 1) == RaceWatch::kFine, "even with the silence");
  Check(WatchRace(state, lobby::kDeadRaceSeconds) == RaceWatch::kConnectionLost, "but no race waits forever");

  state.room->seats[0].state = online::SeatState::kReconnecting;
  Check(WatchRace(state, 1.0) == RaceWatch::kFine, "a host's link can come back");
  Check(WatchRace(state, lobby::kPeerGoneSeconds) == RaceWatch::kHostLeft, "and when it did not, the host left");

  online::Snapshot lost = RaceRoom(2, 1);
  lost.reconnecting = true;
  Check(WatchRace(lost, 1.0) == RaceWatch::kFine, "a direct race outlives the service");
  Check(WatchRace(lost, lobby::kPeerGoneSeconds) == RaceWatch::kConnectionLost, "with nothing coming in, this side cannot know");

  online::Snapshot host = RaceRoom(2, 0);
  Check(WatchRace(host, 100.0) == RaceWatch::kFine, "a host is never timed out of its own race");
}

void TestResultScreens() {
  menu::ScreenTracker screens;
  screens.NoteMovie("/ package/ChavoKart_HudManager/ChavoKartHudManager");
  Check(!screens.results_open(), "the finish HUD lives in the race's own movies");
  screens.NoteMovie("/ package/ChavoKart_ResultsScreen/ChavoKartResultsScreen");
  Check(screens.results_open(), "the results movie is the end of it");
  screens.NoteMapChange();
  Check(!screens.results_open(), "for this map");
  screens.NoteMovie("/ package/ChavoKart_EndRace/ChavoKartEndRace");
  Check(screens.results_open(), "the end screen too");

  using lobby::AfterRace;
  const std::string back = "start StartMenu?CharArray=1-5?PlayersArray=0-1?NumGameMode=1?GoBackType=";
  Check(lobby::DestinationOfCommand(back + "2") == AfterRace::kCharacterSelect, "Back to the character screen stays in the session");
  Check(lobby::DestinationOfCommand(back + "3") == AfterRace::kTrackSelect, "and so does the track screen");
  Check(!lobby::DestinationOfCommand(back + "1") && !lobby::DestinationOfCommand(back + "0"), "the mode and the main menu do not");
  Check(!lobby::DestinationOfCommand("start TA-PistaBrasilM_Circuito0?GoBackType=2"), "a race is no way back");
  Check(lobby::IsMenuCommand(back + "0") && !lobby::IsMenuCommand("start TA-PistaBrasilM_Circuito0"), "only menus");

  lobby::Bridge bridge;
  bridge.NoteHostReturn(back + "2");
  Check(!bridge.TakeHostReturn(), "only a host in a race reports its way back");
  lobby::Plan racing = lobby::MakePlan(RaceRoom(2, 0), false, {});
  racing.in_race = true;
  bridge.Publish(racing);
  bridge.NoteHostReturn(back + "2");
  Check(bridge.TakeHostReturn() == back + "2" && !bridge.TakeHostReturn(), "one transition, one report");

  const online::RoomRules rules = lobby::RaceRules(lobby::SetupRules({lobby::kGameTypeStartMenu, 1, 0}), RaceOf(2));
  const online::RoomRules next = lobby::AfterRaceRules(rules, {4, AfterRace::kTrackSelect});
  Check(!lobby::RaceOfRules(next), "the last race does not stay in the rules");
  Check(lobby::SetupOfRules(next) && lobby::SetupOfRules(next)->mode == 1, "the game does");
  const std::optional<lobby::AfterRaceStep> step = lobby::AfterRaceOfRules(next);
  Check(step && step->generation == 4 && step->destination == AfterRace::kTrackSelect, "and where everybody goes");
  Check(lobby::RaceRules(lobby::BaseRules(next), RaceOf(2)).size() <= 16, "the next race fits the rule limit");
  Check(!lobby::AfterRaceOfRules({{"after_race", 4}, {"after_dest", 7}}), "an unknown destination is nothing");
  Check(!lobby::AfterRaceOfRules(rules), "a room with no step has none");

  const std::string track = lobby::TrackSelectCommand(RaceOf(2), 1, {lobby::kGameTypeStartMenu, 1, 0}, 0);
  Check(track.rfind("start StartMenu?CharArray=", 0) == 0, "the game's own way to the track screen");
  Check(track.find("PlayersArray=1-0-99") != std::string::npos && track.find("?GoBackType=3") != std::string::npos, "with this PC's pads");
  Check(track.size() <= lobby::kMaxCommandChars, "within the command limit");
  lobby::TravelRequest travel;
  travel.kind = lobby::TravelKind::kTrackSelect;
  travel.race = RaceOf(2);
  travel.local_seat = 1;
  travel.setup = {lobby::kGameTypeStartMenu, 1, 0};
  Check(lobby::TravelCommand(travel, 0) == track, "the travel runs it");
}

online::Snapshot Reopened(int seats, int mine, int generation, lobby::AfterRace destination) {
  online::Snapshot state = Room(seats, mine);
  state.room->rules = lobby::AfterRaceRules(state.room->rules, {generation, destination});
  return state;
}

void TestAfterTheRaceFlow() {
  using menu::GameScreen;
  using lobby::AfterRace;
  const online::Snapshot racing = RaceRoom(2, 1);
  const auto raced = [&](lobby::LobbyFlow& flow) {
    flow.Update(Room(2, 1), GameScreen::kMainMenu, true, false);
    flow.Update(Room(2, 1), GameScreen::kCharacter, true, false);
    flow.Update(racing, GameScreen::kCharacter, true, false);
    flow.Update(racing, GameScreen::kOther, false, false);
  };

  // The result screen on its own moves nobody.
  lobby::LobbyFlow guest;
  raced(guest);
  for (int i = 0; i < 300; ++i) {
    const lobby::FlowResult waiting = guest.Update(racing, GameScreen::kOther, false, false);
    Check(!waiting.leave && waiting.travel.kind == lobby::TravelKind::kNone, "a finished race ends nothing");
  }

  const online::Snapshot go = Reopened(2, 1, 1, AfterRace::kCharacterSelect);
  const lobby::FlowResult followed = guest.Update(go, GameScreen::kOther, false, false);
  Check(followed.travel.kind == lobby::TravelKind::kEntry && !followed.leave, "the host's Continue moves the guest, from the race");
  Check(lobby::TravelCommand(followed.travel, 0).find("GoBackType=2") != std::string::npos, "to the same screen");
  Check(guest.Update(go, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "a duplicate is ignored");
  Check(guest.Update(go, GameScreen::kCharacter, true, false).travel.kind == lobby::TravelKind::kNone, "and arriving is not a new step");
  Check(guest.in_lobby(), "the guest is back in the lobby");

  lobby::LobbyFlow track;
  raced(track);
  const lobby::FlowResult tracked = track.Update(Reopened(2, 1, 1, AfterRace::kTrackSelect), GameScreen::kOther, false, false);
  Check(tracked.travel.kind == lobby::TravelKind::kTrackSelect && tracked.travel.local_seat == 1, "the track screen is followed too");
  Check(lobby::TravelCommand(tracked.travel, 0).find("CharArray=" ) != std::string::npos, "with the race just run");
  Check(lobby::MakePlan(Reopened(2, 1, 1, AfterRace::kTrackSelect), false, {}).locked, "where the guest has nothing to press");
  Check(!lobby::MakePlan(Reopened(2, 0, 1, AfterRace::kTrackSelect), false, {}).locked, "the host has");
  Check(!lobby::MakePlan(Reopened(2, 1, 1, AfterRace::kCharacterSelect), false, {}).locked, "and a guest picks again");
  const lobby::PadRules locked = lobby::LockedPadRules(0);
  Check(locked[0].own_pad && locked[0].allow == 0 && locked[0].menu_button == lobby::kPadStart, "only the way out");
  Check(locked[1].virtual_pad && locked[1].buttons == 0, "and nothing else");

  // A stale step changes nothing.
  lobby::LobbyFlow stale;
  raced(stale);
  stale.Update(Reopened(2, 1, 5, AfterRace::kNextRace), GameScreen::kOther, false, false);
  const lobby::FlowResult older = stale.Update(Reopened(2, 1, 4, AfterRace::kCharacterSelect), GameScreen::kOther, false, false);
  Check(older.travel.kind == lobby::TravelKind::kNone, "a stale step is ignored");

  // Restart and Next Race: the guest stays on its results until the race is out.
  lobby::LobbyFlow again;
  raced(again);
  const lobby::FlowResult told = again.Update(Reopened(2, 1, 1, AfterRace::kNextRace), GameScreen::kOther, false, false);
  Check(told.travel.kind == lobby::TravelKind::kNone && !told.leave, "the next race is not out yet");
  Check(again.Update(racing, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "the same race is not a new one");
  online::Snapshot second = RaceRoom(2, 1);
  lobby::CanonicalRace race = RaceOf(2);
  race.generation = 2;
  second.room->rules = lobby::RaceRules(lobby::SetupRules({lobby::kGameTypeStartMenu, 3, 1}), race);
  const lobby::FlowResult next = again.Update(second, GameScreen::kOther, false, false);
  Check(next.travel.kind == lobby::TravelKind::kRace && next.travel.race.generation == 2, "the new one starts from the race map");

  // The host's side.
  const online::Snapshot hosting = RaceRoom(2, 0);
  lobby::LobbyFlow host;
  host.Update(hosting, GameScreen::kOther, false, false);
  host.NoteContinuing();
  Check(!host.Update(hosting, GameScreen::kMainMenu, true, false).leave, "a host going on does not leave while the room catches up");
  Check(!host.Update(Reopened(2, 0, 1, AfterRace::kCharacterSelect), GameScreen::kCharacter, true, false).leave, "nor once it did");
  Check(!host.Update(Room(2, 0), GameScreen::kCharacter, true, false).leave, "the session is a lobby again");
  lobby::LobbyFlow left;
  left.Update(hosting, GameScreen::kOther, false, false);
  Check(left.Update(hosting, GameScreen::kMainMenu, true, false).leave, "with nowhere to go together the host leaves");
  lobby::LobbyFlow local;
  local.Update(hosting, GameScreen::kOther, false, false);
  local.Reset();
  Check(local.Update({}, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "a local race is left alone");

  // Leaving and losing the host still work from the results.
  lobby::LobbyFlow gone;
  raced(gone);
  Check(gone.Update({}, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu, "a host gone from the results sends guests home");
  Check(lobby::kPanelOfSeat[0] == 0 && lobby::kPanelOfSeat[1] == 1, "the panels did not move");
}

void TestLeavingARaceFlow() {
  using menu::GameScreen;
  online::Snapshot none;
  const auto home = [](const lobby::FlowResult& flow) { return flow.travel.kind == lobby::TravelKind::kMainMenu; };

  // Guest: the whole way in, with everything that must not read as a departure.
  const online::Snapshot open = Room(2, 1);
  const online::Snapshot racing = RaceRoom(2, 1);
  lobby::LobbyFlow guest;
  guest.Update(open, GameScreen::kMainMenu, true, false);
  guest.Update(open, GameScreen::kCharacter, true, false);
  const lobby::FlowResult start = guest.Update(racing, GameScreen::kCharacter, true, false);
  Check(start.travel.kind == lobby::TravelKind::kRace && !start.leave, "race start is a travel, not a leave");
  for (const GameScreen screen : {GameScreen::kOther, GameScreen::kTrack, GameScreen::kOther}) {
    const lobby::FlowResult loading = guest.Update(racing, screen, false, false);
    Check(!loading.leave && loading.travel.kind == lobby::TravelKind::kNone, "race travel and loading are not a leave");
  }
  online::Snapshot flag = racing;
  flag.room->seats[0].ready = false;
  flag.room->seats[0].pick = PickState::kNone;
  Check(!guest.Update(flag, GameScreen::kOther, false, false).leave, "and neither is any change to the host's seat");

  const lobby::FlowResult over = guest.Update(none, GameScreen::kOther, false, false);
  Check(home(over) && !over.leave, "the host gone, the guest is taken out of the race");
  Check(!home(guest.Update(none, GameScreen::kOther, false, false)), "once");

  // A guest whose own game went back to the menus.
  lobby::LobbyFlow back;
  back.Update(open, GameScreen::kMainMenu, true, false);
  back.Update(racing, GameScreen::kCharacter, true, false);
  back.Update(racing, GameScreen::kOther, false, false);
  const lobby::FlowResult gone = back.Update(racing, GameScreen::kMainMenu, true, false);
  Check(gone.leave && std::string(gone.source) == "race_return", "a guest that left the race leaves the session");
  Check(home(back.Update(none, GameScreen::kMainMenu, true, false)), "and ends in the menu");

  // The real run: the host's game goes back to its menus and the session has to end with it, not stay racing.
  const online::Snapshot hosting = RaceRoom(2, 0);
  lobby::LobbyFlow host;
  Check(!host.Update(hosting, GameScreen::kTrack, true, false, true).leave, "the host's start is no leave");
  Check(!host.Update(hosting, GameScreen::kOther, false, false).leave, "nor is racing");
  const lobby::FlowResult left = host.Update(hosting, GameScreen::kMainMenu, true, false);
  Check(left.leave && std::string(left.source) == "race_return", "a host back in the menus closes the room for the guests");
  Check(!home(host.Update(none, GameScreen::kMainMenu, true, false)), "its game is already on its way: no second travel over it");

  // A host whose guest left is still in its race.
  online::Snapshot alone = RaceRoom(2, 0);
  alone.room->seats[1].state = online::SeatState::kEmpty;
  alone.room->players = 1;
  lobby::LobbyFlow stays;
  stays.Update(hosting, GameScreen::kOther, false, false);
  const lobby::FlowResult kept = stays.Update(alone, GameScreen::kOther, false, false);
  Check(!kept.leave && !home(kept), "the host stays when a guest leaves");
  Check(lobby::MakePlan(alone, false, {}).local_seat == 0 && !lobby::MakePlan(alone, false, {}).remote[1], "with that seat's kart nobody's");
  Check(lobby::kPanelOfSeat[0] == 0 && lobby::kPanelOfSeat[1] == 1, "the panels did not move");
}

void TestRaceRules() {
  const lobby::CanonicalRace race = RaceOf(4);
  const online::RoomRules rules = lobby::RaceRules(lobby::SetupRules({lobby::kGameTypeStartMenu, 0, 0}), race);
  Check(rules.size() <= 16, "within the service's rule limit, setup included");
  bool bounded = true;
  for (const auto& [key, value] : rules) {
    bounded = bounded && key.size() <= 16 && value >= 0 && value <= 0xFFFF &&
              std::all_of(key.begin(), key.end(), [](char c) { return c == '_' || (c >= 'a' && c <= 'z'); });
  }
  Check(bounded, "every rule is one the service accepts");
  const auto back = lobby::RaceOfRules(rules);
  Check(back && back->characters == race.characters && back->seats == race.seats && back->bots == race.bots && back->teams == race.teams &&
            back->total == race.total && back->humans == race.humans && back->generation == race.generation,
        "rules round trip");
  Check(lobby::SetupOfRules(rules).has_value(), "and keep the setup");
  Check(lobby::RaceCommand(*back, 2, 0) == lobby::RaceCommand(race, 2, 0), "the same command comes out");
  Check(!lobby::RaceOfRules({}), "a room without a race has none");

  const auto with = [&](const char* key, int value) {
    online::RoomRules changed = rules;
    for (auto& rule : changed) {
      if (rule.first == key) rule.second = value;
    }
    return lobby::RaceOfRules(changed).has_value();
  };
  Check(!with("race", 0), "generation 0 is no race");
  Check(!with("race_track", 20), "unknown track");
  Check(!with("race_mode", 7), "unknown mode");
  Check(!with("race_ai", 3), "unknown difficulty");
  Check(!with("race_humans", 1), "too few humans");
  Check(!with("race_humans", 5), "too many humans");
  Check(!with("race_karts", 9), "too many karts");
  Check(!with("race_chars_low", 0xCCCC), "unknown character");
  Check(!with("race_bots", 0xFF), "a human seat cannot be a bot");
  Check(!with("race_teams", 0xFFFF), "teams beyond the karts");
  Check(!with("race_seats", 0), "one seat twice");
  Check(!with("race_seats", 0xFFF), "a seat that does not exist");
}

online::Snapshot Racing(int seats, int mine, online::RoomState room_state = online::RoomState::kRacing, int generation = 1) {
  online::Snapshot state = Room(seats, mine);
  lobby::CanonicalRace race = RaceOf(seats);
  race.generation = generation;
  state.room->rules = lobby::RaceRules(state.room->rules, race);
  state.room->state = room_state;
  return state;
}

void TestGuestRaceFlow() {
  using menu::GameScreen;
  lobby::LobbyFlow flow;
  const online::Snapshot lobby_state = Room(2, 1);
  flow.Update(lobby_state, GameScreen::kMainMenu, true, false);
  flow.Update(lobby_state, GameScreen::kCharacter, true, false);

  const lobby::FlowResult starting = flow.Update(Racing(2, 1, online::RoomState::kStarting), GameScreen::kCharacter, true, false);
  Check(starting.travel.kind == lobby::TravelKind::kNone && !starting.leave, "a guest waits for the room, with no timer of its own");
  const lobby::FlowResult go = flow.Update(Racing(2, 1), GameScreen::kOther, true, false);
  Check(go.travel.kind == lobby::TravelKind::kRace, "racing sends the guest into the race");
  Check(go.travel.local_seat == 1 && !go.leave, "as seat 1, still in the session");
  Check(lobby::TravelCommand(go.travel, 0).rfind("start PistaBrasilM_Circuito0?", 0) == 0, "to the host's map, not to StartMenu");

  Check(flow.Update(Racing(2, 1), GameScreen::kOther, true, false).travel.kind == lobby::TravelKind::kNone, "a duplicate is ignored");
  Check(flow.Update(Racing(2, 1, online::RoomState::kRacing, 0 + 1), GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone,
        "as is the same race in gameplay");
  const lobby::FlowResult in_race = flow.Update(Racing(2, 1), GameScreen::kOther, false, false);
  Check(!in_race.leave && in_race.travel.kind == lobby::TravelKind::kNone, "the world going away is nothing to the session");

  const online::Snapshot none;
  Check(flow.Update(none, GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kMainMenu, "a real host departure ends the race");

  lobby::LobbyFlow fresh;
  fresh.Update(Racing(2, 1, online::RoomState::kRacing, 5), GameScreen::kOther, true, false);
  Check(fresh.Update(Racing(2, 1, online::RoomState::kRacing, 4), GameScreen::kOther, true, false).travel.kind == lobby::TravelKind::kNone,
        "a stale generation is ignored");

  lobby::LobbyFlow bad;
  online::Snapshot broken = Racing(2, 1);
  for (auto& rule : broken.room->rules) {
    if (rule.first == "race_track") rule.second = 99;
  }
  const lobby::FlowResult refused = bad.Update(broken, GameScreen::kCharacter, true, false);
  Check(refused.leave && refused.travel.kind == lobby::TravelKind::kNone, "a malformed race is left, never loaded");
  Check(refused.note == "race aborted reason=race_payload_invalid", "with its reason");

  lobby::LobbyFlow outsider;
  Check(outsider.Update(Racing(2, 3), GameScreen::kCharacter, true, false).leave, "a race without this seat is not loaded");

  lobby::LobbyFlow third;
  const lobby::FlowResult c = third.Update(Racing(3, 2), GameScreen::kCharacter, true, false);
  Check(c.travel.local_seat == 2 && Load(lobby::TravelCommand(c.travel, 0)).local_slot == 2, "C is seat 2 in kart slot 2");
}

void TestHostRaceFlow() {
  using menu::GameScreen;
  lobby::LobbyFlow flow;
  Check(!flow.Update(Racing(2, 0), GameScreen::kOther, false, false).leave, "loading is not leaving");
  Check(!flow.Update(Racing(2, 0), GameScreen::kTrack, true, false, true).leave, "nor is the track screen of a start in progress");
  Check(!flow.Update(Room(2, 0), GameScreen::kCompetition, true, false, true).leave, "and a start in progress holds the session");
  Check(flow.Update(Racing(2, 0), GameScreen::kOther, false, false).travel.kind == lobby::TravelKind::kNone, "the host never follows itself");

  lobby::HostRaceStart start;
  online::Snapshot state = Room(2, 0);
  Check(!start.active() && !start.Update(state, 0).publish, "nothing before a race is held");
  start.Begin(0);
  Check(start.active(), "a held race is in progress");
  Check(!start.Update(state, 1).publish, "the paths come first");
  state.paths_ready = true;
  Check(start.Update(state, 2).publish, "then the host publishes");
  Check(start.Update(state, 2.1).publish, "until the service takes it");
  start.NotePublished();
  Check(!start.Update(state, 3).publish && start.active(), "once");
  state.room->state = online::RoomState::kStarting;
  Check(!start.Update(state, 4).commit, "starting is not the commit");
  state.room->state = online::RoomState::kRacing;
  Check(start.Update(state, 5).commit, "racing is");
  Check(!start.active() && !start.Update(state, 6).commit, "and only once");

  lobby::HostRaceStart slow;
  slow.Begin(0);
  online::Snapshot stuck = Room(2, 0);
  Check(!slow.Update(stuck, lobby::kRaceStartSeconds - 0.1).abort, "inside the budget it waits");
  const lobby::RaceStep late = slow.Update(stuck, lobby::kRaceStartSeconds);
  Check(late.abort && std::string(late.abort) == "race_transition_timeout", "the budget ends it, by name");
  Check(!slow.active(), "once");

  lobby::HostRaceStart lost;
  lost.Begin(0);
  const lobby::RaceStep gone = lost.Update(Room(1, 0), 1);
  Check(gone.abort && std::string(gone.abort) == "peer_lost", "a host left alone does not race locally by accident");
  lobby::HostRaceStart dropped;
  dropped.Begin(0);
  Check(dropped.Update({}, 1).abort != nullptr, "nor does a host whose session is gone");
}

void TestHostOnAnotherPad() {
  // A host whose pad is not pad 0: the URL names panels, so the released race has to name pads.
  const lobby::CanonicalRace race = RaceOf(2);
  const std::string released = lobby::RaceCommand(race, 0, 1);
  Check(released.find("PlayersArray=1-0-99") != std::string::npos, "the host's kart stays on the host's real pad");
  lobby::Bridge bridge;
  bridge.Publish(lobby::MakePlan(Room(2, 0), false, {}));
  bridge.SetPadSeats({0, 1, -1, -1}, 1);
  bridge.HoldRace(kRealUrl);
  Check(bridge.TakeHeldRace()->physical_pad == 1, "the host's pad travels with the held race");
  bridge.ReleaseRace(released);
  Check(bridge.TakeReleasedRace() == released, "and the told race is the one that goes out");
}

void TestHoldingTheRace() {
  lobby::Bridge bridge;
  Check(!bridge.HoldRace(kRealUrl), "with no session the race is the game's");
  bridge.Publish(lobby::MakePlan(Room(1, 0), false, {}));
  Check(!bridge.HoldRace(kRealUrl), "nor with nobody else in it");
  bridge.Publish(lobby::MakePlan(Room(2, 1), false, {}));
  Check(!bridge.HoldRace(kRealUrl), "a guest never holds");
  bridge.Publish(lobby::MakePlan(Room(2, 0), false, {}));
  bridge.SetPadSeats({0, 1, -1, -1});
  Check(bridge.HoldRace(kRealUrl), "a host with a guest holds");
  const auto held = bridge.TakeHeldRace();
  Check(held && held->command == kRealUrl && held->seats[1] == 1, "the command and who holds each pad");
  Check(!bridge.TakeHeldRace(), "once");
  Check(bridge.TakeReleasedRace().empty(), "nothing goes out before the commit");
  bridge.ReleaseRace();
  Check(bridge.TakeReleasedRace() == kRealUrl, "the commit releases it");
  Check(bridge.TakeReleasedRace().empty(), "once");
  bridge.HoldRace(kRealUrl);
  bridge.DropRace();
  Check(!bridge.TakeHeldRace() && bridge.TakeReleasedRace().empty(), "an aborted race is gone");
}

void TestRaceStateOffset() {
  namespace layout = eck::online_probe::layout;
  Check(layout::kMainGameRaceState == 0x3E0, "RaceState past the three delegates before it");
  Check(layout::kGamePrimaryController == 0x390, "PrimaryControllerId past GameInfo's delegate");
}

void TestBridge() {
  lobby::Bridge bridge;
  Check(!bridge.RuleFor(9).virtual_pad && bridge.RuleFor(9).allow == 0xFFFF, "an unknown pad has the default rule");
  bridge.NoteOwnBack(true, false);
  Check(!bridge.TakeBackPress(), "B that closes the panel is the game's");
  bridge.NoteOwnBack(true, true);
  Check(!bridge.TakeBackPress(), "and holding it past the close does not leave");
  bridge.NoteOwnBack(false, true);
  bridge.NoteOwnBack(true, true);
  Check(bridge.TakeBackPress() && !bridge.TakeBackPress(), "a new press on a closed panel does, once");

  bridge.RequestTravel({lobby::TravelKind::kMainMenu, {}});
  Check(bridge.TakeTravel().kind == lobby::TravelKind::kMainMenu, "a travel is handed over");
  Check(bridge.TakeTravel().kind == lobby::TravelKind::kNone, "once");
}

void TestPacer() {
  lobby::PickPacer pacer;
  lobby::PanelView panel{PickState::kSelecting, 1};
  Check(pacer.Due(0.0, panel).has_value(), "the first pick goes out");
  Check(!pacer.Due(0.01, panel), "the same pick does not");
  panel.character = 2;
  Check(!pacer.Due(0.05, panel), "a quick change waits");
  panel.character = 3;
  const auto due = pacer.Due(0.2, panel);
  Check(due && due->character == 3, "and only the latest goes out");
  pacer.Reset();
  Check(pacer.Due(0.21, panel).has_value(), "a new session starts over");
}

}  // namespace

int main() {
  TestRegionBounds();
  TestInspectionByPackage();
  TestMovieSearch();
  TestTrackerAcrossReopenedMovies();
  TestPanelScan();
  TestSetupCapture();
  TestMovieArguments();
  TestInviteDoesNotDependOnPanels();
  TestSetup();
  TestTravelCommands();
  TestTwoPlayers();
  TestBackingOutOfAPick();
  TestUnreachableCharacterWaits();
  TestThreePlayers();
  TestFourPlayers();
  TestRemotePanelsSwapCharacters();
  TestCanonicalPanels();
  TestOwnPickBelongsToItsSession();
  TestLocalGameIsUntouched();
  TestPadRules();
  TestPlan();
  TestGuestFlow();
  TestHostFlow();
  TestHostLeavesAndPanelsClose();
  TestFinalRaceUrl();
  TestKartOwnership();
  TestGuestConfirmation();
  TestCommandLength();
  TestRacePads();
  TestRaceRules();
  TestGuestRaceFlow();
  TestHostRaceFlow();
  TestHoldingTheRace();
  TestHostOnAnotherPad();
  TestLeaveButtons();
  TestRaceWatch();
  TestLeavingARaceFlow();
  TestResultScreens();
  TestAfterTheRaceFlow();
  TestRaceStateOffset();
  TestBridge();
  TestPacer();
  return online_test::Finish();
}
