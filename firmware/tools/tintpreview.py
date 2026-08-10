#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franco Raso
"""Render the base map exactly as the device would, from real CARTO tiles.

Why this exists
---------------
The desktop harness draws a SYNTHETIC ground, because the point of the harness
is layout and typography and it must run with no network. That is fine until
the question is "is the map too bright", at which point the synthetic ground is
the one thing you cannot ask -- its base luminance is chosen, not measured.

This closes that gap without touching the panel: it fetches the same tiles from
the same host at the same zoom the firmware would pick, then runs the identical
tint + coverage-lens arithmetic from maptiles.cpp. What comes out is what the
device will put in its framebuffer, pixel for pixel.

It still is NOT the panel. The IPS renders these numbers differently from a
Mac display, and that difference is exactly what the original x1.6 calibration
was measuring. Use this to compare two values honestly; use the panel to pick
between them.

    ./tintpreview.py --lat 46.5 --lon -81.0 --range 250 --tint 16 11
    ./tintpreview.py --lat 46.5 --lon -81.0 --stats

Requires Pillow. Tiles are cached in .tilecache/ so repeat runs are offline.
"""
import argparse
import math
import os
import statistics
import sys
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  pip3 install pillow")

# ---- constants, mirrored from config.h and maptiles.cpp ----------------------
TILE_HOST = "basemaps.cartocdn.com"
TILE_STYLE = "dark_nolabels"          # AR_TILE_STYLE
TILE_PX = 256
GRID_W, GRID_H = 5, 3                 # the firmware's mosaic
MAP_W, MAP_H = 800, 480
SCOPE_R = 212
MAP_SIZE = SCOPE_R * 2
MAP_DIM_PCT = 30
MAP_DIM_FEATHER = 10
TINT_R_PCT, TINT_R_ADD = 32, 7
TINT_G_PCT, TINT_G_ADD = 62, 14
TINT_B_PCT, TINT_B_ADD = 105, 26
VIGNETTE_STRENGTH = 0.55
TINT_LUM_DEN = 10
MERC_MPP_Z0 = 156543.03
ZOOM_MIN, ZOOM_MAX = 3, 12

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".tilecache")


def deg2num(lat, lon, z):
    n = 2 ** z
    r = math.radians(lat)
    return ((lon + 180.0) / 360.0 * n,
            (1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * n)


def choose_zoom(lat, range_km):
    """Byte-for-byte the firmware's chooseZoom(): the zoom whose metres-per-pixel
    is CLOSEST to one output pixel, not the largest that fits. Guessing this
    differently is how you end up previewing a resample the device never does --
    the step ratio is what aliases the road network."""
    target = (2.0 * range_km * 1000.0) / MAP_SIZE
    cos_lat = math.cos(math.radians(lat))
    best, best_err = ZOOM_MIN, 1e30
    for z in range(ZOOM_MIN, ZOOM_MAX + 1):
        mpp = MERC_MPP_Z0 * cos_lat / (1 << z)
        if abs(mpp - target) < best_err:
            best, best_err = z, abs(mpp - target)
    return best


def fetch(z, x, y):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, f"{TILE_STYLE}_{z}_{x}_{y}.png")
    if not os.path.exists(path):
        url = f"https://{TILE_HOST}/{TILE_STYLE}/{z}/{x}/{y}.png"
        req = urllib.request.Request(url, headers={"User-Agent": "AirRadar-dev/1.0"})
        with urllib.request.urlopen(req, timeout=20) as fh:
            open(path, "wb").write(fh.read())
    return Image.open(path).convert("RGB")


def build_mosaic(lat, lon, z):
    cx, cy = deg2num(lat, lon, z)
    x0, y0 = int(cx) - GRID_W // 2, int(cy) - GRID_H // 2
    mos = Image.new("RGB", (TILE_PX * GRID_W, TILE_PX * GRID_H))
    for dy in range(GRID_H):
        for dx in range(GRID_W):
            mos.paste(fetch(z, x0 + dx, y0 + dy), (dx * TILE_PX, dy * TILE_PX))
    return mos, (cx - x0) * TILE_PX, (cy - y0) * TILE_PX


