<#
.SYNOPSIS
  The complete release parity gate: every headless check of the Windows and Linux builds. Nothing starts the game or
  the launcher, opens a window, a browser or audio, or contacts the production service or a real router.

.DESCRIPTION
  .\tools\test.ps1 -RequireLinux is the complete gate and the only mode that may print ALL REQUIRED CHECKS PASSED. It
  requires every stage below to run; one that cannot run fails the gate instead of being skipped.
    - the Windows console suites listed in tests\required-suites.txt (installer, UI text, achievements, launcher logic,
      crash report, cursor, hotkeys, graphics, frame limiter, input focus, online, update, paths, wire vectors, ...)
    - the Worker tests (server\eck-lobby, npm test command) and the production client against the Worker's own code
    - the platform-sensitive and changed production sources, compiled for Windows, with no new compiler warnings
    - the ReXGlue patch sets of both platforms: classification, freshness, and stale/modified/stock runtimes refused
    - the release folder checker against folders that each add one forbidden item, and a secret and privacy scan
    - generated\default freshness, no codegen in the build graph, source tree hygiene
    - the Linux gate, tools/test.sh, inside WSL (Ubuntu 20.04, glibc 2.31): Linux suites, Worker, packaging policy,
      ELF checks, fixture AppImage with extract-and-run and FUSE fallback, a scratch AppImage built and verified with
      package-linux.sh --verify-only, and a race between the Windows and Linux builds through one Worker
  A plain run keeps the fast path: it skips the scratch AppImage and the Windows compile, and ends as a partial run.

.EXAMPLE
  .\tools\test.ps1 -RequireLinux        # the complete release gate
  .\tools\test.ps1                      # developer run: Windows suites, and the Linux gate when WSL can run it
  .\tools\test.ps1 -Filter installer    # only the console test whose name matches
  .\tools\test.ps1 -SkipLinux           # Windows only
  $env:ECK_WSL_DISTRO = 'Ubuntu-20.04'; .\tools\test.ps1 -RequireLinux    # choose the WSL distro
#>
param(
    [string]$Filter = '',
    [string]$BuildDir,  # relative to the repository root; default: out\tests
    [switch]$SkipLinux,
    [switch]$RequireLinux
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'build-common.ps1')
. (Join-Path $PSScriptRoot 'rexruntime-patch.ps1')
. (Join-Path $PSScriptRoot 'codegen-inputs.ps1')
$BuildDir = if (-not $BuildDir) { Join-Path $Root 'out\tests' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }
if ($RequireLinux -and ($SkipLinux -or $Filter)) { throw '-RequireLinux is the complete gate: it cannot be combined with -SkipLinux or -Filter' }
$Complete = [bool]$RequireLinux

$Failures = New-Object System.Collections.Generic.List[string]
$GroupNames = 'Windows', 'Linux', 'Settings', 'Worker', 'Cross-platform', 'AppImage', 'Updater', 'Installer', 'ReXGlue', 'glibc / ELF', 'Packaging', 'Privacy', 'Hygiene'
$Groups = [ordered]@{}
foreach ($name in $GroupNames) { $Groups[$name] = @{ Pass = 0; Fail = 0 } }
$Stats = [ordered]@{}
$script:StepSkipped = $false

function Add-Evidence([string[]]$Names, [bool]$Ok) {
    foreach ($name in $Names) { if ($Ok) { $Groups[$name].Pass++ } else { $Groups[$name].Fail++ } }
}

function Skip-Step([string]$Reason) {
    if ($Complete) { throw "required by the complete gate but not run: $Reason" }
    Write-Host "NOT RUN, this run is partial: $Reason" -ForegroundColor Yellow
    $script:StepSkipped = $true
}

function Test-Step([string]$Name, [string[]]$Evidence, [scriptblock]$Body) {
    Write-Host "== $Name" -ForegroundColor Cyan
    $script:StepSkipped = $false
    try {
        & $Body
        if (-not $script:StepSkipped) { Add-Evidence $Evidence $true }
    } catch {
        $Failures.Add("${Name}: $($_.Exception.Message)")
        Add-Evidence $Evidence $false
        Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Read-Counts([string[]]$Lines) {
    $checks = 0
    $failures = 0
    foreach ($line in $Lines) {
        if ($line -match '(\d+) checks, (\d+) failures') { $checks += [int]$Matches[1]; $failures += [int]$Matches[2] }
        elseif ($line -match '(passed|FAILED) \((\d+) checks, (\d+) failed\)') { $checks += [int]$Matches[2]; $failures += [int]$Matches[3] }
    }
    return @{ Checks = $checks; Failures = $failures }
}

function Get-NodeProgram {
    $node = Get-Command node -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $node) { throw 'Node.js was not found; the Worker tests need Node 22 or newer' }
    return $node.Source
}

# Starts the Worker's own code locally (node:http and node:sqlite, in memory) and returns the process and its port.
function Start-LocalWorker {
    $node = Get-NodeProgram
    $failures = @()
    foreach ($try in 1..6) {
        $port = Get-Random -Minimum 23000 -Maximum 29000
        $log = Join-Path ([IO.Path]::GetTempPath()) "eck-worker-$port.log"
        $arguments = @((Join-Path $Root 'server\eck-lobby\local\server.js'), '--host', '127.0.0.1', '--port', "$port", '--db', ':memory:')
        $process = Start-Process -FilePath $node -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        for ($attempt = 0; $attempt -lt 50 -and -not $process.HasExited; $attempt++) {
            try {
                $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $port)
                $client.Dispose()
                return @{ Process = $process; Port = $port; Log = $log }
            } catch { Start-Sleep -Milliseconds 100 }
        }
        $failures += "port ${port}: $((Get-Content -Raw -LiteralPath "$log.err" -ErrorAction SilentlyContinue) -replace '\s+', ' ')"
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $log, "$log.err" -ErrorAction SilentlyContinue
    }
    throw "the local Worker did not start: $($failures -join ' | ')"
}

function Stop-LocalWorker($Worker) {
    Stop-Process -Id $Worker.Process.Id -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $Worker.Log, "$($Worker.Log).err" -ErrorAction SilentlyContinue
}

