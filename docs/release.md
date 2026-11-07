# Release and self-update

The project version is the `project(el_chavo_kart VERSION x.y.z)` line in `CMakeLists.txt`. The executable's version
resource, the launcher subtitle and About page, the updater's current version, `tools\package-release.ps1` and the
artifact names all read it.

## Windows package

    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package-release.ps1            # Release build, then package
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package-release.ps1 -Clean     # clean Release build first
    pwsh -NoProfile -ExecutionPolicy Bypass -File tools\package-release.ps1 -VerifyOnly dist\<name>.zip

It builds `build-release\` (Release), requires `tools\rexglue` to hold the Release `rexruntime.dll` and
`rexgpu-xenos.dll` built from `patches\rexglue-sdk` (`rexruntime-patch.txt` fingerprint; stock or stale files are
refused), stages the allowlisted files into `dist\ElChavoKart-v<version>-windows-x64\`, runs
`tools\check-release-folder.ps1` on it, writes the ZIP and `.sha256` next to it, then extracts the ZIP and checks it
again.

The ZIP holds one folder, `ElChavoKart-v<version>-windows-x64`, with `el_chavo_kart.exe`, `trg-updater.exe`,
`rexruntime.dll`, `rexgpu-xenos.dll`, `el_chavo_kart.toml`, `README.txt`, `LICENSE`,
`licenses\ReXGlue-LICENSE.txt`, `licenses\THIRD-PARTY-LICENSES.txt`, `assets\launcher\header-art.png` and `assets\launcher\sponsor-heart.png`. Nothing else:
no game files, `default.xex` or `default.xexp` in any casing or any other `*.xex` or `*.xexp`, disc images,
`user_data`, logs, dumps, symbols, RenderDoc or Vulkan layers, updater workspaces or `trg-updater.log`, developer paths
or `DEBUG_SAULO` text.

`trg-updater.exe` in the ZIP is not the CMake build: `tools\build-updater.ps1` compiles the same `src\updater` sources
with MSYS2 CLANG64 (`C:\msys64`, package `mingw-w64-clang-x86_64-toolchain`): static libc++, only Windows system DLLs
and the Windows UCRT. Its debug information goes to `dist\ElChavoKart-v<version>-windows-x64-symbols\trg-updater.debug`,
outside the ZIP. `tools\test.ps1` runs the helper tests against it as well, and rebuilds it when it is missing or stale.

`tools\test-release-check.ps1` (part of `tools\test.ps1`) runs the scanner against staged folders that each add one
forbidden item.

## Linux AppImage (experimental)

    tools/package-linux.sh            # on Ubuntu 20.04 or WSL; incremental Release build, then package
    tools/package-linux.sh --clean    # clean Release build first
    tools/package-linux.sh --verify-only dist/ElChavoKart-v<version>-x86_64.AppImage

It builds the patched ReXGlue Linux runtime when the patches changed, builds the `linux-amd64-release` preset, stages an
allow-listed AppDir, checks RPATH, unresolved libraries, the glibc 2.31 baseline, local paths and forbidden files, builds the
AppImage with the pinned runtime (`patches/appimage-runtime`), extracts it again and checks it, and writes
`dist/ElChavoKart-v<version>-x86_64.AppImage` and `.sha256`. The program's debug information goes to
`dist/ElChavoKart-v<version>-x86_64-symbols/`, outside the AppImage. See [linux.md](linux.md).

The AppImage holds exactly the program, `librexruntime.so`, `librexgpu-xenos.so`, the conditional C++ runtime in `usr/lib`,
`el_chavo_kart.toml`, the launcher art, the licenses, `README.txt` (`packaging/linux/README.txt`), `AppRun`, the desktop file
and the icons. Nothing else: the rules of the Windows ZIP apply, and `tools/test.sh` proves them case by case.

Before a release, run `.\tools\test.ps1 -RequireLinux` (see Complete validation).

## Complete validation

    .\tools\test.ps1 -RequireLinux

This is the one command that says whether a release is ready. It ends with `ALL REQUIRED CHECKS PASSED` only when every
group below ran and passed; a stage that cannot run fails the gate instead of being skipped. A plain `.\tools\test.ps1`
is the faster developer run: it leaves out the scratch AppImage and the Windows compile, and ends as a partial run.

It needs Windows with the Visual Studio Clang toolchain, Node.js 22 or newer, and a WSL distribution prepared by
`tools/setup-linux-build-env.sh` (Ubuntu 20.04, glibc 2.31; `ECK_WSL_DISTRO` picks one). It starts neither the game nor the
launcher, opens no window, browser or audio device, never contacts the production service, a STUN server or a real router,
and does not touch `dist`: its scratch AppImage is built in a temporary folder.

| Group | What runs |
| --- | --- |
| Windows | the console suites listed for Windows in `tests\required-suites.txt` (among them the commands behind `Open logs folder`, `Report a bug` and links, built by the real `trg::OpenUrl` and `trg::OpenInFileManager` and captured instead of run), the Worker tests, the production client against the Worker running locally, the platform-sensitive and changed sources compiled with no new warning (`tests\known-warnings.txt` lists the accepted ones), the code-generation and build-graph checks |
| Linux | `tools/test.sh` in WSL: the Linux suites, the production sources, a local Worker with and without TLS, the real disc image installed with the production installer when `game\` holds one |
| Settings | the production `GameSettings` on real temporary files on Windows and on Linux: save, reload, invalid or partly written `settings.toml` set aside as `settings.toml.invalid`, folders with spaces and accents, the Connection test result, the AppImage user-data location with nothing written into the mount; the save files through the runtime's virtual file system (create, write, reopen, overwrite in place, letter case, delete); the achievements through ReXGlue's manager (46, 1000 gamerscore, unlock, duplicate, reload) |
| Worker | `npm test` in `server\eck-lobby` on Windows and on Linux, with the platform values and the dashboard schema |
| Cross-platform | the wire vectors of both builds byte for byte and read by the Worker, and a race between the Windows and the Linux build through one Worker, in both directions: sign-in, friends, invite, room, Direct or Relay path, race configuration, snapshot, input, RTT, telemetry, leaving |
| AppImage | a fixture AppImage in extract-and-run mode and with FUSE taken away (exit status, cleanup, original path), and a scratch AppImage built by `tools/package-linux.sh` and checked with `--verify-only` |
| Updater, Installer | the update logic for both platforms (asset selection, checksums, replacement, rollback, awkward paths) and the installer on synthetic images and on the real one |
| ReXGlue | both patch sets classified (`patches\rexglue-sdk\platforms.txt`), their stamps current, a stale, modified, stock or unstamped runtime refused, a stock GPU plugin refused, the Vulkan Internal render resolution decision (100%, 75% and 50% on the host render target path, 100% and a warning on any other) compiled from the header patch 0051 gives the GPU plugin, and that warning present in the built plugin |
| glibc / ELF | the highest GLIBC of every shipped binary against 2.31, GLIBCXX and CXXABI against what the bundled libstdc++ provides, RUNPATH and the libraries every binary needs |
| Packaging | the AppDir policy against a clean fixture and one forbidden item at a time, the pinned AppImage runtime, and the Windows release folder checker |
| Privacy | secrets, tokens, identity files and friend codes refused in both packages |
| Hygiene | conflict markers, game data, temporary files, `DEBUG_SAULO`, TODO, secret shapes and local paths in every release-facing file |

Every suite is named in `tests\required-suites.txt`. The gate fails when a listed suite is missing, when a suite runs that is
not listed, and when a test program is built and never executed. Counts are printed, not fixed: the suites may grow.

Passing it does not replace trying the game. Vulkan rendering, controller hardware, audio, NAT traversal over the internet,
a real router's UPnP and a Steam Deck are only proven by running the game on them.

## Release notes

`.github\release-notes-v<version>.md`. Its Checksums table is filled from the generated ZIP and AppImage.

## Publishing a release

The updater accepts only the latest stable GitHub release of `saulob/el-chavo-kart-recompiled`, tagged `v<version>`
(`v` prefix optional), with these assets next to each other:

| Asset | Content |
| --- | --- |
| `ElChavoKart-v<version>-windows-x64.zip` | the package above |
| `ElChavoKart-v<version>-windows-x64.sha256` | `<sha256>  ElChavoKart-v<version>-windows-x64.zip` |
| `ElChavoKart-v<version>-x86_64.AppImage` | the Linux AppImage |
| `ElChavoKart-v<version>-x86_64.sha256` | `<sha256>  ElChavoKart-v<version>-x86_64.AppImage` |

The SHA-256 is trusted from the GitHub asset digest first and from the `.sha256` asset otherwise. A release without one
of them is never installed. Drafts and prereleases are ignored.

## How the launcher updates itself

1. One asynchronous check per launcher session; a failure is silent and never blocks Play.
2. A newer stable release shows the banner in the header: version, release notes link and UPDATE NOW.
3. UPDATE NOW downloads the ZIP, checks its size and SHA-256, extracts it into a temporary staging folder (never over the
   installation) with the path, link and size checks, resolves the staging root (flat, or exactly one wrapper folder)
   and builds the manifest of release-owned files.
4. RESTART TO UPDATE copies the installed `trg-updater.exe` to `%TEMP%\trg-update-*`, writes the job there and quits the
   launcher. The helper waits for the launcher, backs up the release-owned files, replaces them, verifies the installed
   files, restarts `el_chavo_kart.exe` with the launcher open (`--eck_launcher=true`; the original arguments are kept
   and any `--eck_skip_launcher` / `--eck_launcher` is dropped) and removes the backup, staging folder and job. On any
   failure it restores the backup and restarts the previous program. The helper writes `trg-updater.log` in the
   workspace; the launcher deletes workspaces older than a day at a later start.
5. A release older than the updater does not need to contain `trg-updater.exe`: the running build supplies the helper.

Protected, never part of the manifest and never touched: `game`, `game-folder.txt`, `game-install-temp`,
`game-install-backup`, `user_data`, `user_data.migrating`, `logs`, `cache`, `content`, `save`, `saves`, `dlc`,
`475807D7`, `screenshots`, `settings.toml` (+ `.bak`, `.invalid`), and files with the extensions `.xex .xexp .xsh .xpso
.iso .xxx .upk .tfc .fsbcache .bik .sav .log .dmp .pdb .ilk .obj .bak .tmp .invalid .part .zip .appimage .7z .rar .tar
.gz`.

Config ownership: `el_chavo_kart.toml` beside the program is release-owned static configuration and is replaced by an
update. The player's choices live in `<user data>\settings.toml` (normally `user_data\settings.toml`) and are never
replaced.

On Linux the AppImage is the whole installation: the launcher downloads the new AppImage, checks its size and SHA-256 and
replaces the file this run was started from, keeping its permissions, then starts it again; if it cannot be started, the
previous AppImage is put back. See [linux.md](linux.md#launcher-updates).

## Testing the banner without a public release

On Windows from a disposable extraction of the package, on Linux from a disposable copy of the AppImage. The master
switch makes the other variables effective;
without it they do nothing.

    $env:ECK_UPDATE_TEST = '1'
    $env:ECK_UPDATE_TEST_CURRENT_VERSION = '0.0.1'            # reported as the running version
    $env:ECK_UPDATE_TEST_RELEASE_JSON = 'C:\path\release.json' # GitHub release JSON used instead of the network
    $env:ECK_UPDATE_TEST_ASSET_SHA256 = '<sha256 of the ZIP>'  # trusted digest for the injected release

The variables are removed from the environment of the restarted program; everything else is kept.
