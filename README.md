<div align="center">

# El Chavo Kart - PC Port

### El Chavo Kart (Chaves Kart), the Xbox 360 kart racer, recompiled to run natively on Windows, with the TRG launcher and Brazilian Portuguese support

<sub>The original Xbox 360 game code, statically recompiled to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own El Chavo Kart (USA) Xbox 360 disc image.</sub>

</div>

## Status

**v0.1.0, the first public Windows release.** This is a native PC recompilation, not an emulator, and it needs your own
copy of the game. The [known limitations](#known-limitations) list what is still open.

## Features

- **Native static recompilation** of the game's own code with ReXGlue 0.10.0. Native Windows build.
- **TRG launcher** in the style of the other ports: Play, Display, Graphics, Gameplay, Achievements, Online, Controls and
  About pages, keyboard and controller navigation, saved window placement, DPI-aware layout and fonts.
- **Languages**: English is the default language. Português (Brasil) selects the game's own official Brazilian
  localization (the `PTB` packages on the disc) and switches the launcher and overlays to Portuguese. English,
  Español (América Latina) and Español (España) select the game's `INT`, `ESM` and `ESN` text and keep the launcher in
  English. A saved language choice is kept.
- **Disc image import**: the launcher and `tools\build.ps1` read your `.iso` directly, check its `default.xex` by SHA-256 and
  copy the game files (about 2 GB). Other games and other builds are refused with a clear message.
- **Frame rate**: 30 (the console rate), 60, 90, 120, 144, 165, 240 or Unlimited, in the launcher; game speed follows real
  time. Rates above 60 are new and still being validated.
- **Graphics options**: render resolution 1x to 4x of the game's 1280x720, internal render resolution, FXAA, 2x/4x MSAA,
  anisotropic filtering, graphics card selection, Windowed or Borderless Fullscreen (F11), VSync.
- **Local multiplayer**: the game's own 1 to 4 player split screen; players 2 to 4 each need a controller.
- **Online play**: invite up to 3 friends to a race, with friend codes, a connection test, direct UDP and a relay
  fallback. Off by default; see [Online play](#online-play).
- **Controller and keyboard**: Xbox and SDL-compatible controllers work without setup; the keyboard drives the same
  pad with a kart layout, every key remappable in the launcher.
- **Local saves and achievements**: no Xbox Live needed. The 46 achievements (1000G) come from the game's own data in
  the selected language, unlock from the game's own calls and are kept on this PC. F7 shows them in game.
- **Portable user data**: saves, achievements and settings are in a `user_data` folder beside the program, and logs in a
  `logs` folder next to it.

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
and checks the source tree for tracked game data and hard-coded local paths. The online service has its own tests:
`npm test` in `server\eck-lobby`.

Other developer tools: `tools\extract-iso.ps1` extracts the disc on its own, `python -I tools\xex_image.py` dumps the
decrypted image to `game\analysis`, and `python -I tools\find-missing-functions.py` rescans it for functions reached
only through vtables or function pointers (`--apply-high` rewrites `config\discovered-functions.toml`).

## Releases and updates

`tools\package-release.ps1` builds the Release configuration and writes `dist\ElChavoKart-v<version>-windows-x64.zip`
and its `.sha256`; see [docs/release.md](docs/release.md). The launcher checks the latest stable GitHub release once per
start and shows an update banner in its header. Nothing is installed until you choose UPDATE NOW and RESTART TO UPDATE;
game files, saves, settings and logs are never touched.

## The launcher

The launcher opens before the game. Turn **Show this launcher** off on the Play page to start the game directly; hold
**Shift** while starting to bring the launcher back.

- **Play**: game file status, Show this launcher, and the disc image installer. Reinstalling verifies the new files
  before it replaces the old ones, so a failed or wrong image leaves the game as it was. Your saves, settings and logs
  live outside the game folder and are not touched.
- **Display**: Windowed or Borderless Fullscreen, window size, monitor and VSync.
- **Graphics**: graphics card, render resolution, anti-aliasing, multisampling and texture filtering. A card that is no
  longer present falls back to the first available card.
- **Gameplay**: language, skip intro, unlock secret characters, unlock race tracks, frame rate, frame rate and render
  resolution in the window title, frame counter and local player names.
- **Achievements**: the game's achievements and which ones you have unlocked.
- **Online**: Online play, online name, friends, the connection test and the Ping display.
- **Controls**: keyboard on or off, and every keyboard key.
- **About**: the game, save and log folders, the settings file, detailed logging, Report a bug and version information.

Enter plays, Ctrl+S saves, F11 switches fullscreen and Alt+F4 quits. Escape does not quit.

The launcher takes its colours from the game's logo. Clickable controls (action buttons, the selected page, the active
toggle option and PLAY) are yellow `#F2C318` with a thin red `#D9481F` outline, lighter on hover and darker while
pressed. Links, check marks and the keyboard focus ring use the lighter red-orange `#FF704A`. The outline and the extra
colours come from `patches\trg-launcher\0005-accent-outline.patch`; the application icon is drawn by
`python -I tools\make-app-icon.py` in the same colours.

## Controls

On a controller the game uses its own layout: left stick steers, **RT** accelerates, **LT** brakes and reverses, **A**
jumps and drifts, **B** boosts, **X** uses the item, **Y** looks back, **Back** returns you to the track and **Start**
pauses. These come from the game's own input configuration.

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

Port hotkeys: **F2** frame counter (session only; the Gameplay page sets the starting state), **F3** Ping display
(session only; the Online page sets the starting state; needs Online play), **F7** achievements,
**F11** Windowed / Borderless Fullscreen, **Alt+F4** exit. **F4** is unassigned; Escape does not quit.

## Saves and achievements

Everything is kept beside the program (in `build` for a development build):

- Saves: `user_data\<profile>\475807D7\00000001\`, written through the game's own Xbox 360 save calls.
- Achievements: `user_data\achievements\475807D7.toml`.
- Settings: `user_data\settings.toml`, loaded after `el_chavo_kart.toml`.
- Logs: `logs\`; the About page opens the folder. A crash adds a `CRASH REPORT` block to that session's log.

To report a bug, use **Report a bug** on the About page: it shows the newest log and opens the project's issue page.
Nothing is uploaded; check the log and attach it yourself.

## Online play

Online play is off by default.

1. On the launcher's **Online** page, turn **Online play** on and choose an online name.
2. Run the **Connection test**. Play waits until one test has finished, whatever its result.
3. Share **Your friend code**. Add a friend's code under **Friends**; they accept it under **Friend requests**.
4. In the game, go to Free Race and pick a competition and difficulty. On the character screen, **X** invites a friend, up
   to 4 players in all.

The race runs directly between the host and each friend over UDP; a friend the host cannot reach is relayed through the
online service. **F3** shows the Ping while racing. See [docs/online.md](docs/online.md).

## Known limitations

- Online play is new. The host simulates the race; items and their effects, laps, placement, the finish and the return
  from the race are not synchronized between players.
- Spanish (España) relies on the game reading the console country through ReXGlue's `user_country`; if the log shows
  the Spanish text from Latin America instead, that is the cause.
- Localized voice banks were not identified statically; voice language follows the game's own package selection.
- Controller prompts stay the game's Xbox 360 buttons.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the static recompilation toolkit and runtime
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, MIT License
- The launcher, installer, settings, logging, crash reporting and window handling come from
  [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled); the launcher translation
  layer from Super Street Fighter II Turbo HD Remix Recompiled; ReXGlue runtime patches from Hard Corps: Uprising,
  Marvel Ultimate Alliance and Crackdown Recompiled. Third-party notices are in
  [packaging/licenses](packaging/licenses).

El Chavo Kart is a trademark of its respective owners. This project is not affiliated with or endorsed by them. This
repository contains no game code or assets, and release packages contain no game files.

## License

GPL-2.0-only, see [LICENSE](LICENSE). Third-party components keep their own licenses, listed in
[packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).
