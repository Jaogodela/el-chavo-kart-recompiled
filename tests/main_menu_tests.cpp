#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "main_menu.h"
#include "main_menu_swf.h"
#include "sha256.h"

namespace {

using Bytes = std::vector<std::uint8_t>;

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void PutLe16(Bytes& out, std::uint32_t v) {
  out.push_back(std::uint8_t(v));
  out.push_back(std::uint8_t(v >> 8));
}
void PutLe32(Bytes& out, std::uint32_t v) {
  PutLe16(out, v & 0xFFFF);
  PutLe16(out, v >> 16);
}
void PutCString(Bytes& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
  out.push_back(0);
}
std::uint32_t Le16(const Bytes& d, std::size_t p) { return d[p] | (d[p + 1] << 8); }
std::uint32_t Le32(const Bytes& d, std::size_t p) { return Le16(d, p) | (Le16(d, p + 2) << 16); }

Bytes MakeTag(std::uint16_t code, const Bytes& body) {
  Bytes tag;
  if (body.size() < 63) {
    PutLe16(tag, (code << 6) | body.size());
  } else {
    PutLe16(tag, (code << 6) | 63);
    PutLe32(tag, std::uint32_t(body.size()));
  }
  tag.insert(tag.end(), body.begin(), body.end());
  return tag;
}

struct WalkedTag {
  std::uint16_t code;
  std::size_t header;
  std::size_t body;
  std::size_t size;
};

bool Walk(const Bytes& d, std::size_t begin, std::size_t end, std::vector<WalkedTag>& out) {
  std::size_t pos = begin;
  while (pos + 2 <= end) {
    const std::uint32_t head = Le16(d, pos);
    std::size_t length = head & 63;
    std::size_t header_size = 2;
    if (length == 63) {
      length = Le32(d, pos + 2);
      header_size = 6;
    }
    if (pos + header_size + length > end) {
      return false;
    }
    out.push_back({std::uint16_t(head >> 6), pos, pos + header_size, length});
    pos += header_size + length;
    if ((head >> 6) == 0) {
      return pos == end;
    }
  }
  return false;
}

constexpr std::size_t kTagsBegin = 8 + 9 + 4;

Bytes ItemPlacement(std::uint16_t depth, int y, const std::string& name, const std::string& command) {
  Bytes actions;
  actions.push_back(0x96);
  Bytes push;
  push.push_back(0);
  PutCString(push, "_actionCommand");
  push.push_back(0);
  PutCString(push, command);
  PutLe16(actions, std::uint32_t(push.size()));
  actions.insert(actions.end(), push.begin(), push.end());
  actions.push_back(0x1D);
  actions.push_back(0);

  Bytes body;
  body.push_back(0xA6);
  PutLe16(body, depth);
  PutLe16(body, 22);
  // No scale or rotation, then a 15-bit translation of x = 838 and y: 37 bits padded to 5 bytes.
  const std::uint64_t packed = (std::uint64_t(15) << 33) | (std::uint64_t(838) << 18) | (std::uint64_t(y) << 3);
  for (int i = 4; i >= 0; --i) {
    body.push_back(std::uint8_t(packed >> (8 * i)));
  }
  PutCString(body, name);
  PutLe16(body, 0);
  PutLe32(body, 0x00040000);
  PutLe32(body, 0x00040000);
  PutLe32(body, std::uint32_t(actions.size()));
  body.insert(body.end(), actions.begin(), actions.end());
  PutLe32(body, 0);
  return MakeTag(26, body);
}

struct Synthetic {
  Bytes bytes;
  std::vector<Bytes> original_items;
};

Synthetic BuildOriginal(std::uint32_t sprite_length_extra = 0) {
  Synthetic result;
  Bytes sprite_body;
  PutLe16(sprite_body, 99);
  PutLe16(sprite_body, 1);
  const int ys[] = {4972, 6549, 8126, 9703, 11282};
  for (std::size_t i = 0; i < 5; ++i) {
    Bytes tag = ItemPlacement(std::uint16_t(150 + 8 * i), ys[i], std::string(eck::kMainMenuItems[i].name),
                              std::string(eck::kMainMenuItems[i].command));
    result.original_items.push_back(tag);
    sprite_body.insert(sprite_body.end(), tag.begin(), tag.end());
  }
  const Bytes show = MakeTag(1, {});
  const Bytes end = MakeTag(0, {});
  sprite_body.insert(sprite_body.end(), show.begin(), show.end());
  sprite_body.insert(sprite_body.end(), end.begin(), end.end());

  Bytes class_body;
  PutLe16(class_body, 130);
  PutCString(class_body, "CKMainMenuRoot");
  PutCString(class_body, "COMMAND_GOTO_ACHIEVEMENTS");

  Bytes tags = MakeTag(39, sprite_body);
  if (sprite_length_extra != 0) {
    const std::uint32_t length = Le32(tags, 2) + sprite_length_extra;
    for (int i = 0; i < 4; ++i) {
      tags[2 + i] = std::uint8_t(length >> (8 * i));
    }
  }
  const Bytes init = MakeTag(59, class_body);
  tags.insert(tags.end(), init.begin(), init.end());
  tags.insert(tags.end(), show.begin(), show.end());
  tags.insert(tags.end(), end.begin(), end.end());

  Bytes file = {'G', 'F', 'X', 9, 0, 0, 0, 0};
  // RECT with 16-bit fields 0, 25600, 0, 14400: 69 bits padded to 9 bytes.
  std::vector<bool> rect;
  const auto put_bits = [&rect](std::uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      rect.push_back((value >> i) & 1);
    }
  };
  put_bits(16, 5);
  for (const std::uint32_t field : {0u, 25600u, 0u, 14400u}) {
    put_bits(field, 16);
  }
  rect.resize(72);
  for (std::size_t i = 0; i < 72; i += 8) {
    std::uint8_t byte = 0;
    for (std::size_t bit = 0; bit < 8; ++bit) {
      byte = std::uint8_t((byte << 1) | (rect[i + bit] ? 1 : 0));
    }
    file.push_back(byte);
  }
  PutLe16(file, 0x3C00);
  PutLe16(file, 1);
  file.insert(file.end(), tags.begin(), tags.end());
  for (int i = 0; i < 4; ++i) {
    file[4 + i] = std::uint8_t(file.size() >> (8 * i));
  }
  result.bytes = std::move(file);
  return result;
}

