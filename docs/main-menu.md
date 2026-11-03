# Main menu: Free Race / Multiplayer and Exit Game

Nothing is written to the game files. The menu movie and its texts are patched in memory, only when they are the
supported build.

## Where the menu comes from

| Part | Where |
| --- | --- |
| Rows | AS2 movie `ChavoKart_MainMenu.ChavoKartMainMenu`, export 89 of `StartMenu.xxx` (GFX v9, 176,575 bytes). Sprite 99 places five `TabCommon` rows (symbol 22), x 41.9, y 248.6 to 564.1 in steps of about 78.9, on a 1280x720 stage |
| Row order | `TabItemGroup.AddTabItem(item, sort)` sorts by `_y` |
| Row action | Each row sets `_actionCommand` in a Construct clip action. A press calls `ExternalInterface.call("SelectMenu", actionCommand)` |
| Script action | `KartScreen_MainMenu.SelectMenu(menu:string)`, export 8056 of `ChavoKartGame.xxx`: Cups, Challenges, Exhibition, Extras, Achievements |
| Labels and help | `Coalesced_<LANG>.bin`, section `KartScreen_MainMenu`, keys `mainMenu_*` and `mainMenu_*_description`. The movie asks `RequestTextFieldUpdate(key)`, which ends in the native exec below |

## Hooks

| Address | What | Used for |
| --- | --- | --- |
| `sub_82BD99F8` | Scaleform memory-file constructor `(this, name, data, size)`; r5 is the buffer, r6 its size | Replaces the main-menu movie buffer when size is 176575 and SHA-256 is `6ac9fb05fdac97a9070878a99000ac6bd217b1678c68cf8ae5784e92b4ae8d7e` |
| `sub_82C22F28` | Scaleform open-by-URL wrapper `(loader, url, flags)`, returns the opened file object; its virtual call is the game's file opener | Looks through the returned object (up to four levels, bounded) for a size word of 176575 next to a pointer to `GFX` bytes, and swaps both for the patched copy only when the fingerprint matches |
| `sub_82A46B68` | Exec `UKartScreen_Base::execGetFieldValueByName(this, FFrame*, FString* result)`, registered at `0x83BE3148` | Overrides the four main-menu texts and receives the exit request |

The exec is reproduced step by step (parameter through the native table at `0x83C77308`, virtual at vtable offset 424,
FString assign `sub_823B5E08`, free `sub_822E80C8`) so the field name can be read before the result is copied out.

## Movie patch (`src/main_menu_swf.cpp`)

The fingerprint check gates everything. The patch then locates sprite 99 and the `COMMAND_GOTO_ACHIEVEMENTS` class
script by structure, and fails safely if either differs.

- A sixth `TabCommon` row `mainMenu_exit` is placed after Achievements at the spacing of the other rows, with
  `_actionCommand = "ECK_EXIT_GAME"`.
- A second init script for sprite 130 wraps `onLoad`, `OnTabItemSelected` and `OnGamepadA` of `CKMainMenuRoot`. It
  binds the exit help text, shows it when the row is selected, and sends `RequestTextFieldUpdate("ECK_EXIT_GAME")`
  when the row is confirmed. Every other row runs the original methods.

## Exit

The movie sends the reserved key through the game's own text request. The exec hook sees the exact key `ECK_EXIT_GAME`
and runs `eck::HardExit`, the same path as closing the window (`PrepareHardExit`, then `_Exit(0)`). No other key exits.

## Texts (`src/main_menu.h`)

| | INT | ESM / ESN | PTB |
| --- | --- | --- | --- |
| `mainMenu_exhibition` | Free Race / Multiplayer | Carrera Libre / Multijugador | Corrida Livre / Multiplayer |
| `mainMenu_exhibition_description` | Choose the game mode, players, race and track to begin | Elige el modo de juego, jugadores, carrera y pista para comenzar | Escolha o modo de jogo, jogadores, corrida e pista para começar |
| `mainMenu_exit` | Exit Game | Salir del Juego | Sair do Jogo |
| `mainMenu_exit_description` | Exit the game | Salir del juego | Sair do jogo |

The language is `eck_language`, the same setting that selects the game's package.

## Check

`tools\test.ps1 -Filter main_menu` runs `eck_main_menu_tests`. Passing the path of the extracted, unpatched movie as
the first argument also patches the real asset and writes `<path>.patched`.
