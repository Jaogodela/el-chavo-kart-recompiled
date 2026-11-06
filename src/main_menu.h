#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace eck {

inline constexpr std::string_view kExitCommand = "ECK_EXIT_GAME";

// Reserved text requests the patched movie sends; the exec hook answers them and every other key is left alone.
inline constexpr std::string_view kOnlineMenuCommand = "ECK_ONLINE_MENU";  // the main-menu movie loaded

struct MainMenuItem {
  std::string_view name;
  std::string_view command;
};

inline constexpr std::array<MainMenuItem, 6> kMainMenuItems{{
    {"mainMenu_cups", "Cups"},
    {"mainMenu_challenges", "Challenges"},
    {"mainMenu_exhibition", "Exhibition"},
    {"mainMenu_extras", "Extras"},
    {"mainMenu_achievements", "Achievements"},
    {"mainMenu_exit", kExitCommand},
}};

inline constexpr std::string_view kExitItemName = "mainMenu_exit";
inline constexpr std::string_view kExitDescriptionKey = "mainMenu_exit_description";

// Index order matches kMenuPackages.
inline constexpr std::array<std::string_view, 4> kMenuPackages{"INT", "ESM", "ESN", "PTB"};

struct MainMenuTextEntry {
  std::string_view key;
  std::array<std::u16string_view, 4> text;
};

inline constexpr std::array<MainMenuTextEntry, 4> kMainMenuText{{
    {"mainMenu_exhibition",
     {u"Free Race / Multiplayer", u"Carrera Libre / Multijugador", u"Carrera Libre / Multijugador",
      u"Corrida Livre / Multiplayer"}},
    {"mainMenu_exhibition_description",
     {u"Choose the game mode, players, race and track to begin",
      u"Elige el modo de juego, jugadores, carrera y pista para comenzar",
      u"Elige el modo de juego, jugadores, carrera y pista para comenzar",
      u"Escolha o modo de jogo, jogadores, corrida e pista para começar"}},
    {kExitItemName, {u"Exit Game", u"Salir del Juego", u"Salir del Juego", u"Sair do Jogo"}},
    {kExitDescriptionKey, {u"Exit the game", u"Salir del juego", u"Salir del juego", u"Sair do jogo"}},
}};

constexpr std::optional<std::size_t> MenuPackageIndex(std::string_view package) {
  for (std::size_t i = 0; i < kMenuPackages.size(); ++i) {
    if (kMenuPackages[i] == package) {
      return i;
    }
  }
  return std::nullopt;
}

// Only the exact main-menu keys are overridden; every other key falls through to the game's own text.
constexpr std::optional<std::u16string_view> MainMenuTextOverride(std::string_view package, std::string_view key) {
  const std::optional<std::size_t> language = MenuPackageIndex(package);
  if (!language) {
    return std::nullopt;
  }
  for (const MainMenuTextEntry& entry : kMainMenuText) {
    if (entry.key == key) {
      return entry.text[*language];
    }
  }
  return std::nullopt;
}

constexpr bool IsExitCommand(std::string_view key) { return key == kExitCommand; }

enum class ReservedKey { kNone, kExit, kOnlineMenu };

// Exact matches only: a key that merely starts with ECK_ is an ordinary key.
constexpr ReservedKey ClassifyReservedKey(std::string_view key) {
  if (key == kExitCommand) return ReservedKey::kExit;
  if (key == kOnlineMenuCommand) return ReservedKey::kOnlineMenu;
  return ReservedKey::kNone;
}

}  // namespace eck
