#pragma once

#include <cstdint>
#include <string_view>

namespace eck {

inline constexpr char kLanguageSetting[] = "eck_language";
inline constexpr char kPortugueseBrazilKey[] = "pt-BR";
inline constexpr char kDefaultLanguage[] = "pt-BR";
inline constexpr char kLanguageLabel[] = "Language";
inline constexpr char kLanguageHelp[] =
    "The language of the game's text and menus, and of this launcher. Applied when the game starts.";

// The title maps XGetLanguage() and XGetLocale() to its package suffix: Portuguese gives PTB, Spanish gives ESN
// only for the Spain locale and ESM otherwise, English gives INT. user_country is the XConfig country byte that
// XGetLocale() translates through the title's own table (31 Spain, 71 Mexico, 13 Brazil, 103 United States).
struct GameLanguage {
  const char* key;
  const char* label;
  const char* package;
  uint32_t xconfig_language;
  uint32_t xconfig_country;
};

inline constexpr GameLanguage kGameLanguages[] = {
    {"pt-BR", "Portugu\xC3\xAAs (Brasil)", "PTB", 9, 13},
    {"en-US", "English", "INT", 1, 103},
    {"es-MX", "Espa\xC3\xB1ol (Am\xC3\xA9rica Latina)", "ESM", 5, 71},
    {"es-ES", "Espa\xC3\xB1ol (Espa\xC3\xB1" "a)", "ESN", 5, 31},
};

// An unknown value selects the default language.
constexpr const GameLanguage& LanguageFor(std::string_view key) {
  for (const GameLanguage& language : kGameLanguages) {
    if (key == language.key) {
      return language;
    }
  }
  return kGameLanguages[0];
}

}  // namespace eck
