# Launcher header art

`header-art.svg` is the source of truth. Edit it first.

`header-art.png` is the runtime asset, generated from the SVG at 3840x432 RGBA. Do not edit it by hand; regenerate it
after every SVG change.

From the repository root on Windows, with Edge installed:

```powershell
$svg = (Resolve-Path assets/launcher/header-art.svg).Path -replace '\\', '/'
$png = Join-Path (Get-Location).Path 'assets\launcher\header-art.png'
& 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' `
  --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
  --default-background-color=00000000 "--user-data-dir=$env:TEMP\eck-header-art" `
  --window-size=3840,432 "--screenshot=$png" "file:///$svg" | Out-Null
```

## Palette

The colors live in the `<style>` block at the top of the SVG, so a change there applies everywhere.

| Class | Color | Matches |
| --- | --- | --- |
| `.yellow` | `#F2C318` | action yellow (`kAccent`) |
| `.red` | `#D9481F` | action outline (`kActionBorder`) |
| `.coral` | `#FF704A` | link color (`kActionLink`) |
| `.leaf` | `#1F6638` | `kLeaf` |
| `.gold`, `.orange`, `.stop-deep-gold`, `.stop-gold` | warm yellows | sky and details |
| `.green`, `.far-hill`, `.far-tree` | greens | vegetation |
| `.stop-sky`, `.stop-sky-pale` | light blues | sky, right side only |

## Layout

The launcher anchors the art to the top, right and bottom of the header and crops any excess from the left, so the
title space is part of the art. Keep the left 55% empty.

The `wash` gradient fades to transparent at the left edge: windows wider than the art show the launcher gradient
there. The launcher also darkens the title side and fades the bottom 35% of the header, so keep details above y=130.

# Sponsor heart

`sponsor-heart.svg` is the source of truth for the heart in the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is the runtime asset, a white 64x64 rendering of it
with a transparent background. Do not edit it by hand; regenerate it after every SVG change, from the repository root
with rsvg-convert:

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```
