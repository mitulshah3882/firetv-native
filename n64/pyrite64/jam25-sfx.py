#!/usr/bin/env python3
"""Cathode Quest 64's sound effects, synthesized: our own replacements for the 16 WAVs in
Pyrite64's n64/examples/jam25/assets/sfx, whose source isn't documented.

  jam25-sfx.py <out-dir>   -> <out-dir>/<Name>.wav for each sound below

The file names stay the game's, so its code and scenes load them unchanged (tools/n64-pyrite64.sh
writes them over the project copy's assets/sfx before the build). Each sound is mono, 32 kHz, at
about the loudness of the one it replaces (TARGET_RMS), so the game's mix stays balanced.

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: MIT
"""
import math
import os
import random
import struct
import sys
import wave

SR = 32000
TAU = 2 * math.pi

# RMS level of each original, measured once; ours are scaled to match.
TARGET_RMS = {
    "BoxBreak": 0.230, "CoinGet": 0.063, "CoinHit": 0.120, "FadeIn": 0.170, "FadeOut": 0.206,
    "Head_Suprise": 0.061, "PlayerJump00": 0.184, "PlayerLand": 0.026, "PotBreak": 0.084,
    "StepDef": 0.107, "StepStone00": 0.016, "StepStone01": 0.030, "StepStone02": 0.021,
    "UiOk": 0.138, "UiSelect": 0.117, "VoidDisable": 0.145,
}


def frames(seconds):
    return int(seconds * SR)


def env_ad(t, attack, decay):
    """Linear attack, then exponential decay with time constant `decay`."""
    if t < attack:
        return t / attack
    return math.exp(-(t - attack) / decay)


class OnePole:
    """One-pole low-pass (cutoff in Hz); high-pass is input minus this."""
    def __init__(self, cutoff):
        self.a = 1 - math.exp(-TAU * cutoff / SR)
        self.y = 0.0

    def __call__(self, x):
        self.y += self.a * (x - self.y)
        return self.y


def square(phase):
    return 1.0 if (phase % 1.0) < 0.5 else -1.0


def tri(phase):
    p = phase % 1.0
    return 4 * p - 1 if p < 0.5 else 3 - 4 * p


# ---- the sounds ------------------------------------------------------------------------------
def box_break():
    """A wooden crate splitting: a low knock, then crackling splinters."""
    rnd, n = random.Random(1), frames(0.65)
    lp, lp2 = OnePole(1800), OnePole(250)
    out, ph = [], 0.0
    cracks = sorted(rnd.uniform(0.0, 0.35) for _ in range(9))
    for i in range(n):
        t = i / SR
        ph += (110 - 60 * min(t / 0.1, 1)) / SR
        knock = math.sin(TAU * ph) * env_ad(t, 0.002, 0.05)
        noise = rnd.uniform(-1, 1)
        crackle = sum(env_ad(t - c, 0.001, 0.018) for c in cracks if t >= c)
        splinter = lp(noise) * crackle * 1.6
        body = lp2(noise) * env_ad(t, 0.003, 0.12) * 2.5
        out.append(0.9 * knock + splinter + body)
    return out


def coin_get():
    """A bright two-note chime, B5 then E6, with a little shimmer."""
    n, out = frames(0.73), []
    lp = OnePole(5000)
    for i in range(n):
        t = i / SR
        f = 987.8 if t < 0.07 else 1318.5
        t0 = t if t < 0.07 else t - 0.07
        e = env_ad(t0, 0.002, 0.07 if t < 0.07 else 0.22)
        s = 0.6 * square(f * t) + 0.4 * math.sin(TAU * 2 * f * t)
        shimmer = math.sin(TAU * 3 * f * t) * 0.15 * (0.5 + 0.5 * math.sin(TAU * 18 * t))
        out.append(lp(s + shimmer) * e)
    return out


def coin_hit():
    """A coin bouncing off something: an inharmonic metallic ping."""
    n, out = frames(0.41), []
    partials = [(2093.0, 1.0, 0.08), (3311.0, 0.6, 0.05), (5130.0, 0.35, 0.03), (1567.0, 0.4, 0.12)]
    for i in range(n):
        t = i / SR
        out.append(sum(a * math.sin(TAU * f * t) * env_ad(t, 0.001, d) for f, a, d in partials))
    return out


def sweep(rising, seconds):
    """The screen fades: a filtered-noise whoosh with a sine gliding up (in) or down (out)."""
    rnd, n = random.Random(3 if rising else 4), frames(seconds)
    lp = OnePole(900)
    out, ph = [], 0.0
    for i in range(n):
        x = i / n
        t = x if rising else 1 - x
        f = 180 + 520 * t * t
        ph += f / SR
        shape = math.sin(math.pi * min(1.0, x * 1.15)) ** 1.5
        whoosh = lp(rnd.uniform(-1, 1)) * 2.2
        out.append((0.55 * math.sin(TAU * ph) + whoosh) * shape)
    return out


def head_surprise():
    """The stone head, startled: a quick rising 'hm?!' (a formant-shaped chirp)."""
    n, out, ph = frames(0.5), [], 0.0
    lp = OnePole(1400)
    for i in range(n):
        t = i / SR
        f = 140 + 160 * (t / 0.5) ** 0.6
        ph += f / SR
        buzz = tri(ph) * 0.7 + square(ph) * 0.3
        vib = 1 + 0.04 * math.sin(TAU * 7 * t)
        out.append(lp(buzz * vib) * env_ad(t, 0.02, 0.18))
    return out