eck::SwfFingerprint FingerprintOf(const Bytes& bytes, std::string& storage) {
  storage = eck::Sha256Hex(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  return {bytes.size(), storage};
}

std::size_t CountOf(const Bytes& haystack, const Bytes& needle) {
  std::size_t count = 0;
  for (auto it = haystack.begin(); (it = std::search(it, haystack.end(), needle.begin(), needle.end())) != haystack.end();
       ++it) {
    ++count;
  }
  return count;
}

Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }

void TestRows(const Bytes& original, const Bytes& patched, const std::string& context);

void TestMenuTable() {
  const auto& items = eck::kMainMenuItems;
  Check(items.size() == 6, "the main menu has six rows");
  const char* order[] = {"mainMenu_cups",   "mainMenu_challenges", "mainMenu_exhibition",
                         "mainMenu_extras", "mainMenu_achievements", "mainMenu_exit"};
  for (std::size_t i = 0; i < 6; ++i) {
    Check(items[i].name == order[i], std::string("row order ") + order[i]);
  }
  Check(items[2].command == "Exhibition", "Free Race / Multiplayer still maps to Exhibition");
  Check(items[0].command == "Cups" && items[1].command == "Challenges" && items[3].command == "Extras" &&
            items[4].command == "Achievements",
        "the five original actions are unchanged");
  std::size_t exits = 0;
  for (const auto& item : items) {
    exits += eck::IsExitCommand(item.command) ? 1 : 0;
  }
  Check(exits == 1 && eck::IsExitCommand(items[5].command), "only the last row maps to the exit command");
  Check(eck::IsExitCommand("ECK_EXIT_GAME"), "the exit command matches exactly");
  for (const char* other : {"", "Cups", "Exhibition", "SelectMenu", "eck_exit_game", "ECK_EXIT_GAME ", "ECK_EXIT_GAM",
                            "mainMenu_exit"}) {
    Check(!eck::IsExitCommand(other), std::string("not an exit command: ") + other);
  }
}

