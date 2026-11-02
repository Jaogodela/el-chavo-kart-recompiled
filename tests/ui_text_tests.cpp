#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "language_setting.h"
#include "ui_text.h"

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

}  // namespace

int main() {
  const eck::UiTextTable& table = eck::PortugueseBrazilUiText();
  for (const std::string& error : table.errors) {
    Check(false, "pt-BR table " + error);
  }
  Check(table.rows.size() > 150, "the pt-BR table has its rows");
  std::set<std::string> english;
  for (const eck::UiTextRow& row : table.rows) {
    Check(!row.text.empty(), "row has a translation: " + row.english);
    Check(english.insert(row.english).second, "row is listed once: " + row.english);
  }

  Check(std::string(eck::kDefaultLanguage) == eck::kPortugueseBrazilKey, "Portuguese (Brazil) is the default");
  Check(std::string(eck::LanguageFor("unknown").key) == eck::kPortugueseBrazilKey, "an unknown key selects the default");
  Check(eck::LanguageFor("es-ES").xconfig_country == 31 && eck::LanguageFor("es-MX").xconfig_country == 71,
        "Spanish variants differ by country");

  eck::SetActiveUiLanguage("pt-BR");
  Check(std::strcmp(eck::Tr("Play"), "Jogar") == 0, "pt-BR translates");
  Check(std::strcmp(eck::Tr("No such text"), "No such text") == 0, "missing text stays English");
  eck::SetActiveUiLanguage("en-US");
  Check(std::strcmp(eck::Tr("Play"), "Play") == 0, "English keeps the English text");
  eck::SetActiveUiLanguage("es-MX");
  Check(std::strcmp(eck::Tr("Play"), "Play") == 0, "Spanish keeps the launcher in English");

  std::printf("%s\n", g_failures ? "FAILED" : "passed");
  return g_failures ? 1 : 0;
}