function Read-SuiteManifest {
    $manifest = @{ both = @(); windows = @(); linux = @(); service = @(); 'linux-service' = @() }
    foreach ($line in Get-Content -LiteralPath (Join-Path $Root 'tests\required-suites.txt')) {
        if ($line -match '^(both|windows|linux|service|linux-service)\s+(\S+)\s*$') { $manifest[$Matches[1]] += $Matches[2] }
    }
    foreach ($kind in 'both', 'windows', 'linux', 'service', 'linux-service') {
        if (-not $manifest[$kind].Count) { throw "tests\required-suites.txt lists no '$kind' suite" }
    }
    return $manifest
}

function Get-PlatformSensitiveSources {
    Push-Location $Root
    try {
        $sensitive = @(& git grep -l -E '_WIN32|__linux__' -- 'src/*.cpp')
        $changed = @(& git diff --name-only HEAD -- 'src/*.cpp') + @(& git ls-files --others --exclude-standard -- 'src/*.cpp')
        return @($sensitive + $changed | Where-Object { $_ -and $_ -notmatch 'third_party' -and (Test-Path -LiteralPath $_) } | Sort-Object -Unique)
    } finally { Pop-Location }
}

# Warnings of the program's own sources: known (tests\known-warnings.txt), new, and third-party.
function Get-WarningReport([string[]]$Lines, [string]$Platform) {
    $known = @(Get-Content -LiteralPath (Join-Path $Root 'tests\known-warnings.txt') | Where-Object { $_ -match '^\w+\|' })
    $rootPrefix = ($Root -replace '\\', '/').TrimEnd('/') + '/'
    $report = @{ New = New-Object System.Collections.Generic.List[string]; Known = 0; ThirdParty = 0 }
    foreach ($line in @($Lines | Where-Object { $_ -match ': warning: ' } | Sort-Object -Unique)) {
        if ($line -notmatch '^(?<file>.+?):\d+:\d+: warning: (?<text>.*)$') { continue }
        $file = $Matches.file -replace '\\', '/'
        $flag = if ($Matches.text -match '\[(-W[\w-]+)\]\s*$') { $Matches[1] } else { '' }
        $relative = if ($file.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { $file.Substring($rootPrefix.Length) } else { $file }
        if ($relative -match 'third_party|trg-launcher|tools/rexglue|rexglue-sdk') { $report.ThirdParty++ }
        elseif ($known -contains "$Platform|$relative|$flag") { $report.Known++ }
        else { $report.New.Add($line.Substring(0, [Math]::Min($line.Length, 220))) }
    }
    return $report
}

$Manifest = Read-SuiteManifest
$WindowsSuites = @($Manifest.both) + @($Manifest.windows)
$KnownPrograms = @($Manifest.both) + @($Manifest.windows) + @($Manifest.linux) + @($Manifest.service) + @($Manifest.'linux-service') +
    @('eck_trg_updater', 'eck_update_restart_marker')
$SuiteEvidence = @{ eck_installer_tests = 'Installer'; eck_update_tests = 'Updater'; eck_update_helper_tests = 'Updater'; eck_wire_parity_tests = 'Cross-platform'; eck_settings_tests = 'Settings'; eck_save_files_tests = 'Settings'; eck_achievement_store_tests = 'Settings' }
$WindowsRun = @{ Suites = 0; Checks = 0; Failures = 0 }

if (-not $Filter -or ($WindowsSuites -join ' ') -match $Filter) {
    Test-Step 'Console tests (tests\CMakeLists.txt, suites from tests\required-suites.txt)' @('Windows') {
        $Toolchain = Get-BuildToolchain
        $SavedEnv = Save-BuildEnvironment
        try {
            Import-BuildEnvironment $Toolchain
            $windowsSdk = Join-Path $Root 'tools\rexglue\win-amd64'
            if (-not (Test-Path -LiteralPath (Join-Path $windowsSdk 'lib\cmake\rexglue\rexglueConfig.cmake'))) {
                throw 'tools\rexglue\win-amd64 is not set up: the settings tests link the ReXGlue runtime'
            }
            & $Toolchain.CMake -S (Join-Path $Root 'tests') -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
                "-DCMAKE_CXX_COMPILER:FILEPATH=$($Toolchain.ClangXX)" "-DCMAKE_LINKER:FILEPATH=$($Toolchain.LldLink)" `
                "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($Toolchain.Ninja)" "-DECK_REXGLUE_SDK:PATH=$windowsSdk" | Out-Host
            if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
            & $Toolchain.CMake --build $BuildDir | Out-Host
            if ($LASTEXITCODE) { throw "Test build failed ($LASTEXITCODE)" }
        } finally { Restore-BuildEnvironment $SavedEnv }

        $missing = @(@($WindowsSuites) + @($Manifest.service) | Where-Object { -not (Test-Path -LiteralPath (Join-Path $BuildDir "$_.exe")) })
        if ($missing.Count) { throw "required test programs were not built: $($missing -join ', ')" }
        $configured = @(Select-String -LiteralPath (Join-Path $BuildDir 'build.ninja') -Pattern '^build (eck_\w+)\.exe:' |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
        $unlisted = @($configured | Where-Object { $KnownPrograms -notcontains $_ })
        if ($unlisted.Count) { throw "test programs tests\required-suites.txt does not list: $($unlisted -join ', ')" }

        $failed = New-Object System.Collections.Generic.List[string]
        foreach ($name in $WindowsSuites) {
            if ($Filter -and $name -notmatch $Filter) { continue }
            Write-Host "Running $name..."
            $output = @(& (Join-Path $BuildDir "$name.exe") 2>&1 | ForEach-Object { "$_" })
            $code = $LASTEXITCODE
            $output | Out-Host
            $counts = Read-Counts $output
            $WindowsRun.Suites++
            $WindowsRun.Checks += $counts.Checks
            $WindowsRun.Failures += $counts.Failures
            $ok = $code -eq 0 -and $counts.Failures -eq 0
            if ($SuiteEvidence[$name]) { Add-Evidence @($SuiteEvidence[$name]) $ok }
            if (-not $ok) { $failed.Add("$name (exit $code, $($counts.Failures) reported failures)") }
        }
        $Stats['Windows console suites'] = "$($WindowsRun.Suites) suites, $($WindowsRun.Checks) reported checks, $($WindowsRun.Failures) reported failures"
        if ($failed.Count) { throw "failed: $($failed -join '; ')" }
    }
}

if (-not $Filter -or 'worker' -match $Filter) {
    Test-Step 'Worker tests on Windows (server\eck-lobby, canonical npm test command)' @('Worker') {
        $node = Get-NodeProgram
        $package = Get-Content -Raw -LiteralPath (Join-Path $Root 'server\eck-lobby\package.json')
        if ($package -notmatch [regex]::Escape('"test": "node --test \"test/*.test.js\""')) { throw 'the Worker npm test command is not the one this gate runs' }
        $saved = [Console]::OutputEncoding
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        Push-Location (Join-Path $Root 'server\eck-lobby')
        try {
            $output = @(& $node --test 'test/*.test.js' 2>&1 | ForEach-Object { "$_" })
            $code = $LASTEXITCODE
        } finally { Pop-Location; [Console]::OutputEncoding = $saved }
        $output | Select-Object -Last 14 | Out-Host
        $total = @{}
        foreach ($line in $output) { if ($line -match '(?:^|\s)(tests|fail|skipped|cancelled) (\d+)\s*$') { $total[$Matches[1]] = [int]$Matches[2] } }
        $Stats['Worker (Windows Node)'] = "$($total.tests) tests, $($total.fail) failed, $($total.skipped) skipped"
        if ($code -or -not $total.tests -or $total.fail -or $total.skipped -or $total.cancelled) {
            throw "the Worker suite ran $($total.tests) tests: $($total.fail) failed, $($total.skipped) skipped, $($total.cancelled) cancelled, exit $code"
        }
    }
}

if (-not $Filter -or 'integration' -match $Filter) {
    Test-Step 'Online integration on Windows: the production client against the Worker running locally' @('Windows', 'Cross-platform') {
        $program = Join-Path $BuildDir 'eck_online_integration_tests.exe'
        if (-not (Test-Path -LiteralPath $program)) { throw 'eck_online_integration_tests.exe is not built' }
        $worker = Start-LocalWorker
        try {
            $env:ECK_LOBBY_TEST_URL = "ws://127.0.0.1:$($worker.Port)/"
            $output = @(& $program 2>&1 | ForEach-Object { "$_" })
            $code = $LASTEXITCODE
        } finally { Remove-Item Env:ECK_LOBBY_TEST_URL -ErrorAction SilentlyContinue; Stop-LocalWorker $worker }
        $output | Out-Host
        $counts = Read-Counts $output
        $Stats['Windows online integration'] = "$($counts.Checks) checks, $($counts.Failures) failures"
        if ($code -or $counts.Failures -or -not $counts.Checks) { throw "online integration failed (exit $code)" }
    }
}

if (-not $Filter -or 'online-instance-logs' -match $Filter) {
    Test-Step 'Online instances keep the original log name' @('Windows') {
        $app = Get-Content -Raw -LiteralPath (Join-Path $Root 'src\el_chavo_kart_app.h')
        if ($app -notmatch 'new ElChavoKartApp\(ctx, "el_chavo_kart"') { throw 'the log basename is no longer el_chavo_kart' }
        $script = Join-Path $PSScriptRoot 'online-instance.ps1'
        if ((Get-Content -Raw -LiteralPath $script) -notmatch [regex]::Escape('$env:ECK_INSTANCE = $label')) { throw 'ECK_INSTANCE is no longer set' }
        $temp = Join-Path ([IO.Path]::GetTempPath()) "eck-instances-$([Guid]::NewGuid().ToString('N'))"
        try {
            $next = {
                param($Name)
                $text = (& $script -Name $Name -PrepareOnly -InstanceRoot $temp -CopyFrom $temp 6>&1 | ForEach-Object { "$_" }) -join "`n"
                if ($text -notmatch 'log file:\s+(.+)') { throw "no log file reported for instance $Name" }
                $Matches[1].Trim()
            }
            foreach ($name in 'a', 'b') {
                $first = & $next $name
                $expected = [IO.Path]::Combine($temp, $name, 'logs', 'el_chavo_kart_001.log')
                if ($first -ne $expected) { throw "instance ${name}: $first" }
                Set-Content -LiteralPath $first -Value ''
                Set-Content -LiteralPath (Join-Path (Split-Path $first) "eck_${name}_009.log") -Value ''
                $second = Split-Path -Leaf (& $next $name)
                if ($second -ne 'el_chavo_kart_002.log') { throw "instance ${name}: $second" }
            }
            Write-Host 'Instances a and b: el_chavo_kart_NNN.log in their own folders, ECK_INSTANCE kept.'
        } finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force } }
    }
}