void TestReservedKeys() {
  using eck::ReservedKey;
  Check(eck::ClassifyReservedKey("ECK_EXIT_GAME") == ReservedKey::kExit, "exit key");
  Check(eck::ClassifyReservedKey("ECK_ONLINE_ENTER") == ReservedKey::kOnlineEnter, "Online enter key");
  Check(eck::ClassifyReservedKey("ECK_ONLINE_MENU") == ReservedKey::kOnlineMenu, "menu loaded key");
  for (const char* key : {"", "ECK_", "ECK_ONLINE", "ECK_ONLINE_ENTER2", "eck_online_enter", "ECK_ONLINE_ENTE", "Exhibition",
                          "mainMenu_exhibition", "ECK_ONLINE_ROOMS", " ECK_ONLINE_MENU"}) {
    Check(eck::ClassifyReservedKey(key) == ReservedKey::kNone, std::string("unknown key falls through: ") + key);
  }
  Check(eck::kExhibitionCommand == eck::kMainMenuItems[2].command, "the Online request is bound to the Exhibition row");
  Check(!eck::MainMenuTextOverride("PTB", "ECK_ONLINE_ENTER") && !eck::MainMenuTextOverride("INT", "ECK_ONLINE_MENU"),
        "reserved keys have no text override");
  Check(eck::MainMenuTextOverride("PTB", "mainMenu_exhibition").has_value(), "Free Race / Multiplayer text still overridden");
}

void TestText() {
  struct Expected {
    const char* package;
    const char16_t* race;
    const char16_t* race_help;
    const char16_t* exit;
    const char16_t* exit_help;
  };
  const Expected expected[] = {
      {"INT", u"Free Race / Multiplayer", u"Choose the game mode, players, race and track to begin", u"Exit Game",
       u"Exit the game"},
      {"ESM", u"Carrera Libre / Multijugador", u"Elige el modo de juego, jugadores, carrera y pista para comenzar",
       u"Salir del Juego", u"Salir del juego"},
      {"ESN", u"Carrera Libre / Multijugador", u"Elige el modo de juego, jugadores, carrera y pista para comenzar",
       u"Salir del Juego", u"Salir del juego"},
      {"PTB", u"Corrida Livre / Multiplayer", u"Escolha o modo de jogo, jogadores, corrida e pista para começar",
       u"Sair do Jogo", u"Sair do jogo"},
  };
  for (const Expected& e : expected) {
    const std::string package = e.package;
    Check(eck::MainMenuTextOverride(package, "mainMenu_exhibition") == std::u16string_view(e.race),
          package + " race label");
    Check(eck::MainMenuTextOverride(package, "mainMenu_exhibition_description") == std::u16string_view(e.race_help),
          package + " race description");
    Check(eck::MainMenuTextOverride(package, "mainMenu_exit") == std::u16string_view(e.exit), package + " exit label");
    Check(eck::MainMenuTextOverride(package, "mainMenu_exit_description") == std::u16string_view(e.exit_help),
          package + " exit description");
  }
  for (const auto& entry : eck::kMainMenuText) {
    for (const auto text : entry.text) {
      Check(!text.empty(), "no empty text");
      Check(text.find(u"Local") == std::u16string_view::npos && text.find(u"local") == std::u16string_view::npos &&
                text.find(u"Offline") == std::u16string_view::npos,
            "no local-only wording");
    }
  }
  for (const char* key : {"mainMenu_cups", "mainMenu_extras", "mainMenu_achievements", "mainMenu_cups_description",
                          "mainMenu_exhibition ", "", "ECK_EXIT_GAME"}) {
    Check(!eck::MainMenuTextOverride("PTB", key), std::string("falls through: ") + key);
  }
  Check(!eck::MainMenuTextOverride("JPN", "mainMenu_exit"), "an unsupported package falls through");
  Check(!eck::MainMenuTextOverride("", "mainMenu_exit"), "an empty package falls through");
}

