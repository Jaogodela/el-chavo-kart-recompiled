# Linux

The Linux build is a native x86_64 program, not a Windows build under Proton or Wine. It uses the same code and the same
launcher as the Windows build: the ReXGlue SDK 0.10.0 Linux runtime and the Xenos GPU plugin's Vulkan backend, both built
with the project's patches. It ships as one AppImage, and it is the same v0.1.0 release as the Windows ZIP.

**Experimental.** The AppImage is built and checked by the project's scripts, but this release has not yet been validated
on real Linux PCs or Steam Deck hardware. Reports are welcome.

## Requirements

- 64-bit x86 Linux with glibc 2.31 or newer, such as Ubuntu 20.04 or later (`ldd --version` shows it)
- A Vulkan driver (Mesa RADV on most AMD and Intel PCs)
- An X11 or Wayland desktop
- `xdg-desktop-portal`, `zenity` or `kdialog` for the launcher's file picker, and `xdg-open` for its folder and link buttons
- For online play, the system's OpenSSL (`libssl.so.3` or `libssl.so.1.1`, installed on every common distribution); it is
  loaded when the game connects, so its certificate store is the system's own
- Your own El Chavo Kart (USA) Xbox 360 disc image (`.iso`), game version 0.0.0.3; see the [README](../README.md#what-you-need)

The AppImage carries the C++ runtime of its build machine (GCC 13 or newer). It is used only when the system's own copy is
older than GCC 13, so a newer system copy keeps working with the graphics drivers.

## Installing

1. Download **ElChavoKart-v0.1.0-x86_64.AppImage** and put it in a folder you can write to.
2. Make it executable: right-click it > **Properties** > **Permissions** > **Is executable**, or
   `chmod +x ElChavoKart-v0.1.0-x86_64.AppImage`.
3. Run it. The launcher opens.
4. On the **Play** page click **Install from disc image...** and pick your disc image. The launcher checks `default.xex`
   (version, size and SHA-256) before it copies anything, extracts into a temporary folder next to `game`, checks the
   result and only then moves it into place. About 2 GB are needed next to the AppImage.
5. Wait for **Ready to play**, then press **PLAY**.

**Reinstall from disc image...** repairs the game files and keeps settings and saves. An install that was interrupted while
the folders were being swapped is put back at the next start.

### If the AppImage does not start

An AppImage mounts itself with FUSE before any of its own code runs. Where that mount is not possible, the AppImage unpacks
itself into a private folder under `/tmp`, runs the game from there with the same arguments, returns the game's exit status
and removes the folder afterwards. Nobody has to install FUSE to start the game. A restart from the launcher starts the
original AppImage again. Where FUSE works, nothing is unpacked. `game`, `logs` and `user_data` stay next to the AppImage in
both cases.

A line such as `Error: $FUSERMOUNT_PROG not set` can still appear in the terminal; it is expected and harmless. To force the
unpacking, run `APPIMAGE_EXTRACT_AND_RUN=1 ./ElChavoKart-v0.1.0-x86_64.AppImage` or pass `--appimage-extract-and-run`.
`VERBOSE=1` makes the AppImage print which path it took.

### If Vulkan is unavailable

When the Vulkan graphics system cannot start, the program opens a small launcher window drawn without the GPU. It shows the
game files and graphics status; **Play** explains that a working Vulkan driver is needed and points at the `logs` folder
next to the AppImage, and **Quit** closes it.

## Where things are

| What | Where |
| --- | --- |
| Game files | `game` next to the AppImage |
| Logs | `logs` next to the AppImage (`user_data/logs` if that folder is not writable) |
| Settings, saves, achievements, online identity, friends | `user_data` next to the AppImage |

Nothing is written inside the AppImage, which is read-only: every folder above follows the `APPIMAGE` path, so they survive
a relaunch, an update and an install, and they stay in place when the AppImage is replaced by a newer one in the same folder.
ReXGlue's own Linux folder, `$XDG_DATA_HOME/el_chavo_kart`, moves into `user_data` at the first start if it holds data. A build
run outside an AppImage keeps `game`, `logs` and `user_data` next to the program instead. `--game_data_root` and
`--user_data_root` override the folders, as on Windows.

The launcher's About page opens these folders. **Report a bug** shows the newest log and opens the project's issue page
with `xdg-open`, started with the folder or address as its one argument and no shell in between; nothing is uploaded, and
only `http` and `https` addresses are opened. A `settings.toml` that is not valid TOML is kept as `settings.toml.invalid`
and a new one is written, so the next Save leaves a usable file. After a crash the log ends with a CRASH REPORT section, as on Windows, with the same
fields: the fatal signal and the matching exception code, the stack with module offsets, the ReXGlue version, the
operating system, CPU, GPU, display, renderer, language and whether online play was on. It carries no folders, player
names or friend codes. There is no minidump on either platform.

## Launcher and game

Every launcher page and option of the Windows build is here: Play, Display, Graphics, Gameplay, Achievements, Online,
Controls and About, English by default, Português (Brasil) and Spanish, the same keyboard, mouse and controller navigation.

| Key | Action |
| --- | --- |
| **Shift** at start | Show the launcher when it is turned off |
| **F2** | Frame counter |
| **F3** | Online Ping display |
| **F7** | Achievements |
| **F11** | Windowed / Borderless Fullscreen |

**Graphics card** lists the Vulkan hardware devices, the dedicated GPU first. Until a card is chosen there, the runtime
prefers the dedicated GPU, and the usual selection variables (`DRI_PRIME`, `MESA_VK_DEVICE_SELECT`,
`VK_LOADER_DEVICE_SELECT`, `__NV_PRIME_RENDER_OFFLOAD`) take precedence when they are set. For example:

```sh
DRI_PRIME=1 ./ElChavoKart-v0.1.0-x86_64.AppImage
```

Render resolution, Internal render resolution, FXAA, 2x and 4x MSAA, anisotropic filtering, the frame rates 30 to 240 and
Unlimited, VSync, Skip intro and the character and track unlocks are the same settings on both platforms. The frame limiter
and the game's timing are shared code, so the game runs at the same speed.

The program tells the desktop it is `io.github.saulob.el-chavo-kart` (the Wayland app_id and the X11 window class). The
`.desktop` file in `packaging/linux` has the same name and sets it as `StartupWMClass`, and `tools/package-linux.sh` stops if
they differ.

## Online play

Linux and Windows players are in the same player pool: the same production service, the same protocol and the same
friend codes, invites and rooms. There is no separate Linux lobby, and a friend list shows the platform. Everything on the
wire is byte-identical on both platforms: `tools/test.ps1` compares the two builds' output, `tests/wire-vectors.txt` holds the
reference, and the service's own tests read the same file.

Direct UDP, the relay fallback, STUN, UPnP and the Connection test are the same code on both platforms. The Connection test
reports what it really finds on the PC it runs on. Windows Firewall's question has no Linux counterpart, so the test does not
mention it; if a firewall such as `ufw` blocks UDP ports 7101 to 7110, the test reports direct connections as limited and
the relay is used.

## Launcher updates

The launcher checks the project's GitHub releases once per start, ignoring drafts and prereleases. When a newer version
exists it shows the same banner as on Windows; nothing happens until you choose **UPDATE NOW** and then **RESTART TO UPDATE**.

The update downloads `ElChavoKart-v<version>-x86_64.AppImage`, never the Windows ZIP, and verifies its SHA-256 against the
GitHub asset digest or `ElChavoKart-v<version>-x86_64.sha256`. It then replaces only the AppImage this run was started from,
keeps its permissions, and starts that file again. If the new file cannot be started, the previous AppImage is put back.
`game`, `user_data`, saves, settings and logs are never touched.

## Steam and Steam Deck

The AppImage runs as a Non-Steam Game. Steam is not needed to run it, and there is no Steamworks, Steam Input SDK or Steam
DRM in it. **Do not set a compatibility tool**: this is a native Linux program, and Proton is not used.

On Steam Deck, in Desktop Mode:

1. Put the AppImage in a folder you can write to, for example `~/Games/ElChavoKart`.
2. Make it executable: **Properties** > **Permissions** > **Is executable**, or `chmod +x` in Konsole.
3. Run it once and install your disc image from the **Play** page.
4. In Steam, **Games** > **Add a Non-Steam Game to My Library...**, press **Browse...**, set the file type to **All Files**
   and select the AppImage.
5. Return to Game Mode and start it from your library.

The controller drives the launcher and the game through SDL, and FUSE is not required. Steam Deck support stays experimental
until someone has tried it on the hardware.

If it does not start from Steam, set the game's **Launch Options** to `APPIMAGE_EXTRACT_AND_RUN=1 %command%`.

## Known limits

- Not tested on real hardware yet: rendering, audio, controllers and performance are unconfirmed on Linux.
- The launcher updates the AppImage only under its release file name; a renamed file is left alone.
- Holding **Shift** to bring the launcher back works only if the window gets keyboard focus while Shift is still held,
  because SDL cannot read the keyboard before the window has focus. Keep Shift held until the launcher appears.
- The VSync setting maps to Vulkan's present mode: **Off** allows immediate presentation (tearing), **On** uses mailbox or
  FIFO.
- On a Wayland desktop with display scaling above 100%, window sizes can come out larger than chosen.
- **Internal render resolution** below 100% needs Vulkan's host render target path, the default. On a GPU where the
  runtime has to use its fragment shader interlock path instead, it draws at 100% and says so in the log.

## Building the AppImage

Build on Ubuntu 20.04 (glibc 2.31): the AppImage needs the glibc of the machine that built it, and `tools/package-linux.sh`
stops on a newer one. On Windows, a WSL distribution works; `tools/setup-linux-build-env.sh` prepares a clean one, run once as
root:

```sh
wsl --install -d Ubuntu-20.04
sudo bash tools/setup-linux-build-env.sh
```

Then, after `tools\codegen.ps1` has generated `generated/default` on Windows:

```sh
tools/package-linux.sh                          # incremental Release build, then package
tools/package-linux.sh --clean                  # clean Release build first
tools/package-linux.sh --out-dir DIR            # write the artifacts to DIR instead of dist
tools/package-linux.sh --verify-only dist/ElChavoKart-v0.1.0-x86_64.AppImage
```

The sources are copied to `~/eck-linux-build` (`ECK_WORK`) on the Linux file system: a Windows drive under `/mnt` is slow and
every file on it looks executable. The script never regenerates guest code: `tools/check-guest-code.sh` compares
`generated/codegen-inputs.stamp` with the manifest and config, and the build stops if they differ. No game file is needed or
packaged.

It builds the patched ReXGlue runtime when the patches changed (`tools/build-patched-rexruntime.sh`), builds the
`linux-amd64-release` preset, stages an allow-listed AppDir, audits it, builds the AppImage with the pinned runtime, extracts it
again and audits that, and writes `dist/ElChavoKart-v<version>-x86_64.AppImage` and `.sha256`. The program's debug information
goes to `dist/ElChavoKart-v<version>-x86_64-symbols/`, outside the AppImage. See [release.md](release.md).

`tools/build.sh` is the plain Linux build, into `out/build/<preset>`.

### The AppImage runtime

The runtime at the front of the AppImage is built by `tools/build-appimage-runtime.sh` from AppImage/type2-runtime at the tag,
commit, Alpine root file system and SHA-256 pinned in `patches/appimage-runtime/baseline.env`, with one project patch: when the
FUSE mount fails, it falls back to extract-and-run instead of giving up. Packaging refuses any runtime that is not byte for
byte the reviewed one, and never downloads a `continuous` or `latest` runtime. appimagetool is pinned by version and SHA-256.

## Tests

`tools/test.sh` is the Linux gate, real and headless: it compiles every production source, builds and runs the C++ suites,
runs the Worker's tests, checks a WebSocket over TLS with the system's OpenSSL, installs the real disc image with the
production installer when `game/` has one, inspects the GLIBC, RUNPATH and dependencies of the binaries, stages a fixture AppDir
and rejects one forbidden item at a time, and runs a fixture AppImage in extract-and-run mode and with FUSE taken away. Nothing
opens a window, plays audio or starts the game.

`tools\test.ps1` runs it inside WSL and compares the wire vectors of the two builds. `tools\test.ps1 -RequireLinux` is the
complete release gate: it also builds a scratch AppImage, checks it with `package-linux.sh --verify-only`, and has the Windows
and Linux builds race each other through one local Worker. It fails when WSL cannot run the gate. The exact command, what it
requires and what it cannot prove are in [release.md](release.md#complete-validation).

Suites that stay Windows-only because their code does not exist on Linux: `eck_update_helper_tests` (`trg-updater.exe`, the
Windows ZIP installer; the AppImage is replaced by the running program, covered by `eck_linux_appimage_tests`) and
`tools\test-release-check.ps1` (the ZIP folder scanner; the AppDir scanner has the same cases in `tools/test.sh`).