if (-not $Filter -or 'trg-patches' -match $Filter) {
    Test-Step 'TRG Launcher patch set' @('Packaging') {
        $dir = Join-Path $Root 'patches\trg-launcher'
        $files = @(Get-ChildItem -LiteralPath $dir -Filter '*.patch' -File)
        if (-not $files.Count) { throw 'No patches in patches\trg-launcher' }
        $numbers = @{}
        foreach ($file in $files) {
            if ($file.Name -notmatch '^(?<number>[0-9]{4})-[a-z0-9]+(-[a-z0-9]+)*\.patch$') {
                throw "patches\trg-launcher\$($file.Name) is not named NNNN-name.patch"
            }
            $number = $Matches['number']
            if ($numbers.ContainsKey($number)) { throw "patches\trg-launcher\$($file.Name) and $($numbers[$number]) share the number $number" }
            $numbers[$number] = $file.Name
        }
        Write-Host "$($files.Count) patches, uniquely numbered 0001-$($files.Count.ToString('0000'))."
    }
}

if (-not $Filter -or 'release-check' -match $Filter) {
    Test-Step 'Release folder check cases' @('Packaging', 'Privacy') {
        & (Join-Path $PSScriptRoot 'test-release-check.ps1') | Out-Host
        if ($LASTEXITCODE) { throw 'tools\test-release-check.ps1 failed' }
    }
}

