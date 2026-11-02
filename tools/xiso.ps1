# Reads the XDVDFS game partition of an Xbox 360 disc image. Dot-sourced by build.ps1 and tools\extract-iso.ps1.

$XisoSector = 2048
$XisoMagic = 'MICROSOFT*XBOX*MEDIA'
# Where the game partition starts on the disc formats an image can hold (XGD1, XGD2, XGD3, a bare partition).
$XisoPartitionBases = @(0x18300000, 0xFD90000, 0x2080000, 0)

function Read-XisoBytes([IO.Stream]$Stream, [long]$Offset, [int]$Count) {
    $buffer = New-Object byte[] $Count
    $Stream.Position = $Offset
    $read = 0
    while ($read -lt $Count) {
        $n = $Stream.Read($buffer, $read, $Count - $read)
        if ($n -le 0) { throw "The disc image ends early (offset $Offset)" }
        $read += $n
    }
    return , $buffer
}

function Get-XisoBase([IO.Stream]$Stream) {
    foreach ($base in $XisoPartitionBases) {
        $header = [long]$base + 32 * $XisoSector
        if ($header + 0x800 -gt $Stream.Length) { continue }
        $descriptor = Read-XisoBytes $Stream $header 0x800
        $head = [Text.Encoding]::ASCII.GetString($descriptor, 0, 20)
        $tail = [Text.Encoding]::ASCII.GetString($descriptor, 0x7EC, 20)
        if ($head -eq $XisoMagic -and $tail -eq $XisoMagic) {
            return [pscustomobject]@{
                Base = [long]$base
                RootSector = [BitConverter]::ToUInt32($descriptor, 20)
                RootSize = [BitConverter]::ToUInt32($descriptor, 24)
            }
        }
    }
    throw 'Not an Xbox 360 disc image (no XDVDFS volume descriptor)'
}

# Every entry of the image: Path ('/'-separated), Offset, Size, Directory.
function Get-XisoEntries([IO.Stream]$Stream) {
    $volume = Get-XisoBase $Stream
    $entries = New-Object System.Collections.Generic.List[object]
    $folders = New-Object System.Collections.Generic.Stack[object]
    $folders.Push([pscustomobject]@{ Sector = [long]$volume.RootSector; Size = [long]$volume.RootSize; Prefix = ''; Depth = 0 })
    while ($folders.Count) {
        $folder = $folders.Pop()
        if ($folder.Size -eq 0) { continue }
        if ($folder.Depth -gt 32 -or $folder.Size -gt 64MB) { throw "Damaged directory table below '$($folder.Prefix)'" }
        $table = Read-XisoBytes $Stream ($volume.Base + $folder.Sector * $XisoSector) ([int]$folder.Size)
        $visited = New-Object System.Collections.Generic.HashSet[int]
        $pending = New-Object System.Collections.Generic.Stack[int]
        $pending.Push(0)
        while ($pending.Count) {
            $offset = $pending.Pop()
            if (-not $visited.Add($offset)) { continue }
            if ($offset + 14 -gt $table.Length) { throw 'Damaged directory entry' }
            $left = [BitConverter]::ToUInt16($table, $offset)
            $right = [BitConverter]::ToUInt16($table, $offset + 2)
            $sector = [BitConverter]::ToUInt32($table, $offset + 4)
            $size = [BitConverter]::ToUInt32($table, $offset + 8)
            $attributes = $table[$offset + 12]
            $nameLength = $table[$offset + 13]
            if ($nameLength -eq 0 -or $offset + 14 + $nameLength -gt $table.Length) { throw 'Damaged directory entry' }
            $name = [Text.Encoding]::GetEncoding(28591).GetString($table, $offset + 14, $nameLength)
            if ($name -eq '.' -or $name -eq '..' -or $name -match '[\x00-\x1F/\\:*?"<>|]') { throw "Unsafe name in the image: $name" }
            if ($left -ne 0 -and $left -ne 0xFFFF) { $pending.Push($left * 4) }
            if ($right -ne 0 -and $right -ne 0xFFFF) { $pending.Push($right * 4) }
            $path = $folder.Prefix + $name
            $directory = ($attributes -band 0x10) -ne 0
            $entry = [pscustomobject]@{ Path = $path; Offset = $volume.Base + [long]$sector * $XisoSector; Size = [long]$size; Directory = $directory }
            if (-not $directory -and $entry.Offset + $entry.Size -gt $Stream.Length) { throw "'$path' extends past the end of the image" }
            $entries.Add($entry)
            if ($directory) {
                $folders.Push([pscustomobject]@{ Sector = [long]$sector; Size = [long]$size; Prefix = "$path/"; Depth = $folder.Depth + 1 })
            }
        }
    }
    return $entries
}

function Find-XisoEntry($Entries, [string]$Path) {
    foreach ($entry in $Entries) {
        if ([string]::Equals($entry.Path, $Path, [StringComparison]::OrdinalIgnoreCase)) { return $entry }
    }
    return $null
}

function Get-XisoFileSha256([IO.Stream]$Stream, $Entry) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $buffer = New-Object byte[] (1MB)
        $Stream.Position = $Entry.Offset
        $remaining = $Entry.Size
        while ($remaining -gt 0) {
            $n = $Stream.Read($buffer, 0, [int][Math]::Min($remaining, $buffer.Length))
            if ($n -le 0) { throw "The disc image ends inside $($Entry.Path)" }
            [void]$sha.TransformBlock($buffer, 0, $n, $null, 0)
            $remaining -= $n
        }
        [void]$sha.TransformFinalBlock($buffer, 0, 0)
        return ([BitConverter]::ToString($sha.Hash) -replace '-', '')
    } finally { $sha.Dispose() }
}

# Writes the files below $Destination, leaving out the top-level folders named in $Skip.
function Export-XisoFiles([IO.Stream]$Stream, $Entries, [string]$Destination, [string[]]$Skip = @()) {
    $selected = @($Entries | Where-Object {
        $top = ($_.Path -split '/')[0]
        -not ($Skip | Where-Object { [string]::Equals($_, $top, [StringComparison]::OrdinalIgnoreCase) })
    })
    $total = ($selected | Where-Object { -not $_.Directory } | Measure-Object -Property Size -Sum).Sum
    $done = [long]0
    $reported = -1
    $buffer = New-Object byte[] (4MB)
    foreach ($entry in $selected) {
        $target = Join-Path $Destination ($entry.Path -replace '/', '\')
        if ($entry.Directory) {
            [void](New-Item -ItemType Directory -Force -Path $target)
            continue
        }
        [void](New-Item -ItemType Directory -Force -Path (Split-Path $target))
        $out = [IO.File]::Open($target, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $Stream.Position = $entry.Offset
            $remaining = $entry.Size
            while ($remaining -gt 0) {
                $n = $Stream.Read($buffer, 0, [int][Math]::Min($remaining, $buffer.Length))
                if ($n -le 0) { throw "The disc image ends inside $($entry.Path)" }
                $out.Write($buffer, 0, $n)
                $remaining -= $n
                $done += $n
            }
        } finally { $out.Dispose() }
        $percent = if ($total) { [int](100 * $done / $total) } else { 100 }
        if ($percent -ge $reported + 5) {
            $reported = $percent - ($percent % 5)
            Write-Host ("  {0,3}%  {1:N1} of {2:N1} GB" -f $percent, ($done / 1GB), ($total / 1GB))
        }
    }
    return [pscustomobject]@{ Files = @($selected | Where-Object { -not $_.Directory }).Count; Bytes = $done }
}