void CheckPatched(const Synthetic& original, const Bytes& patched) {
  Check(patched.size() > original.bytes.size(), "the patch grows the movie");
  Check(Le32(patched, 4) == patched.size(), "the movie length field matches the buffer");
  std::vector<WalkedTag> top;
  Check(Walk(patched, kTagsBegin, patched.size(), top), "top-level tags end exactly at the buffer end");
  std::vector<WalkedTag> original_top;
  Walk(original.bytes, kTagsBegin, original.bytes.size(), original_top);
  Check(top.size() == original_top.size() + 1, "exactly one script tag was added");

  const auto sprite = std::find_if(top.begin(), top.end(), [](const WalkedTag& t) { return t.code == 39; });
  Check(sprite != top.end(), "the menu sprite remains");
  if (sprite == top.end()) {
    return;
  }
  std::vector<WalkedTag> inner;
  Check(Walk(patched, sprite->body + 4, sprite->body + sprite->size, inner), "sprite tags end exactly at the sprite end");
  std::size_t rows = 0;
  for (const WalkedTag& tag : inner) {
    if (tag.code != 26) {
      continue;
    }
    const std::string text(patched.begin() + tag.body, patched.begin() + tag.body + tag.size);
    rows += text.find("mainMenu_") != std::string::npos ? 1 : 0;
  }
  Check(rows == 6, "the patched menu has six rows");

  for (const Bytes& item : original.original_items) {
    Check(CountOf(patched, item) == 1, "an original row is unchanged");
  }
  const Bytes exit_action = [] {
    Bytes b = Text("_actionCommand");
    b.push_back(0);
    b.push_back(0);
    const Bytes command = Text("ECK_EXIT_GAME");
    b.insert(b.end(), command.begin(), command.end());
    b.push_back(0);
    return b;
  }();
  Check(CountOf(patched, exit_action) == 1, "exactly one row carries the exit command");
  for (std::size_t i = 0; i < 5; ++i) {
    Bytes b = Text("_actionCommand");
    b.push_back(0);
    b.push_back(0);
    const Bytes command = Text(std::string(eck::kMainMenuItems[i].command));
    b.insert(b.end(), command.begin(), command.end());
    b.push_back(0);
    Check(CountOf(patched, b) == 1, std::string("action intact: ") + std::string(eck::kMainMenuItems[i].command));
  }
  Bytes exit_name = Text("mainMenu_exit");
  exit_name.push_back(0);
  Check(CountOf(patched, exit_name) == 1, "the exit row is named once");
  Check(CountOf(patched, Text("RequestTextFieldUpdate")) == 3,
        "the exit, Online and menu-loaded requests use the text request");
  for (const char* key : {"ECK_ONLINE_ENTER", "ECK_ONLINE_MENU"}) {
    Bytes b = Text(key);
    b.push_back(0);
    Check(CountOf(patched, b) == 1, std::string("the reserved key is sent from one place: ") + key);
  }
  // Local: the original OnGamepadA is stored once and called after the Online request, never replaced.
  Bytes base = Text("ECK_BaseOnGamepadA");
  base.push_back(0);
  Check(CountOf(patched, base) == 2, "the original OnGamepadA is saved once and called once");
  const auto find_at = [&](const Bytes& needle, bool last) {
    auto it = last ? std::find_end(patched.begin(), patched.end(), needle.begin(), needle.end())
                   : std::search(patched.begin(), patched.end(), needle.begin(), needle.end());
    return it == patched.end() ? std::ptrdiff_t(-1) : it - patched.begin();
  };
  Bytes enter = Text("ECK_ONLINE_ENTER");
  enter.push_back(0);
  Check(find_at(enter, false) > 0 && find_at(enter, false) < find_at(base, true),
        "the Online request comes before the call to the original Exhibition handler");
  Bytes exhibition = Text("Exhibition");
  exhibition.push_back(0);
  Check(find_at(exhibition, true) > 0 && find_at(exhibition, true) < find_at(enter, false),
        "the Online request is guarded by the Exhibition command");
}