def lens(d):
    r_in, r_out = SCOPE_R - MAP_DIM_FEATHER, SCOPE_R + MAP_DIM_FEATHER
    if d <= r_in:
        return 1.0 - VIGNETTE_STRENGTH * (d * d) / float(SCOPE_R * SCOPE_R)
    if d >= r_out:
        return MAP_DIM_PCT / 100.0
    t = (d - r_in) / (r_out - r_in)
    inner = 1.0 - VIGNETTE_STRENGTH
    return inner + (MAP_DIM_PCT / 100.0 - inner) * t


def render(mos, cx_px, cy_px, step, lum_num):
    src = mos.load()
    mw, mh = mos.size
    out = Image.new("RGB", (MAP_W, MAP_H))
    dst = out.load()
    ocx, ocy = MAP_W // 2, MAP_H // 2
    for oy in range(MAP_H):
        my = cy_px + (oy + 0.5 - ocy) * step
        syi = min(max(int(my), 0), mh - 1)
        dy = oy - ocy
        for ox in range(MAP_W):
            mx = cx_px + (ox + 0.5 - ocx) * step
            sxi = min(max(int(mx), 0), mw - 1)
            r8, g8, b8 = src[sxi, syi]
            lum = min(((r8 * 77 + g8 * 150 + b8 * 29) >> 8) * lum_num // TINT_LUM_DEN, 255)
            r = lum * TINT_R_PCT // 100 + TINT_R_ADD
            g = lum * TINT_G_PCT // 100 + TINT_G_ADD
            b = min(lum * TINT_B_PCT // 100 + TINT_B_ADD, 255)
            k = lens(math.hypot(ox - ocx, dy))
            dst[ox, oy] = (int(r * k), int(g * k), int(b * k))
    return out


def disc_stats(img):
    px = img.convert("RGB").load()
    ocx, ocy = MAP_W // 2, MAP_H // 2
    v = sorted(sum(px[x, y]) / 3.0
               for x in range(ocx - 150, ocx + 150, 3)
               for y in range(ocy - 150, ocy + 150, 3))
    return statistics.mean(v), v[len(v) // 2], v[-1]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float, required=True)
    ap.add_argument("--lon", type=float, required=True)
    ap.add_argument("--range", type=int, default=250, help="scope range in km")
    ap.add_argument("--tint", type=int, nargs="+", default=[11],
                    help="one or more TINT_LUM_NUM values to render")
    ap.add_argument("--stats", action="store_true",
                    help="also report the raw tile luminance distribution")
    ap.add_argument("--out", default="tint",
                    help="output prefix; writes <prefix><N>.png per value")
    a = ap.parse_args()

    z = choose_zoom(a.lat, a.range)
    mpp = MERC_MPP_Z0 * math.cos(math.radians(a.lat)) / (1 << z)
    step = (a.range * 1000.0 / mpp) * 2.0 / MAP_SIZE
    print(f"zoom {z}  {mpp:.1f} m/px  step {step:.3f} mosaic px per output px")

    mos, cx_px, cy_px = build_mosaic(a.lat, a.lon, z)

    if a.stats:
        px = mos.load()
        v = sorted((px[x, y][0] * 77 + px[x, y][1] * 150 + px[x, y][2] * 29) >> 8
                   for x in range(0, mos.size[0], 3)
                   for y in range(0, mos.size[1], 3))
        q = lambda p: v[int(len(v) * p)]
        print(f"raw tile luma: mean {statistics.mean(v):.1f} median {q(.5)} "
              f"p25 {q(.25)} p75 {q(.75)} max {v[-1]}")
        print("  (desktop/fakemap.cpp is calibrated against these -- re-measure "
              "if AR_TILE_STYLE changes)")

    for n in a.tint:
        img = render(mos, cx_px, cy_px, step, n)
        path = f"{a.out}{n}.png"
        img.save(path)
        mean, med, mx = disc_stats(img)
        print(f"tint {n:>3}: disc mean {mean:5.1f}  median {med:5.1f}  max {mx:5.1f}"
              f"   -> {path}")


if __name__ == "__main__":
    main()
