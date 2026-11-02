<#
.SYNOPSIS
  Runs the project's lightweight, headless checks: no game files, no ReXGlue SDK, no GUI.
    - builds and runs the pure-logic tests in tests\ (installer/extractor and PT-BR translation table)
    - validates the ReXGlue SDK patch set (unique numbering, deterministic order)
    - checks for overlapping addresses between config\overrides.toml and config\discovered-functions.toml
    - reports generated\default codegen freshness, if it has been generated at all
    - source tree hygiene: git diff --check, DEBUG_SAULO markers, hard-coded local paths, tracked game data

.EXAMPLE
  .\tools\test.ps1                      # run every check
  .\tools\test.ps1 -Filter installer    # only the console test whose name matches
#>
param(
    [string]$Filter = '',
    [string]$BuildDir  # relative to the repository root; default: out\tests
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'build-common.ps1')
. (Join-Path $PSScriptRoot 'rexruntime-patch.ps1')
. (Join-Path $PSScriptRoot 'codegen-inputs.ps1')
$BuildDir = if (-not $BuildDir) { Join-Path $Root 'out\tests' } elseif ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $Root $BuildDir }

$Failures = New-Object System.Collections.Generic.List[string]
function Test-Step([string]$Name, [scriptblock]$Body) {
    Write-Host "== $Name" -ForegroundColor Cyan
    try { & $Body }
    catch { $Failures.Add("${Name}: $($_.Exception.Message)"); Write-Host "  FAILED: $($_.Exception.Message)" -ForegroundColor Red }
}

