param(
  [string]$Root = "C:\Users\joaop\el-chavo-kart-recompiled",
  # String on purpose: `powershell -File ... -Counts 2,3,4` used to bind as the integer 234
  # (culture thousands separators) and that opened a window per player until the desktop filled.
  [string]$Counts = "2",
  [int]$RaceSamples = 55,
  [int]$EverySeconds = 6
)
$ErrorActionPreference = 'Continue'

$lock = Join-Path $env:TEMP 'eck-online-review.lock'
if (Test-Path $lock) {
  $owner = Get-Content $lock -ErrorAction SilentlyContinue
  $alive = $owner -and (Get-Process -Id $owner -ErrorAction SilentlyContinue)
  if ($alive) { Write-Host "uma revisao ja esta rodando (pid $owner); saindo"; exit 1 }
}
Set-Content -LiteralPath $lock -Value $PID

$wanted = @($Counts -split '[, ]+' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$rejected = @()
$CountList = @()
foreach ($raw in $wanted) {
  $n = 0
  if (-not [int]::TryParse($raw, [ref]$n)) { $rejected += $raw; continue }
  if ($n -lt 2 -or $n -gt 4) { $rejected += "$n"; continue }
  $CountList += $n
}
$CountList = @($CountList | Select-Object -Unique)
if ($rejected) { Write-Host ("ignorado (so 2, 3 ou 4): " + ($rejected -join ', ')) }
if (-not $CountList) {
  Remove-Item $lock -Force -ErrorAction SilentlyContinue
  Write-Host "nada a fazer: use -Counts 2, 3 ou 4"
  exit 1
}

$userRoot = Join-Path $Root 'build\online-instances'
$report = Join-Path $Root 'build'

function C([string]$port, [string]$request) {
  $tcp = $null
  try {
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.NoDelay = $true
    $iar = $tcp.BeginConnect('127.0.0.1', [int]$port, $null, $null)
    if (-not $iar.AsyncWaitHandle.WaitOne(3000, $false)) { throw 'connect timeout' }
    $tcp.EndConnect($iar)
    $stream = $tcp.GetStream()
    $stream.ReadTimeout = 15000
    $stream.WriteTimeout = 3000
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $bytes = $utf8.GetBytes($request + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $reader = New-Object System.IO.StreamReader($stream, $utf8)
    $lines = New-Object System.Collections.Generic.List[string]
    while ($true) {
      $line = $reader.ReadLine()
      if ($null -eq $line) { break }
      $line = $line.Trim()
      if (-not $line) { continue }
      [void]$lines.Add($line)
      if ($line -eq 'end') { break }
    }
    return ($lines -join "`n")
  } catch {
    return "cannot reach 127.0.0.1:${port}: $($_.Exception.Message)"
  } finally {
    if ($tcp) { $tcp.Close() }
  }
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
  $dir = Join-Path $userRoot "$instance\logs"
  $hit = Get-ChildItem $dir -Filter '*.log' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($hit) { return $hit.FullName }
  return $null
}
function Wait-For([string]$instance, [string]$pattern, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $deadline) {
    $path = LogPath $instance
    if ($path) {
      $hit = Get-Content $path | Select-String -Pattern $pattern | Select-Object -Last 1
      if ($hit) { return $hit.Line }
    }
    Start-Sleep -Milliseconds 700
  }
  return ""
}
function Own([string]$port) {
  $raw = C $port "state"
  $line = (($raw -split "`n") | Where-Object { $_ -like '{"race_state*' } | Select-Object -First 1)
  if (-not $line) { return $null }
  try { return ($line | ConvertFrom-Json) } catch { return $null }
}
function KartCount { return @(Get-Process -Name el_chavo_kart -ErrorAction SilentlyContinue).Count }
function Stop-Karts {
  Get-Process -Name el_chavo_kart -ErrorAction SilentlyContinue | Stop-Process -Force
  Get-CimInstance Win32_Process -Filter "Name='node.exe'" | Where-Object { $_.CommandLine -like '*eck-lobby*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
}
function Say([string]$t) { Write-Host $t }

function RunCount([int]$n) {
  $file = Join-Path $report "review-$n.txt"
  $samplesFile = Join-Path $report "review-$n-samples.txt"
  Set-Content $file "=== $n jogadores === $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
  Set-Content $samplesFile "=== samples $n === $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
  function Note([string]$t) {
    $line = "$(Get-Date -Format 'HH:mm:ss') $t"
    Add-Content $file $line
    Say $line
  }
  if ($n -lt 2 -or $n -gt 4) { Note "abortado: n=$n fora de 2..4"; return }

  $players = @()
  for ($i = 0; $i -lt $n; ++$i) {
    $letter = [string][char](0x61 + $i)
    if ($letter -notmatch '^[a-d]$') { Note "abortado: nome de instancia invalido ($letter)"; return }
    $players += [pscustomobject]@{ Name = $letter; Udp = 7101 + $i; Ctrl = 8799 + $i }
  }
  if ($players.Count -ne $n -or $players.Count -gt 4) { Note "abortado: lista de jogadores inconsistente"; return }
  $ports = $players | ForEach-Object { "($($_.Name) udp=$($_.Udp) ctrl=$($_.Ctrl))" }
  Note ("jogadores: " + ($ports -join ' '))
  $udpSet = @($players.Udp | Select-Object -Unique)
  $ctrlSet = @($players.Ctrl | Select-Object -Unique)
  if ($udpSet.Count -ne $n -or $ctrlSet.Count -ne $n) { Note "abortado: portas repetidas"; return }

  Note "teto: $n janelas, cliente TCP in-process, uma revisao"
  Say "[$n] limpando e subindo o lobby"
  Stop-Karts
  for ($wait = 0; $wait -lt 20; ++$wait) {
    if ((KartCount) -eq 0) { break }
    Start-Sleep -Milliseconds 500
  }
  if ((KartCount) -ne 0) { Note "abortado: instancias antigas ainda abertas"; return }

  Set-Content (Join-Path $report 'lobby.log') ""
  Start-Process -FilePath "node" -ArgumentList "server/eck-lobby/local/server.js", "--port", "8791" -WorkingDirectory $Root -WindowStyle Hidden -RedirectStandardOutput (Join-Path $report 'lobby.log') -RedirectStandardError (Join-Path $report 'lobby.err')
  Start-Sleep -Seconds 4
  $lobbyUp = Get-Content (Join-Path $report 'lobby.log') -ErrorAction SilentlyContinue | Select-String -Pattern 'listening' | Select-Object -First 1
  if (-not $lobbyUp) { Note "abortado: lobby nao subiu"; Stop-Karts; return }
  Note "lobby ok em ws://127.0.0.1:8791/"

  for ($i = 0; $i -lt $n; ++$i) {
    $p = $players[$i]
    $user = Join-Path $userRoot ($p.Name + '\user_data')
    New-Item -ItemType Directory -Force $user | Out-Null
    $settings = Join-Path $user 'settings.toml'
    $text = if (Test-Path $settings) { Get-Content $settings -Raw } else { "" }
    $kvs = @(
      @('eck_launcher', 'false'),
      @('eck_skip_intro', 'true'),
      @('eck_online_enabled', 'true'),
      @('eck_online_connection_test', '"udp 7101-7110"'),
      @('eck_launcher_fullscreen', 'false'),
      @('eck_launcher_maximized', 'false'),
      @('eck_launcher_width', '960'),
      @('eck_launcher_height', '540'),
      @('eck_launcher_x', [string]($i * 960)),
      @('eck_launcher_y', '40'),
      @('detailed_logging', 'true')
    )
    foreach ($kv in $kvs) {
      if ($text -match "(?m)^$($kv[0])\s*=") { $text = [regex]::Replace($text, "(?m)^$($kv[0])\s*=.*$", "$($kv[0]) = $($kv[1])") } else { $text += "`n$($kv[0]) = $($kv[1])`n" }
    }
    Set-Content -LiteralPath $settings -Value $text -NoNewline
  }

  $instance = Join-Path $Root 'tools\online-instance.ps1'
  foreach ($p in $players) {
    if ((KartCount) -ge $n) { Note "abortado: ja existem $(KartCount) janelas antes de abrir $($p.Name)"; Stop-Karts; return }
    $env:ECK_LOBBY = "ws://127.0.0.1:8791/"
    $env:ECK_TEST_CONTROL = "1"
    $env:ECK_TEST_CONTROL_PORT = $p.Ctrl
    Note "abrindo $($p.Name) udp=$($p.Udp) ctrl=$($p.Ctrl)"
    & $instance -Name $p.Name -Port ([int]$p.Udp) -Lobby "ws://127.0.0.1:8791/" | Out-Null
    Start-Sleep -Seconds 3
    $now = KartCount
    Note "janelas agora: $now"
    if ($now -gt $n) { Note "abortado: passou do teto ($now > $n)"; Stop-Karts; return }
  }
  Start-Sleep -Seconds 24
  $got = @(Get-Process -Name el_chavo_kart -ErrorAction SilentlyContinue)
  Note ("janelas apos boot: $($got.Count) esperado $n pids=" + (($got | ForEach-Object { $_.Id }) -join ','))
  if ($got.Count -ne $n) { Note "abortado: quantidade de janelas diferente do pedido"; Stop-Karts; return }

  foreach ($p in $players) {
    $help = C $p.Ctrl "help"
    if ($help -notmatch 'end|ok|state') { Note "abortado: canal $($p.Name):$($p.Ctrl) morto: $help"; Stop-Karts; return }
    Note "canal $($p.Name):$($p.Ctrl) ok"
  }

  try {
    $codesPy = Join-Path $env:TEMP "eck-codes-r$n.py"
    $py = @'
import hashlib, io, json, os, re, sys
alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
out = {}
root = sys.argv[1]
for letter in sys.argv[2]:
    path = os.path.join(root, "build", "online-instances", letter, "user_data", "eck_online_identity.ini")
    if not os.path.isfile(path):
        continue
    text = io.open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"token=([0-9a-f]{64})", text)
    if not m:
        continue
    d = hashlib.sha256(("eck-player-id:" + m.group(1)).encode()).hexdigest()
    chars = "".join(alphabet[int(d[2 * i:2 * i + 2], 16) % 32] for i in range(8))
    out[letter] = chars[:4] + "-" + chars[4:]
print(json.dumps(out))
'@
    Set-Content -LiteralPath $codesPy -Value $py -Encoding utf8
    $letters = -join ($players | ForEach-Object { $_.Name })
    $codes = python -I $codesPy $Root $letters | ConvertFrom-Json
    Note ("codes: " + ($codes | ConvertTo-Json -Compress))

    foreach ($from in $players) {
      foreach ($to in $players) {
        if ($from.Name -eq $to.Name) { continue }
        $answer = C $from.Ctrl ("friend add " + $codes.($to.Name))
        Note ("friend " + $from.Name + "->" + $to.Name + ": " + (($answer -split "`n")[0]))
      }
    }
    Start-Sleep -Seconds 2

    foreach ($p in $players) {
      Note "menu $($p.Name): Start, desce ate Free Race, A"
      Tap $p.Ctrl "16" 1300 5000
      Tap $p.Ctrl "2,2,4096" 450 800
      Tap $p.Ctrl "4096" 450 2400
      Tap $p.Ctrl "4096" 450 3200
    }

    for ($i = 1; $i -lt $n; ++$i) {
      $guest = $players[$i]
      $invite = C $players[0].Ctrl ("match host " + $codes.($guest.Name))
      $heard = Wait-For $guest.Name "invite received" 15
      $accept = C $guest.Ctrl "match accept"
      $joined = Wait-For $guest.Name "session joined" 15
      Note ("invite $($guest.Name): $invite | heard=$(if ($heard) { 'yes' } else { 'no' }) | accept=$accept | joined=$(if ($joined) { 'yes' } else { 'no' })")
    }

    for ($i = 0; $i -lt $n; ++$i) {
      $p = $players[$i]
      if ($i -gt 0) { Tap $p.Ctrl ((1..$i | ForEach-Object { "8" }) -join ",") 300 260 }
      Tap $p.Ctrl "4096" 450 900
      $picked = C $p.Ctrl ("pick 2 " + $i)
      Note ("pick $($p.Name) seat=${i}: " + (($picked -split "`n")[0]))
    }

    $seatLines = @()
    for ($seat = 0; $seat -lt $n; ++$seat) {
      $hit = Wait-For $players[0].Name ("panel" + $seat + " seat=" + $seat + " .*state=2") 45
      if (-not $hit) {
        $again = C $players[$seat].Ctrl ("pick 2 " + $seat)
        Note ("pick retry seat=${seat}: " + (($again -split "`n")[0]))
        $hit = Wait-For $players[0].Name ("panel" + $seat + " seat=" + $seat + " .*state=2") 20
      }
      $seatLines += "panel$seat=$(if ($hit) { 'ok' } else { 'PENDING' })"
    }
    Note ("picks: " + ($seatLines -join " "))
    foreach ($p in $players) {
      $path = LogPath $p.Name
      $last = @()
      if ($path) { $last = (Get-Content $path | Select-String -Pattern "ONLINE-GAME\] panel\d" | Select-Object -Last 8).Line }
      $map = @{}
      foreach ($line in $last) { if ($line -match "panel(\d) seat=(\d) .*character=(-?\d+) state=(\d+)") { $map[$matches[2]] = "s$($matches[2])=c$($matches[3])/st$($matches[4])" } }
      Note ("panels " + $p.Name + ": " + (($map.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Value }) -join " "))
    }

    $leader = $players[0]
    Note "host confirma, Start, 4x baixo ate Chavoltepec (track 4), A"
    Tap $leader.Ctrl "4096" 700 3000
    Tap $leader.Ctrl "16" 1500 4000
    Tap $leader.Ctrl "2,2,2,2" 400 500
    Tap $leader.Ctrl "4096" 700 3500
    $publish = Wait-For $leader.Name "race published" 30
    Note ("publish: $(if ($publish) { 'ok  ' + $publish } else { 'FAILED' })")
    foreach ($p in $players) {
      $begun = Wait-For $p.Name "race_begin" 45
      Note ("race_begin $($p.Name): $(if ($begun) { $begun } else { 'MISSING' })")
    }
    if (-not $publish) {
      foreach ($p in $players) {
        $path = LogPath $p.Name
        $errs = @()
        if ($path) { $errs = (Get-Content $path | Select-String -Pattern "\[error\]|\[warning\]" | Select-Object -Last 5).Line }
        Note ("  " + $p.Name + " last errors: " + (($errs | ForEach-Object { ($_ -split '\] ')[-1] }) -join " ||| "))
      }
      return
    }

    foreach ($p in $players) { C $p.Ctrl "pad 0 0 0 255 0" | Out-Null }
    Note "throttle em todos; amostrando a corrida"

    $last = @{}; $lastLap = @{}; $back = @{}; $spread1 = -1; $rsMismatch = 0
    for ($s = 0; $s -lt $RaceSamples; ++$s) {
      Start-Sleep -Seconds $EverySeconds
      $live = KartCount
      if ($live -ne $n) { Note "abortado no meio da corrida: $live janelas (esperado $n)"; break }
      $values = @()
      $states = @()
      $parts = @()
      for ($i = 0; $i -lt $n; ++$i) {
        $p = $players[$i]
        $json = Own $p.Ctrl
        if (-not $json) { $parts += "$($p.Name)=no_state"; continue }
        $states += [int]$json.race_state
        $view = @()
        foreach ($k in @($json.karts)) {
          $view += ("s{0}:lap={1}:dist={2}:place={3}" -f $k.slot, $k.lap, $k.distance, $k.place)
        }
        $mine = $json.karts | Where-Object { $_.slot -eq $i } | Select-Object -First 1
        if ($mine) {
          $d = [int]$mine.distance
          $lap = [int]$mine.lap
          if ($last.ContainsKey($p.Name)) {
            if ($d -lt ($last[$p.Name] - 500) -and $lap -le $lastLap[$p.Name]) { $back[$p.Name] = 1 + [int]$back[$p.Name] }
          }
          $last[$p.Name] = $d; $lastLap[$p.Name] = $lap
          $values += $d
        }
        $parts += ("{0} rs={1} [{2}]" -f $p.Name, $json.race_state, ($view -join '; '))
      }
      $spread = -1
      if ($values.Count -eq $n) { $spread = ($values | Measure-Object -Maximum).Maximum - ($values | Measure-Object -Minimum).Minimum }
      if ($s -eq 1) { $spread1 = $spread }
      $sample = ("t={0} sample={1} spread={2}  {3}" -f (Get-Date -Format 'HH:mm:ss'), $s, $spread, ($parts -join '  |  '))
      Add-Content $samplesFile $sample
      Note $sample
      if ($states.Count -eq $n) {
        $uniq = @($states | Select-Object -Unique)
        if ($uniq.Count -gt 1) {
          $rsMismatch++
          Note ("RS DESSINC sample=$s  " + (($players | ForEach-Object { $_.Name }) -join '/') + " rs=" + ($states -join '/'))
        }
      }
      if ($states.Count -eq $n -and ($states | Where-Object { $_ -le 2 }).Count -eq 0) { Note "corrida saiu do estado de pista (rs>2)"; break }
    }
    Note ("rs_mismatch_samples=$rsMismatch")

    $final = @()
    $totalBack = 0
    foreach ($p in $players) { $totalBack += [int]($back[$p.Name]) }
    for ($i = 0; $i -lt $n; ++$i) {
      $p = $players[$i]
      $json = Own $p.Ctrl
      if (-not $json) { $final += ($p.Name + "=no_state"); continue }
      $mine = $json.karts | Where-Object { $_.slot -eq $i } | Select-Object -First 1
      $final += ("{0}: rs={1} lap={2} dist={3} place={4}" -f $p.Name, $json.race_state, $(if ($mine) { $mine.lap } else { '-' }), $(if ($mine) { $mine.distance } else { '-' }), $(if ($mine) { $mine.place } else { '-' }))
    }
    Note ("start spread (t=1): $spread1   backwards: $totalBack")
    foreach ($line in $final) { Note ("final " + $line) }

    foreach ($p in $players) {
      $path = LogPath $p.Name
      if (-not $path) { Note ("log $($p.Name): missing"); continue }
      $log = Get-Content $path
      $errors = @($log | Select-String -Pattern "\[error\]")
      $warnings = @($log | Select-String -Pattern "\[warning\]")
      $markers = @()
      foreach ($pattern in @("session ended", "results before the host", "guest results", "world pawns", "finish_rx",
                            "post_race", "url.map=", "race travel abandoned", "guest holding", "rejected=[1-9]",
                            "dropped_old=[1-9]", "own_snaps=[1-9]", "remote jump smoothed", "remote teleport",
                            "finish refused", "race published", "invite received", "session joined",
                            "scaleform skipped", "race_begin", "start instant", "datagram refused")) {
        $count = ($log | Select-String -Pattern $pattern).Count
        if ($count -gt 0) { $markers += "$pattern=$count" }
      }
      $map = ($log | Select-String -Pattern "url.map=" | Select-Object -Last 1).Line
      $ping = ($log | Select-String -Pattern "ONLINE-NET\] gameplay" | Select-Object -Last 1).Line
      Note ("log $($p.Name): errors=$($errors.Count) warnings=$($warnings.Count) " + ($markers -join " "))
      if ($map) { Note ("  map " + ($map -split 'url.map=')[-1]) }
      if ($ping) { Note ("  net " + ($ping -split '\] ')[-1]) }
      $uniqErr = @{}
      foreach ($line in $errors) {
        $msg = ($line.Line -split '\] ', 6)[-1]
        if (-not $uniqErr.ContainsKey($msg)) { $uniqErr[$msg] = 0 }
        $uniqErr[$msg]++
      }
      foreach ($k in ($uniqErr.Keys | Sort-Object)) { Note ("  ERROR x$($uniqErr[$k]): $k") }
      $uniqWarn = @{}
      foreach ($line in $warnings) {
        $msg = ($line.Line -split '\] ', 6)[-1]
        if (-not $uniqWarn.ContainsKey($msg)) { $uniqWarn[$msg] = 0 }
        $uniqWarn[$msg]++
      }
      foreach ($k in ($uniqWarn.Keys | Sort-Object)) { Note ("  WARN x$($uniqWarn[$k]): $k") }
      foreach ($pattern in @("results before the host", "guest results", "world pawns", "post_race", "scaleform skipped",
                            "race_begin", "start instant", "datagram refused", "finish_rx", "finish refused",
                            "remote jump smoothed", "remote teleport")) {
        foreach ($hit in @($log | Select-String -Pattern $pattern | Select-Object -First 8)) {
          Note ("  HIT $($p.Name): " + $hit.Line)
        }
      }
    }
  } catch {
    Note ("ERRO: " + $_.Exception.Message)
  } finally {
    Note "encerrando $n janelas e o lobby"
    Stop-Karts
    Start-Sleep -Seconds 2
    $dir = Join-Path $report "review-$n-logs"
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($p in $players) {
      $src = LogPath $p.Name
      if ($src -and (Test-Path $src)) {
        Copy-Item $src (Join-Path $dir ("$($p.Name).log")) -Force
        $all = Get-Content $src
        $extract = $all | Select-String -Pattern '\[ONLINE|\[error\]|\[warning\]|invite received|session joined|race published|StartGate|gameplay|world pawns|guest results|results before'
        Set-Content (Join-Path $dir "$($p.Name)-online.txt") ($extract | ForEach-Object { $_.Line })
        Set-Content (Join-Path $dir "$($p.Name)-errors.txt") @($all | Select-String -Pattern '\[error\]|\[warning\]' | ForEach-Object { $_.Line })
        Set-Content (Join-Path $dir "$($p.Name)-net.txt") @($all | Select-String -Pattern 'ONLINE-NET' | ForEach-Object { $_.Line })
      }
    }
    Copy-Item (Join-Path $report 'lobby.log') (Join-Path $dir 'lobby.log') -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $report 'lobby.err') (Join-Path $dir 'lobby.err') -ErrorAction SilentlyContinue
    Note ("logs copiados para $dir  janelas_restantes=$(KartCount)")
  }
}

Say ("revisao: " + ($CountList -join ', ') + " jogadores, uma rodada por vez")
try {
  foreach ($n in $CountList) {
    RunCount $n
  }
} finally {
  Stop-Karts
  Remove-Item $lock -Force -ErrorAction SilentlyContinue
}
Say "revisao concluida  janelas=$(KartCount)"
