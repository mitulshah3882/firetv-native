#!/usr/bin/env python3
"""Sword Strike's two sounds, synthesized: our own replacements for the jam's files, which we don't
fetch (the Wilhelm scream's provenance is murky and `challengers.wav` has no documented source).

  swordstrike-sfx.py <out-dir>   -> <out-dir>/wilhelm_scream.wav, <out-dir>/challengers.wav

The file names stay the game's, so its code loads them unchanged.
  wilhelm_scream.wav  the knock-out: a sliding square-wave yelp over a noise thud (0.7 s)
  challengers.wav     the fight music: an 8-bar chiptune loop in A minor at 132 BPM (square lead,
                      pulse arpeggio, triangle bass, noise hats), seamless when looped

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: MIT
"""
import math
import os
import random
import struct
import sys
import wave

SR = 22050


def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        peak = max(1e-9, max(abs(s) for s in samples))
        k = 0.85 / peak
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * k)) * 32767)) for s in samples))


def knockout():
    n = int(0.7 * SR)
    rnd = random.Random(7)
    out, ph = [], 0.0
    for i in range(n):
        t = i / SR
        f = 880.0 * (0.17 ** (t / 0.7)) * (1.0 + 0.04 * math.sin(2 * math.pi * 11 * t))  # 880 -> 150 Hz, wobbling
        ph += f / SR
        sq = 1.0 if (ph % 1.0) < 0.5 else -1.0
        env = min(1.0, t / 0.01) * math.exp(-t * 2.2)
        thud = rnd.uniform(-1, 1) * math.exp(-t * 30.0) * 0.8
        out.append(0.55 * sq * env + thud)
    return out


def music():
    bpm, bars = 132.0, 8
    beat = 60.0 / bpm
    n = int(round(bars * 4 * beat * SR))
    out = [0.0] * n
    # Am F G E, twice; the second pass climbs higher
    chords = [(57, (0, 3, 7)), (53, (0, 4, 7)), (55, (0, 4, 7)), (52, (0, 4, 7))] * 2
    lead = [  # (beat offset in the bar, length in beats, semitones above A4) per bar
        [(0, 1, 0), (1, 0.5, 3), (1.5, 0.5, 7), (2, 1, 12), (3, 1, 10)],
        [(0, 1.5, 8), (1.5, 0.5, 7), (2, 1, 5), (3, 1, 3)],
        [(0, 1, 2), (1, 0.5, 5), (1.5, 0.5, 7), (2, 1.5, 10), (3.5, 0.5, 7)],
        [(0, 2, 8), (2, 1, 7), (3, 1, 11)],
        [(0, 1, 12), (1, 0.5, 15), (1.5, 0.5, 12), (2, 1, 19), (3, 1, 17)],
        [(0, 1.5, 15), (1.5, 0.5, 12), (2, 1, 17), (3, 1, 15)],
        [(0, 1, 14), (1, 0.5, 17), (1.5, 0.5, 19), (2, 1.5, 22), (3.5, 0.5, 19)],
        [(0, 2, 20), (2, 1, 19), (3, 1, 16)],
    ]

    def mtof(m):
        return 440.0 * 2 ** ((m - 69) / 12.0)

    def add(start, length, f, kind, vol, decay=0.0):
        i0, i1 = int(start * SR), min(n, int((start + length) * SR))
        ph = 0.0
        for i in range(i0, i1):
            t = (i - i0) / SR
            ph += f / SR
            p = ph % 1.0
            if kind == "square":
                x = 1.0 if p < 0.5 else -1.0
            elif kind == "pulse":
                x = 1.0 if p < 0.25 else -1.0
            else:  # triangle
                x = 4.0 * abs(p - 0.5) - 1.0
            env = min(1.0, t / 0.004) * min(1.0, (i1 - i) / (0.01 * SR))
            if decay:
                env *= math.exp(-t * decay)
            out[i % n] += x * env * vol

    rnd = random.Random(3)
    for b, (root, quality) in enumerate(chords):
        bar0 = b * 4 * beat
        for k in range(8):  # bass: root and fifth on 8ths
            note = root - 24 + (7 if k % 4 == 3 else 0)
            add(bar0 + k * beat / 2, beat / 2 * 0.9, mtof(note), "tri", 0.30)
        for k in range(16):  # arpeggio on 16ths
            iv = quality[k % 3] + (12 if (k // 3) % 2 else 0)
            add(bar0 + k * beat / 4, beat / 4 * 0.8, mtof(root + iv), "pulse", 0.07, decay=9.0)
        for start, length, semi in lead[b]:
            add(bar0 + start * beat, length * beat * 0.92, mtof(69 + semi), "square", 0.13)
        for k in range(8):  # hats on 8ths, kick-like thump on beats 1 and 3
            i0 = int((bar0 + k * beat / 2) * SR)
            for i in range(int(0.03 * SR)):
                out[(i0 + i) % n] += rnd.uniform(-1, 1) * math.exp(-i / (0.006 * SR)) * (0.10 if k % 2 else 0.05)
            if k % 4 == 0:
                ph = 0.0
                for i in range(int(0.15 * SR)):
                    t = i / SR
                    ph += (55.0 + 110.0 * math.exp(-t * 35.0)) / SR
                    out[(i0 + i) % n] += math.sin(2 * math.pi * ph) * math.exp(-t * 18.0) * 0.45
    return out


if __name__ == "__main__":
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    write(os.path.join(d, "wilhelm_scream.wav"), knockout())
    write(os.path.join(d, "challengers.wav"), music())
