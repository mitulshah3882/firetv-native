#!/usr/bin/env bash
# Builds Gravity Choir for the Mac (desktop GL 3.3 core) against a host build of the pinned SDL3,
# for iterating on visuals and gameplay without the Fire TV.
#
#   apps/gravity-choir/host-preview.sh run                      # a window (arrows, Space/Enter, P, Esc)
#   apps/gravity-choir/host-preview.sh shots <script> [every] [count]
#                                                               # renders PNGs to build/host-gravity-choir/shots-<script>/
#
# Scripts: title (attract mode), play (the AI plays), crescendo (the AI plays; Crescendo at 6 s).
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_SRC="$ROOT/third_party/_src/SDL3-3.4.18"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-gravity-choir"
[[ -d "$SDL_SRC" ]] || "$ROOT/tools/fetch-deps.sh"
if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  cmake -S "$SDL_SRC" -B "$SDL_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_OUT/install" >/dev/null
  ninja -C "$SDL_OUT" install >/dev/null
fi

mkdir -p "$OUT/assets/fonts"
cp "$ROOT"/third_party/_src/fonts/ChakraPetch-*.ttf "$OUT/assets/fonts/"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
SRC="$ROOT/apps/gravity-choir/src"
AUDIO="${GC_AUDIO_SRC:-$SRC/audio.c}"
# shellcheck disable=SC2086
cc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DGC_HOST_GL \
  -I"$SRC" -I"$ROOT/third_party/_src/stb" -I"$SDL_OUT/install/include" \
  "$SRC"/main.c "$SRC"/game.c "$SRC"/gfx.c "$SRC"/grid.c "$SRC"/particles.c "$AUDIO" \
  $LIBS -lm -o "$OUT/gravity-choir"

cd "$OUT"
case "${1:-run}" in
  run) GC_ASSETS="$OUT/assets/" ./gravity-choir ;;
  shots)
    script="${2:-title}"
    dir="$OUT/shots-$script"
    rm -rf "$dir" && mkdir -p "$dir"
    GC_ASSETS="$OUT/assets/" ./gravity-choir --script "$script" --shots "$dir" "${3:-60}" "${4:-6}"
    for f in "$dir"/*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
    ls "$dir"
    ;;
esac
