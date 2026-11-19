#!/usr/bin/env bash
# Regenerates every app's 512x512 launcher icon (apps/*/icon/icon.png) from its artwork: the
# arcade's own logo frame, and for each game the same card art the arcade shows.
# Vega's "Your Apps & Channels" row only lists sideloaded apps whose icon is exactly 512x512.
#
#   tools/make-icons.sh
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-tools"
if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  "$ROOT/apps/arcade/host-preview.sh" >/dev/null  # builds the host SDL3 as a side effect
fi
mkdir -p "$OUT"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c11 -O2 -Wall -I"$SDL_OUT/install/include" "$ROOT/tools/make-icon.c" $LIBS -o "$OUT/make-icon"

make() { "$OUT/make-icon" "$ROOT/apps/$1/icon/icon.png" "$2" && echo "apps/$1/icon/icon.png"; }
mkdir -p "$ROOT/apps/sdl-demo/icon" "$ROOT/apps/n64-brew-volley/icon"
make arcade "$ROOT/apps/arcade/icon/banner.png"
make doom "$ROOT/apps/arcade/art/doom.png"
make quake "$ROOT/apps/arcade/art/quake.png"
make sdl-demo --starfield
make n64-brew-volley "$ROOT/apps/arcade/art/brewvolley.png"
make supertux "$ROOT/apps/arcade/art/supertux.png"
make supertuxkart "$ROOT/apps/arcade/art/supertuxkart.png"
make n64-landgrab "$ROOT/apps/arcade/art/landgrab.png"
make n64-hydraharmonics "$ROOT/apps/arcade/art/hydraharmonics.png"
make n64-swordstrike "$ROOT/apps/arcade/art/swordstrike.png"
make prism-spire "$ROOT/apps/arcade/art/prismspire.png"
make chroma-orbit "$ROOT/apps/arcade/art/chromaorbit.png"
