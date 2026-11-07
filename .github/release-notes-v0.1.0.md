## El Chavo Kart PC Port v0.1.0

**v0.1.0 is the first public release**, for Windows x64 and, as an experimental native build, for Linux x86_64.

This is a native recompilation of the game's own code, not an emulator. No game files are included: you need your own El Chavo Kart (USA) Xbox 360 disc image (`.iso`, game version 0.0.0.3).

## Features

- **TRG launcher** with Play, Display, Graphics, Gameplay, Achievements, Online, Controls and About pages, and keyboard, mouse and controller navigation.
- **Install from your disc image**: the launcher checks `default.xex` by version, size and SHA-256 and copies the game files. Reinstall repairs them and keeps settings and saves.
- **Languages**: English by default, Português (Brasil) and Spanish.
- **Graphics**: Windowed or Borderless Fullscreen (**F11**), render resolution, internal render resolution, FXAA, MSAA, anisotropic filtering, VSync and graphics card selection.
- **Frame rate**: 30, 60, 90, 120, 144, 165, 240 or Unlimited. Game speed follows real time.
- **Local multiplayer**: the game's own 1 to 4 player split screen.
- **Online play**: invite up to 3 friends, with friend codes, a connection test, direct UDP, STUN, UPnP and a relay fallback. Off by default.
- **Local saves and 46 achievements** (**F7**), with no Xbox Live needed.
- **Launcher updates**: a newer GitHub release is offered when the launcher starts and installed only when you choose it. Game files, saves, settings and logs are kept.

## Linux x86_64 AppImage (experimental)

The Linux build is a native program using Vulkan, packaged as one AppImage. It does not use Proton or Wine.

- Same platform-neutral launcher and game features as Windows.
- Windows and Linux players play together, through the same online service.
- Steam Deck is experimental: the AppImage can be added as a Non-Steam Game, with no compatibility tool set.
- The AppImage starts without FUSE, and needs glibc 2.31 or newer (Ubuntu 20.04 or later).

This release has not been validated on real Linux PCs or Steam Deck hardware. The project's automated checks cover the build and packaging only; rendering, audio, controllers and performance on Linux are unconfirmed, and reports are welcome.

## Install

- **Windows**: unzip **ElChavoKart-v0.1.0-windows-x64.zip** anywhere you can write to and run **el_chavo_kart.exe**. On the launcher's **Play** page click **Install from disc image...** and pick your `.iso`.
- **Linux AppImage (experimental)**: download **ElChavoKart-v0.1.0-x86_64.AppImage**, run `chmod +x` on it and start it from a folder you can write to. `game`, `logs` and `user_data` go next to the AppImage. See [docs/linux.md](https://github.com/saulob/el-chavo-kart-recompiled/blob/main/docs/linux.md).

## Known limitations

- Online play is new. The host simulates the race; items and their effects, laps, placement, the finish and the return from the race are not synchronized between players.
- Controller prompts stay the game's Xbox 360 buttons.
- Windows is the supported platform. The Linux AppImage and Steam Deck are experimental.

## Reporting problems

If the game does not start, crashes or an online race fails, use **Report a bug** on the launcher's **About** page. It shows the newest log and opens the issue page; nothing is uploaded, so check the log and attach it yourself.

## Requirements

- **Windows**: Windows 10 or 11 (64-bit), a Direct3D 12 graphics card and the latest Microsoft Visual C++ 2015-2022 Redistributable (x64).
- **Linux**: x86_64 with Vulkan, an X11 or Wayland desktop and glibc 2.31 or newer. Online play uses the system's OpenSSL.

## Checksums

The SHA-256 of each file is published with the release as `ElChavoKart-v0.1.0-windows-x64.sha256` and `ElChavoKart-v0.1.0-x86_64.sha256`, so you can verify your download.

| File | SHA-256 |
| --- | --- |
| `ElChavoKart-v0.1.0-windows-x64.zip` | `e72e7d21cf13855de0a97f16b6d9d7a6cf2d7498e113be6b39aa9263e5271f0a` |
| `ElChavoKart-v0.1.0-x86_64.AppImage` | `42d78a4ab56e367b9ad7c4618285522e1d980eb18979b610e25f2f9cddf4eadb` |
