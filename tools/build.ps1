<#
.SYNOPSIS
  Builds El Chavo Kart Recompiled in one command: build\el_chavo_kart.exe, ready to start by hand.
  Guest code is never regenerated here: run tools\codegen.ps1 first, and again whenever it reports stale
  inputs. The ReXGlue runtime and GPU plugin are checked before the build and rebuilt from patches\rexglue-sdk
  when they are not current (network is needed the first time).

.DESCRIPTION
  1. Checks Visual Studio (C++ x64 tools and C++ Clang tools), git and the pinned TRG Launcher checkout.
  2. Checks that tools\rexglue (the pinned ReXGlue SDK) is present; run tools\codegen.ps1 first if it is not.
  3. Checks the game files in game\extracted; when they are missing, incomplete or from another build, extracts
     your disc image from game\*.iso (or -Iso) into a staging folder, verifies it and only then swaps it in.
     Only El Chavo Kart (USA), default.xex SHA-256 1DB4E96B...5CE120F, is accepted.
  4. Checks that generated\default matches the manifest, the XEX and the override files; stops with the exact
     command to run if it does not.
  5. Rebuilds the patched ReXGlue runtime and GPU plugin from patches\rexglue-sdk when they are not current.
  6. Configures and builds with Visual Studio's clang++, lld-link, CMake and Ninja (reusing an existing, matching
     CMake cache and Ninja graph when there is one), stages the runtime next to the executable and checks it.
  Only -Run launches the game.

.EXAMPLE
  .\tools\build.ps1                               # incremental build (RelWithDebInfo)
  .\tools\build.ps1 -Clean                        # clean configure and build (keeps build\user_data)
  .\tools\build.ps1 -Iso 'D:\Xbox 360\El Chavo Kart (USA).iso'
  .\tools\build.ps1 -Config Release -BuildDir build-release
  .\tools\build.ps1 -Run                          # build, then start the game

Exit codes: 0 success, 1 configure, build or runtime check failed, 2 a prerequisite or the game data is missing,
unsupported or stale.
#>
param(
    [switch]$Clean,
    [switch]$Run,
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',
    [string]$BuildDir,  # relative to the repository root; default: build
    [string]$Iso,       # default: the one .iso in game\
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'build-common.ps1')
. (Join-Path $PSScriptRoot 'codegen-inputs.ps1')
. (Join-Path $PSScriptRoot 'rexruntime-patch.ps1')
. (Join-Path $PSScriptRoot 'xiso.ps1')
. (Join-Path $PSScriptRoot 'game-data.ps1')

$TrgRepository = 'https://github.com/TekRantGaming/trg-launcher.git'
$TrgRevision = 'b384a28c712c9320eea8f9d50639dec49981b6c1'