void TestSwfPatch() {
  const Synthetic original = BuildOriginal();
  std::string sha;
  const eck::SwfFingerprint fingerprint = FingerprintOf(original.bytes, sha);

  const eck::SwfPatchResult applied = eck::PatchMainMenuSwf(original.bytes, fingerprint);
  Check(applied.status == eck::SwfPatchStatus::kApplied, "the expected fingerprint is accepted: " + applied.detail);
  if (applied.status == eck::SwfPatchStatus::kApplied) {
    CheckPatched(original, applied.bytes);
    TestRows(original.bytes, applied.bytes, "synthetic movie");
    const eck::SwfPatchResult twice = eck::PatchMainMenuSwf(applied.bytes, fingerprint);
    Check(twice.status != eck::SwfPatchStatus::kApplied && twice.bytes.empty(),
          "a patched movie is never patched again");
  }

  Bytes changed = original.bytes;
  changed[changed.size() / 2] ^= 1;
  const eck::SwfPatchResult wrong = eck::PatchMainMenuSwf(changed, fingerprint);
  Check(wrong.status == eck::SwfPatchStatus::kFingerprintMismatch && wrong.bytes.empty(),
        "a different movie is rejected");

  Bytes shorter(original.bytes.begin(), original.bytes.end() - 1);
  const eck::SwfPatchResult short_result = eck::PatchMainMenuSwf(shorter, fingerprint);
  Check(short_result.status == eck::SwfPatchStatus::kSizeMismatch && short_result.bytes.empty(),
        "a different size is rejected");

  const eck::SwfPatchResult production = eck::PatchMainMenuSwf(original.bytes);
  Check(production.status == eck::SwfPatchStatus::kSizeMismatch, "the production fingerprint rejects other movies");
  Check(eck::kMainMenuSwfFingerprint.size == 176575 && eck::kMainMenuSwfFingerprint.sha256.size() == 64,
        "the production fingerprint is well formed");

  const Synthetic oversized = BuildOriginal(1000);
  std::string oversized_sha;
  const eck::SwfPatchResult overrun =
      eck::PatchMainMenuSwf(oversized.bytes, FingerprintOf(oversized.bytes, oversized_sha));
  Check(overrun.status == eck::SwfPatchStatus::kUnexpectedStructure && overrun.bytes.empty(),
        "a tag that runs past the buffer is rejected");

  Synthetic missing = BuildOriginal();
  const Bytes name = Text("mainMenu_extras");
  const auto at = std::search(missing.bytes.begin(), missing.bytes.end(), name.begin(), name.end());
  *at = 'x';
  std::string missing_sha;
  const eck::SwfPatchResult no_row = eck::PatchMainMenuSwf(missing.bytes, FingerprintOf(missing.bytes, missing_sha));
  Check(no_row.status == eck::SwfPatchStatus::kUnexpectedStructure && no_row.bytes.empty(),
        "a movie without the expected rows is rejected");
}

void CheckRowOrder(const std::vector<eck::MenuRow>& rows, std::size_t count, const std::string& context) {
  Check(rows.size() == count, context + " has " + std::to_string(count) + " rows");
  for (std::size_t i = 0; i < rows.size() && i < count; ++i) {
    Check(rows[i].name == eck::kMainMenuItems[i].name && rows[i].command == eck::kMainMenuItems[i].command,
          context + " row " + std::to_string(i));
  }
}

void TestRows(const Bytes& original, const Bytes& patched, const std::string& context) {
  const auto before = eck::ListMainMenuRows(original);
  const auto after = eck::ListMainMenuRows(patched);
  Check(before && after, context + " rows can be listed");
  if (!before || !after) {
    return;
  }
  CheckRowOrder(*before, 5, context + " original");
  CheckRowOrder(*after, 6, context + " patched");
  Check(after->back().name == "mainMenu_exit" && after->back().command == "ECK_EXIT_GAME" &&
            after->back().y > (*after)[4].y,
        context + " exit row is last");
  Check((*after)[2].command == "Exhibition", context + " exhibition action unchanged");
  Check(CountOf(patched, Text("ECK_EXIT_GAME")) >= 3, context + " exit command is in the AS2");
}

