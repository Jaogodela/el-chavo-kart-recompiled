<#
.SYNOPSIS
  Starts one isolated test instance of El Chavo Kart for online testing on a single PC: its own user folder
  (settings, online identity, friend code, saves, caches), its own log file and its own gameplay UDP port.
.DESCRIPTION
  Instance "a" uses build\online-instances\a\user_data and build\online-instances\a\logs, instance "b" the same with
  "b", and so on, so run it once per player: two instances for a 2-player room, three or four for a bigger one. The
  first start copies the display and graphics settings of the main user folder (-CopyFrom) and gives the instance its
  own online name (Test-a, Test-b, or -OnlineName) with Online play Off; later starts keep the instance's own files and
  only restore a missing online name. The online identity is never copied, so every instance makes its own friend code
  on first use. Every start writes a new log named after the instance (eck_a_001.log, _002, ...) inside that
  instance's logs folder.
  Each instance gets its own gameplay UDP port through ECK_ONLINE_PORT: -Port, or 7101 + the instance letter (a is 7101,
  b is 7102, ... j is 7110) when the name is a single letter a-j or a digit. Any other name needs -Port. Every instance
  uses the online service the game defaults to; -Lobby, or ECK_LOBBY from the environment, overrides it for development.
  ECK_INSTANCE only labels the instance's logs. Local input follows window focus in every copy of the game, so only the
  focused instance reacts to the keyboard, the mouse and the controller; the others keep running online.
  Runs the launcher; Online play is switched on from the instance's own Online page. -PrepareOnly sets the folders up
  and prints the same summary without starting the game. Nothing starts unless this script is run.
.EXAMPLE
  tools\online-instance.ps1 -Name a           # terminal 1
  tools\online-instance.ps1 -Name b           # terminal 2
  tools\online-instance.ps1 -Name c           # a third player
.EXAMPLE
  tools\online-instance.ps1 -Name a -PrepareOnly
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9-]{1,16}$')][string]$Name,
    [string]$OnlineName = '',
    [string]$CopyFrom = '',
    [string]$Executable = '',
    [string]$GameData = '',
    [string]$InstanceRoot = '',
    [string]$Lobby = '',
    [ValidateRange(0, 65535)][int]$Port = 0,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$BuildDir = Join-Path $Root 'build'
if (-not $Executable) { $Executable = Join-Path $BuildDir 'el_chavo_kart.exe' }
if (-not $GameData) { $GameData = Join-Path $Root 'game\extracted' }
if (-not $CopyFrom) { $CopyFrom = Join-Path $BuildDir 'user_data' }
if (-not $InstanceRoot) { $InstanceRoot = Join-Path $BuildDir 'online-instances' }
$label = $Name.ToLowerInvariant()
if (-not $OnlineName) { $OnlineName = "Test-$label" }
$OnlineName = ($OnlineName -replace '[^A-Za-z0-9_-]', '')
if ($OnlineName.Length -gt 16) { $OnlineName = $OnlineName.Substring(0, 16) }
if (-not $OnlineName) { $OnlineName = 'Player' }

if (-not $Port) {
    if ($label -match '^[a-j]$') { $Port = 7101 + ([int][char]$label - [int][char]'a') }
    elseif ($label -match '^[0-9]$') { $Port = 7101 + [int]$label }
    else { throw "Instance '$label' has no default UDP port: pass -Port (7101 to 7110 is the game's range)." }
}
if (-not $Lobby) { $Lobby = $env:ECK_LOBBY }

$user = Join-Path (Join-Path $InstanceRoot $label) 'user_data'
$logs = Join-Path (Join-Path $InstanceRoot $label) 'logs'
$settingsFile = Join-Path $user 'settings.toml'

# Window placement, the online choices and the logging choice are the instance's own; everything else is copied once.
$OwnKeys = '^\s*eck_(launcher_(x|y|width|height|maximized|fullscreen)|online_(enabled|name|connection_test))\s*=|^\s*detailed_logging\s*='
$NameKey = 'eck_online_name'

function Next-LogFile([string]$Folder, [string]$Label) {
    $prefix = "eck_${Label}_"
    $last = 0
    foreach ($file in Get-ChildItem -LiteralPath $Folder -Filter "$prefix*.log" -ErrorAction SilentlyContinue) {
        if ($file.BaseName.Substring($prefix.Length) -match '^\d{1,9}$') { $last = [Math]::Max($last, [int]$Matches[0]) }
    }
    Join-Path $Folder ('{0}{1:000}.log' -f $prefix, ($last + 1))
}

$fresh = -not (Test-Path -LiteralPath $user)
if ($fresh) {
    New-Item -ItemType Directory -Force $user | Out-Null
    $lines = @('# El Chavo Kart: online test instance settings.')
    $source = Join-Path $CopyFrom 'settings.toml'
    if (Test-Path -LiteralPath $source) {
        $lines = @(Get-Content -LiteralPath $source | Where-Object { $_ -notmatch $OwnKeys })
    }
    $lines += "$NameKey = `"$OnlineName`""
    Set-Content -LiteralPath $settingsFile -Value $lines -Encoding utf8
} elseif (-not (Select-String -LiteralPath $settingsFile -Pattern ('^\s*' + $NameKey + '\s*=') -Quiet)) {
    Add-Content -LiteralPath $settingsFile -Value "$NameKey = `"$OnlineName`"" -Encoding utf8
}
New-Item -ItemType Directory -Force $logs | Out-Null
$logFile = Next-LogFile $logs $label

Write-Host "Online test instance $($label.ToUpperInvariant())$(if ($fresh) { ' (new)' })"
Write-Host "  instance:       $label"
Write-Host "  user folder:    $user"
Write-Host "  settings:       $settingsFile"
Write-Host "  identity:       $(Join-Path $user 'eck_online_identity.ini') (made on first use; this instance has its own friend code)"
Write-Host "  logs folder:    $logs"
Write-Host "  log file:       $logFile"
Write-Host "  UDP port:       $Port (ECK_ONLINE_PORT)"
Write-Host "  online service: $(if ($Lobby) { "ECK_LOBBY $Lobby" } else { 'the game default' })"

if ($PrepareOnly) { return }

if (-not (Test-Path -LiteralPath $Executable)) { throw "Not built yet: $Executable" }
$gameArgs = @("--game_data_root=`"$GameData`"", "--user_data_root=`"$user`"", "--log_file=`"$logFile`"")
$env:ECK_ONLINE_PORT = "$Port"
$env:ECK_INSTANCE = $label
if ($Lobby) { $env:ECK_LOBBY = $Lobby }
Start-Process -FilePath $Executable -ArgumentList $gameArgs -WorkingDirectory (Split-Path -Parent $Executable)