$SdkRoot = Join-Path $Root 'tools\rexglue\win-amd64'
$Rexglue = Join-Path $SdkRoot 'bin\rexglue.exe'
$GameData = Join-Path $Root 'game\extracted'
$BuildDir = if (-not $BuildDir) { Join-Path $Root 'build' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
$Exe = Join-Path $BuildDir 'el_chavo_kart.exe'

function Stop-Build([int]$Code, [string]$Message) {
    Write-Host ''
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit $Code
}

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Invoke-Git {
    & git @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed ($LASTEXITCODE)" }
}

function Initialize-TrgLauncher {
    $dir = Join-Path $Root 'third_party\trg-launcher'
    if (Test-Path -LiteralPath (Join-Path $dir 'CMakeLists.txt')) { return }
    $registered = "$(git -C $Root ls-files -s -- third_party/trg-launcher)".Trim()
    if ($registered -match '^160000 ') {
        Write-Host 'Fetching the TRG Launcher submodule...'
        Invoke-Git -C $Root submodule update --init third_party/trg-launcher
        return
    }
    Write-Host "Fetching TRG Launcher $($TrgRevision.Substring(0, 7)) (network)..."
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    Invoke-Git clone --quiet $TrgRepository $dir
    Invoke-Git -C $dir checkout --quiet $TrgRevision
}

# -------------------------------------------------------------------------------------------------------------------
Write-Host '===================================================='
Write-Host " El Chavo Kart Recompiled - $Config build"
Write-Host '===================================================='

Write-Step 'Prerequisites'
if ($env:OS -ne 'Windows_NT') { Stop-Build 2 'tools\build.ps1 builds the Windows version; run it on Windows.' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Stop-Build 2 'git is required (https://git-scm.com/download/win).' }
try { $Toolchain = Get-BuildToolchain } catch { Stop-Build 2 "$($_.Exception.Message) Install Visual Studio with 'Desktop development with C++' and 'C++ Clang tools for Windows'." }
Write-Host "Toolchain: $($Toolchain.Id)"
try { Initialize-TrgLauncher } catch { Stop-Build 2 "TRG Launcher: $($_.Exception.Message)" }
Write-Host "TRG Launcher: $(("$(git -C (Join-Path $Root 'third_party\trg-launcher') rev-parse HEAD)").Trim().Substring(0, 7))"

Write-Step 'ReXGlue SDK'
$installedSdk = if (Test-Path -LiteralPath $Rexglue) { "$(& $Rexglue --version)".Trim() } else { '' }
if (-not $installedSdk) { Stop-Build 2 'Missing tools\rexglue. Run tools\codegen.ps1 first.' }
if ($installedSdk -ne $EckSdkVersion) {
    Stop-Build 2 "tools\rexglue has ReXGlue $installedSdk; the baseline is $EckSdkVersion. Delete tools\rexglue, then run tools\codegen.ps1."
}
Write-Host "ReXGlue SDK $installedSdk"

Write-Step 'Game files'
$problem = Get-InstalledGameProblem $GameData
if ($problem) {
    Write-Host "game\extracted: $problem"
    try { Install-GameData (Find-DiscImage (Join-Path $Root 'game') $Iso) $GameData } catch { Stop-Build 2 $_.Exception.Message }
} else {
    Write-Host 'game\extracted: El Chavo Kart (USA) 0.0.0.3, verified'
}

Write-Step 'Recompiled code'
if (-not (Test-Path -LiteralPath (Join-Path $Root "$CodegenOutput\sources.cmake"))) {
    Stop-Build 2 "Missing generated\default. Run:`n  .\tools\codegen.ps1`n  .\tools\build.ps1$(if ($Run) { ' -Run' })"
}
$guest = Get-CodegenFreshness $Root $installedSdk
if (-not $guest.Current) {
    Stop-Build 2 ("generated\default is stale: $($guest.Reasons -join '; ')`nRun:`n  .\tools\codegen.ps1`n  .\tools\build.ps1$(if ($Run) { ' -Run' })")
}
Write-Host 'generated\default: current'

Write-Step 'ReXGlue runtime'
if ($Config -eq 'Debug') {
    Write-Host 'Debug uses the stock rexruntimed.dll and rexgpu-xenosd.dll (patches apply to Release and RelWithDebInfo).'
} else {
    try { Update-PatchedRuntime $SdkRoot $Jobs } catch { Stop-Build 1 $_.Exception.Message }
}

Write-Step 'Build'
$SavedEnv = Save-BuildEnvironment
$Failure = $null
try {
    Import-BuildEnvironment $Toolchain
    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Write-Host 'Cleaning the build folder (keeping user_data)...'
        Get-ChildItem -LiteralPath $BuildDir -Force | Where-Object { $_.Name -ne 'user_data' } | Remove-Item -Recurse -Force
    }
    $cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
    $cache = Read-CMakeCache $BuildDir
    $problems = @(Get-CMakeCacheProblems $cache $Config $Toolchain $SdkRoot)
    $configured = (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja')) -and ($cache.Count -gt 0) -and ($problems.Count -eq 0)
    if (-not $configured) {
        if ($problems.Count -and (Test-Path -LiteralPath $cacheFile)) {
            Write-Host "Reconfiguring: $($problems -join '; ')"
            Remove-Item -LiteralPath $cacheFile -Force
        }
        Write-Host 'Configuring...'
        & $Toolchain.CMake -S $Root -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
            "-DCMAKE_CXX_COMPILER:FILEPATH=$($Toolchain.ClangXX)" "-DCMAKE_C_COMPILER:FILEPATH=$($Toolchain.Clang)" `
            "-DCMAKE_LINKER:FILEPATH=$($Toolchain.LldLink)" "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($Toolchain.Ninja)"
        if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    if (Test-BuildGraphRunsCodegen $BuildDir) { throw 'The build graph runs ReXGlue codegen; only tools\codegen.ps1 may run it.' }
    Write-Host "Building ($Config)..."
    $parallel = if ($Jobs -gt 0) { @('--parallel', $Jobs) } else { @() }
    & $Toolchain.CMake --build $BuildDir @parallel
    if ($LASTEXITCODE) { throw "Build failed ($LASTEXITCODE)" }
} catch {
    $Failure = $_.Exception.Message
} finally {
    Restore-BuildEnvironment $SavedEnv
}
if ($Failure) { Stop-Build 1 $Failure }

Write-Step 'Output'
Sync-StagedRuntime $SdkRoot $BuildDir $Config
$problems = @(Test-StagedRuntime $SdkRoot $BuildDir $Config)
if ($Config -ne 'Debug') { $problems += @(Test-PatchedRuntime $SdkRoot) }
foreach ($file in @('el_chavo_kart.toml')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir $file) -PathType Leaf)) { $problems += "missing: $file" }
}
if ($problems.Count) {
    foreach ($problem in $problems) { Write-Host "  $problem" -ForegroundColor Red }
    Stop-Build 1 'The build output is not usable (see above).'
}
# Points the development build at game\extracted; a release package uses <exe folder>\game instead.
[IO.File]::WriteAllText((Join-Path $BuildDir 'game-folder.txt'), "$GameData`r`n", (New-Object Text.UTF8Encoding $false))

Write-Host ''
Write-Host '===================================================='
Write-Host ' BUILD SUCCESSFUL'
Write-Host '===================================================='
Write-Host "  Executable: $Exe"
Write-Host "  Game files: $GameData"
Write-Host "  Runtime:    $((Get-RuntimeDllNames $Config) -join ', ') match tools\rexglue"
Write-Host "  Saves, settings and logs: $(Join-Path $BuildDir 'user_data')"
Write-Host ''
if (-not $Run) { exit 0 }

$running = Get-Process -Name 'el_chavo_kart' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }
if ($running) {
    Write-Warning "el_chavo_kart.exe is already running (PID $($running.Id -join ', ')); not starting another copy."
    exit 0
}
Write-Host "Starting $Exe"
Start-Process -FilePath $Exe -WorkingDirectory $BuildDir
exit 0
