#!/usr/bin/env bash
# Builds the arcade for the host (macOS/Linux) against a host build of the pinned SDL3, and renders
# preview frames offscreen. No Fire TV needed; used for screenshots and the launcher icon.
#
#   apps/arcade/host-preview.sh                         # build + render build/host-arcade/shot-<sel>.png
#   apps/arcade/host-preview.sh --sel 2 --frames 200    # extra args go to the arcade binary
#   apps/arcade/host-preview.sh run                     # build + open a normal window (keyboard: arrows/Enter/Esc)
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDL_SRC="$ROOT/third_party/_src/SDL3-3.4.18"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-arcade"
[[ -d "$SDL_SRC" ]] || "$ROOT/tools/fetch-deps.sh"

if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  cmake -S "$SDL_SRC" -B "$SDL_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$SDL_OUT/install" >/dev/null
  ninja -C "$SDL_OUT" install >/dev/null
fi

mkdir -p "$OUT/assets/fonts" "$OUT/assets/art"
cp "$ROOT"/third_party/_src/fonts/*.ttf "$OUT/assets/fonts/"
cp "$ROOT"/apps/arcade/art/*.png "$OUT/assets/art/"
# shellcheck disable=SC2046
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/third_party/_src/stb" -I"$SDL_OUT/install/include" \
  "$ROOT"/apps/arcade/src/{arcade,text,synth}.c $LIBS -o "$OUT/arcade"

cd "$OUT"
if [[ "${1:-}" == "run" ]]; then
  ARCADE_ASSETS="$OUT/assets/" ./arcade
else
  sel=0
  args=("$@")
  for ((i = 0; i < ${#args[@]}; i++)); do [[ "${args[$i]}" == "--sel" ]] && sel="${args[$((i + 1))]}"; done
  SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software ARCADE_ASSETS="$OUT/assets/" \
    ./arcade --shot "$OUT/shot-$sel.png" "$@"
fi
