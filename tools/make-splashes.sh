#!/usr/bin/env bash
# Regenerates each game's loading splash: its card art, its title and "LOADING" at 1920x1080.
#   apps/<app>/icon/splash.astc           ASTC 8x8; vegaplat draws it as the first frame
#                                         (vp_gl_create_context), so slow loaders don't sit on black
#   apps/<app>/icon/SplashScreenImages.zip  the same picture as Vega's system splash, shown from the
#                                         launch until that first frame
# Apps without them keep a black first frame.
#
#   tools/make-splashes.sh            (PNGs for review land in build/host-tools/splash/)
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDL_OUT="$ROOT/build/host-sdl3"
OUT="$ROOT/build/host-tools"
SRC="$ROOT/third_party/_src"
if [[ ! -f "$SDL_OUT/install/lib/libSDL3.a" ]]; then
  "$ROOT/apps/arcade/host-preview.sh" >/dev/null  # builds the host SDL3 as a side effect
fi
mkdir -p "$OUT/splash"
LIBS=$(sed -n 's/^Libs: //p' "$SDL_OUT/install/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL_OUT/install/lib#")
# shellcheck disable=SC2086
cc -std=c11 -O2 -Wall -Wno-unused-function -I"$SDL_OUT/install/include" -I"$SRC/stb" \
  "$ROOT/tools/make-splash.c" $LIBS -o "$OUT/make-splash"

ASTCENC="$OUT/astcenc-build/Source/astcenc-neon"
if [[ ! -x "$ASTCENC" ]]; then
  [[ -d "$SRC/astc-encoder-5.7.0" ]] || "$ROOT/tools/fetch-deps.sh" astcenc
  cmake -S "$SRC/astc-encoder-5.7.0" -B "$OUT/astcenc-build" -DCMAKE_BUILD_TYPE=Release \
    -DASTCENC_ISA_NEON=ON -DASTCENC_CLI=ON >/dev/null
  cmake --build "$OUT/astcenc-build" -j 8 >/dev/null
fi

FONT="$SRC/fonts/ChakraPetch-SemiBold.ttf"
splash() {  # app art title r g b
  local png="$OUT/splash/$1.png"
  "$OUT/make-splash" "$png" "$ROOT/apps/arcade/art/$2" "$3" "$FONT" "$4" "$5" "$6"
  "$ASTCENC" -cl "$png" "$ROOT/apps/$1/icon/splash.astc" 8x8 -medium -silent
  # The same picture as Vega's system splash (assets/raw/SplashScreenImages.zip: one 1920x1080 PNG
  # in _loop/ plus desc.txt), which the system shows the moment the app is launched, before our
  # process starts; our first frame then draws the same image.
  local z="$OUT/splash/$1-zip"
  rm -rf "$z" && mkdir -p "$z/_loop"
  cp "$png" "$z/_loop/loop00000.png"
  printf '1920 1080 30\nc 0 0 _loop\n' > "$z/desc.txt"
  local zip="$ROOT/apps/$1/icon/SplashScreenImages.zip"
  # Keep the existing zip when its picture is unchanged (a new zip differs in its timestamps).
  if [[ -f "$zip" ]] && unzip -p "$zip" _loop/loop00000.png | cmp -s - "$png"; then
    echo "apps/$1/icon/splash.astc (zip unchanged)"
    return
  fi
  rm -f "$zip"
  (cd "$z" && zip -q -X -0 -r "$zip" desc.txt _loop)
  echo "apps/$1/icon/splash.astc, SplashScreenImages.zip"
}
# Accent colours match the arcade's cards.
splash gravity-choir gravitychoir.png "Gravity Choir" 204 102 255
splash doom doom.png "Freedoom" 255 84 51
splash quake quake.png "LibreQuake" 255 178 41
splash supertux supertux.png "SuperTux" 115 209 255
splash supertuxkart supertuxkart.png "SuperTuxKart" 255 158 31
splash n64-brew-volley brewvolley.png "Brew Volley" 125 255 79
splash n64-landgrab landgrab.png "Land Grab" 255 97 97
splash n64-hydraharmonics hydraharmonics.png "Hydra Harmonics" 255 112 176
splash n64-swordstrike swordstrike.png "Sword Strike" 255 80 64
splash n64-tohubohu tohubohu.png "Le Tohu-bohu" 255 196 64
splash n64-cathode cathode.png "Cathode Quest 64" 255 214 92
splash prism-spire prismspire.png "Prism Spire" 255 170 110
splash chroma-orbit chromaorbit.png "Chroma Orbit" 120 200 255
