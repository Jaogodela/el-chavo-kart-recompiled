#!/usr/bin/env python3
"""Writes packaging/icons/app-icon.svg in the launcher's action colours and renders it into app-icon-<size>.png and
app-icon.ico in the same folder.

Developer tool: the generated files are committed, so building the port needs no SVG converter.
Needs Microsoft Edge or Chrome for the rasterization; the rest is the Python standard library.

  python -I tools/make-app-icon.py
  python -I tools/make-app-icon.py --browser /path/to/chrome
"""
import argparse
import base64
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS = ROOT / "packaging" / "icons"
PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
BROWSERS = (
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "microsoft-edge", "google-chrome", "chromium", "chromium-browser",
)

BODY_PATH = (
    "M310.933,342C312.734,342 314.302,343.257 314.733,345.044C315.385,347.753 316.341,351.726 "
    "317.044,354.648C317.555,356.77 316.305,358.919 314.237,359.473C313.369,359.705 312.459,359.949 "
    "311.635,360.17C310.039,360.598 308.403,359.633 307.967,358.006C307.967,358.005 307.967,358.004 "
    "307.967,358.003C307.65,356.82 306.598,356 305.398,356C304.675,356 303.861,356 303.138,356C301.938,356 "
    "300.886,356.82 300.569,358.003C300.569,358.004 300.569,358.005 300.569,358.006C300.133,359.633 298.497,360.598"
    " 296.901,360.17C296.04,359.939 295.085,359.684 294.183,359.442C292.161,358.9 290.913,356.83 "
    "291.348,354.74C291.949,351.853 292.772,347.901 293.341,345.168C293.725,343.321 295.321,342 "
    "297.168,342C300.942,342 307.204,342 310.933,342Z"
)
DETAIL_PATH = (
    "M310.812,342C307.001,342 300.601,342 296.743,342C294.383,342 292.344,343.651 291.853,345.96C291.271,348.693 "
    "290.43,352.645 289.816,355.532C289.261,358.144 290.856,360.732 293.439,361.409L296.216,362.137C298.378,362.704"
    " 300.593,361.425 301.183,359.27L301.184,359.267C301.389,358.519 302.069,358 302.845,358C302.845,358 "
    "305.155,358 305.155,358C305.931,358 306.611,358.519 306.816,359.267L306.817,359.27C307.407,361.425 "
    "309.622,362.704 311.784,362.137L314.443,361.44C317.085,360.748 318.682,358.062 318.03,355.409C317.311,352.487 "
    "316.334,348.514 315.667,345.806C315.118,343.571 313.113,342 310.812,342L310.812,342ZM310.812,344C312.193,344 "
    "313.395,344.943 313.725,346.283C313.725,346.283 316.088,355.887 316.088,355.887C316.479,357.479 315.521,359.09"
    " 313.935,359.506C313.935,359.506 311.277,360.203 311.277,360.203C310.175,360.491 309.046,359.84 "
    "308.746,358.742C308.746,358.742 308.745,358.739 308.745,358.739C308.302,357.121 306.832,356 "
    "305.155,356L302.845,356C301.168,356 299.698,357.121 299.255,358.739C299.255,358.739 299.254,358.742 "
    "299.254,358.742C298.954,359.84 297.825,360.491 296.723,360.203C296.723,360.203 293.946,359.475 "
    "293.946,359.475C292.396,359.068 291.439,357.516 291.772,355.948C292.387,353.062 293.228,349.109 "
    "293.809,346.376C294.104,344.99 295.327,344 296.743,344L310.812,344ZM297,349L296,349C295.735,349 295.48,349.105"
    " 295.293,349.293C295.105,349.48 295,349.735 295,350C295,350.265 295.105,350.52 295.293,350.707C295.48,350.895 "
    "295.735,351 296,351L297,351L297,352C297,352.265 297.105,352.52 297.293,352.707C297.48,352.895 297.735,353 "
    "298,353C298.265,353 298.52,352.895 298.707,352.707C298.895,352.52 299,352.265 "
    "299,352L299,351L300,351C300.265,351 300.52,350.895 300.707,350.707C300.895,350.52 301,350.265 "
    "301,350C301,349.735 300.895,349.48 300.707,349.293C300.52,349.105 300.265,349 "
    "300,349L299,349L299,348C299,347.735 298.895,347.48 298.707,347.293C298.52,347.105 298.265,347 "
    "298,347C297.735,347 297.48,347.105 297.293,347.293C297.105,347.48 297,347.735 "
    "297,348L297,349ZM310,351C310.552,351 311,351.448 311,352C311,352.552 310.552,353 310,353C309.448,353 "
    "309,352.552 309,352C309,351.448 309.448,351 310,351ZM308,349C308.552,349 309,349.448 309,350C309,350.552 "
    "308.552,351 308,351C307.448,351 307,350.552 307,350C307,349.448 307.448,349 308,349ZM312,349C312.552,349 "
    "313,349.448 313,350C313,350.552 312.552,351 312,351C311.448,351 311,350.552 311,350C311,349.448 311.448,349 "
    "312,349ZM310,347C310.552,347 311,347.448 311,348C311,348.552 310.552,349 310,349C309.448,349 309,348.552 "
    "309,348C309,347.448 309.448,347 310,347Z"
)
SVG = """<?xml version="1.0" ?>
<!DOCTYPE svg  PUBLIC '-//W3C//DTD SVG 1.1//EN'  'http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd'>
<!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg fill="#000000" width="800px" height="800px" viewBox="0 0 32 32" style="fill-rule:evenodd;clip-rule:evenodd;stroke-linejoin:round;stroke-miterlimit:2;" version="1.1" xml:space="preserve" xmlns="http://www.w3.org/2000/svg" xmlns:serif="http://www.serif.com/" xmlns:xlink="http://www.w3.org/1999/xlink">
<g transform="matrix(1,0,0,1,-288,-336)">
<g transform="matrix(1.02211,0,0,1,-6.99591,1)">
<path d="{body_path}" style="fill:{body};"/>
</g>
<path d="{detail_path}" style="fill:{detail};"/>
</g>
</svg>
"""