if (-not $Filter -or 'release-helper' -match $Filter) {
    Test-Step 'Release update helper (MSYS2 CLANG64 build)' @('Updater', 'Windows') {
        $tests = Join-Path $BuildDir 'eck_update_helper_tests.exe'
        if (-not (Test-Path -LiteralPath $tests -PathType Leaf)) { throw 'eck_update_helper_tests.exe is not built (run the console tests first)' }
        $releaseUpdaterDir = Join-Path $BuildDir 'trg-updater-clang64'
        $releaseUpdater = Join-Path $releaseUpdaterDir 'trg-updater.exe'
        $updaterSources = @(Get-ChildItem -LiteralPath (Join-Path $Root 'src\updater') -File | Where-Object Extension -in '.cpp', '.h') +
            @(Get-Item -LiteralPath (Join-Path $Root 'CMakeLists.txt'), (Join-Path $PSScriptRoot 'build-updater.ps1'))
        $newestSource = ($updaterSources | Measure-Object LastWriteTimeUtc -Maximum).Maximum
        if (-not (Test-Path -LiteralPath $releaseUpdater) -or (Get-Item -LiteralPath $releaseUpdater).LastWriteTimeUtc -lt $newestSource) {
            & (Join-Path $PSScriptRoot 'build-updater.ps1') -OutDir $releaseUpdaterDir | Out-Host
        }
        & $tests $releaseUpdater | Out-Host
        if ($LASTEXITCODE) { throw "eck_update_helper_tests against the release helper failed ($LASTEXITCODE)" }
    }
}

if (-not $Filter -or 'function-overrides' -match $Filter) {
    Test-Step 'config\overrides.toml vs config\discovered-functions.toml' @('Windows') {
        function Get-TomlAddresses([string]$Path) {
            $addresses = New-Object System.Collections.Generic.List[string]
            foreach ($line in Get-Content -LiteralPath $Path) {
                if ($line -match '^\s*(0x[0-9A-Fa-f]+)\s*=') { $addresses.Add($Matches[1].ToUpperInvariant()) }
            }
            return $addresses
        }
        $overrides = Get-TomlAddresses (Join-Path $Root 'config\overrides.toml')
        $discovered = Get-TomlAddresses (Join-Path $Root 'config\discovered-functions.toml')
        $overlap = @($overrides | Where-Object { $discovered -contains $_ })
        if ($overlap.Count) { throw "addresses in both files: $($overlap -join ', ')" }
        $dupOverrides = @($overrides | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
        if ($dupOverrides.Count) { throw "duplicate addresses in config\overrides.toml: $($dupOverrides -join ', ')" }
        $dupDiscovered = @($discovered | Group-Object | Where-Object Count -gt 1 | ForEach-Object Name)
        if ($dupDiscovered.Count) { throw "duplicate addresses in config\discovered-functions.toml: $($dupDiscovered -join ', ')" }
        Write-Host "$($overrides.Count) manual overrides, $($discovered.Count) discovered functions, no overlap or duplicates."
    }
}

if (-not $Filter -or 'discovered-functions' -match $Filter) {
    Test-Step 'Discovered functions are generated and calibrated' @('Windows') {
        $discovered = Get-Content -LiteralPath (Join-Path $Root 'config\discovered-functions.toml')
        foreach ($address in '0x830F6C58', '0x830F6D7C', '0x830F6A60', '0x824B7168') {
            if (-not ($discovered | Select-String -SimpleMatch "$address =" -Quiet)) { throw "$address is missing from config\discovered-functions.toml" }
        }
        $register = Join-Path $Root 'generated\default\el_chavo_kart_register.cpp'
        $image = Join-Path $Root 'game\analysis\default.image.bin'
        $python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not (Test-Path -LiteralPath $register) -or -not (Test-Path -LiteralPath $image) -or -not $python) {
            Skip-Step 'calibration verification needs generated\default, game\analysis\default.image.bin and Python'
            return
        }
        & $python.Source -I (Join-Path $PSScriptRoot 'find-missing-functions.py') --verify | Out-Host
        if ($LASTEXITCODE) { throw 'discovered functions are not all registered exactly once' }
    }
}

if ($Filter -match 'callbacks') {
    Test-Step 'Guest callback targets are registered' @('Windows') {
        $python = Get-Command python, py -ErrorAction Stop | Select-Object -First 1
        & $python.Source -I (Join-Path $PSScriptRoot 'find-missing-functions.py') --callbacks | Out-Host
        if ($LASTEXITCODE) { throw 'unregistered guest callback target' }
    }
}

if (-not $Filter -or 'codegen' -match $Filter) {
    Test-Step 'tools\codegen.ps1 exists and parses' @('Windows') {
        $path = Join-Path $PSScriptRoot 'codegen.ps1'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'tools\codegen.ps1 is missing' }
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors) | Out-Null
        if ($errors.Count) { throw "$($errors.Count) parse error(s) in tools\codegen.ps1" }
        Write-Host 'tools\codegen.ps1 exists and parses.'
    }
}

if (-not $Filter -or 'codegen-freshness' -match $Filter) {
    Test-Step 'generated\default freshness' @('Windows') {
        $sdkVersion = Get-InstalledRexglueVersion $Root
        if (-not $sdkVersion) { Skip-Step 'tools\rexglue is not installed (tools\codegen.ps1 installs it)'; return }
        if (-not (Test-Path -LiteralPath (Join-Path $Root "$CodegenOutput\sources.cmake") -PathType Leaf)) {
            throw "$CodegenOutput\sources.cmake is missing; run tools\codegen.ps1"
        }
        $freshness = Get-CodegenFreshness $Root $sdkVersion
        if ($freshness.Current) { Write-Host 'generated\default is current and matches its input stamp.' }
        else { throw "generated\default is out of date: $($freshness.Reasons -join '; ') (run tools\codegen.ps1)" }
    }
}

