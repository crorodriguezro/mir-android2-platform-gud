#!/usr/bin/env python3
"""Generate the deterministic E4-T02 RGB565 qualification raster.

The raw RGB565 file is the source of truth.  The PNG is decoded back from that
file (rather than being generated from a separate RGB image), which makes the
visual reference exercise the same byte order and 5/6/5 quantisation as the
electronic probes.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
import zlib
from pathlib import Path


WIDTH = 1280
HEIGHT = 720
BYTES_PER_PIXEL = 2
PITCH = WIDTH * BYTES_PER_PIXEL

# Canonical RGB888 colours.  Corner markers intentionally differ from the
# four one-pixel edge markers so every corner remains identifiable.
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
CYAN = (0, 255, 255)
MAGENTA = (255, 0, 255)
YELLOW = (255, 255, 0)
ORANGE = (255, 128, 0)
DARK_GRAY = (32, 32, 32)
LIGHT_GRAY = (192, 192, 192)


def rgb565(color: tuple[int, int, int]) -> int:
    r, g, b = color
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand_rgb565(value: int) -> tuple[int, int, int]:
    r = (value >> 11) & 0x1F
    g = (value >> 5) & 0x3F
    b = value & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def pattern_color(x: int, y: int, width: int, height: int) -> tuple[int, int, int]:
    """Return one intentionally asymmetric RGB888 source pixel."""
    corner = 8
    if x < corner and y < corner:
        return YELLOW
    if x >= width - corner and y < corner:
        return CYAN
    if x < corner and y >= height - corner:
        return MAGENTA
    if x >= width - corner and y >= height - corner:
        return ORANGE

    # One-pixel edge markers are kept distinct from all interior content.
    if y == 0:
        return GREEN
    if y == height - 1:
        return WHITE
    if x == 0:
        return RED
    if x == width - 1:
        return BLUE

    # Seven canonical bars make channel swaps immediately visible.
    if 16 <= y < 112:
        bars = (RED, GREEN, BLUE, WHITE, BLACK, CYAN, MAGENTA)
        return bars[min(len(bars) - 1, (x * len(bars)) // width)]

    # Asymmetric red L in the upper-left and blue marker in the lower-right.
    if (24 <= x < 42 and 136 <= y < 270) or (24 <= y < 154 and 24 <= x < 170):
        return RED
    if (width - 170 <= x < width - 24 and height - 154 <= y < height - 136) or (
        width - 42 <= x < width - 24 and height - 270 <= y < height - 24
    ):
        return BLUE

    # Coordinate grid: major boundaries and row bands expose pitch and row
    # order, while the non-symmetric colour formula exposes shifted columns.
    if x % 128 < 3 or y % 64 < 3:
        return LIGHT_GRAY
    if 288 <= y < height - 64:
        red = (x * 251 + y * 17) & 0xFF
        green = (y * 251 + x * 7) & 0xFF
        blue = ((x // 17) * 29 + (y // 11) * 13) & 0xFF
        return (red, green, blue)

    # Bottom ramps, leaving the bottom edge marker untouched.
    if height - 64 <= y < height - 1:
        ramp = ((x * 255) // max(1, width - 1))
        if y < height - 48:
            return (ramp, ramp, ramp)
        if y < height - 32:
            return (ramp, 0, 0)
        if y < height - 16:
            return (0, ramp, 0)
        return (0, 0, ramp)

    return DARK_GRAY


def generate_rgb565(width: int = WIDTH, height: int = HEIGHT) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    output = bytearray(width * height * BYTES_PER_PIXEL)
    for y in range(height):
        row = y * width * BYTES_PER_PIXEL
        for x in range(width):
            output[row + 2 * x : row + 2 * x + 2] = rgb565(pattern_color(x, y, width, height)).to_bytes(2, "little")
    return bytes(output)


def probes(width: int = WIDTH, height: int = HEIGHT) -> list[tuple[int, int]]:
    points = [(0, 0), (1, 0), (width - 1, 0), (0, 1), (0, height - 1),
              (width - 1, height - 1), (width // 2, height // 2)]
    points += [(x, 120) for x in (127, 128, 255, 256, 639, 640, width - 2)]
    points += [(640, y) for y in (1, 15, 16, 255, 256, 359, 360, height - 2)]
    return list(dict.fromkeys((x, y) for x, y in points if 0 <= x < width and 0 <= y < height))


def write_png(path: Path, raw: bytes, width: int, height: int) -> None:
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        row = raw[y * width * 2 : (y + 1) * width * 2]
        for x in range(width):
            rows.extend(expand_rgb565(int.from_bytes(row[x * 2 : x * 2 + 2], "little")))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def write_probe_csv(path: Path, raw: bytes, width: int, height: int) -> None:
    with path.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["x", "y", "offset", "expected_rgb888", "rgb565", "byte0", "byte1"])
        for x, y in probes(width, height):
            offset = y * width * 2 + x * 2
            value = int.from_bytes(raw[offset : offset + 2], "little")
            writer.writerow([x, y, offset, "#%02x%02x%02x" % pattern_color(x, y, width, height),
                             f"0x{value:04x}", f"0x{raw[offset]:02x}", f"0x{raw[offset + 1]:02x}"])


def generate(output: Path, width: int, height: int) -> None:
    output.mkdir(parents=True, exist_ok=True)
    raw = generate_rgb565(width, height)
    raw_path = output / "reference-pattern.rgb565"
    raw_path.write_bytes(raw)
    write_png(output / "reference-pattern.png", raw, width, height)
    write_probe_csv(output / "probes.csv", raw, width, height)
    sha = hashlib.sha256(raw).hexdigest()
    (output / "reference-sha256.txt").write_text(f"{sha}  reference-pattern.rgb565\n")
    (output / "metadata.txt").write_text(
        f"width={width}\nheight={height}\npitch={width * 2}\nbytes_per_pixel=2\n"
        f"size={len(raw)}\nsha256={sha}\nrow_order=top-down\nbyte_order=little-endian\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=WIDTH)
    parser.add_argument("--height", type=int, default=HEIGHT)
    args = parser.parse_args()
    if args.width != WIDTH or args.height != HEIGHT:
        raise SystemExit("T02 reference dimensions are fixed at 1280x720")
    generate(args.output, args.width, args.height)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
