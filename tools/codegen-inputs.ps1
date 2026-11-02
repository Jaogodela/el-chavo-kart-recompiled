# Guest code freshness for generated\default. tools\codegen.ps1 writes the stamp after a successful run;
# tools\build.ps1 only compares it against the current inputs; it never regenerates guest code itself.

$CodegenManifest = 'el_chavo_kart_manifest.toml'
$CodegenXex = 'game\extracted\default.xex'
$CodegenStamp = 'generated\codegen-inputs.stamp'
$CodegenLog = 'generated\codegen.log'
$CodegenOutput = 'generated\default'

function Get-InstalledRexglueVersion([string]$Root) {
    $file = Join-Path $Root 'tools\rexglue\win-amd64\lib\cmake\rexglue\rexglueConfigVersion.cmake'
    if (-not (Test-Path -LiteralPath $file)) { return '' }
    [regex]::Match([IO.File]::ReadAllText($file), 'set\(PACKAGE_VERSION "([^"]+)"\)').Groups[1].Value
}

# Quoted paths in the includes arrays of a TOML file. ReXGlue resolves them from that file's folder.
function Get-TomlIncludes([string]$Text) {
    $paths = @()
    foreach ($list in [regex]::Matches($Text, '(?m)^\s*includes\s*=\s*\[([^\]]*)\]')) {
        foreach ($item in [regex]::Matches($list.Groups[1].Value, '["'']([^"'']+)["'']')) { $paths += $item.Groups[1].Value }
    }
    return $paths
}

function Get-CodegenInputKey([string]$Root, [string]$Path) {
    $rootPath = Get-NormalizedPath $Root
    if ($Path.StartsWith("$rootPath\", [StringComparison]::OrdinalIgnoreCase)) {
        return $Path.Substring($rootPath.Length + 1).Replace('\', '/')
    }
    return $Path
}

# Adds the includes of a TOML file depth first, each file once.
function Add-CodegenIncludes([string]$Root, [string]$TomlPath, [hashtable]$Seen, [System.Collections.Generic.List[object]]$Items) {
    if (-not (Test-Path -LiteralPath $TomlPath -PathType Leaf)) { return }
    $folder = Split-Path -Parent $TomlPath
    foreach ($include in Get-TomlIncludes ([IO.File]::ReadAllText($TomlPath))) {
        $path = Get-NormalizedPath ([IO.Path]::Combine($folder, $include))
        if ($Seen.ContainsKey($path)) { continue }
        $Seen[$path] = $true
        $Items.Add([pscustomobject]@{ Key = Get-CodegenInputKey $Root $path; Path = $path })
        Add-CodegenIncludes $Root $path $Seen $Items
    }
}

# The files codegen reads, in stamp order: the manifest, the XEX, then the manifest's includes
# (config\overrides.toml, config\discovered-functions.toml).
function Get-CodegenInputItems([string]$Root) {
    $manifest = Get-NormalizedPath (Join-Path $Root $CodegenManifest)
    $xex = Get-NormalizedPath (Join-Path $Root $CodegenXex)
    $items = New-Object System.Collections.Generic.List[object]
    $items.Add([pscustomobject]@{ Key = Get-CodegenInputKey $Root $manifest; Path = $manifest })
    $items.Add([pscustomobject]@{ Key = Get-CodegenInputKey $Root $xex; Path = $xex })
    $seen = @{}
    $seen[$manifest] = $true
    $seen[$xex] = $true
    Add-CodegenIncludes $Root $manifest $seen $items
    return $items
}

# None of patches\rexglue-sdk touch src/codegen, so rexglue.exe is always the stock SDK build; only the runtime
# and GPU plugin are patched (tools\rexruntime-patch.ps1).
function Get-CodegenInputStamp([string]$Root, [string]$SdkVersion) {
    $lines = @("rexglue  $SdkVersion")
    foreach ($item in @(Get-CodegenInputItems $Root)) {
        $hash = if (Test-Path -LiteralPath $item.Path -PathType Leaf) { Get-FileSha256 $item.Path } else { 'missing' }
        $lines += "$hash  $($item.Key)"
    }
    return $lines -join "`n"
}

function Read-CodegenStamp([string]$Text) {
    $values = [ordered]@{}
    foreach ($line in $Text -split "`n") {
        $parts = $line -split '  ', 2
        if ($parts.Count -ne 2) { continue }
        if ($parts[0] -eq 'rexglue') { $values['rexglue'] = $parts[1] } else { $values[$parts[1]] = $parts[0] }
    }
    return $values
}

# Whether generated\default matches its inputs, and which inputs changed when it does not.
function Get-CodegenFreshness([string]$Root, [string]$SdkVersion) {
    $stampFile = Join-Path $Root $CodegenStamp
    if (-not (Test-Path -LiteralPath $stampFile -PathType Leaf)) {
        return [pscustomobject]@{ Current = $false; Reasons = @('no successful codegen record') }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Root "$CodegenOutput\sources.cmake") -PathType Leaf)) {
        return [pscustomobject]@{ Current = $false; Reasons = @("$CodegenOutput\sources.cmake is missing") }
    }
    $current = Get-CodegenInputStamp $Root $SdkVersion
    $stored = [IO.File]::ReadAllText($stampFile)
    if ($stored -ceq $current) { return [pscustomobject]@{ Current = $true; Reasons = @() } }
    $reasons = @(Get-ValueChanges (Read-CodegenStamp $stored) (Read-CodegenStamp $current))
    if (-not $reasons.Count) { $reasons = @('the codegen inputs changed') }
    return [pscustomobject]@{ Current = $false; Reasons = @($reasons | ForEach-Object { $_.Replace('/', '\') }) }
}