if (-not $Filter -or 'eck_installer_tests eck_ui_text_tests' -match $Filter) {
    Test-Step 'Console tests (tests\CMakeLists.txt)' {
        $Toolchain = Get-BuildToolchain
        $SavedEnv = Save-BuildEnvironment
        try {
            Import-BuildEnvironment $Toolchain
            & $Toolchain.CMake -S (Join-Path $Root 'tests') -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
                "-DCMAKE_CXX_COMPILER:FILEPATH=$($Toolchain.ClangXX)" "-DCMAKE_LINKER:FILEPATH=$($Toolchain.LldLink)" `
                "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($Toolchain.Ninja)" | Out-Host
            if ($LASTEXITCODE) { throw "CMake configure failed ($LASTEXITCODE)" }
            & $Toolchain.CMake --build $BuildDir | Out-Host
            if ($LASTEXITCODE) { throw "Test build failed ($LASTEXITCODE)" }
        } finally { Restore-BuildEnvironment $SavedEnv }
        foreach ($name in @('eck_installer_tests', 'eck_ui_text_tests')) {
            if ($Filter -and $name -notmatch $Filter) { continue }
            $exe = Join-Path $BuildDir "$name.exe"
            Write-Host "Running $name..."
            & $exe | Out-Host
            if ($LASTEXITCODE) { throw "$name failed ($LASTEXITCODE)" }
        }
    }
}

if (-not $Filter -or 'rexglue-patches' -match $Filter) {
    Test-Step 'ReXGlue SDK patch set' {
        $patches = @(Get-RuntimePatchFiles)
        Write-Host "$($patches.Count) patches, applied in this order:"
        $patches | ForEach-Object { Write-Host "  $($_.Name)" }
    }
}

if (-not $Filter -or 'trg-patches' -match $Filter) {
    Test-Step 'TRG Launcher patch set' {
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

if (-not $Filter -or 'function-overrides' -match $Filter) {
    Test-Step 'config\overrides.toml vs config\discovered-functions.toml' {
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
    Test-Step 'Discovered functions are generated and calibrated' {
        $discovered = Get-Content -LiteralPath (Join-Path $Root 'config\discovered-functions.toml')
        foreach ($address in '0x830F6C58', '0x830F6D7C', '0x830F6A60', '0x824B7168') {
            if (-not ($discovered | Select-String -SimpleMatch "$address =" -Quiet)) { throw "$address is missing from config\discovered-functions.toml" }
        }
        $register = Join-Path $Root 'generated\default\el_chavo_kart_register.cpp'
        $image = Join-Path $Root 'game\analysis\default.image.bin'
        $python = Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not (Test-Path -LiteralPath $register) -or -not (Test-Path -LiteralPath $image) -or -not $python) {
            Write-Host 'Calibration entries present; verification skipped (needs generated\default, game\analysis\default.image.bin and Python).'
            return
        }
        & $python.Source -I (Join-Path $PSScriptRoot 'find-missing-functions.py') --verify | Out-Host
        if ($LASTEXITCODE) { throw 'discovered functions are not all registered exactly once' }
    }
}

if ($Filter -match 'callbacks') {
    Test-Step 'Guest callback targets are registered' {
        $python = Get-Command python, py -ErrorAction Stop | Select-Object -First 1
        & $python.Source -I (Join-Path $PSScriptRoot 'find-missing-functions.py') --callbacks | Out-Host
        if ($LASTEXITCODE) { throw 'unregistered guest callback target' }
    }
}

if (-not $Filter -or 'codegen' -match $Filter) {
    Test-Step 'tools\codegen.ps1 exists and parses' {
        $path = Join-Path $PSScriptRoot 'codegen.ps1'
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'tools\codegen.ps1 is missing' }
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors) | Out-Null
        if ($errors.Count) { throw "$($errors.Count) parse error(s) in tools\codegen.ps1" }
        Write-Host 'tools\codegen.ps1 exists and parses.'
    }
}

if (-not $Filter -or 'codegen-freshness' -match $Filter) {
    Test-Step 'generated\default freshness' {
        $sdkVersion = Get-InstalledRexglueVersion $Root
        if (-not $sdkVersion) { Write-Host 'tools\rexglue is not installed; skipped (run tools\codegen.ps1 to generate).'; return }
        if (-not (Test-Path -LiteralPath (Join-Path $Root "$CodegenOutput\sources.cmake") -PathType Leaf)) {
            throw "$CodegenOutput\sources.cmake is missing; run tools\codegen.ps1"
        }
        $freshness = Get-CodegenFreshness $Root $sdkVersion
        if ($freshness.Current) { Write-Host 'generated\default is current and matches its input stamp.' }
        else { throw "generated\default is out of date: $($freshness.Reasons -join '; ') (run tools\codegen.ps1)" }
    }
}

if (-not $Filter -or 'build-graph' -match $Filter) {
    Test-Step 'CMake/Ninja build graph does not run ReXGlue codegen' {
        $sdkReady = Test-Path -LiteralPath (Join-Path $Root 'tools\rexglue\win-amd64\bin\rexglue.exe')
        $guestReady = Test-Path -LiteralPath (Join-Path $Root 'generated\default\sources.cmake')
        $trgReady = (Test-Path -LiteralPath (Join-Path $Root 'third_party\trg-launcher\CMakeLists.txt')) -or $env:ECK_TRG_LAUNCHER_DIR
        if (-not ($sdkReady -and $guestReady -and $trgReady)) {
            Write-Host 'Skipped: tools\rexglue, generated\default or third_party\trg-launcher is not set up yet.'
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
    Test-Step 'ReXGlue runtime patch freshness' {
        $sdkDir = Join-Path $Root 'tools\rexglue\win-amd64'
        if (-not (Test-Path -LiteralPath (Join-Path $sdkDir 'bin\rexruntime.dll'))) {
            Write-Host 'tools\rexglue is not installed; skipped (run tools\codegen.ps1).'
            return
        }
        $problems = @(Test-PatchedRuntime $sdkDir)
        if (-not $problems.Count) { Write-Host 'Installed runtime and GPU plugin match patches\rexglue-sdk.'; return }
        if (-not (Test-Path -LiteralPath (Join-Path $sdkDir 'rexruntime-patch.txt'))) {
            Write-Host 'Patched runtime not built yet; skipped (tools\build.ps1 builds it).'
            return
        }
        throw "installed runtime does not match patches\rexglue-sdk: $($problems -join '; ')"
    }
}

if (-not $Filter -or 'hygiene' -match $Filter) {
    Test-Step 'Source tree hygiene' {
        Push-Location $Root
        try {
            & git diff --check | Out-Host
            if ($LASTEXITCODE) { throw 'git diff --check found conflict markers or whitespace errors' }
            # Tracked plus untracked-but-not-ignored: what a commit would add, without requiring one.
            $tracked = @(git ls-files) + @(git ls-files --others --exclude-standard) | Sort-Object -Unique
            $gameDataExtensions = '\.(iso|xex|xexp|xxx|xx\d|upk|tfc|fsbcache|bik|bnk|fsb)$'
            $badTracked = @($tracked | Where-Object { $_ -match $gameDataExtensions -or $_ -match '(^|/)game/' -or $_ -match '(^|/)generated/' })
            if ($badTracked.Count) { throw "game data or generated output is tracked by git: $($badTracked -join ', ')" }
            $debugMarkers = @($tracked | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-String -Pattern 'DEBUG_SAULO' -List -ErrorAction SilentlyContinue)
            if ($debugMarkers.Count) { throw "DEBUG_SAULO markers left in: $(($debugMarkers | ForEach-Object Path) -join ', ')" }
            $localPaths = @($tracked | Where-Object { $_ -match '\.(ps1|cpp|h|md|toml|txt|cmake)$' } | ForEach-Object { Get-Item -LiteralPath $_ } |
                Select-String -Pattern 'S:\\VisualStudio|C:\\Users\\[A-Za-z0-9_.-]+' -List -ErrorAction SilentlyContinue)
            if ($localPaths.Count) { throw "hard-coded local machine paths in: $(($localPaths | ForEach-Object Path) -join ', ')" }
            Write-Host "$($tracked.Count) tracked files: no conflict markers, no tracked game data, no DEBUG_SAULO markers, no local paths."
        } finally { Pop-Location }
    }
}

Write-Host ''
if ($Failures.Count) {
    Write-Host "$($Failures.Count) check(s) failed:" -ForegroundColor Red
    foreach ($failure in $Failures) { Write-Host "  $failure" -ForegroundColor Red }
    exit 1
}
Write-Host 'All checks passed.' -ForegroundColor Green
exit 0