void TestRealMovie(const char* path) {
  std::ifstream file(path, std::ios::binary);
  const Bytes movie((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const eck::SwfPatchResult result = eck::PatchMainMenuSwf(movie);
  Check(result.status == eck::SwfPatchStatus::kApplied, "the supported movie is accepted: " + result.detail);
  if (result.status != eck::SwfPatchStatus::kApplied) {
    return;
  }
  std::vector<WalkedTag> top;
  Check(Walk(result.bytes, kTagsBegin, result.bytes.size(), top), "real movie: patched tags re-parse");
  Check(Le32(result.bytes, 4) == result.bytes.size(), "real movie: length field");
  TestRows(movie, result.bytes, "real movie");

  Bytes changed = movie;
  changed[movie.size() / 3] ^= 0x10;
  const eck::SwfPatchResult wrong = eck::PatchMainMenuSwf(changed);
  Check(wrong.status == eck::SwfPatchStatus::kFingerprintMismatch && wrong.bytes.empty(),
        "real movie: a changed copy is rejected");
  const Bytes shorter(movie.begin(), movie.end() - 1);
  Check(eck::PatchMainMenuSwf(shorter).status == eck::SwfPatchStatus::kSizeMismatch,
        "real movie: a different size is rejected");
  const Bytes truncated(movie.begin(), movie.begin() + movie.size() / 2);
  std::string sha;
  const eck::SwfPatchResult broken = eck::PatchMainMenuSwf(truncated, FingerprintOf(truncated, sha));
  Check(broken.status == eck::SwfPatchStatus::kUnexpectedStructure && broken.bytes.empty(),
        "real movie: broken structure is rejected under a matching fingerprint");

  std::ofstream out(std::string(path) + ".patched", std::ios::binary);
  out.write(reinterpret_cast<const char*>(result.bytes.data()), std::streamsize(result.bytes.size()));
}

std::string ReadText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void TestHookRegistration() {
  const std::string repo = ECK_REPO_DIR;
  const std::string hooks = ReadText(repo + "/src/main_menu_hooks.cpp");
  Check(!hooks.empty(), "the hook source is readable");
  for (const char* hook : {"REX_HOOK_RAW(sub_82BD99F8)", "REX_HOOK_RAW(sub_82C22F28)", "REX_HOOK_RAW(sub_82A46B68)"}) {
    Check(hooks.find(hook) != std::string::npos, std::string("hook defined: ") + hook);
  }
  const std::string cmake = ReadText(repo + "/CMakeLists.txt");
  Check(cmake.find("src/main_menu_hooks.cpp") != std::string::npos, "the hook source is part of the executable");
  Check(cmake.find("src/hard_exit.cpp") != std::string::npos, "the shared exit is part of the executable");
  const std::string app = ReadText(repo + "/src/el_chavo_kart_app.h");
  Check(app.find("eck::LogMainMenuHooks()") != std::string::npos, "the hook report runs at startup");

  const std::string table = ReadText(repo + "/generated/default/el_chavo_kart_init.cpp");
  if (!table.empty()) {
    for (const char* entry : {"{ 0x82BD99F8, sub_82BD99F8 }", "{ 0x82C22F28, sub_82C22F28 }",
                              "{ 0x82A46B68, sub_82A46B68 }", "{ 0x823B5E08, sub_823B5E08 }",
                              "{ 0x822E80C8, sub_822E80C8 }"}) {
      Check(table.find(entry) != std::string::npos, std::string("recompiled function exists: ") + entry);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  TestMenuTable();
  TestText();
  TestReservedKeys();
  TestSwfPatch();
  TestHookRegistration();
  if (argc > 1) {
    TestRealMovie(argv[1]);
  }
  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
