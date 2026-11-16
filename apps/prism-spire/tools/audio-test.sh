#!/usr/bin/env bash
# Offline check of Prism Spire's synthesizer: peak/RMS per second and a WAV to listen to.
#   apps/prism-spire/tools/audio-test.sh [out.wav]
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-prism-spire"
mkdir -p "$OUT"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c17 -O2 -Wall -I"$ROOT/apps/prism-spire/src" -I"$SDL_OUT/install/include" \
  "$ROOT/apps/prism-spire/tools/audio-test.c" "$ROOT/apps/prism-spire/src/audio.c" $LIBS -lm -o "$OUT/audio-test"
"$OUT/audio-test" "${1:-$OUT/prism-spire-audio.wav}"
