<div align="center">

# El Chavo Kart - PC Port

### El Chavo Kart (Chaves Kart), the Xbox 360 kart racer, recompiled to run natively on Windows, with the TRG launcher and Brazilian Portuguese by default

<sub>The original Xbox 360 game code, statically recompiled to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own El Chavo Kart (USA) Xbox 360 disc image.</sub>

</div>

## Status

**Early development, not yet released.** The full pipeline is in place (disc import, recompilation, patched runtime,
launcher, saves, achievements), but the first build and the first runtime tests are still to be done. Nothing below has
been confirmed in a running game yet; the [known limitations](#known-limitations) list what is still open.

## Features

- **Native static recompilation** of the game's own code with ReXGlue 0.10.0. Native Windows build.
- **TRG launcher** in the style of the other ports: Play, Display, Graphics, Gameplay, Achievements, Controls and About
  pages, keyboard and controller navigation, saved window placement, DPI-aware layout and fonts.
- **Brazilian Portuguese first**: Português (Brasil) is the default language. It selects the game's own official
  Brazilian localization (the `PTB` packages on the disc) and switches the launcher and overlays to Portuguese. English,
  Español (América Latina) and Español (España) select the game's `INT`, `ESM` and `ESN` text and keep the launcher in
  English.
- **Disc image import**: the launcher and `tools\build.ps1` read your `.iso` directly, check its `default.xex` by SHA-256 and
  copy the game files (about 2 GB). Other games and other builds are refused with a clear message.
- **Original 60 FPS**: the game presents without VSync and paces itself to 62 FPS on the console; the port holds its
  swaps to exactly 60 per second. There are no high frame rate options.
- **Graphics options**: render resolution 1x to 4x of the game's 1280x720, FXAA, 2x/4x MSAA, anisotropic filtering,
  graphics card selection, Windowed or Borderless Fullscreen (F11), VSync.
- **Controller and keyboard**: Xbox and SDL-compatible controllers work without setup; the keyboard drives the same
  pad with a kart layout, every key remappable in the launcher.
- **Local saves and achievements**: no Xbox Live needed. The 46 achievements (1000G) come from the game's own data in
  the selected language, unlock from the game's own calls and are kept on this PC. F7 shows them in game.
- **Portable user data**: saves, achievements, settings and logs are in a `user_data` folder beside the program.

## What you need

- Windows 10 or 11, 64-bit, and a GPU with Direct3D 12.
- Your own **El Chavo Kart (USA)** Xbox 360 disc image (`.iso`). The supported build is:

  | | |
  | --- | --- |
  | Title ID | 475807D7 |
  | Media ID | 5C2ECDD6 |
  | Game version | 0.0.0.3 (built 2013-11-28) |
  | `default.xex` | 30,003,200 bytes, SHA-256 `1DB4E96B413D5629206FFE63E73C0E1CFACA9EEA24BCF6FCED6DB9B135CE120F` |
  | Reference image | 7,838,695,424 bytes, SHA-256 `3B4C1BA6CBA41A20D94A89727018526B27FBB31FCD5AD32317D7C244C7C4B9C4` |

  Other dumps of the same disc work as long as their `default.xex` matches.

## Building it yourself

Requirements:

- Visual Studio 2022 or newer with **Desktop development with C++** and **C++ Clang tools for Windows** (clang, lld,
  CMake and Ninja are taken from it)
- Git
- Internet access the first time, to fetch the ReXGlue SDK release, the ReXGlue source for the patched runtime and the
  TRG launcher

Put your disc image in the `game` folder (for example `game\El Chavo Kart (USA).iso`), then from the repository root:

```powershell
.\tools\codegen.ps1   # downloads the pinned ReXGlue SDK, extracts the game files, generates generated\default
.\tools\build.ps1     # rebuilds the patched ReXGlue runtime if needed, then builds build\el_chavo_kart.exe
```

`tools\codegen.ps1` is the only command that (re)generates the recompiled C++ sources; it skips the work when
`generated\default` is already current for the manifest, the XEX and `config\overrides.toml` /
`config\discovered-functions.toml`. `tools\build.ps1` never regenerates guest code itself: if it is missing or stale,
`tools\build.ps1` stops and names the exact command to run. The first run of each script takes a while (the ReXGlue
runtime and the recompiled code are large); later runs are incremental. `tools\build.ps1` options: `-Clean`,
`-Config Release`, `-BuildDir build-release`, `-Iso <path>`, `-Jobs <n>`, and `-Run` to start the game after building.

Start `build\el_chavo_kart.exe`. The development build finds the game files through `build\game-folder.txt`; a packaged
build looks for them in a `game` folder beside the program, and the launcher can install them there from your `.iso`.

Lightweight, headless checks (no game files, no ReXGlue SDK, no GUI) live in `tools\test.ps1`: it builds and runs the
console tests in `tests\`, validates the patch sets in `patches\rexglue-sdk` and `patches\trg-launcher`, checks
`config\overrides.toml` against `config\discovered-functions.toml` for overlaps, reports `generated\default` freshness,
and checks the source tree for tracked game data, `DEBUG_SAULO` markers and hard-coded local paths.

Other developer tools: `tools\extract-iso.ps1` extracts the disc on its own, `python -I tools\xex_image.py` dumps the
decrypted image to `game\analysis`, and `python -I tools\find-missing-functions.py` rescans it for functions reached
only through vtables or function pointers (`--apply-high` rewrites `config\discovered-functions.toml`).

## The launcher

The launcher opens before the game. Hold **Shift** while starting to show it when it is turned off.

- **Play**: game file status and the disc image installer.
- **Display**: Windowed or Borderless Fullscreen, window size, monitor and VSync.
- **Graphics**: graphics card, render resolution, anti-aliasing, multisampling and texture filtering.
- **Gameplay**: language, frame rate in the window title and player names.
- **Achievements**: the game's achievements and which ones you have unlocked.
- **Controls**: keyboard on or off, and every keyboard key.
- **About**: the game, save and log folders, the settings file, detailed logging and version information.

Enter plays, Ctrl+S saves, F11 switches fullscreen and Alt+F4 quits. Escape does not quit.

## Controls

On a controller the game uses its own layout: left stick steers, **RT** accelerates, **LT** brakes and reverses, **A**
jumps and drifts, **B** boosts, **X** uses the item, **Y** looks back, **Back** returns you to the track and **Start**
pauses. These come from the game's own input configuration and still need to be confirmed in game.

Default keyboard layout:

| Action | Keys |
| --- | --- |
| Accelerate (RT) | W |
| Brake / reverse (LT) | S |
| Steer | A / D, Left / Right |
| Left stick up / down | Up / Down |
| Jump / drift (A) | Space |
| Boost / back (B) | E, Backspace |
| Use item (X) | Q |
| Look back (Y) | C |
| Left / right bumper | Z / R |
| Return to track (Back) | Tab |
| Pause / Start | Enter, P |
| D-pad | Shift + arrows |
| Right stick | I / J / K / L |

Port hotkeys: **F2** frame counter (session only; the Gameplay page sets the starting state), **F7** achievements,
**F11** Windowed / Borderless Fullscreen, **Alt+F4** exit. **F3** and **F4** are unassigned; Escape does not quit.

## Saves and achievements

Everything is kept in `user_data` beside the program (`build\user_data` for a development build):

- Saves: `user_data\<profile>\475807D7\00000001\`, written through the game's own Xbox 360 save calls.
- Achievements: `user_data\achievements\475807D7.toml`.
- Settings: `user_data\settings.toml`, loaded after `el_chavo_kart.toml`.
- Logs: in the logs folder the About page opens.

## Known limitations

- Not yet built or run: everything in this README is implemented from static analysis of the game and the reference
  ports, and still needs a first runtime pass.
- Spanish (España) relies on the game reading the console country through ReXGlue's `user_country`; if the log shows
  the Spanish text from Latin America instead, that is the cause.
- Localized voice banks were not identified statically; voice language follows the game's own package selection.
- Local multiplayer for players 2 to 4 uses ReXGlue's default sign-in for extra controllers and has not been
  investigated in this pass. Online play is not part of the port.
- There is no Skip intro option yet.
- Controller prompts stay the game's Xbox 360 buttons.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the static recompilation toolkit and runtime
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, MIT License
- The launcher, installer, settings, logging, crash reporting and window handling come from
  [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled); the launcher translation
  layer from Super Street Fighter II Turbo HD Remix Recompiled; ReXGlue runtime patches from Hard Corps: Uprising,
  Marvel Ultimate Alliance and Crackdown Recompiled. Third-party notices are in
  [packaging/licenses](packaging/licenses).

El Chavo Kart is a trademark of its respective owners. This project is not affiliated with or endorsed by them, and it
contains no game code or assets.

## License

GPL-2.0-only, see [LICENSE](LICENSE). Third-party components keep their own licenses, listed in
[packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).
