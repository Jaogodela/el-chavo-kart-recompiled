param(
  [string]$Root = "C:\Users\joaop\el-chavo-kart-recompiled",
  [int]$SpreadSamples = 8
)
$ErrorActionPreference = 'Stop'
$client = Join-Path $Root 'tools\test-client.py'
$userRoot = Join-Path $Root 'build\online-instances'
$players = @(@('a', 7101, '8799', 0), @('b', 7102, '8800', 2), @('c', 7103, '8801', 4))

function C([string]$port, [string]$request) {
  $env:ECK_TEST_CONTROL_PORT = $port
  return ((& python -I $client $request 2>&1) -join "`n")
}
function Tap([string]$port, [string]$keys, [int]$hold = 400, [int]$gap = 700) {
  foreach ($key in ($keys -split ',')) {
    C $port ("pad $key 0 0 0 0") | Out-Null
    Start-Sleep -Milliseconds $hold
    C $port "pad 0 0 0 0 0" | Out-Null
    Start-Sleep -Milliseconds $gap
  }
}
function LogPath([string]$instance) {
  return (Get-ChildItem (Join-Path $userRoot "$instance\logs\*.log") | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
function Wait-For([string]$instance, [string]$pattern, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    $hit = Get-Content (LogPath $instance) | Select-String -Pattern $pattern | Select-Object -Last 1
    if ($hit) { return $hit.Line }
    Start-Sleep -Milliseconds 600
  }
  return ""
}
function Note([string]$t) { Write-Host ("[" + (Get-Date -Format "HH:mm:ss") + "] " + $t) }
function Panels([string]$instance) {
  $lines = (Get-Content (LogPath $instance) | Select-String -Pattern "ONLINE-GAME\] panel\d+" | Select-Object -Last 8).Line
  $map = @{}
  foreach ($line in $lines) {
    if ($line -match "panel(\d) seat=(\d) .*character=(-?\d+) state=(\d+)") { $map[$matches[2]] = "seat$($matches[2])=char$($matches[3])" }
  }
  return (($map.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Value }) -join " ")
}

# Settings before anything runs.
foreach ($p in $players) {
  $user = Join-Path $userRoot ($p[0] + '\user_data')
  New-Item -ItemType Directory -Force $user | Out-Null
  $settings = Join-Path $user 'settings.toml'
  $text = if (Test-Path $settings) { Get-Content $settings -Raw } else { "" }
  foreach ($kv in @(@('eck_launcher', 'false'), @('eck_skip_intro', 'true'), @('eck_online_enabled', 'true'),
                    @('eck_online_connection_test', '"udp 7101-7110"'))) {
    if ($text -match "(?m)^$($kv[0])\s*=") { $text = [regex]::Replace($text, "(?m)^$($kv[0])\s*=.*$", "$($kv[0]) = $($kv[1])") }
    else { $text += "`n$($kv[0]) = $($kv[1])`n" }
  }
  Set-Content -LiteralPath $settings -Value $text -NoNewline
}

Get-Process -Name el_chavo_kart -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3
$instance = Join-Path $Root 'tools\online-instance.ps1'
foreach ($p in $players) {
  $env:ECK_LOBBY = "ws://127.0.0.1:8791/"
  $env:ECK_TEST_CONTROL = "1"
  $env:ECK_TEST_CONTROL_PORT = $p[2]
  & $instance -Name $p[0] -Port ([int]$p[1]) -Lobby "ws://127.0.0.1:8791/" | Out-Null
  Start-Sleep -Seconds 2
}
Note "three launched"
Start-Sleep -Seconds 26

$codesPy = Join-Path $env:TEMP 'eck-codes-p3.py'
@'
import hashlib, io, json, os, re, sys
alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
out = {}
for name in ("a", "b", "c"):
    path = os.path.join(sys.argv[1], "build", "online-instances", name, "user_data", "eck_online_identity.ini")
    if not os.path.isfile(path):
        continue
    text = io.open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"token=([0-9a-f]{64})", text)
    if not m:
        continue
    d = hashlib.sha256(("eck-player-id:" + m.group(1)).encode()).hexdigest()
    chars = "".join(alphabet[int(d[2 * i:2 * i + 2], 16) % 32] for i in range(8))
    out[name] = chars[:4] + "-" + chars[4:]
print(json.dumps(out))
'@ | Set-Content -LiteralPath $codesPy -Encoding utf8
$codes = python -I $codesPy $Root | ConvertFrom-Json
Note ("codes " + ($codes | ConvertTo-Json -Compress))
foreach ($from in @(@('8799', 'b'), @('8799', 'c'), @('8800', 'a'), @('8800', 'c'), @('8801', 'a'), @('8801', 'b'))) {
  C $from[0] ("friend add " + $codes.($from[1])) | Out-Null
  Start-Sleep -Milliseconds 700
}

foreach ($p in $players) {
  Note ($p[0] + ": to the character screen")
  Tap $p[2] "16" 1300 5000
  Tap $p[2] "2,2,4096" 450 800
  Tap $p[2] "4096" 450 2400
  Tap $p[2] "4096" 450 3200
}

Note "A invites, both accept"
C 8799 ("match host " + $codes.b) | Out-Null
Note ("  " + (Wait-For b "invite received" 12))
C 8800 "match accept" | Out-Null
Note ("  " + (Wait-For b "session joined" 12))
C 8799 ("match host " + $codes.c) | Out-Null
Note ("  " + (Wait-For c "invite received" 12))
C 8801 "match accept" | Out-Null
Note ("  " + (Wait-For c "session joined" 12))

# Each player picks in the game's own panel, which is what a player does: move with the dpad, confirm with A. Nothing goes
# through match.pick, so the only thing the room hears is what the panel really shows.
Note "picks through the panels"
foreach ($p in $players) {
  if ($p[3] -gt 0) {
    $keys = (1..$p[3] | ForEach-Object { "8" }) -join ","
    Tap $p[2] $keys 300 260
  }
  Tap $p[2] "4096" 450 900
}
Start-Sleep -Seconds 3

# The game's own "all players ready?" gate wants every panel confirmed, and a remote panel takes a few seconds to walk to
# its player's character and confirm: pressing Start before that is what kept a race from being published.
Note "waiting for every panel to be confirmed"
foreach ($seat in 0..2) {
  $hit = Wait-For a ("panel" + $seat + " seat=" + $seat + " .*state=2") 45
  Note ("  panel" + $seat + " " + $(if ($hit) { "confirmed" } else { "STILL PENDING" }))
}

Note "panels, as each PC sees them"
foreach ($p in $players) { Note ("  " + $p[0] + ": " + (Panels $p[0])) }

Note "host: START, then the track"
Tap 8799 "16" 1500 4000
Tap 8799 "4096" 700 3000
Note ("  " + (Wait-For a "race published" 25))

Note "start spread"
& (Join-Path $Root 'tools\online-start-spread.ps1') -Samples $SpreadSamples
