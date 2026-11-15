#!/usr/bin/env bash
# Builds and runs the Gravity Choir audio engine test on the host (no Fire TV needed): pitch,
# 1/16 quantization, the beat clock, a 64 s scripted session rendered to a WAV, the crescendo's
# silent bar and downbeat, and CPU cost.
#
#   apps/gravity-choir/tools/audio-test.sh [out-dir]      # default out-dir: build/gc-audio
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SDL_SRC="$ROOT/third_party/_src/SDL3-3.4.18"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="${1:-$ROOT/build/gc-audio}"
mkdir -p "$OUT"
[[ -d "$SDL_SRC" ]] || "$ROOT/tools/fetch-deps.sh"

if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  cmake -S "$SDL_SRC" -B "$SDL_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_OUT/install" >/dev/null
  ninja -C "$SDL_OUT" install >/dev/null
fi

LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c11 -O2 -Wall -Wextra -Wshadow -Wno-unused-function -I"$SDL_OUT/install/include" \
  "$ROOT/apps/gravity-choir/tools/audio-test.c" $LIBS -o "$OUT/audio-test"
"$OUT/audio-test" "$OUT" "${@:2}"
