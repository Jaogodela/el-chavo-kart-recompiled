## El Chavo Kart Recompiled v0.1.0

**v0.1.0 is the first public release**, for Windows x64 and, as an experimental native build, for Linux x86_64 and the Steam Deck.

El Chavo Kart for Xbox 360, running natively through static recompilation: the original game code is recompiled to native PC code with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It is not an emulator. Windows is the supported platform; Linux and Steam Deck are experimental.

**No game files are included.** You need your own legitimate El Chavo Kart (USA) Xbox 360 game (disc image `.iso`, game version 0.0.0.3).

## Downloads

- **Windows**: `ElChavoKart-v0.1.0-windows-x64.zip`
- **Linux / Steam Deck (experimental)**: `ElChavoKart-v0.1.0-x86_64.AppImage`

A `.sha256` file is published with each download.

## Features

- **Native static recompilation** of the game's own code with ReXGlue 0.10.0, for Windows x64 and, experimentally, Linux x86_64 and the Steam Deck.
- **TRG launcher** with Play, Display, Graphics, Gameplay, Achievements, Online, Controls and About pages, and keyboard, mouse and controller navigation.
- **Install from your disc image**: the launcher checks `default.xex` by version, size and SHA-256 and copies the game files. Reinstall repairs them and keeps settings and saves.
- **Languages**: English by default, Português (Brasil) and Spanish.
- **Graphics**: Windowed or Borderless Fullscreen (**F11**), render resolution, internal render resolution, FXAA, MSAA, anisotropic filtering, VSync and graphics card selection.
- **Gameplay options**: skip the intro, unlock secret characters and unlock race tracks (the save is not changed).
- **Frame rate**: 30, 60, 90, 120, 144, 165, 240 or Unlimited. Game speed follows real time. **F2** shows the frame counter and **F3** the Ping display.
- **Local multiplayer**: the game's own 1 to 4 player split screen.
- **Online multiplayer** for up to 4 players: friends, friend codes, invites, direct UDP, relay fallback, STUN, UPnP and a connection test. Off by default.
- **Local saves and 46 achievements** (**F7**), with no Xbox Live needed.
- **Crash reporting and Report a bug**: a crash adds a report block to the session log, and **Report a bug** shows the newest log and opens the issue page. Nothing is uploaded.
- **Launcher updates**: a newer GitHub release is offered when the launcher starts and installed only when you choose it. Game files, saves, settings and logs are kept.

## Linux / Steam Deck (experimental)

- Native Linux x86_64 program using Vulkan, packaged as one AppImage.
- No Wine, and no Proton required.
- Steam Deck support is experimental. The AppImage can be added as a Non-Steam Game (Desktop Mode, make it executable, **Games > Add a Non-Steam Game to My Library**), with no compatibility tool set.
- Built against the Ubuntu 20.04 / glibc 2.31 baseline; the highest glibc the AppImage needs is 2.30. It starts without FUSE by unpacking itself.
- Settings, saves and logs live in folders next to the AppImage, not inside it.

The Linux build shares everything that does not depend on Windows:

- Launcher, install from your disc image, settings, languages (English, Português (Brasil), Spanish)
- Local saves and the 46 achievements (**F7**)
- Graphics options, the frame rate choices, **F2** frame counter, **F3** ping, **F11** window mode
- Local multiplayer and online multiplayer: friends, invites, Direct, Relay, STUN, UPnP and the Connection test
- Launcher updates and **Report a bug**

Windows and Linux players play together through the same online service.

This release has not been validated on real Linux PCs or Steam Deck hardware. The project's automated checks cover the build and packaging only; rendering, audio, controllers and performance on Linux are unconfirmed, and reports are welcome.

## Install

- **Windows**: unzip **ElChavoKart-v0.1.0-windows-x64.zip** anywhere you can write to and run **el_chavo_kart.exe**. On the launcher's **Play** page click **Install from disc image...** and pick your `.iso`. Windows may warn the first time you run the program, because the release is not digitally signed: choose **More info**, then **Run anyway**.
- **Linux AppImage (experimental)**: download **ElChavoKart-v0.1.0-x86_64.AppImage**, run `chmod +x` on it and start it from a folder you can write to. `game`, `logs` and `user_data` go next to the AppImage. See [docs/linux.md](https://github.com/saulob/el-chavo-kart-recompiled/blob/main/docs/linux.md).

Installation steps, requirements and the supported game version are in the [README](https://github.com/saulob/el-chavo-kart-recompiled#readme).

## Known limitations

- Online play is new. The host simulates the race; items and their effects, laps, placement, the finish and the return from the race are not synchronized between players.
- Controller prompts stay the game's Xbox 360 buttons.
- Windows is the supported platform. The Linux AppImage and Steam Deck are experimental.

## Reporting problems

If the game does not start, crashes or an online race fails, use **Report a bug** on the launcher's **About** page. It shows the newest log and opens the issue page; nothing is uploaded, so check the log and attach it yourself.

## Requirements

- **Windows**: Windows 10 or 11 (64-bit), a Direct3D 12 graphics card and the latest Microsoft Visual C++ 2015-2022 Redistributable (x64).
- **Linux**: x86_64 with Vulkan, an X11 or Wayland desktop and glibc 2.31 or newer. Online play uses the system's OpenSSL.

## Security

The final Windows ZIP, Linux AppImage, Windows executable and updater were checked with VirusTotal before release and returned no detections at the time of testing

A VirusTotal scan is an additional check, not a guarantee of safety, so please also verify the SHA-256 of your download

| File | SHA-256 | VirusTotal |
| --- | --- | --- |
| `ElChavoKart-v0.1.0-windows-x64.zip` | `45408a77d2625b9c1d9333b29d99e3869b1e566f30903c6ec8f35298a225e4ac` | [VirusTotal](https://www.virustotal.com/gui/file/45408a77d2625b9c1d9333b29d99e3869b1e566f30903c6ec8f35298a225e4ac?nocache=1) |
| `ElChavoKart-v0.1.0-x86_64.AppImage` | `10d229ca2012b22471f8d9e8bdcec28c6eea6b893ce9e2fcfbd28f5c2271a080` | [VirusTotal](https://www.virustotal.com/gui/file/10d229ca2012b22471f8d9e8bdcec28c6eea6b893ce9e2fcfbd28f5c2271a080?nocache=1) |
| `el_chavo_kart.exe` (inside the ZIP) | `a4b3009140d91e754f171208f446891bea6248e6a25009b56ec3c840afd94222` | [VirusTotal](https://www.virustotal.com/gui/file/a4b3009140d91e754f171208f446891bea6248e6a25009b56ec3c840afd94222?nocache=1) |
| `trg-updater.exe` (inside the ZIP) | `66ea40c270ad67907f8fdef4cbbc046aa110803a4ab1cadab00661a662d4329a` | [VirusTotal](https://www.virustotal.com/gui/file/66ea40c270ad67907f8fdef4cbbc046aa110803a4ab1cadab00661a662d4329a?nocache=1) |

## Checksums

The SHA-256 of each download is also published with the release as `ElChavoKart-v0.1.0-windows-x64.sha256` and `ElChavoKart-v0.1.0-x86_64.sha256`, so you can verify your download.

```
45408a77d2625b9c1d9333b29d99e3869b1e566f30903c6ec8f35298a225e4ac  ElChavoKart-v0.1.0-windows-x64.zip
10d229ca2012b22471f8d9e8bdcec28c6eea6b893ce9e2fcfbd28f5c2271a080  ElChavoKart-v0.1.0-x86_64.AppImage
```
