#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace eck {

// The text of the active launcher language for an English UI string. English text with no translation is returned as
// is, so a missing entry never shows an empty or foreign string. The result outlives the call.
const char* Tr(const char* english);
std::string Tr(const std::string& english);

// The launcher and the overlays follow the game language: Portuguese (Brazil) shows the pt-BR table, every other
// language keeps English. Set from the saved setting at start and by the launcher as the choice changes.
void SetActiveUiLanguage(std::string_view key);
bool PortugueseBrazilUiSelected();

struct UiTextRow {
  std::string english;
  std::string text;
};

struct UiTextTable {
  std::vector<UiTextRow> rows;
  std::vector<std::string> errors;
};

// Rows are `english<TAB>text`; both columns accept the escapes \n, \t and \.
UiTextTable ParseUiTextTable(std::string_view tsv);

const UiTextTable& PortugueseBrazilUiText();

}  // namespace eck
