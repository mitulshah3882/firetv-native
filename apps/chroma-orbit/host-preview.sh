#!/usr/bin/env bash
# Builds Chroma Orbit for the Mac (desktop GL 3.3 core) against a host build of the pinned SDL3, for
# iterating on visuals and gameplay without the Fire TV.
#
#   apps/chroma-orbit/host-preview.sh run                      # a window (arrows, Enter, Esc, M)
#   apps/chroma-orbit/host-preview.sh shots <script> [every] [count]
#                                                             # renders PNGs to build/host-chroma-orbit/shots-<script>/
#
# Scripts: title (attract mode), play (the AI plays), keyart (no UI), howto, credits, nova (a supernova
# every 10 pairs: every world), over (the game-over screen), sky (each backdrop for a second). On the TV: pkg://<component>?script=nova.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
# shellcheck disable=SC2086
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_SRC="$ROOT/third_party/_src/SDL3-3.4.18"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-chroma-orbit"
[[ -d "$SDL_SRC" ]] || "$ROOT/tools/fetch-deps.sh"
if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  cmake -S "$SDL_SRC" -B "$SDL_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_OUT/install" >/dev/null
  ninja -C "$SDL_OUT" install >/dev/null
fi
mkdir -p "$OUT/assets/fonts"
cp "$ROOT"/third_party/_src/fonts/ChakraPetch-*.ttf "$OUT/assets/fonts/"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
SRC="$ROOT/apps/chroma-orbit/src"
cc -std=c17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -DCO_HOST_GL \
  -I"$SRC" -I"$ROOT/third_party/_src/stb" -I"$SDL_OUT/install/include" \
  "$SRC"/main.c "$SRC"/game.c "$SRC"/board.c "$SRC"/ai.c "$SRC"/render.c "$SRC"/audio.c \
  $LIBS -lm -o "$OUT/chroma-orbit"

cd "$OUT"
export CO_DATA="$OUT"
case "${1:-run}" in
  run) CO_ASSETS="$OUT/assets/" ./chroma-orbit ;;
  shots)
    script="${2:-title}"
    dir="$OUT/shots-$script"
    rm -rf "$dir" && mkdir -p "$dir"
    CO_ASSETS="$OUT/assets/" ./chroma-orbit --script "$script" --shots "$dir" "${3:-60}" "${4:-6}"
    for f in "$dir"/*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
    ls "$dir"
    ;;
esac
