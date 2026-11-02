# Checks and installs the game files in game\extracted. Dot-sourced by build.ps1 and tools\extract-iso.ps1; needs
# tools\xiso.ps1. Functions throw with a message for the player on failure.

$GameXexSize = 30003200
$GameXexSha256 = '1DB4E96B413D5629206FFE63E73C0E1CFACA9EEA24BCF6FCED6DB9B135CE120F'
# The Redump-style image this port was made from; other dumps of the same disc are accepted by their default.xex.
$KnownIsoSize = 7838695424
$KnownIsoSha256 = '3B4C1BA6CBA41A20D94A89727018526B27FBB31FCD5AD32317D7C244C7C4B9C4'
# Same list as kRequiredFiles in src\game_files.cpp.
$GameRequiredFiles = [ordered]@{
    'ChavoKartGame\Xbox360TOC.txt'                              = 25370
    'ChavoKartGame\Xbox360TOC_PTB.txt'                          = 462
    'ChavoKartGame\CookedXbox360\Core.xxx'                      = 32768
    'ChavoKartGame\CookedXbox360\Engine.xxx'                    = 1146880
    'ChavoKartGame\CookedXbox360\Startup.xxx'                   = 98304
    'ChavoKartGame\CookedXbox360\ChavoKartGame.xxx'             = 3899392
    'ChavoKartGame\CookedXbox360\FMODAudio.xxx'                 = 32768
    'ChavoKartGame\CookedXbox360\GlobalShaderCache-Xbox360.bin' = 939069
    'ChavoKartGame\CookedXbox360\Coalesced_INT.bin'             = 367604
    'ChavoKartGame\CookedXbox360\Coalesced_PTB.bin'             = 371206
    'ChavoKartGame\CookedXbox360\Coalesced_ESM.bin'             = 371408
    'ChavoKartGame\CookedXbox360\Coalesced_ESN.bin'             = 371762
    'ChavoKartGame\CookedXbox360\TitleMap.xxx'                  = 17760256
    'ChavoKartGame\CookedXbox360\StartMenu.xxx'                 = 15925248
    'ChavoKartGame\CookedXbox360\Textures.tfc'                  = 33239975
    'ChavoKartGame\CookedXbox360\Lighting.tfc'                  = 108650780
    'ChavoKartGame\Movies\INTRO.BIK'                            = 53437548
    'ChavoKartGame\Movies\DISCLAIMER01_PTB.BIK'                 = 8551000
}
$GameMarkerName = '.eck-game-files'

# Why the folder is not a complete, supported copy of the game; empty when it is.
function Get-GameDataProblem([string]$Folder) {
    $xex = Join-Path $Folder 'default.xex'
    if (-not (Test-Path -LiteralPath $xex -PathType Leaf)) { return 'default.xex is missing' }
    if ((Get-Item -LiteralPath $xex).Length -ne $GameXexSize) { return 'default.xex is not the supported version (size)' }
    if ((Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash -ne $GameXexSha256) { return 'default.xex is not the supported version (SHA-256)' }
    foreach ($file in $GameRequiredFiles.Keys) {
        $path = Join-Path $Folder $file
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return "$file is missing" }
        if ((Get-Item -LiteralPath $path).Length -ne $GameRequiredFiles[$file]) { return "$file has the wrong size" }
    }
    return ''
}

# Why game\extracted needs installing; empty when a verified install is there.
function Get-InstalledGameProblem([string]$GameData) {
    if (-not (Test-Path -LiteralPath (Join-Path $GameData $GameMarkerName) -PathType Leaf)) { return 'not extracted yet' }
    return Get-GameDataProblem $GameData
}

function Find-DiscImage([string]$GameFolder, [string]$Iso) {
    if ($Iso) {
        if (-not (Test-Path -LiteralPath $Iso -PathType Leaf)) { throw "Disc image not found: $Iso" }
        return (Resolve-Path -LiteralPath $Iso).Path
    }
    $images = @(Get-ChildItem -LiteralPath $GameFolder -Filter '*.iso' -File -ErrorAction SilentlyContinue)
    if ($images.Count -eq 1) { return $images[0].FullName }
    if ($images.Count -gt 1) { throw "More than one .iso in game\: $(($images | ForEach-Object Name) -join ', '). Pick one with -Iso." }
    throw ("No game files in game\extracted and no disc image in game\. Put your own El Chavo Kart (USA) Xbox 360 disc " +
        "image (.iso) in the game folder, or pass -Iso <path>.")
}

# Extracts the image into <GameData>.staging, checks it, then swaps it in for GameData.
function Install-GameData([string]$Image, [string]$GameData) {
    $staging = "$GameData.staging"
    $previous = "$GameData.previous"
    Write-Host "Disc image: $Image"
    $stream = [IO.File]::OpenRead($Image)
    try {
        try { $entries = Get-XisoEntries $stream } catch { throw "$([IO.Path]::GetFileName($Image)) is not a readable Xbox 360 disc image: $($_.Exception.Message)" }
        $xex = Find-XisoEntry $entries 'default.xex'
        if (-not $xex -or -not (Find-XisoEntry $entries 'ChavoKartGame')) { throw "$([IO.Path]::GetFileName($Image)) is not an El Chavo Kart disc image" }
        $hash = Get-XisoFileSha256 $stream $xex
        if ($xex.Size -ne $GameXexSize -or $hash -ne $GameXexSha256) {
            throw ("Unsupported disc image: its default.xex has SHA-256 $hash. This port supports El Chavo Kart (USA), " +
                "game version 0.0.0.3, default.xex SHA-256 $GameXexSha256.")
        }
        Write-Host 'default.xex: supported (El Chavo Kart (USA), title 475807D7, version 0.0.0.3)'
        if ($stream.Length -eq $KnownIsoSize) { Write-Host 'Image size matches the reference dump.' }

        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
        Write-Host 'Extracting the game files (about 2 GB)...'
        $result = Export-XisoFiles $stream $entries $staging @('$SystemUpdate')
    } finally { $stream.Dispose() }

    $problem = Get-GameDataProblem $staging
    if ($problem) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        throw "The extracted game files are not usable: $problem"
    }
    [IO.File]::WriteAllText((Join-Path $staging $GameMarkerName),
        "default.xex sha256=$GameXexSha256`nfiles=$($result.Files)`nbytes=$($result.Bytes)`n")
    if (Test-Path -LiteralPath $previous) { Remove-Item -LiteralPath $previous -Recurse -Force }
    if (Test-Path -LiteralPath $GameData) { Move-Item -LiteralPath $GameData -Destination $previous }
    Move-Item -LiteralPath $staging -Destination $GameData
    if (Test-Path -LiteralPath $previous) { Remove-Item -LiteralPath $previous -Recurse -Force }
    Write-Host "Game files ready: $($result.Files) files, $([Math]::Round($result.Bytes / 1GB, 2)) GB"
}