def player_jump():
    """The TV hops: a rising square-wave sweep."""
    n, out, ph = frames(0.47), [], 0.0
    lp = OnePole(3500)
    for i in range(n):
        t = i / SR
        f = 220 * 2 ** (t / 0.16)
        ph += min(f, 1600) / SR
        out.append(lp(square(ph)) * env_ad(t, 0.004, 0.13))
    return out


def player_land():
    """Landing: a short low thud with a rattle (a TV set touching down)."""
    rnd, n = random.Random(5), frames(0.58)
    lp, lp2 = OnePole(160), OnePole(2400)
    out, ph = [], 0.0
    for i in range(n):
        t = i / SR
        ph += (70 - 30 * min(t / 0.15, 1)) / SR
        thud = math.sin(TAU * ph) * env_ad(t, 0.002, 0.07)
        rattle = lp2(rnd.uniform(-1, 1)) * env_ad(t, 0.001, 0.03) * 0.5
        out.append(thud + lp(rnd.uniform(-1, 1)) * env_ad(t, 0.002, 0.05) * 2 + rattle)
    return out


def pot_break():
    """A clay pot shattering: a bright crash and shards tinkling down."""
    rnd, n = random.Random(6), frames(1.25)
    hp = OnePole(2500)
    shards = [(rnd.uniform(0.02, 0.9), rnd.uniform(2600, 5200), rnd.uniform(0.3, 0.8)) for _ in range(22)]
    out = []
    for i in range(n):
        t = i / SR
        noise = rnd.uniform(-1, 1)
        crash = (noise - 0.7 * hp(noise)) * env_ad(t, 0.001, 0.08)
        tink = sum(a * math.sin(TAU * f * (t - s)) * env_ad(t - s, 0.0005, 0.025) for s, f, a in shards if s <= t < s + 0.2)
        out.append(crash * 0.9 + tink * 0.5)
    return out


def step(seed, pitch, grit, seconds):
    """A footstep: a knock at `pitch` with `grit` worth of gravel."""
    rnd, n = random.Random(seed), frames(seconds)
    lp, bp = OnePole(pitch * 6), OnePole(3000)
    out, ph = [], 0.0
    for i in range(n):
        t = i / SR
        ph += pitch * (1 - 0.3 * min(t / 0.05, 1)) / SR
        knock = math.sin(TAU * ph) * env_ad(t, 0.001, 0.025)
        noise = rnd.uniform(-1, 1)
        gravel = (noise - bp(noise)) * env_ad(t, 0.001, 0.02 + 0.03 * grit) * grit
        out.append(lp(knock * 1.2 + gravel))
    return out


def ui_ok():
    """Menu confirm: two quick rising notes (E5, B5)."""
    n, out = frames(0.44), []
    lp = OnePole(4500)
    for i in range(n):
        t = i / SR
        f, t0 = (659.3, t) if t < 0.08 else (987.8, t - 0.08)
        e = env_ad(t0, 0.002, 0.05 if t < 0.08 else 0.12)
        out.append(lp(0.7 * square(f * t) + 0.3 * math.sin(TAU * f * t)) * e)
    return out


def ui_select():
    """Menu move: a short soft blip."""
    n, out = frames(0.47), []
    lp = OnePole(2500)
    for i in range(n):
        t = i / SR
        f = 880 * (1 + 0.15 * math.exp(-t / 0.01))
        out.append(lp(square(f * t)) * env_ad(t, 0.002, 0.045))
    return out


def void_disable():
    """A void portal switching off, like a CRT: a falling whine, a pop and fading static."""
    rnd, n = random.Random(8), frames(1.17)
    lp = OnePole(3000)
    out, ph = [], 0.0
    for i in range(n):
        t = i / SR
        f = 1800 * math.exp(-t / 0.25) + 60
        ph += f / SR
        whine = math.sin(TAU * ph) * env_ad(t, 0.01, 0.35)
        pop = math.sin(TAU * 55 * t) * env_ad(t - 0.05, 0.002, 0.06) if t > 0.05 else 0.0
        static = lp(rnd.uniform(-1, 1)) * env_ad(t, 0.02, 0.4) * 0.6
        out.append(0.6 * whine + pop + static)
    return out


SOUNDS = {
    "BoxBreak": box_break,
    "CoinGet": coin_get,
    "CoinHit": coin_hit,
    "FadeIn": lambda: sweep(True, 1.0),
    "FadeOut": lambda: sweep(False, 0.88),
    "Head_Suprise": head_surprise,
    "PlayerJump00": player_jump,
    "PlayerLand": player_land,
    "PotBreak": pot_break,
    "StepDef": lambda: step(10, 160, 0.6, 0.16),
    "StepStone00": lambda: step(11, 120, 1.0, 0.38),
    "StepStone01": lambda: step(12, 135, 1.2, 0.39),
    "StepStone02": lambda: step(13, 108, 0.9, 0.46),
    "UiOk": ui_ok,
    "UiSelect": ui_select,
    "VoidDisable": void_disable,
}


def write(path, samples, target_rms):
    # a short fade at both ends (no clicks), then scaled to the original's RMS, peaks capped
    fade = min(64, len(samples) // 4)
    for k in range(fade):
        samples[k] *= k / fade
        samples[-1 - k] *= k / fade
    rms = math.sqrt(sum(s * s for s in samples) / len(samples)) or 1e-9
    peak = max(abs(s) for s in samples) or 1e-9
    gain = min(target_rms / rms, 0.95 / peak)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * gain)) * 32767)) for s in samples))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    for name, make in SOUNDS.items():
        write(os.path.join(sys.argv[1], name + ".wav"), make(), TARGET_RMS[name])


if __name__ == "__main__":
    main()
