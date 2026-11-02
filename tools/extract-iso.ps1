<#
.SYNOPSIS
  Extracts your El Chavo Kart (USA) disc image into game\extracted, the step build.ps1 runs when the game files are
  missing. The image is only read; the files go to a staging folder, are checked, and only then replace
  game\extracted.

.EXAMPLE
  .\tools\extract-iso.ps1                      # the one .iso in game\
  .\tools\extract-iso.ps1 -Iso D:\chavo.iso
  .\tools\extract-iso.ps1 -Force               # extract again over a verified install
  .\tools\extract-iso.ps1 -VerifyImage         # also compare the whole image with the reference dump (slow)
#>
param(
    [string]$Iso,
    [switch]$Force,
    [switch]$VerifyImage
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'xiso.ps1')
. (Join-Path $PSScriptRoot 'game-data.ps1')
$GameData = Join-Path $Root 'game\extracted'

try {
    $problem = Get-InstalledGameProblem $GameData
    if (-not $problem -and -not $Force) {
        Write-Host 'game\extracted already holds the verified game files (use -Force to extract again).'
        exit 0
    }
    $image = Find-DiscImage (Join-Path $Root 'game') $Iso
    if ($VerifyImage) {
        Write-Host 'Hashing the whole image...'
        $hash = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        Write-Host ("Image SHA-256 {0}: {1}" -f $hash, $(if ($hash -eq $KnownIsoSha256) { 'matches the reference dump' } else { 'another dump; accepted by its default.xex' }))
    }
    Install-GameData $image $GameData
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 2
}
