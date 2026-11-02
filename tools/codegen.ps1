<#
.SYNOPSIS
  Downloads the pinned ReXGlue SDK, checks game\extracted\default.xex and generates the recompiled C++ sources
  into generated\default. This is the only entry point that regenerates them; tools\build.ps1 only checks that
  they are current and stops with this script's name when they are not.

.EXAMPLE
  .\tools\codegen.ps1             # generate, or do nothing if generated\default is already current
  .\tools\codegen.ps1 -Force      # regenerate even if current
  .\tools\codegen.ps1 -Check      # report only; exit 1 if generated\default is out of date
  .\tools\codegen.ps1 -Iso D:\chavo.iso   # extract game files from this image first, if game\extracted is missing
#>
param(
    [switch]$Force,
    [switch]$Check,
    [string]$Iso
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'build-common.ps1')
. (Join-Path $PSScriptRoot 'codegen-inputs.ps1')
. (Join-Path $PSScriptRoot 'xiso.ps1')
. (Join-Path $PSScriptRoot 'game-data.ps1')

$SdkDir = Join-Path $Root 'tools\rexglue'
$Rexglue = Join-Path $SdkDir 'win-amd64\bin\rexglue.exe'
$GameData = Join-Path $Root 'game\extracted'

if ($Check) {
    $installedSdk = if (Test-Path -LiteralPath $Rexglue) { "$(& $Rexglue --version)".Trim() } else { '' }
    $fresh = ($installedSdk -eq $EckSdkVersion) -and (Get-CodegenFreshness $Root $EckSdkVersion).Current
    Write-Host "generated\default: $(if ($fresh) { 'current' } else { 'out of date' })"
    exit $(if ($fresh) { 0 } else { 1 })
}

function Initialize-Sdk {
    if (Test-Path -LiteralPath $Rexglue) { return }
    $zip = Join-Path $SdkDir "rexglue-sdk-$EckSdkVersion-win-amd64.zip"
    New-Item -ItemType Directory -Force $SdkDir | Out-Null
    if (-not (Test-Path -LiteralPath $zip)) {
        $url = "https://github.com/rexglue/rexglue-sdk/releases/download/v$EckSdkVersion/rexglue-sdk-$EckSdkVersion-win-amd64.zip"
        Write-Host "Downloading ReXGlue SDK $EckSdkVersion (network)..."
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    if ((Get-FileSha256 $zip) -ne $EckSdkZipSha256) {
        throw "$zip is not the official ReXGlue $EckSdkVersion release (SHA-256 mismatch). Delete it and run again."
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $SdkDir -Force
}

try { Initialize-Sdk } catch { throw "ReXGlue SDK: $($_.Exception.Message)" }
$installedSdk = "$(& $Rexglue --version)".Trim()
if ($installedSdk -ne $EckSdkVersion) {
    throw "tools\rexglue has ReXGlue $installedSdk; expected $EckSdkVersion. Delete tools\rexglue and run again."
}
Write-Host "ReXGlue SDK $installedSdk"

$problem = Get-InstalledGameProblem $GameData
if ($problem) {
    Write-Host "game\extracted: $problem"
    Install-GameData (Find-DiscImage (Join-Path $Root 'game') $Iso) $GameData
} else {
    Write-Host 'game\extracted: El Chavo Kart (USA) 0.0.0.3, verified'
}

if (-not $Force) {
    $freshness = Get-CodegenFreshness $Root $installedSdk
    if ($freshness.Current) {
        Write-Host 'generated\default is current; nothing to do (-Force to regenerate anyway).'
        exit 0
    }
    Write-Host "generated\default is out of date: $($freshness.Reasons -join '; ')"
}

New-Item -ItemType Directory -Force (Join-Path $Root 'generated') | Out-Null
$log = Join-Path $Root $CodegenLog
Push-Location $Root
try {
    # rexglue logs to stderr; in Windows PowerShell 5.1 that would trip 'Stop', so judge by the exit code.
    $ErrorActionPreference = 'Continue'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $extra = if ($Force) { @('--ignore-stamp') } else { @() }
    & $Rexglue codegen @extra $CodegenManifest 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log
    $exitCode = $LASTEXITCODE
    Write-Host ("codegen exit code {0} after {1:N1} s (log: generated\codegen.log)" -f $exitCode, $timer.Elapsed.TotalSeconds)
    if ($exitCode -ne 0) { throw "codegen failed ($exitCode)" }
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }

if (-not (Test-Path -LiteralPath (Join-Path $Root "$CodegenOutput\sources.cmake"))) {
    throw 'codegen did not write generated\default\sources.cmake'
}
[IO.File]::WriteAllText((Join-Path $Root $CodegenStamp), (Get-CodegenInputStamp $Root $installedSdk), (New-Object Text.UTF8Encoding $false))
Write-Host "generated\default: current (ReXGlue $installedSdk)"
