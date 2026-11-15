#!/usr/bin/env bash
# Builds the trimmed SuperTuxKart data the package ships (apps/supertuxkart/build/stk-data/data):
# see make-assets.py for what is scaled, converted and dropped. Needs python3; it sets up a
# private venv with Pillow and builds the ogg-reencode host tool from the pinned libogg/libvorbis.
#
#   apps/supertuxkart/make-assets.sh
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$APP/../.." && pwd)"
SRC="$ROOT/third_party/_src"
STK="$SRC/SuperTuxKart-1.5-src"
B="$APP/build"
[[ -d "$STK/data" ]] || { echo "Run tools/fetch-deps.sh stk first" >&2; exit 1; }
for d in ogg vorbis; do [[ -n "$(ls -d "$SRC"/lib$d-* 2>/dev/null)" ]] || "$ROOT/tools/fetch-deps.sh" "$d"; done
mkdir -p "$B/host"

if [[ ! -x "$B/venv/bin/python" ]]; then
  python3 -m venv "$B/venv"
  "$B/venv/bin/pip" install -q "pillow==11.3.0"
fi

OGG_TOOL="$B/host/ogg-reencode"
if [[ ! -x "$OGG_TOOL" ]]; then
  OGG="$SRC/libogg-1.3.6"; VORBIS="$SRC/libvorbis-1.3.7"
  mkdir -p "$B/host/ogg"
  cat > "$B/host/ogg/config_types.h" <<'EOF'
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t; typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t; typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t; typedef uint64_t ogg_uint64_t;
#endif
EOF
  libs=()
  for f in analysis bitrate block codebook envelope floor0 floor1 info lookup lpc lsp mapping0 mdct \
           psy registry res0 sharedbook smallft synthesis vorbisenc vorbisfile window; do
    libs+=("$VORBIS/lib/$f.c")
  done
  cc -O2 -w -I"$B/host" -I"$OGG/include" -I"$VORBIS/include" -I"$VORBIS/lib" \
     "$APP/tools/ogg-reencode.c" "$OGG/src/bitwise.c" "$OGG/src/framing.c" "${libs[@]}" -lm -o "$OGG_TOOL"
fi

ASTCENC="$B/host/astcenc-build/Source/astcenc-neon"
if [[ ! -x "$ASTCENC" ]]; then
  [[ -d "$SRC/astc-encoder-5.7.0" ]] || "$ROOT/tools/fetch-deps.sh" astcenc
  cmake -S "$SRC/astc-encoder-5.7.0" -B "$B/host/astcenc-build" -DCMAKE_BUILD_TYPE=Release \
        -DASTCENC_ISA_NEON=ON -DASTCENC_UNIVERSAL_BUILD=OFF -DASTCENC_WERROR=OFF >/dev/null
  cmake --build "$B/host/astcenc-build" -j 8 >/dev/null
fi

"$B/venv/bin/python" "$APP/make-assets.py" "$STK/data" "$B/stk-data/data" --stk-src "$STK" \
  --ogg-tool "$OGG_TOOL" --astcenc "$ASTCENC"
du -sh "$STK/data" "$B/stk-data/data"
