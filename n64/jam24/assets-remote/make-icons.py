#!/usr/bin/env python3
"""Draws the Fire TV remote button icons that our Land Grab remote patch (../patches/landgrab.patch)
shows in the game's control legend, in place of its N64 joypad icons.

Our own pixel art, in the style of the game's 12x12 icons: an 11 px round button, light rim on
top, dark rim below, white glyph with a drop shadow. Near-black, like the remote's own buttons.
Colours are multiples of 8, so RGBA16 (5551) keeps them exactly. Standard library only.

  python3 n64/jam24/assets-remote/make-icons.py      # rewrites landgrab/remote_*.rgba16.png

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


def inside(rows, x, y):
    return 0 <= y < len(rows) and 0 <= x < len(rows[y]) and rows[y][x] == "#"


def draw(glyph):
    px = [[(0, 0, 0, 0)] * SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        for x in range(SIZE):
            if not inside(BUTTON, x, y):
                continue
            edge = any(not inside(BUTTON, x + dx, y + dy) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if edge:
                c = RIM_LIGHT if y <= 5 else RIM_DARK
            else:
                c = BODY if y <= 5 else BODY_LOW
            px[y][x] = c + (255,)
    g = GLYPHS[glyph]
    for y in range(SIZE):
        for x in range(SIZE):
            if inside(g, x, y):
                px[y][x] = GLYPH + (255,)
            elif inside(g, x - 1, y - 1) and inside(BUTTON, x, y):
                px[y][x] = SHADOW + (255,)
    return px


def write_png(path, px):
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "landgrab")
    os.makedirs(out, exist_ok=True)
    for name in GLYPHS:
        path = os.path.join(out, "remote_%s.rgba16.png" % name)
        write_png(path, draw(name))
        print(os.path.relpath(path))


if __name__ == "__main__":
    main()
