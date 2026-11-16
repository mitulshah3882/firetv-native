#!/usr/bin/env bash
# Builds Prism Spire for the Mac (desktop GL 3.3 core) against a host build of the pinned SDL3, for
# iterating on visuals and gameplay without the Fire TV.
#
#   apps/prism-spire/host-preview.sh run                      # a window (arrows, Enter, Esc, M)
#   apps/prism-spire/host-preview.sh shots <script> [every] [count]
#                                                             # renders PNGs to build/host-prism-spire/shots-<script>/
#
# Scripts: title (attract mode), play (the AI plays a 4-player party), collapse (the AI plays riskily),
# solo (the AI plays the Daily Spire), howto, credits.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
# shellcheck disable=SC2086
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_SRC="$ROOT/third_party/_src/SDL3-3.4.18"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-prism-spire"
B2="$ROOT/third_party/_src/box2d-3.1.1"
[[ -d "$SDL_SRC" && -d "$B2" ]] || "$ROOT/tools/fetch-deps.sh"
if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  cmake -S "$SDL_SRC" -B "$SDL_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_OUT/install" >/dev/null
  ninja -C "$SDL_OUT" install >/dev/null
fi
mkdir -p "$OUT/assets/fonts" "$OUT/b2"
cp "$ROOT"/third_party/_src/fonts/ChakraPetch-*.ttf "$OUT/assets/fonts/"
# Box2D once (as on the TV: SIMD off).
if [[ ! -f "$OUT/libbox2d.a" ]]; then
  for f in "$B2"/src/*.c; do
    cc -std=c17 -O2 -DBOX2D_DISABLE_SIMD -I"$B2/include" -I"$B2/src" -c "$f" -o "$OUT/b2/$(basename "$f" .c).o" &
  done
  wait
  ar rcs "$OUT/libbox2d.a" "$OUT"/b2/*.o
fi
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
SRC="$ROOT/apps/prism-spire/src"
cc -std=c17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -DPS_HOST_GL \
  -I"$SRC" -I"$B2/include" -I"$ROOT/third_party/_src/stb" -I"$SDL_OUT/install/include" \
  "$SRC"/main.c "$SRC"/game.c "$SRC"/render.c "$SRC"/audio.c "$OUT/libbox2d.a" \
  $LIBS -lm -o "$OUT/prism-spire"

cd "$OUT"
export PS_DATA="$OUT"
case "${1:-run}" in
  run) PS_ASSETS="$OUT/assets/" ./prism-spire ;;
  shots)
    script="${2:-title}"
    dir="$OUT/shots-$script"
    rm -rf "$dir" && mkdir -p "$dir"
    PS_ASSETS="$OUT/assets/" ./prism-spire --script "$script" --shots "$dir" "${3:-60}" "${4:-6}"
    for f in "$dir"/*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
    ls "$dir"
    ;;
esac
