#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/_src (git-ignored).
# Sources are verified against SHA-256 checksums recorded in third_party/deps.lock
# (first fetch records the checksum; later fetches must match).
#
#   tools/fetch-deps.sh            # fetch everything
#   tools/fetch-deps.sh sdl3       # fetch one dependency
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/_src"
LOCK="$ROOT/third_party/deps.lock"
mkdir -p "$SRC"
touch "$LOCK"

# name | version | url | extracted-dir
DEPS=(
  "sdl3|3.4.18|https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz|SDL3-3.4.18"
  "doomgeneric|e257a09|https://github.com/ozkl/doomgeneric/archive/e257a09527062bfb986b6e5e0780839b6cbd5b71.tar.gz|doomgeneric-e257a09527062bfb986b6e5e0780839b6cbd5b71"
  "chocolate-doom|2.3.0|https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-2.3.0.tar.gz|chocolate-doom-chocolate-doom-2.3.0"
  "libdragon|39d0d60|https://github.com/DragonMinded/libdragon/archive/39d0d6096130836a65710da6730290701d060272.tar.gz|libdragon-39d0d6096130836a65710da6730290701d060272"
  "quakegeneric|1305210|https://github.com/erysdren/quakegeneric/archive/13052102577c629650cf07a46151a4b6e1b19c3c.tar.gz|quakegeneric-13052102577c629650cf07a46151a4b6e1b19c3c"
  "idquake|bf4ac42|https://github.com/id-Software/Quake/archive/bf4ac424ce754894ac8f1dae6a3981954bc9852d.tar.gz|Quake-bf4ac424ce754894ac8f1dae6a3981954bc9852d"
  "librequake|0.09-beta|https://github.com/lavenderdotpet/LibreQuake/releases/download/v0.09-beta/lite.zip|librequake-0.09-lite"
  "freedoom|0.13.0|https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip|freedoom-0.13.0"
  # Git checkouts (for host tools with submodules): <url>.git#<full commit>; the commit is the pin.
  "n64recomp|598ce20|https://github.com/N64Recomp/N64Recomp.git#598ce2080004a5672d604dceadbda436045307b7|N64Recomp-598ce20"
  # Single files (fonts and header-only libraries) land in <extracted-dir>/<file name>.
  "stb_truetype|4e9b368|https://raw.githubusercontent.com/nothings/stb/4e9b368bfc071e97206ea5c6adba68dd0cbf8897/stb_truetype.h|stb"
  "stb_vorbis|4e9b368|https://raw.githubusercontent.com/nothings/stb/4e9b368bfc071e97206ea5c6adba68dd0cbf8897/stb_vorbis.c|stb"
  "font-chakrapetch-regular|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-Regular.ttf|fonts"
  "font-chakrapetch-semibold|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-SemiBold.ttf|fonts"
  "font-chakrapetch-bold|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-Bold.ttf|fonts"
  "font-chakrapetch-ofl|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/OFL.txt|fonts/ChakraPetch"
  "font-pressstart2p|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/pressstart2p/PressStart2P-Regular.ttf|fonts"
  "font-pressstart2p-ofl|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/pressstart2p/OFL.txt|fonts/PressStart2P"
)

fetch() {
  local name="$1" version="$2" url="$3" dir="$4"
  if [[ "$url" == *.git#* ]]; then
    if [[ -d "$SRC/$dir" ]]; then echo "[$name] already present: $SRC/$dir"; return; fi
    local repo="${url%%#*}" commit="${url##*#}"
    echo "[$name] cloning $repo @ $commit"
    git clone -q "$repo" "$SRC/$dir"
    git -C "$SRC/$dir" -c advice.detachedHead=false checkout -q "$commit"
    git -C "$SRC/$dir" submodule update -q --init --recursive
    grep -q "^$name $version " "$LOCK" || echo "$name $version $commit" >> "$LOCK"
    echo "[$name] checked out to $SRC/$dir"
    return
  fi
  local tarball="$SRC/$(basename "$url")"
  local single=0
  case "$url" in *.h|*.c|*.ttf|*.txt) single=1 ;; esac
  if [[ $single == 1 && -f "$SRC/$dir/$(basename "$url")" ]]; then echo "[$name] already present"; return; fi
  if [[ $single == 0 && -d "$SRC/$dir" ]]; then echo "[$name] already present: $SRC/$dir"; return; fi
  echo "[$name] downloading $url"
  curl -fL --retry 3 -o "$tarball" "$url"
  local sum; sum="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
  local locked; locked="$(grep "^$name $version " "$LOCK" | cut -d' ' -f3 || true)"
  if [[ -z "$locked" ]]; then
    echo "$name $version $sum" >> "$LOCK"
    echo "[$name] recorded sha256 $sum"
  elif [[ "$locked" != "$sum" ]]; then
    echo "[$name] CHECKSUM MISMATCH: expected $locked got $sum" >&2; rm -f "$tarball"; exit 1
  fi
  if [[ $single == 1 ]]; then
    mkdir -p "$SRC/$dir"; mv "$tarball" "$SRC/$dir/"; echo "[$name] saved to $SRC/$dir/"; return
  fi
  case "$tarball" in
    *.zip) if unzip -Z1 "$tarball" | head -1 | grep -q "^$dir/"; then unzip -q "$tarball" -d "$SRC"; else unzip -q "$tarball" -d "$SRC/$dir"; fi ;;
    *) tar -xzf "$tarball" -C "$SRC" ;;
  esac
  rm -f "$tarball"
  echo "[$name] extracted to $SRC/$dir"
}

want="${1:-}"
for d in "${DEPS[@]}"; do
  IFS='|' read -r name version url dir <<<"$d"
  if [[ -z "$want" || "$want" == "$name" ]]; then fetch "$name" "$version" "$url" "$dir"; fi
done
