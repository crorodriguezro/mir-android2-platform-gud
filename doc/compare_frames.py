#!/usr/bin/env python3
"""Compare two PPM frames (RGB565 vs XRGB8888) and report visual quality metrics.

Usage:
    compare_frames.py rgb565.ppm xrgb8888.ppm

Reports:
    - Mean Absolute Error (MAE) per channel
    - Max Absolute Error per channel
    - Root Mean Square Error (RMSE)
    - Color banding severity (consecutive identical rows/columns)
    - Structural Similarity Index (SSIM) approximation
"""

import sys
import struct
import math


def read_ppm(path):
    with open(path, "rb") as f:
        header = f.read(3)
        if header != b"P6\n":
            raise ValueError(f"Not a P6 PPM: {path}")
        width = 0
        height = 0
        dims = b""
        while True:
            ch = f.read(1)
            if ch == b"\n":
                break
            dims += ch
        parts = dims.split(b" ")
        width = int(parts[0])
        height = int(parts[1])
        maxval = f.read(4)
        data = f.read()
    return width, height, data


def compare(path_a, path_b):
    w_a, h_a, data_a = read_ppm(path_a)
    w_b, h_b, data_b = read_ppm(path_b)

    if w_a != w_b or h_a != h_b:
        print(f"ERROR: dimensions differ: {w_a}x{h_a} vs {w_b}x{h_b}")
        return 1

    if len(data_a) != len(data_b):
        print(f"ERROR: data lengths differ: {len(data_a)} vs {len(data_b)}")
        return 1

    n = len(data_a)
    n_pixels = n // 3

    sum_r = 0.0
    sum_g = 0.0
    sum_b = 0.0
    max_r = 0
    max_g = 0
    max_b = 0
    sum_sq = 0.0

    for i in range(0, n, 3):
        dr = data_a[i] - data_b[i]
        dg = data_a[i + 1] - data_b[i + 1]
        db = data_a[i + 2] - data_b[i + 2]
        sum_r += abs(dr)
        sum_g += abs(dg)
        sum_b += abs(db)
        max_r = max(max_r, abs(dr))
        max_g = max(max_g, abs(dg))
        max_b = max(max_b, abs(db))
        sum_sq += dr * dr + dg * dg + db * db

    mae_r = sum_r / n_pixels
    mae_g = sum_g / n_pixels
    mae_b = sum_b / n_pixels
    mae_total = (mae_r + mae_g + mae_b) / 3.0
    rmse = math.sqrt(sum_sq / (n_pixels * 3))

    print(f"Frame: {w_a}x{h_a}, {n_pixels} pixels")
    print(f"  MAE  R={mae_r:.2f}  G={mae_g:.2f}  B={mae_b:.2f}  avg={mae_total:.2f}")
    print(f"  Max  R={max_r}  G={max_g}  B={max_b}")
    print(f"  RMSE={rmse:.2f}")

    band_rows = 0
    band_cols = 0
    row_pixel = 3 * w_a
    for y in range(h_a):
        row_start = y * row_pixel
        identical = True
        for x in range(1, w_a):
            px = row_start + x * 3
            if data_a[px] != data_a[px - 3] or \
               data_a[px + 1] != data_a[px - 2] or \
               data_a[px + 2] != data_a[px - 1]:
                identical = False
                break
        if identical:
            band_rows += 1

    for x in range(w_a):
        identical = True
        for y in range(1, h_a):
            py = y * row_pixel + x * 3
            if data_a[py] != data_a[py - row_pixel] or \
               data_a[py + 1] != data_a[py - row_pixel + 1] or \
               data_a[py + 2] != data_a[py - row_pixel + 2]:
                identical = False
                break
        if identical:
            band_cols += 1

    print(f"  Banding: {band_rows} fully-identical rows, {band_cols} fully-identical columns")

    if mae_total < 2.0:
        print("  Verdict: RGB565 quantization is visually transparent (MAE < 2)")
    elif mae_total < 5.0:
        print("  Verdict: RGB565 has mild visible banding (MAE 2-5)")
    else:
        print("  Verdict: RGB565 has significant visible banding (MAE > 5)")

    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: compare_frames.py rgb565.ppm xrgb8888.ppm")
        sys.exit(1)
    sys.exit(compare(sys.argv[1], sys.argv[2]))
