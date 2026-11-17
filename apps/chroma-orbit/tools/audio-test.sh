#!/usr/bin/env bash
# Offline check of Chroma Orbit's synthesizer: peak/RMS per second and a WAV to listen to.
#   apps/chroma-orbit/tools/audio-test.sh [out.wav]
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-chroma-orbit"
mkdir -p "$OUT"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c17 -O2 -Wall -I"$ROOT/apps/chroma-orbit/src" -I"$SDL_OUT/install/include" \
  "$ROOT/apps/chroma-orbit/tools/audio-test.c" "$ROOT/apps/chroma-orbit/src/audio.c" $LIBS -lm -o "$OUT/audio-test"
"$OUT/audio-test" "${1:-$OUT/chroma-orbit-audio.wav}"
