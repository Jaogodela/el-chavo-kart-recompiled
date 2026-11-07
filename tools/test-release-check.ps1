<#
.SYNOPSIS
  Checks that check-release-folder.ps1 accepts a fresh extraction and refuses player data, logs, game files,
  debug markers and developer paths. Scratch folders only; nothing is built, launched or published.
#>
$ErrorActionPreference = 'Stop'
$ReleaseCheck = Join-Path $PSScriptRoot 'check-release-folder.ps1'
$Root = Split-Path $PSScriptRoot -Parent
$StageRoot = Join-Path ([IO.Path]::GetTempPath()) 'eck_release_check'

# Each case adds one item to a clean extraction: a relative path, or path -> content.
$Cases = [ordered]@{
    'fresh extraction'   = $null
    'empty user_data'    = @{ Path = 'user_data\' }
    'settings'           = @{ Path = 'user_data\settings.toml' }
    'title save folder'  = @{ Path = '475807D7\content' }
    'cache'              = @{ Path = 'cache\shaders\a.bin' }
    'migration leftover' = @{ Path = 'user_data.migrating\' }
    'log'                = @{ Path = 'logs\el_chavo_kart_001.log' }
    'game folder'        = @{ Path = 'game\default.xex' }
    'game file mixed'    = @{ Path = 'game\Default.xex' }
    'game file upper'    = @{ Path = 'DEFAULT.XEX' }
    'game folder note'   = @{ Path = 'game-folder.txt' }
    'game install temp'  = @{ Path = 'game-install-temp\' }
    'game install backup' = @{ Path = 'game-install-backup\' }
    'title update'       = @{ Path = 'game\default.xexp' }
    'any xexp'           = @{ Path = 'other.xexp' }
    'disc image'         = @{ Path = 'El Chavo Kart.iso' }
    'cooked package'     = @{ Path = 'Startup.xxx' }
    'engine package'     = @{ Path = 'Engine.upk' }
    'texture cache'      = @{ Path = 'Textures.tfc' }
    'audio cache'        = @{ Path = 'Sounds.fsbcache' }
    'movie'              = @{ Path = 'Intro.bik' }
    'pdb'                = @{ Path = 'el_chavo_kart.pdb' }
    'updater pdb'        = @{ Path = 'trg-updater.pdb' }
    'debug file'         = @{ Path = 'trg-updater.debug' }
    'renderdoc'          = @{ Path = 'renderdoc.dll' }
    'vulkan layer'       = @{ Path = 'VkLayer_test.json' }
    'unexpected file'    = @{ Path = 'notes.txt' }
    'nested xex'         = @{ Path = 'licenses\Default.Xex' }
    'any xex'            = @{ Path = 'other.xex' }
    'crash dump'         = @{ Path = 'crash.dmp' }
    'backup'             = @{ Path = 'el_chavo_kart.toml.bak' }
    'temp file'          = @{ Path = 'x.tmp' }
    'save file'          = @{ Path = 'slot.sav' }
    'user profile data'  = @{ Path = 'achievements\475807D7.toml' }
    'online instances'   = @{ Path = 'online-instances\a\settings.toml' }
    'missing license'    = @{ Path = 'licenses\ReXGlue-LICENSE.txt'; Delete = $true }
    'missing readme'     = @{ Path = 'README.txt'; Delete = $true }
    'missing updater'    = @{ Path = 'trg-updater.exe'; Delete = $true }
    'missing header art' = @{ Path = 'assets\launcher\header-art.png'; Delete = $true }
    'updater workspace'  = @{ Path = 'trg-update-1234\job.txt' }
    'updater log'        = @{ Path = 'trg-updater.log' }
    'extra asset'        = @{ Path = 'assets\launcher\other.png' }
    'extra asset folder' = @{ Path = 'assets\game\a.bin' }
    'user profile path'  = @{ Path = 'rexruntime.dll'; Bytes = [Text.Encoding]::ASCII.GetBytes('x ' + 'C:' + '\Users\someone\source\a.cpp') }
    'user path 16'       = @{ Path = 'rexgpu-xenos.dll'; Bytes = [Text.Encoding]::Unicode.GetBytes('x D:\Users\someone\a.cpp') }
    'repos path'         = @{ Path = 'el_chavo_kart.exe'; Bytes = [Text.Encoding]::ASCII.GetBytes('x:/VisualStudio/repos/y/a.cpp') }
    'xex magic'          = @{ Path = 'rexruntime.dll'; Bytes = [Text.Encoding]::ASCII.GetBytes('XEX2....') }
    'package magic'      = @{ Path = 'rexruntime.dll'; Bytes = [Text.Encoding]::ASCII.GetBytes('LIVE....') }
    'debug marker'       = @{ Path = 'el_chavo_kart.toml'; Bytes = [Text.Encoding]::ASCII.GetBytes('DEBUG_SAULO: x') }
    'admin token name'   = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('ADMIN_TOKEN=x') }
    'cloudflare token'   = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('CLOUDFLARE_API_TOKEN') }
    'github token'       = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('ghp_' + ('a' * 36)) }
    'github fine token'  = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('github_pat_' + ('b' * 40)) }
    'private key'        = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('-----BEGIN RSA PRIVATE KEY-----') }
    'identity token'     = @{ Path = 'el_chavo_kart.toml'; Bytes = [Text.Encoding]::ASCII.GetBytes('"token":"' + ('a' * 64) + '"') }
    'friend code'        = @{ Path = 'README.txt'; Bytes = [Text.Encoding]::ASCII.GetBytes('player_id=ABCD-2345') }
    'identity file'      = @{ Path = 'eck_online_identity.ini' }
    'repository path'    = @{ Path = 'rexruntime.dll'; Bytes = [Text.Encoding]::ASCII.GetBytes("path $Root x") }
    'repository path 16' = @{ Path = 'rexgpu-xenos.dll'; Bytes = [Text.Encoding]::Unicode.GetBytes("path $Root x") }
    'analysis folder'    = @{ Path = 'analysis\default.image.bin' }
    'build output'       = @{ Path = 'out\codegen\a.bin' }
}
$failures = 0
try {
    New-Item -ItemType Directory -Force $StageRoot | Out-Null
    foreach ($case in $Cases.Keys) {
        $stage = Join-Path $StageRoot ($case -replace '\W', '_')
        New-Item -ItemType Directory -Force $stage | Out-Null
        foreach ($leaf in 'el_chavo_kart.exe', 'trg-updater.exe', 'rexruntime.dll', 'rexgpu-xenos.dll', 'el_chavo_kart.toml', 'README.txt', 'LICENSE') {
            Set-Content -LiteralPath (Join-Path $stage $leaf) -Value 'placeholder'
        }
        New-Item -ItemType Directory -Force (Join-Path $stage 'assets\launcher') | Out-Null
        Set-Content -LiteralPath (Join-Path $stage 'assets\launcher\header-art.png') -Value 'placeholder'
        New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
        foreach ($leaf in 'ReXGlue-LICENSE.txt', 'THIRD-PARTY-LICENSES.txt') {
            Set-Content -LiteralPath (Join-Path $stage "licenses\$leaf") -Value 'placeholder'
        }
        $extra = $Cases[$case]
        if ($extra) {
            $target = Join-Path $stage $extra.Path
            if ($extra.Delete) {
                Remove-Item -LiteralPath $target
            } elseif ($extra.Path.EndsWith('\')) {
                New-Item -ItemType Directory -Force $target | Out-Null
            } else {
                New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
                if ($extra.Bytes) { [IO.File]::WriteAllBytes($target, $extra.Bytes) } else { Set-Content -LiteralPath $target -Value 'placeholder' }
            }
        }
        $global:LASTEXITCODE = 0
        & $ReleaseCheck -Folder $stage *> $null
        $accepted = $LASTEXITCODE -eq 0
        if ($accepted -ne ($null -eq $extra)) {
            Write-Host "  FAIL $case"
            $failures++
        }
    }
} finally {
    Remove-Item -LiteralPath $StageRoot -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "$($Cases.Count) release check cases, $failures failures"
if ($failures) { throw 'Release check tests failed' }
$global:LASTEXITCODE = 0