def find_browser(requested):
    for candidate in (requested,) if requested else BROWSERS:
        found = shutil.which(candidate)
        if found:
            return found
    sys.exit("No Edge or Chrome found: pass --browser")


def launcher_colour(name):
    source = (ROOT / "src" / "launcher.cpp").read_text(encoding="utf-8")
    match = re.search(name + r" = Rgb\(0x(\w\w), 0x(\w\w), 0x(\w\w)\)", source)
    if not match:
        sys.exit(f"{name} not found in src/launcher.cpp")
    return "#" + "".join(match.groups()).upper()


def gamepad_svg():
    return SVG.format(body_path=BODY_PATH, body=launcher_colour("kAccent"), detail_path=DETAIL_PATH,
                      detail=launcher_colour("kActionBorder")).encode("utf-8")


def render(browser, svg, size, work):
    page = work / f"{size}.html"
    uri = base64.b64encode(svg).decode()
    page.write_text(
        '<!doctype html><body style="margin:0;background:transparent">'
        f'<img src="data:image/svg+xml;base64,{uri}" width="{size}" height="{size}" style="display:block">',
        encoding="utf-8")
    out = work / f"{size}.png"
    subprocess.run(
        [browser, "--headless", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
         "--default-background-color=00000000", f"--user-data-dir={work / 'profile'}",
         f"--window-size={size},{size}", f"--screenshot={out}", page.as_uri()],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    png = out.read_bytes()
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", png[16:29])
    if (width, height, depth, color, interlace) != (size, size, 8, 6, 0):
        sys.exit(f"{size}px render is not an 8-bit RGBA {size}x{size} PNG")
    return png


def rgba_pixels(png):
    width, height = struct.unpack(">II", png[16:24])
    data, pos = b"", 8
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        if kind == b"IDAT":
            data += png[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw, stride = zlib.decompress(data), width * 4
    rows, prev = [], bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - 4] if x >= 4 else 0
            up = prev[x]
            corner = prev[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + left) & 255
            elif kind == 2:
                line[x] = (line[x] + up) & 255
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(line)
        prev = line
    return rows


def dib_frame(png):
    rows = rgba_pixels(png)
    size = len(rows)
    pixels, mask = b"", b""
    for line in reversed(rows):
        pixels += b"".join(bytes((line[i + 2], line[i + 1], line[i], line[i + 3])) for i in range(0, size * 4, 4))
        bits = "".join("1" if line[i + 3] == 0 else "0" for i in range(0, size * 4, 4)).ljust((size + 31) // 32 * 32, "0")
        mask += int(bits, 2).to_bytes(len(bits) // 8, "big")
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
    return header + pixels + mask


def build_ico(pngs):
    frames = [(size, pngs[size] if size == 256 else dib_frame(pngs[size])) for size in ICO_SIZES]
    out = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    for size, data in frames:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _, data in frames)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--browser", help="Edge or Chrome executable")
    browser = find_browser(parser.parse_args().browser)
    svg = gamepad_svg()
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as work:
        pngs = {size: render(browser, svg, size, pathlib.Path(work)) for size in PNG_SIZES}
    (ICONS / "app-icon.svg").write_bytes(svg)
    for size, png in pngs.items():
        (ICONS / f"app-icon-{size}.png").write_bytes(png)
    (ICONS / "app-icon.ico").write_bytes(build_ico(pngs))
    print(f"Wrote app-icon.svg, {len(pngs)} PNGs and app-icon.ico ({', '.join(map(str, ICO_SIZES))}) to {ICONS}")


if __name__ == "__main__":
    main()
