#!/usr/bin/env python3
"""Draws the Fire TV remote button icons that our remote patches (../patches/<game>.patch) show in
place of the N64 joypad icons: Land Grab's control legend (12x12, like its own icons) and Hydra
Harmonics' controls panel (16x16).

Our own pixel art: a round button, light rim on top, dark rim below, white glyph with a drop shadow.
Near-black, like the remote's own buttons. Colours are multiples of 8, so RGBA16 (5551) keeps them
exactly. Standard library only.

  python3 n64/jam24/assets-remote/make-icons.py      # rewrites <game>/remote_*.rgba16.png

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: MIT
"""
import os
import struct
import zlib

SIZE = 12
RIM_LIGHT = (0xA0, 0xA0, 0xB0)
BODY = (0x38, 0x38, 0x48)
BODY_LOW = (0x28, 0x28, 0x38)
RIM_DARK = (0x10, 0x10, 0x18)
GLYPH = (0xF8, 0xF8, 0xF8)
SHADOW = (0x10, 0x10, 0x18)

# The button: 11x11, centred on (5, 5). One string per row, '#' = inside.
BUTTON = [
    "...#####...",
    "..#######..",
    ".#########.",
    "###########",
    "###########",
    "###########",
    "###########",
    "###########",
    ".#########.",
    "..#######..",
    "...#####...",
]

# Glyphs, drawn over the button at (0, 0). '#' = white, 'o' = button colour (no glyph, no shadow).
GLYPHS = {
    # The select button in the middle of the remote's ring.
    "ok": [
        "...........",
        "...........",
        "...........",
        "..###.#.#..",
        "..#.#.##...",
        "..#.#.#....",
        "..#.#.##...",
        "..###.#.#..",
    ],
    # The remote's direction ring (the D-pad): four arrowheads around the middle.
    "ring": [
        "...........",
        ".....#.....",
        "....###....",
        "...........",
        "..#.....#..",
        ".##.....##.",
        "..#.....#..",
        "...........",
        "....###....",
        ".....#.....",
    ],
    "menu": [
        "...........",
        "...........",
        "...........",
        "...#####...",
        "...........",
        "...#####...",
        "...........",
        "...#####...",
    ],
    "playpause": [
        "...........",
        "...........",
        "...........",
        "..#...#.#..",
        "..##..#.#..",
        "..###.#.#..",
        "..##..#.#..",
        "..#...#.#..",
    ],
    "rewind": [
        "...........",
        "...........",
        "...........",
        "....#..#...",
        "...##.##...",
        "..#####....",
        "...##.##...",
        "....#..#...",
    ],
    "ff": [
        "...........",
        "...........",
        "...........",
        "..#..#.....",
        "..##.##....",
        "...#####...",
        "..##.##....",
        "..#..#.....",
    ],
}


# Hydra Harmonics: 16x16, a 15 px button centred on (7, 7).
BUTTON16 = [
    ".....#####.....",
    "...#########...",
    "..###########..",
    ".#############.",
    ".#############.",
    "###############",
    "###############",
    "###############",
    "###############",
    "###############",
    ".#############.",
    ".#############.",
    "..###########..",
    "...#########...",
    ".....#####.....",
]
GLYPHS16 = {
    # Up and down on the remote's direction ring.
    "up": [
        "...............",
        "...............",
        "...............",
        ".......#.......",
        "......###......",
        ".....#####.....",
        "....#######....",
        "......###......",
        "......###......",
        "......###......",
        "......###......",
    ],
    "down": [
        "...............",
        "...............",
        "...............",
        "......###......",
        "......###......",
        "......###......",
        "......###......",
        "....#######....",
        ".....#####.....",
        "......###......",
        ".......#.......",
    ],
    "ok": [
        "...............",
        "...............",
        "...............",
        "...............",
        "...##...#..#...",
        "..#..#..#.#....",
        "..#..#..##.....",
        "..#..#..#.#....",
        "...##...#..#...",
    ],
}


def inside(rows, x, y):
    return 0 <= y < len(rows) and 0 <= x < len(rows[y]) and rows[y][x] == "#"


def draw(g, button=BUTTON, size=SIZE):
    mid = len(button) // 2
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if not inside(button, x, y):
                continue
            edge = any(not inside(button, x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if edge:
                c = RIM_LIGHT if y <= mid else RIM_DARK
            else:
                c = BODY if y <= mid else BODY_LOW
            px[y][x] = c + (255,)
    for y in range(size):
        for x in range(size):
            if inside(g, x, y):
                px[y][x] = GLYPH + (255,)
            elif inside(g, x - 1, y - 1) and inside(button, x, y):
                px[y][x] = SHADOW + (255,)
    return px


def write_png(path, px):
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", len(px[0]), len(px), 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for game, glyphs, button, size in (("landgrab", GLYPHS, BUTTON, SIZE),
                                       ("hydraharmonics", GLYPHS16, BUTTON16, 16)):
        out = os.path.join(here, game)
        os.makedirs(out, exist_ok=True)
        for name, g in glyphs.items():
            path = os.path.join(out, "remote_%s.rgba16.png" % name)
            write_png(path, draw(g, button, size))
            print(os.path.relpath(path))


if __name__ == "__main__":
    main()
