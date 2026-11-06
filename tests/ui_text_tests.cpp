#include <cstdio>
#include <cstring>
#include <iterator>
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

  Check(std::string(eck::kDefaultLanguage) == eck::kEnglishKey, "English is the default");
  Check(std::string(eck::LanguageFor("unknown").key) == eck::kEnglishKey, "an unknown key selects the default");
  Check(std::string(eck::LanguageFor("").key) == eck::kEnglishKey, "a clean setting selects English");
  Check(std::string(eck::LanguageFor(eck::kDefaultLanguage).label) == "English", "the default is labelled English");

  const eck::GameLanguage& english_language = eck::LanguageFor("en-US");
  Check(std::string(english_language.package) == "INT" && english_language.xconfig_language == 1 &&
            english_language.xconfig_country == 103,
        "English maps to INT, language 1, country 103");
  const eck::GameLanguage& portuguese = eck::LanguageFor("pt-BR");
  Check(std::string(portuguese.key) == eck::kPortugueseBrazilKey && std::string(portuguese.package) == "PTB" &&
            portuguese.xconfig_language == 9 && portuguese.xconfig_country == 13,
        "Portuguese (Brazil) stays PTB, language 9, country 13");
  Check(std::string(eck::LanguageFor("es-MX").package) == "ESM" && eck::LanguageFor("es-MX").xconfig_language == 5,
        "Latin American Spanish mapping is unchanged");
  Check(std::string(eck::LanguageFor("es-ES").package) == "ESN" && eck::LanguageFor("es-ES").xconfig_language == 5,
        "Spain Spanish mapping is unchanged");
  Check(eck::LanguageFor("es-ES").xconfig_country == 31 && eck::LanguageFor("es-MX").xconfig_country == 71,
        "Spanish variants differ by country");

  Check(std::size(eck::kGameLanguages) == 4 && std::string(eck::kGameLanguages[0].key) == "pt-BR" &&
            std::string(eck::kGameLanguages[1].key) == "en-US",
        "every language stays selectable in the same order");

  Check(!eck::PortugueseBrazilUiSelected(), "the launcher is English before any language is applied");
  eck::SetActiveUiLanguage(eck::kDefaultLanguage);
  Check(std::strcmp(eck::Tr("Play"), "Play") == 0, "the launcher shows English on clean settings");
  Check(!eck::PortugueseBrazilUiSelected(), "clean settings do not select Portuguese");
  eck::SetActiveUiLanguage(eck::LanguageFor("pt-BR").key);
  Check(eck::PortugueseBrazilUiSelected() && std::strcmp(eck::Tr("Play"), "Jogar") == 0,
        "a saved Portuguese (Brazil) preference still shows Portuguese");
  eck::SetActiveUiLanguage(eck::LanguageFor("en-US").key);
  Check(!eck::PortugueseBrazilUiSelected(), "a saved English preference shows English");

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
