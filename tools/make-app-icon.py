#!/usr/bin/env python3
"""Draws the port's icon (a checkered racing wheel in the launcher colours) into packaging/icons: app-icon-<size>.png
and app-icon.ico. Developer tool: the generated files are committed. Standard library only.

  python -I tools/make-app-icon.py
"""
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS = ROOT / "packaging" / "icons"
PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE = 4

BACKGROUND = (22, 52, 112)
BACKGROUND_EDGE = (12, 28, 66)
TYRE = (24, 24, 30)
ACCENT = (250, 204, 41)  # kAccent in src/launcher.cpp
LIGHT = (246, 246, 240)
DARK = (34, 34, 40)


def shade(u, v):
    """Colour and coverage of one sample, u and v in [-1, 1]."""
    corner = 0.32
    qx, qy = max(abs(u) - (1 - corner), 0.0), max(abs(v) - (1 - corner), 0.0)
    if math.hypot(qx, qy) > corner:
        return None
    t = min(1.0, math.hypot(u, v) / 1.4)
    color = tuple(round(a + (b - a) * t) for a, b in zip(BACKGROUND, BACKGROUND_EDGE))
    r = math.hypot(u, v)
    if r < 0.80:
        color = TYRE
    if r < 0.66:
        color = ACCENT
    if r < 0.56:
        angle = math.atan2(v, u)
        ring = int((r - 0.16) / 0.10)
        sector = int((angle + math.pi) / (math.pi / 6))
        color = LIGHT if (ring + sector) % 2 == 0 else DARK
    if r < 0.18:
        color = ACCENT
    if r < 0.08:
        color = TYRE
    return color


def render(size):
    n = size * SUPERSAMPLE
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    u = ((x * SUPERSAMPLE + sx + 0.5) / n) * 2 - 1
                    v = ((y * SUPERSAMPLE + sy + 0.5) / n) * 2 - 1
                    c = shade(u, v)
                    if c:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        acc[3] += 1
            samples = SUPERSAMPLE * SUPERSAMPLE
            if acc[3]:
                row += bytes((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] * 255 // samples))
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, payload = b"", b""
    for size, data in images:
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(payload))
        payload += data
    return header + entries + payload


def main():
    ICONS.mkdir(parents=True, exist_ok=True)
    encoded = {}
    for size in PNG_SIZES:
        encoded[size] = png(size, render(size))
        (ICONS / f"app-icon-{size}.png").write_bytes(encoded[size])
    (ICONS / "app-icon.ico").write_bytes(ico([(s, encoded[s]) for s in ICO_SIZES]))
    print(f"wrote {len(PNG_SIZES)} PNGs and app-icon.ico to {ICONS}")


if __name__ == "__main__":
    main()
