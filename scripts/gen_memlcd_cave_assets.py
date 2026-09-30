#!/usr/bin/env python3
"""Generates main/display/memlcd/memlcd_assets_cave.c — the bitmaps of the
left screen's cave (plan 2026-09-30-left-screen-cave), all LV_IMG_CF_ALPHA_1BIT
(no palette, MSB = leftmost pixel, stride ceil(w/8); the ink colour comes from
img_recolor):

  memlcd_img_rock_top / _bottom  procedural rock silhouette + fading Bayer
                                 shadow, DETERMINISTIC (fixed tooth tables)
  memlcd_img_dither_wide         the countdown's shadow strip (Bayer 8/16)
  memlcd_img_cadenas             the chest padlock, 24 x 28 (Mae, 2026-09-29)
  memlcd_img_lien                the TRRS link, 16 x 12, one arrow each way

The rock and dither come from the design simulator's gen_rock.py (tools/
memlcd_sim, round 2); the padlock and the link from memlcd_backend.c, drawn
by hand in 2026-09 — kept here as ASCII art so the source IS the picture.
The Niphargus logos (28 and 56 px) come from scripts/gen_logo_memlcd.sh.

Usage: scripts/gen_memlcd_cave_assets.py [--preview]
"""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "main", "display", "memlcd", "memlcd_assets_cave.c")

W = 68

BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]

def bayer_ink(x, y, density):
    """density 0..16: how many of the 16 Bayer cells are inked."""
    return BAYER4[y % 4][x % 4] < density

def teeth_depth(x, bounds, apexes):
    for i in range(len(bounds) - 1):
        a, b = bounds[i], bounds[i + 1]
        if a <= x < b:
            mid = (a + b) / 2.0
            half = (b - a) / 2.0
            d = apexes[i] * (1.0 - abs(x - mid) / half)
            return max(0, round(d))
    return 0

TOP_BOUNDS  = [0, 3, 7, 9, 13, 18, 21, 25, 30, 33, 37, 42, 45, 49, 54, 57, 61, 65, 68]
TOP_APEX    = [8, 5, 7, 4, 8, 6, 3, 8, 5, 7, 4, 8, 6, 8, 5, 3, 7, 8]
BOT_BOUNDS  = [0, 4, 8, 12, 15, 19, 24, 27, 31, 36, 39, 43, 48, 51, 55, 60, 64, 68]
BOT_APEX    = [5, 3, 6, 4, 5, 3, 6, 4, 5, 6, 3, 5, 4, 6, 3, 5, 4]

def make_grid(h):
    return [[0] * W for _ in range(h)]

def render_top(h_rock, h_shadow):
    h = h_rock + h_shadow
    g = make_grid(h)
    for x in range(W):
        d = teeth_depth(x, TOP_BOUNDS, TOP_APEX)
        d = min(d, h_rock)
        for y in range(d):
            g[y][x] = 1
    # fading dither shadow rows right under the rock: density decreases
    # with distance from the rock edge.
    for row in range(h_shadow):
        y = h_rock + row
        density = max(0, 10 - row * 4)  # 10, 6, 2, ... out of 16
        if density <= 0:
            continue
        for x in range(W):
            if bayer_ink(x, y, density):
                g[y][x] = 1
    return g

def render_bottom(h_rock, h_shadow):
    h = h_rock + h_shadow
    g = make_grid(h)
    for row in range(h_shadow):
        y = row
        density = max(0, 10 - (h_shadow - 1 - row) * 4)
        if density <= 0:
            continue
        for x in range(W):
            if bayer_ink(x, y, density):
                g[y][x] = 1
    for x in range(W):
        d = teeth_depth(x, BOT_BOUNDS, BOT_APEX)
        d = min(d, h_rock)
        for y in range(d):
            g[h - 1 - y][x] = 1
    return g

def render_dither_strip(w, h, density):
    g = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if bayer_ink(x, y, density):
                g[y][x] = 1
    return g

def grid_to_bits(g):
    h = len(g); w = len(g[0])
    rowbytes = (w + 7) // 8
    out = bytearray(rowbytes * h)
    for y in range(h):
        for x in range(w):
            if g[y][x]:
                out[y * rowbytes + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out), w, h

def emit(name, g, f):
    bits, w, h = grid_to_bits(g)
    f.write(f"static const uint8_t {name}_map[] = {{\n")
    for i in range(0, len(bits), 16):
        f.write("  " + ",".join(f"0x{b:02x}" for b in bits[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"const lv_img_dsc_t {name} = {{ .header.cf = LV_IMG_CF_ALPHA_1BIT, "
            f".header.always_zero = 0, .header.w = {w}, .header.h = {h}, "
            f".data_size = {len(bits)}, .data = {name}_map }};\n")


CADENAS = """
........########........
......############......
.....#####....#####.....
....####........####....
....###..........###....
....###..........###....
....###..........###....
....###..........###....
....###..........###....
....###..........###....
....###..........###....
....###..........###....
.######################.
########################
########################
########################
##########....##########
#########......#########
#########......#########
##########....##########
###########..###########
###########..###########
###########..###########
###########..###########
########################
########################
########################
.######################.
"""

LIEN = """
...........#....
...........##...
##############..
##############..
...........##...
...........#....
....#...........
...##...........
..##############
..##############
...##...........
....#...........
"""


def art(s):
    return [[1 if c == "#" else 0 for c in row] for row in s.strip().splitlines()]


def ascii_preview(g):
    return "\n".join("".join("#" if c else "." for c in row) for row in g)


if __name__ == "__main__":
    assets = [
        ("memlcd_img_rock_top", render_top(8, 4)),
        ("memlcd_img_rock_bottom", render_bottom(6, 3)),
        ("memlcd_img_dither_wide", render_dither_strip(44, 4, 8)),
        ("memlcd_img_cadenas", art(CADENAS)),
        ("memlcd_img_lien", art(LIEN)),
    ]
    if "--preview" in sys.argv:
        for name, g in assets:
            print(f"=== {name} ===")
            print(ascii_preview(g))
        sys.exit(0)
    with open(OUT, "w") as f:
        f.write("/* GENERATED by scripts/gen_memlcd_cave_assets.py — do not edit by hand.\n")
        f.write(" * The cave's bitmaps: rock edges, the countdown's shadow, the chest padlock,\n")
        f.write(" * the TRRS link. LV_IMG_CF_ALPHA_1BIT, recoloured by the engine. */\n")
        f.write('#include "lvgl.h"\n')
        for name, g in assets:
            emit(name, g, f)
    print(f"wrote {os.path.relpath(OUT, ROOT)}")
