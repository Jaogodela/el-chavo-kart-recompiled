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
`licenses\ReXGlue-LICENSE.txt`, `licenses\THIRD-PARTY-LICENSES.txt` and `assets\launcher\header-art.png`. Nothing else:
no game files, `default.xex` or `default.xexp` in any casing or any other `*.xex` or `*.xexp`, disc images,
`user_data`, logs, dumps, symbols, RenderDoc or Vulkan layers, updater workspaces or `trg-updater.log`, developer paths
or `DEBUG_SAULO` text.

`trg-updater.exe` in the ZIP is not the CMake build: `tools\build-updater.ps1` compiles the same `src\updater` sources
with MSYS2 CLANG64 (`C:\msys64`, package `mingw-w64-clang-x86_64-toolchain`): static libc++, only Windows system DLLs
and the Windows UCRT. Its debug information goes to `dist\ElChavoKart-v<version>-windows-x64-symbols\trg-updater.debug`,
outside the ZIP. `tools\test.ps1` runs the helper tests against it as well, and rebuilds it when it is missing or stale.

`tools\test-release-check.ps1` (part of `tools\test.ps1`) runs the scanner against staged folders that each add one
forbidden item.

## Publishing a release

The updater accepts only the latest stable GitHub release of `saulob/el-chavo-kart-recompiled`, tagged `v<version>`
(`v` prefix optional), with these assets next to each other:

| Asset | Content |
| --- | --- |
| `ElChavoKart-v<version>-windows-x64.zip` | the package above |
| `ElChavoKart-v<version>-windows-x64.sha256` | `<sha256>  ElChavoKart-v<version>-windows-x64.zip` |

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

Linux has no AppImage package for this project, so the launcher does not update itself there.

## Testing the banner without a public release

Windows only, from a disposable extraction of the package. The master switch makes the other variables effective;
without it they do nothing.

    $env:ECK_UPDATE_TEST = '1'
    $env:ECK_UPDATE_TEST_CURRENT_VERSION = '0.0.1'            # reported as the running version
    $env:ECK_UPDATE_TEST_RELEASE_JSON = 'C:\path\release.json' # GitHub release JSON used instead of the network
    $env:ECK_UPDATE_TEST_ASSET_SHA256 = '<sha256 of the ZIP>'  # trusted digest for the injected release

The variables are removed from the environment of the restarted program; everything else is kept.