if (-not $Filter -or 'build-graph' -match $Filter) {
    Test-Step 'CMake/Ninja build graph does not run ReXGlue codegen' @('Windows') {
        $sdkReady = Test-Path -LiteralPath (Join-Path $Root 'tools\rexglue\win-amd64\bin\rexglue.exe')
        $guestReady = Test-Path -LiteralPath (Join-Path $Root 'generated\default\sources.cmake')
        $trgReady = (Test-Path -LiteralPath (Join-Path $Root 'third_party\trg-launcher\CMakeLists.txt')) -or $env:ECK_TRG_LAUNCHER_DIR
        if (-not ($sdkReady -and $guestReady -and $trgReady)) {
            Skip-Step 'tools\rexglue, generated\default or third_party\trg-launcher is not set up'
            return
        }
        $configDir = if (Test-Path -LiteralPath (Join-Path $Root 'build\CMakeCache.txt')) { Join-Path $Root 'build' } else { Join-Path $Root 'out\build-graph-check' }
        $Toolchain = Get-BuildToolchain
        $SavedEnv = Save-BuildEnvironment
        try {
            Import-BuildEnvironment $Toolchain
            $cache = Read-CMakeCache $configDir
            $problems = @(Get-CMakeCacheProblems $cache 'RelWithDebInfo' $Toolchain (Join-Path $Root 'tools\rexglue\win-amd64'))
            $configured = (Test-Path -LiteralPath (Join-Path $configDir 'build.ninja')) -and ($cache.Count -gt 0) -and ($problems.Count -eq 0)
            if (-not $configured) {
                & $Toolchain.CMake -S $Root -B $configDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
                    "-DCMAKE_CXX_COMPILER:FILEPATH=$($Toolchain.ClangXX)" "-DCMAKE_C_COMPILER:FILEPATH=$($Toolchain.Clang)" `
                    "-DCMAKE_LINKER:FILEPATH=$($Toolchain.LldLink)" "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($Toolchain.Ninja)" | Out-Host
                if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
            }
        } finally { Restore-BuildEnvironment $SavedEnv }
        if (Test-BuildGraphRunsCodegen $configDir) { throw "$configDir\build.ninja runs ReXGlue codegen; only tools\codegen.ps1 may run it" }
        Write-Host "$configDir\build.ninja does not run ReXGlue codegen."
    }
}

if (-not $Filter -or 'runtime-patch-freshness' -match $Filter) {
    Test-Step 'ReXGlue runtime patch freshness' @('ReXGlue') {
        $sdkDir = Join-Path $Root 'tools\rexglue\win-amd64'
        if (-not (Test-Path -LiteralPath (Join-Path $sdkDir 'bin\rexruntime.dll'))) {
            Skip-Step 'tools\rexglue is not installed (tools\codegen.ps1 installs it)'
            return
        }
        $problems = @(Test-PatchedRuntime $sdkDir)
        if (-not $problems.Count) { Write-Host 'Installed runtime and GPU plugin match patches\rexglue-sdk.'; return }
        if (-not (Test-Path -LiteralPath (Join-Path $sdkDir 'rexruntime-patch.txt'))) {
            Skip-Step 'the patched Windows runtime is not built (tools\build.ps1 builds it)'
            return
        }
        throw "the installed runtime is stale (tools\build.ps1 rebuilds it): $($problems -join '; ')"
    }
}

if (-not $Filter -or 'rexglue-patches' -match $Filter) {
    Test-Step 'ReXGlue patches: classification, fingerprints, stale/modified/stock runtimes refused' @('ReXGlue') {
        $patchDir = Join-Path $Root 'patches\rexglue-sdk'
        $applied = @(Get-RuntimePatchFiles)
        $appliedNames = @($applied | ForEach-Object Name)
        Write-Host "Windows: $($applied.Count) patches, fingerprint $((Get-RuntimeInputsSha256).Substring(0, 16))"
        $all = @(Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' -File | ForEach-Object Name)
        $pairs = @(Get-Content -LiteralPath (Join-Path $patchDir 'platforms.txt') | Where-Object { $_ -match '^\S+\s+\S+\s*$' } |
            ForEach-Object { $parts = $_ -split '\s+'; [pscustomobject]@{ Windows = $parts[0]; Linux = $parts[1] } })
        $problems = New-Object System.Collections.Generic.List[string]
        foreach ($name in $all) {
            if ($name -like '*.linux.patch') {
                if ($appliedNames -contains $name) { $problems.Add("$name is a Linux patch the Windows build applies") }
                if (-not ($pairs | Where-Object { $_.Linux -eq $name })) { $problems.Add("$name is not listed in platforms.txt") }
            } elseif ($name -like '*.windows.patch') {
                if ($appliedNames -notcontains $name) { $problems.Add("$name is a Windows patch the Windows build does not apply") }
                if (-not ($pairs | Where-Object { $_.Windows -eq $name })) { $problems.Add("$name has no Linux counterpart in platforms.txt") }
            } elseif ($appliedNames -notcontains $name) {
                $problems.Add("the common patch $name is missing from the Windows build")
            }
        }
        foreach ($pair in $pairs) {
            foreach ($file in $pair.Windows, $pair.Linux) { if ($file -ne '-' -and $all -notcontains $file) { $problems.Add("platforms.txt names $file, which does not exist") } }
        }
        if ($problems.Count) { throw ($problems -join '; ') }
        $common = @($all | Where-Object { $_ -notlike '*.windows.patch' -and $_ -notlike '*.linux.patch' }).Count
        Write-Host "classification: $common common, $(@($all | Where-Object { $_ -like '*.windows.patch' }).Count) Windows-only (each paired), $(@($all | Where-Object { $_ -like '*.linux.patch' }).Count) Linux"
        $Stats['ReXGlue Windows'] = "$($applied.Count) patches, fingerprint $((Get-RuntimeInputsSha256).Substring(0, 16))"

        $sdk = Join-Path $Root 'tools\rexglue\win-amd64'
        if (-not (Test-Path -LiteralPath (Join-Path $sdk 'rexruntime-patch.txt'))) { Skip-Step 'the patched Windows runtime is not built (tools\build.ps1 builds it)'; return }
        $current = @(Test-PatchedRuntime $sdk)
        if ($current.Count) { throw "the installed runtime is stale: $($current -join '; ')" }

        $control = Join-Path $Root 'out\gate-rexglue'
        Remove-Item -LiteralPath $control -Recurse -Force -ErrorAction SilentlyContinue
        try {
            foreach ($leaf in $RuntimeFiles) {
                $target = Join-Path $control $leaf
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                New-Item -ItemType HardLink -Path $target -Target (Join-Path $sdk $leaf) | Out-Null
            }
            $stamp = Join-Path $control $RuntimeStampName
            Copy-Item -LiteralPath (Join-Path $sdk $RuntimeStampName) $stamp
            if (@(Test-PatchedRuntime $control).Count) { throw 'the control copy of the patched runtime is not accepted' }
            $good = [IO.File]::ReadAllText($stamp)
            [IO.File]::WriteAllText($stamp, ($good -replace '(?m)^inputs_sha256=.*$', 'inputs_sha256=0000'))
            if (-not (@(Test-PatchedRuntime $control) -match 'other inputs')) { throw 'a stale runtime was accepted' }
            Write-Host 'refused: stale (built from other inputs)'
            [IO.File]::WriteAllText($stamp, $good)
            $modified = Join-Path $control 'lib\rexruntime.lib'
            Remove-Item -LiteralPath $modified
            Copy-Item -LiteralPath (Join-Path $sdk 'lib\rexruntime.lib') $modified
            [IO.File]::AppendAllText($modified, 'x')
            if (-not (@(Test-PatchedRuntime $control) -match 'does not match')) { throw 'a modified runtime was accepted' }
            Write-Host 'refused: modified'
            Remove-Item -LiteralPath $modified
            New-Item -ItemType HardLink -Path $modified -Target (Join-Path $sdk 'lib\rexruntime.lib') | Out-Null

            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $Root 'tools\rexglue\rexglue-sdk-0.10.0-win-amd64.zip'))
            try {
                $entry = $zip.Entries | Where-Object { $_.FullName -eq 'win-amd64/bin/rexruntime.dll' } | Select-Object -First 1
                if (-not $entry) { throw 'the SDK download has no rexruntime.dll for the control run' }
                $dll = Join-Path $control 'bin\rexruntime.dll'
                Remove-Item -LiteralPath $dll
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dll)
            } finally { $zip.Dispose() }
            $stockHash = Get-FileSha256 (Join-Path $control 'bin\rexruntime.dll')
            if ($stockHash -ne (Read-RuntimeBaseline).OFFICIAL_WIN_RUNTIME_SHA256) { throw 'the SDK download is not the stock runtime the baseline names' }
            [IO.File]::WriteAllText($stamp, ($good -replace '(?m)^[0-9a-f]{64}  bin/rexruntime\.dll$', "$stockHash  bin/rexruntime.dll"))
            if (-not (@(Test-PatchedRuntime $control) -match 'stock ReXGlue')) { throw 'the stock runtime was accepted' }
            Write-Host 'refused: stock ReXGlue'
            Remove-Item -LiteralPath $stamp
            if (-not (@(Test-PatchedRuntime $control) -match 'is missing')) { throw 'an unstamped runtime was accepted' }
            Write-Host 'refused: unstamped'
        } finally { Remove-Item -LiteralPath $control -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

if (-not $Filter -or 'compile' -match $Filter) {
    if ($Complete) {
        Test-Step 'Windows: platform-sensitive and changed production sources compile with no new warnings' @('Windows') {
            $sources = Get-PlatformSensitiveSources
            $dir = Join-Path $Root 'out\gate-windows'
            $Toolchain = Get-BuildToolchain
            $SavedEnv = Save-BuildEnvironment
            try {
                Import-BuildEnvironment $Toolchain
                & $Toolchain.CMake -S $Root -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release `
                    "-DCMAKE_CXX_COMPILER:FILEPATH=$($Toolchain.ClangXX)" "-DCMAKE_C_COMPILER:FILEPATH=$($Toolchain.Clang)" `
                    "-DCMAKE_LINKER:FILEPATH=$($Toolchain.LldLink)" "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($Toolchain.Ninja)" | Out-Null
                if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
                $known = @(& $Toolchain.Ninja -C $dir -t targets all | ForEach-Object { ($_ -split ':', 2)[0] } | Where-Object { $_ -match '^CMakeFiles/(el_chavo_kart|trg-updater)\.dir/.+\.obj$' })
                $objects = @($sources | ForEach-Object { $source = $_; $known | Where-Object { $_ -like "*/$source.obj" } })
                if (-not $objects.Count) { throw 'no object target matches the platform-sensitive sources' }
                foreach ($object in $objects) { Remove-Item -LiteralPath (Join-Path $dir $object) -ErrorAction SilentlyContinue }
                $output = @(& $Toolchain.Ninja -C $dir -k 0 @objects 2>&1 | ForEach-Object { "$_" })
                $code = $LASTEXITCODE
            } finally { Restore-BuildEnvironment $SavedEnv }
            if ($code) { $output | Select-Object -Last 30 | Out-Host; throw "the production sources do not compile on Windows (ninja exit $code)" }
            $report = Get-WarningReport $output 'windows'
            Write-Host "compiled $($sources.Count) sources: $($report.New.Count) new, $($report.Known) known, $($report.ThirdParty) third-party warnings"
            $Stats['Windows production compile'] = "$($sources.Count) sources, $($report.New.Count) new warnings, $($report.Known) known"
            if ($report.New.Count) { throw "new warnings:`n  $($report.New -join "`n  ")" }
        }
    }
}

function Get-ReleaseFacingFiles {
    Push-Location $Root
    try {
        return @(@(& git ls-files) + @(& git ls-files --others --exclude-standard) | Sort-Object -Unique |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    } finally { Pop-Location }
}

$TextExtensions = '\.(ps1|cpp|h|c|md|toml|txt|cmake|sh|js|mjs|json|yml|yaml|desktop|patch|env|in|rc|manifest)$'
# These name the markers and secret shapes they exist to refuse.
$ScannerFiles = @('docs/release.md', 'tools/check-release-folder.ps1', 'tools/linux-policy.sh', 'tools/test-release-check.ps1',
    'tools/test.sh', 'tools/test.ps1', 'tools/package-linux.sh', 'tools/build-appimage-runtime.sh')
$SecretValuePatterns = @('ghp_[A-Za-z0-9]{30,}', 'github_pat_[A-Za-z0-9_]{30,}', 'gho_[A-Za-z0-9]{30,}', '-----BEGIN [A-Z ]*PRIVATE KEY-----',
    'AKIA[0-9A-Z]{16}', 'xox[baprs]-[A-Za-z0-9-]{10,}', '"token" *: *"[0-9a-f]{64}"', 'player_id=[A-Z2-9]{4}-[A-Z2-9]{4}',
    '(ADMIN_TOKEN|CLOUDFLARE_API_TOKEN|CF_API_TOKEN)\s*[=:]\s*[A-Za-z0-9_./+-]{12,}')

if (-not $Filter -or 'hygiene' -match $Filter) {
    Test-Step 'Source tree hygiene' @('Hygiene') {
        Push-Location $Root
        try {
            & git diff --check | Out-Host
            if ($LASTEXITCODE) { throw 'git diff --check found conflict markers or whitespace errors' }
            $files = Get-ReleaseFacingFiles
            $deleted = @(& git ls-files --deleted)
            $problems = New-Object System.Collections.Generic.List[string]
            $gameDataExtensions = '\.(iso|xex|xexp|xxx|xx\d|upk|tfc|fsbcache|bik|bnk|fsb)$'
            foreach ($file in $files) {
                if ($file -match $gameDataExtensions -or $file -match '(^|/)game/' -or $file -match '(^|/)generated/') { $problems.Add("game data or generated output: $file") }
                if ($file -match '\.(tmp|bak|orig|rej|log|dmp|diff|swp|exe|dll|pdb|rdc)$|~$|(^|/)(user_data|logs)/|settings\.toml$') { $problems.Add("temporary, binary or player file: $file") }
            }
            foreach ($file in $files | Where-Object { $_ -match $TextExtensions -or $_ -match '(^|/)(AppRun|CMakeLists\.txt)$' }) {
                $text = [IO.File]::ReadAllText((Join-Path $Root $file))
                if ($text -match '(?m)^(<{7}|>{7})( |$)') { $problems.Add("conflict marker: $file") }
                if ($file -notin $ScannerFiles -and $text.Contains('DEBUG_SAULO')) { $problems.Add("DEBUG_SAULO: $file") }
                if ($file -match '^(src|tools|packaging|config|cmake)/' -and $file -notmatch 'third_party' -and $file -notin $ScannerFiles -and $text -cmatch '\b(TODO|FIXME|HACK)\b') { $problems.Add("TODO, FIXME or HACK in a release path: $file") }
                if ($text -match 'S:\\VisualStudio|C:\\Users\\[A-Za-z0-9_.-]+|/mnt/[a-z]/Users') { $problems.Add("hard-coded local machine path: $file") }
                if ($file -notin $ScannerFiles -and $file -notmatch '^(tests|server/eck-lobby/test)/') {
                    foreach ($pattern in $SecretValuePatterns) { if ($text -match $pattern) { $problems.Add("secret or token shape: $file"); break } }
                }
            }
            if ($problems.Count) { throw ($problems | Select-Object -First 20) -join '; ' }
            if ($deleted.Count) { Write-Host "$($deleted.Count) tracked file(s) deleted in the working tree: $($deleted -join ', ')" }
            Write-Host "$($files.Count) files: no conflict markers, game data, temporary or player files, DEBUG_SAULO, TODO, secret shapes or local paths."
        } finally { Pop-Location }
    }
}

if (-not $Filter -or 'privacy' -match $Filter) {
    Test-Step 'Privacy: the Windows release package and its checker refuse secrets, tokens and identities' @('Privacy') {
        $zip = @(Get-ChildItem -LiteralPath (Join-Path $Root 'dist') -Filter 'ElChavoKart-v*-windows-x64.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1)
        if (-not $zip.Count) { Skip-Step 'no Windows release ZIP in dist to check (tools\package-release.ps1 builds it)'; return }
        & (Join-Path $PSScriptRoot 'package-release.ps1') -VerifyOnly $zip[0].FullName | Out-Host
        if ($LASTEXITCODE) { throw "the release ZIP $($zip[0].Name) fails the release check" }
    }
}

$AreaEvidence = @{
    'Linux Worker' = 'Worker'
    'Linux settings persistence' = 'Settings'
    'Windows/Linux wire compatibility' = 'Cross-platform'
    'Linux wire vectors' = 'Cross-platform'
    'Linux scratch AppImage' = 'AppImage'
    'Linux AppImage relaunch' = 'AppImage'
    'Linux self-updater' = 'Updater'
    'Linux game installation' = 'Installer'
    'Linux ReXGlue' = 'ReXGlue'
    'Linux GLIBC <= 2.31' = 'glibc / ELF'
    'Linux RPATH/RUNPATH' = 'glibc / ELF'
    'Linux dynamic dependencies' = 'glibc / ELF'
    'Linux packaging policy' = 'Packaging'
    'Linux privacy' = 'Privacy'
}

if (-not $SkipLinux -and (-not $Filter -or 'linux' -match $Filter)) {
    function Get-WslDistros {
        $env:WSL_UTF8 = '1'
        $names = @(& wsl.exe --list --quiet 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ })
        return @($names | Where-Object { $_ -notmatch '^docker-desktop' } | Sort-Object { $_.ToLowerInvariant() })
    }

    function ConvertTo-WslPath([string]$Distro, [string]$Path) {
        $converted = & wsl.exe -d $Distro --exec wslpath -a $Path
        if ($LASTEXITCODE -or -not $converted) { throw "wslpath could not convert $Path in $Distro" }
        return "$converted".Trim()
    }

    # The first installed distro (ECK_WSL_DISTRO, or all of them in name order) that tools/test.sh accepts: Ubuntu 20.04,
    # glibc 2.31 and the build tools.
    function Find-LinuxDistro {
        if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
            return @{ Error = 'wsl.exe was not found; install WSL (wsl --install) and an Ubuntu 20.04 distro with tools/setup-linux-build-env.sh' }
        }
        $installed = Get-WslDistros
        if (-not $installed.Count) {
            return @{ Error = 'WSL has no distribution; install Ubuntu 20.04 (wsl --install -d Ubuntu-20.04) and run tools/setup-linux-build-env.sh in it' }
        }
        $candidates = if ($env:ECK_WSL_DISTRO) { @($env:ECK_WSL_DISTRO) } else { $installed }
        $rejected = [Collections.Generic.List[string]]::new()
        foreach ($name in $candidates) {
            if ($installed -notcontains $name) { $rejected.Add("${name}: not installed"); continue }
            try { $wslRoot = ConvertTo-WslPath $name $Root } catch { $rejected.Add("${name}: $_"); continue }
            $output = @(& wsl.exe -d $name --exec bash "$wslRoot/tools/test.sh" --check-environment 2>&1 | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
            if ($LASTEXITCODE -eq 0) { return @{ Name = $name; Root = $wslRoot } }
            $rejected.Add("${name}: $($output -join ' ')")
        }
        return @{ Error = "no WSL distribution can run the Linux gate (set ECK_WSL_DISTRO to choose one). Checked: $($rejected -join ' | ')" }
    }

    Test-Step 'Linux parity gate in WSL (tools/test.sh)' @('Linux') {
        $found = Find-LinuxDistro
        if ($found.Error) {
            Skip-Step "Linux validation environment is unavailable: $($found.Error)"
            return
        }
        $wire = Join-Path $BuildDir 'eck_wire_parity_tests.exe'
        $integration = Join-Path $BuildDir 'eck_online_integration_tests.exe'
        foreach ($program in $wire, $integration) {
            if (-not (Test-Path -LiteralPath $program -PathType Leaf)) { throw "$(Split-Path -Leaf $program) is not built (run the console tests first)" }
        }
        $stamp = [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $temp = [IO.Path]::GetTempPath()
        $windowsVectors = Join-Path $temp "eck-wire-windows-$stamp.txt"
        $linuxVectors = Join-Path $temp "eck-wire-linux-$stamp.txt"
        $changed = Join-Path $temp "eck-changed-$stamp.txt"
        try {
            [IO.File]::WriteAllText($windowsVectors, ((@(& $wire --print) -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
            [IO.File]::WriteAllText($changed, ((Get-PlatformSensitiveSources) -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
            Write-Host "WSL distro: $($found.Name)"
            $arguments = @("$($found.Root)/tools/test.sh", '--emit-vectors', (ConvertTo-WslPath $found.Name $linuxVectors),
                '--peer-vectors', (ConvertTo-WslPath $found.Name $windowsVectors),
                '--windows-integration', (ConvertTo-WslPath $found.Name $integration),
                '--changed-files', (ConvertTo-WslPath $found.Name $changed))
            if ($env:USERNAME -and $env:USERNAME.Length -ge 4) { $arguments += @('--private-name', $env:USERNAME) }
            if ($Complete) { $arguments += '--complete' }

            $results = New-Object System.Collections.Generic.List[object]
            $savedEncoding = [Console]::OutputEncoding
            [Console]::OutputEncoding = [Text.Encoding]::UTF8
            try {
                & wsl.exe -d $found.Name --exec bash @arguments 2>&1 | ForEach-Object {
                    $line = "$_"
                    if ($line -match '^@@RESULT (PASS|FAIL)\|(.*)\|([^|]*)$') {
                        $results.Add([pscustomobject]@{ Status = $Matches[1]; What = $Matches[2]; Areas = @($Matches[3] -split ';' | Where-Object { $_ }) })
                    } elseif ($line -match '^@@STAT ([^=]+)=(.*)$') {
                        $Stats["Linux $($Matches[1])"] = $Matches[2]
                    } elseif ($line -notmatch '^@@') {
                        Write-Host $line
                    }
                }
                $gate = $LASTEXITCODE
            } finally { [Console]::OutputEncoding = $savedEncoding }

            foreach ($result in $results) {
                foreach ($area in $result.Areas) {
                    if ($AreaEvidence.ContainsKey($area)) { Add-Evidence @($AreaEvidence[$area]) ($result.Status -eq 'PASS') }
                }
            }
            if ($gate -or -not $results.Count -or @($results | Where-Object Status -eq 'FAIL').Count) {
                throw "tools/test.sh failed in WSL '$($found.Name)' (exit $gate, $($results.Count) results, $(@($results | Where-Object Status -eq 'FAIL').Count) failed)"
            }

            if (-not (Test-Path -LiteralPath $linuxVectors -PathType Leaf)) { throw 'the Linux build produced no wire vectors' }
            & $wire --peer $linuxVectors | Out-Host
            if ($LASTEXITCODE) { Add-Evidence @('Cross-platform') $false; throw 'Windows does not read and reproduce the vectors the Linux build produced' }
            $windows = @(& $wire --print) -join "`n"
            if ($windows -cne ((Get-Content -LiteralPath $linuxVectors) -join "`n")) { Add-Evidence @('Cross-platform') $false; throw 'the wire vectors differ between Windows and Linux' }
            Write-Host 'Windows and Linux produce byte-identical wire vectors.'
            $env:ECK_WIRE_VECTORS = $linuxVectors
            Push-Location (Join-Path $Root 'server\eck-lobby')
            try { & (Get-NodeProgram) --test 'test/wire.test.js' | Out-Host; $workerRead = $LASTEXITCODE }
            finally { Pop-Location; Remove-Item Env:ECK_WIRE_VECTORS -ErrorAction SilentlyContinue }
            if ($workerRead) { Add-Evidence @('Cross-platform') $false; throw 'the Worker does not read the vectors the Linux build produced' }
            Add-Evidence @('Cross-platform') $true
        } finally {
            foreach ($file in $windowsVectors, $linuxVectors, $changed) { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
        }
    }
} elseif ($SkipLinux) {
    Write-Host '== Linux gate: NOT RUN, -SkipLinux was given; this run is partial' -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Summary' -ForegroundColor Cyan
foreach ($name in $Groups.Keys) {
    $group = $Groups[$name]
    $status = if ($group.Fail) { 'FAIL' } elseif ($group.Pass) { 'PASS' } else { 'NOT RUN' }
    $color = switch ($status) { 'PASS' { 'Green' } 'FAIL' { 'Red' } default { 'Yellow' } }
    Write-Host ('  {0,-16} {1,-8} {2} check(s)' -f $name, $status, ($group.Pass + $group.Fail)) -ForegroundColor $color
}
foreach ($key in $Stats.Keys) { Write-Host ('  {0}: {1}' -f $key, $Stats[$key]) }
Write-Host ''

$notRun = @($Groups.Keys | Where-Object { -not $Groups[$_].Pass -and -not $Groups[$_].Fail })
if ($Failures.Count) {
    Write-Host "$($Failures.Count) check(s) failed:" -ForegroundColor Red
    foreach ($failure in $Failures) { Write-Host "  $failure" -ForegroundColor Red }
    exit 1
}
if ($Complete) {
    if ($notRun.Count) {
        Write-Host "INCOMPLETE: nothing ran for $($notRun -join ', ')" -ForegroundColor Red
        exit 1
    }
    Write-Host 'ALL REQUIRED CHECKS PASSED' -ForegroundColor Green
    Write-Host 'ALL AUTOMATABLE WINDOWS AND LINUX RELEASE CHECKS PASSED' -ForegroundColor Green
    Write-Host 'This does not replace trying the game: Vulkan rendering, controllers, audio, real network and a Steam Deck are only proven by hand.'
    exit 0
}
Write-Host 'PARTIAL RUN: the checks that ran passed, but this is not the release gate. Run .\tools\test.ps1 -RequireLinux.' -ForegroundColor Yellow
if ($notRun.Count) { Write-Host "Not run: $($notRun -join ', ')" -ForegroundColor Yellow }
exit 0
