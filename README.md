<div align="center">

# Chaves Kart (El Chavo Kart) Recompiled

### Chaves Kart (El Chavo Kart) for Xbox 360 running natively on Windows, and experimentally on Linux and Steam Deck, through static recompilation, with the TRG launcher, modern PC options and online multiplayer

[![Latest release](https://img.shields.io/github/v/release/saulob/el-chavo-kart-recompiled?style=for-the-badge&label=release&color=99db2e&labelColor=07101f&include_prereleases)](https://github.com/saulob/el-chavo-kart-recompiled/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/el-chavo-kart-recompiled/total?style=for-the-badge&color=2e7d32&labelColor=07101f)](https://github.com/saulob/el-chavo-kart-recompiled/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-00acc1?style=for-the-badge&labelColor=07101f)

<sub>The original Xbox 360 game code, statically recompiled to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own legitimate El Chavo Kart (USA) Xbox 360 game.</sub>

</div>

<img width="1281" height="763" alt="2026-11-06_23-51" src="https://github.com/user-attachments/assets/64b41110-7884-45ad-95c9-67fa41656a7c" />

## Features

- **Native static recompilation** of the game's own code with ReXGlue 0.10.0. It is not an emulator. Native **Windows x64** build, plus an experimental native **Linux x86_64 AppImage** that also runs on the **Steam Deck** (Vulkan, no Proton or Wine).
- **TRG launcher** in the style of the other ports: Play, Display, Graphics, Gameplay, Achievements, Online, Controls and
  About pages, keyboard and controller navigation, saved window placement, DPI-aware layout and fonts.
- **Install and reinstall from your own disc image**: the launcher reads your `.iso`, checks its `default.xex` by SHA-256 and
  copies the game files (about 2 GB). Reinstalling repairs the game files and keeps your settings and saves. Other games and
  other builds are refused with a clear message.
- **Languages**: English is the default language. Português (Brasil) selects the game's own official Brazilian
  localization (the `PTB` packages on the disc) and switches the launcher and overlays to Portuguese. English,
  Español (América Latina) and Español (España) select the game's `INT`, `ESM` and `ESN` text and keep the launcher in
  English. A saved language choice is kept.
- **Graphics options**: Windowed or Borderless Fullscreen (**F11**), window size, monitor, VSync, graphics card selection,
  render resolution 1x to 4x of the game's 1280x720, internal render resolution, FXAA, 2x or 4x MSAA and texture
  (anisotropic) filtering.
- **Frame rate**: 30 (the console rate), 60, 90, 120, 144, 165, 240 or Unlimited, in the launcher; game speed follows real
  time. Rates above 60 are new and still being validated. **F2** shows the frame counter and **F3** the Ping display.
- **Gameplay options**: skip the intro, unlock secret characters and unlock race tracks (the save is not changed).
- **Local multiplayer**: the game's own 1 to 4 player split screen; players 2 to 4 each need a controller.
- **Online multiplayer** for up to 4 players: friends, friend codes, invites, direct UDP with a relay fallback, STUN, UPnP
  and a connection test. Off by default, and Windows and Linux players play together. See [Online play](#online-play).
- **Controller and keyboard**: Xbox and SDL-compatible controllers work without setup; the keyboard drives the same
  pad with a kart layout, every key remappable in the launcher.
- **Local saves and 46 achievements (1000G)**: no Xbox Live needed. The achievements come from the game's own data in
  the selected language, unlock from the game's own calls and are kept on this PC. **F7** shows them in game.
- **Portable user data**: saves, achievements and settings are in a `user_data` folder beside the program, and logs in a
  `logs` folder next to it.
- **Crash reporting and Report a bug**: a crash adds a `CRASH REPORT` block to that session's log, and **Report a bug** on
  the About page shows the newest log and opens the issue page. Nothing is uploaded.
- **Launcher self-update**: a newer GitHub release is offered when the launcher starts and installed only when you choose it.

## What you need

- **Windows**: Windows 10 or 11, 64-bit, a GPU with Direct3D 12 and the latest Microsoft Visual C++ 2015-2022 Redistributable (x64).
- **Linux and Steam Deck (experimental)**: 64-bit x86 Linux, glibc 2.31 or newer (Ubuntu 20.04 or later), a Vulkan-capable GPU
  and driver, and an X11 or Wayland desktop. The AppImage's highest glibc requirement is 2.30.
- Your own **El Chavo Kart (USA)** Xbox 360 game, this exact version:

| | |
| --- | --- |
| Game | El Chavo Kart |
| Platform | Xbox 360 |
| Title ID | `475807D7` |
| Media ID | `5C2ECDD6` |
| Game version | `0.0.0.3` (built 2013-11-28) |
| `default.xex` | 30,003,200 bytes, SHA-256 `1DB4E96B413D5629206FFE63E73C0E1CFACA9EEA24BCF6FCED6DB9B135CE120F` |
| Reference disc image | 7,838,695,424 bytes, SHA-256 `3B4C1BA6CBA41A20D94A89727018526B27FBB31FCD5AD32317D7C244C7C4B9C4` |

The launcher checks `default.xex` and refuses any other version, because the port is built for this exact game code. Other
dumps of the same disc work as long as their `default.xex` matches.

## Installing

1. Download **ElChavoKart-v0.1.0-windows-x64.zip** from the [latest release](https://github.com/saulob/el-chavo-kart-recompiled/releases/latest)
   and extract it somewhere you can write to (not `Program Files`).
2. Run **el_chavo_kart.exe**. The launcher opens.
3. On the **Play** page click **Install from disc image...** and pick your own `.iso`. Wait until the game files are ready.
4. Press **PLAY**.

**No game files are included** in the release. **Reinstall** repairs the game files and keeps your settings and saves.

Windows may warn the first time you run the program, because the release is not digitally signed. If you downloaded it
from this repository's releases page, choose **More info**, then **Run anyway**.

### Linux and Steam Deck

**Experimental.** This is a native x86_64 Linux build that uses Vulkan, packaged as one AppImage. It is not Wine and not
Proton, and it has the same platform-neutral launcher and game features as Windows. It is built against the Ubuntu 20.04 /
glibc 2.31 baseline; the highest glibc the final AppImage needs is 2.30. If the system cannot mount the AppImage with
FUSE, it unpacks itself and runs from there. This release has not been validated on real Linux PCs or Steam Deck hardware.

1. Download **ElChavoKart-v0.1.0-x86_64.AppImage** from the [latest release](https://github.com/saulob/el-chavo-kart-recompiled/releases/latest)
   and make it executable (right-click > **Properties** > **Permissions** > **Is executable**, or `chmod +x`).
2. Run it from a folder you can write to, then install your disc image from the **Play** page as above.
3. On a Steam Deck, do this in **Desktop Mode**, then add the AppImage with **Games > Add a Non-Steam Game to My Library**
   and play it from **Game Mode**. No compatibility tool or Proton is needed.

The AppImage is read-only, so `game`, `logs` and `user_data` live in folders next to it, and survive updates. Requirements,
known limits and how to build it are in [docs/linux.md](docs/linux.md).

## Building it yourself

These steps build the Windows program; the Linux build is in [docs/linux.md](docs/linux.md#building-the-appimage). Requirements:

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

Headless checks (no GUI, no game started) live in `tools\test.ps1`; `.\tools\test.ps1 -RequireLinux` is the complete
Windows and Linux release gate ([docs/release.md](docs/release.md#complete-validation)). It builds and runs the
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

## Screenshots

<img width="1282" height="766" alt="2026-11-06_23-52" src="https://github.com/user-attachments/assets/91920cf0-bb8d-4e22-8fb0-9db603102a16" />

<img width="1278" height="762" alt="2026-11-06_23-51_1" src="https://github.com/user-attachments/assets/b9ad0007-d53f-42ce-924f-759ec4744375" />

## Known limitations

- Online play is new. The host simulates the race; items and their effects, laps, placement, the finish and the return
  from the race are not synchronized between players.
- Spanish (España) relies on the game reading the console country through ReXGlue's `user_country`; if the log shows
  the Spanish text from Latin America instead, that is the cause.
- Localized voice banks were not identified statically; voice language follows the game's own package selection.
- Controller prompts stay the game's Xbox 360 buttons.

## Security

The final Windows ZIP, Linux AppImage, Windows executable and updater were checked with VirusTotal before release and returned no detections at the time of testing

A VirusTotal scan is an additional check, not a guarantee of safety, so please also verify the SHA-256 of your download

| File | SHA-256 | VirusTotal |
| --- | --- | --- |
| `ElChavoKart-v0.1.0-windows-x64.zip` | `45408a77d2625b9c1d9333b29d99e3869b1e566f30903c6ec8f35298a225e4ac` | [VirusTotal](https://www.virustotal.com/gui/file/45408a77d2625b9c1d9333b29d99e3869b1e566f30903c6ec8f35298a225e4ac?nocache=1) |
| `ElChavoKart-v0.1.0-x86_64.AppImage` | `10d229ca2012b22471f8d9e8bdcec28c6eea6b893ce9e2fcfbd28f5c2271a080` | [VirusTotal](https://www.virustotal.com/gui/file/10d229ca2012b22471f8d9e8bdcec28c6eea6b893ce9e2fcfbd28f5c2271a080?nocache=1) |
| `el_chavo_kart.exe` (inside the ZIP) | `a4b3009140d91e754f171208f446891bea6248e6a25009b56ec3c840afd94222` | [VirusTotal](https://www.virustotal.com/gui/file/a4b3009140d91e754f171208f446891bea6248e6a25009b56ec3c840afd94222?nocache=1) |
| `trg-updater.exe` (inside the ZIP) | `66ea40c270ad67907f8fdef4cbbc046aa110803a4ab1cadab00661a662d4329a` | [VirusTotal](https://www.virustotal.com/gui/file/66ea40c270ad67907f8fdef4cbbc046aa110803a4ab1cadab00661a662d4329a?nocache=1) |

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), the static recompilation toolkit and runtime
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, MIT License
- The launcher, installer, settings, logging, crash reporting and window handling come from
  [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled); the launcher translation
  layer from Super Street Fighter II Turbo HD Remix Recompiled; ReXGlue runtime patches from Hard Corps: Uprising,
  Marvel Ultimate Alliance and Crackdown Recompiled. Third-party notices are in
  [packaging/licenses](packaging/licenses).

El Chavo Kart is a trademark of its respective owners. This project is not affiliated with or endorsed by them. No game
files are included in this repository or in the release packages.

## License

GPL-2.0-only, see [LICENSE](LICENSE). Third-party components keep their own licenses, listed in
[packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).
