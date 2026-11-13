param(
  [string]$Root = "C:\Users\joaop\el-chavo-kart-recompiled",
  [int]$Samples = 12,
  [int]$EverySeconds = 1
)
# How together the karts leave the gate, without touching a key: the port presses Start itself at the shared instant, so a
# START from here would open the multiplayer menu instead. Each PC reports its own kart, once a sample.
$client = Join-Path $Root 'tools\test-client.py'
$ports = @(@('a', '8799', 0), @('b', '8800', 1), @('c', '8801', 2))

function C([string]$port, [string]$request) {
  $env:ECK_TEST_CONTROL_PORT = $port
  return ((& python -I $client $request 2>&1) -join "`n")
}
function Own([string]$port, [int]$seat) {
  $line = ((C $port "state") -split "`n" | Where-Object { $_ -like '{"race_state*' } | Select-Object -First 1)
  if (-not $line) { return $null }
  try { $json = $line | ConvertFrom-Json } catch { return $null }
  return ($json.karts | Where-Object { $_.slot -eq $seat } | Select-Object -First 1)
}

"waiting for the race to be under way"
$deadline = (Get-Date).AddSeconds(40)
while ((Get-Date) -lt $deadline) {
  $running = 0
  foreach ($p in $ports) {
    $kart = Own $p[1] $p[2]
    if ($kart -and [int]$kart.distance -gt 0) { ++$running }
  }
  if ($running -eq $ports.Count) { break }
  Start-Sleep -Milliseconds 500
}

foreach ($p in $ports) { C $p[1] "pad 0 0 0 255 0" | Out-Null }   # throttle only, no buttons

# A lap resets the distance, so a step backwards only counts when the lap did not change: that is what a teleport looks like.
$last = @{}; $lastLap = @{}; $back = @{}
"t    a-dist/lap   b-dist/lap   c-dist/lap   spread  backwards"
for ($i = 0; $i -lt $Samples; ++$i) {
  $cells = @()
  $values = @()
  foreach ($p in $ports) {
    $kart = Own $p[1] $p[2]
    $d = if ($kart) { [int]$kart.distance } else { -1 }
    $lap = if ($kart) { [int]$kart.lap } else { -1 }
    if ($last.ContainsKey($p[0]) -and $d -lt ($last[$p[0]] - 500) -and $lap -le $lastLap[$p[0]]) {
      $back[$p[0]] = 1 + ($back[$p[0]] | ForEach-Object { $_ })
    }
    $last[$p[0]] = $d
    $lastLap[$p[0]] = $lap
    $values += $d
    $cells += ("{0}/{1}" -f $d, $lap)
  }
  $spread = ($values | Measure-Object -Maximum).Maximum - ($values | Measure-Object -Minimum).Minimum
  $total = 0
  foreach ($k in $back.Keys) { $total += [int]$back[$k] }
  ("{0,2}  {1,12}  {2,12}  {3,12}   {4,6}  {5}" -f $i, $cells[0], $cells[1], $cells[2], $spread, $total)
  Start-Sleep -Seconds $EverySeconds
}
