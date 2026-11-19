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
  "sdl2compat|2.32.74|https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz|sdl2-compat-2.32.74"
  "supertux|0.7.0|https://github.com/SuperTux/supertux/releases/download/v0.7.0/SuperTux-v0.7.0-Source.tar.gz|SuperTux-v0.7.0-Source"
  # Libraries for the SDL2 game ports (SuperTux, SuperTuxKart), built from source per app.
  # Box2D 3.1.1 (MIT): 2D rigid-body physics for Prism Spire.
  "box2d|3.1.1|https://github.com/erincatto/box2d/archive/refs/tags/v3.1.1.tar.gz|box2d-3.1.1"
  "zlib|1.3.2|https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz|zlib-1.3.2"
  "libpng|1.6.58|https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.58.tar.gz|libpng-1.6.58"
  "physfs|3.2.0|https://github.com/icculus/physfs/archive/refs/tags/release-3.2.0.tar.gz|physfs-release-3.2.0"
  "fmt|12.1.0|https://github.com/fmtlib/fmt/releases/download/12.1.0/fmt-12.1.0.zip|fmt-12.1.0"
  "glm|1.0.1|https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz|glm-1.0.1"
  "freetype|2.13.3|https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz|freetype-VER-2-13-3"
  "sdl2image|2.8.10|https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.10/SDL2_image-2.8.10.tar.gz|SDL2_image-2.8.10"
  "openal|1.24.3|https://github.com/kcat/openal-soft/archive/refs/tags/1.24.3.tar.gz|openal-soft-1.24.3"
  "ogg|1.3.6|https://github.com/xiph/ogg/releases/download/v1.3.6/libogg-1.3.6.tar.gz|libogg-1.3.6"
  "vorbis|1.3.7|https://github.com/xiph/vorbis/releases/download/v1.3.7/libvorbis-1.3.7.tar.gz|libvorbis-1.3.7"
  "harfbuzz|14.6.0|https://github.com/harfbuzz/harfbuzz/releases/download/14.6.0/harfbuzz-14.6.0.tar.xz|harfbuzz-14.6.0"
  "mbedtls|3.6.7|https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2|mbedtls-3.6.7"
  "curl|8.23.0|https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.gz|curl-8.23.0"
  "libjpeg|3.1.4.1|https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.4.1/libjpeg-turbo-3.1.4.1.tar.gz|libjpeg-turbo-3.1.4.1"
  # Host tool for apps/supertuxkart/make-assets.sh (precompresses textures to ASTC).
  "astcenc|5.7.0|https://github.com/ARM-software/astc-encoder/archive/refs/tags/5.7.0.tar.gz|astc-encoder-5.7.0"
  "stk|1.5|https://github.com/supertuxkart/stk-code/releases/download/1.5/SuperTuxKart-1.5-src.tar.gz|SuperTuxKart-1.5-src"
  "doomgeneric|e257a09|https://github.com/ozkl/doomgeneric/archive/e257a09527062bfb986b6e5e0780839b6cbd5b71.tar.gz|doomgeneric-e257a09527062bfb986b6e5e0780839b6cbd5b71"
  "chocolate-doom|2.3.0|https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-2.3.0.tar.gz|chocolate-doom-chocolate-doom-2.3.0"
  "libdragon|39d0d60|https://github.com/DragonMinded/libdragon/archive/39d0d6096130836a65710da6730290701d060272.tar.gz|libdragon-39d0d6096130836a65710da6730290701d060272"
  # tiny3d (MIT), the 3D library of most N64brew Jam 2024 games: built against our libdragon for the N64 3D work (docs/research/tiny3d-hle.md)
  "tiny3d|6f5ac0b|https://github.com/HailToDodongo/tiny3d/archive/6f5ac0ba098f86d04e549ea754834f1be3c150e1.tar.gz|tiny3d-6f5ac0ba098f86d04e549ea754834f1be3c150e1"
  "quakegeneric|1305210|https://github.com/erysdren/quakegeneric/archive/13052102577c629650cf07a46151a4b6e1b19c3c.tar.gz|quakegeneric-13052102577c629650cf07a46151a4b6e1b19c3c"
  "idquake|bf4ac42|https://github.com/id-Software/Quake/archive/bf4ac424ce754894ac8f1dae6a3981954bc9852d.tar.gz|Quake-bf4ac424ce754894ac8f1dae6a3981954bc9852d"
  "librequake|0.09-beta|https://github.com/lavenderdotpet/LibreQuake/releases/download/v0.09-beta/lite.zip|librequake-0.09-lite"
  "freedoom|0.13.0|https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip|freedoom-0.13.0"
  # Git checkouts (for host tools with submodules): <url>.git#<full commit>; the commit is the pin.
  "n64recomp|598ce20|https://github.com/N64Recomp/N64Recomp.git#598ce2080004a5672d604dceadbda436045307b7|N64Recomp-598ce20"
  # Sparse git checkouts: a 5th field lists the only paths fetched (blobless, depth 1, no submodules).
  # N64brew Game Jam 2024 (MIT): the harness, the few harness assets our minigames load, and the 2D
  # minigames we recompile. Other minigames are never fetched (Hydra Harmonics bundles a commercial
  # font). Never run git commands that need every blob (ls-tree -l, grep, log -p) in this checkout:
  # a blobless clone fetches missing blobs on demand.
  "n64jam2024|0dea720|https://github.com/n64brew/N64brew-GameJam2024.git#0dea720fc295797ae779302425791c01cb67d244|N64brew-GameJam2024-0dea720|/*.c /*.h /Makefile /LICENSE /README.md /.gitmodules /assets/squarewave.ttf /assets/core/StartButton.png /assets/core/AButton.png /assets/core/Start.wav /assets/core/Countdown.wav /assets/core/Stop.wav /assets/core/Winner.wav /assets/minigames/landgrab.txt /assets/minigames/mallard.txt /code/landgrab/ /assets/landgrab/ /code/mallard/ /assets/mallard/ /assets/minigames/hydraharmonics.txt /code/hydraharmonics/ /assets/hydraharmonics/ !/assets/hydraharmonics/*.ttf /assets/minigames/swordstrike.txt /code/swordstrike/ /assets/swordstrike/ !/assets/swordstrike/*.wav /assets/minigames/tohubohu.txt /code/tohubohu/ /assets/tohubohu/ !/assets/tohubohu/*.blend"
  # Hydra Harmonics bundles a commercial font (Superclarendon); the "!" pattern keeps it out, and
  # apps/n64-hydraharmonics builds with an OFL font instead.
  # Single files (fonts and header-only libraries) land in <extracted-dir>/<file name>.
  "stb_truetype|4e9b368|https://raw.githubusercontent.com/nothings/stb/4e9b368bfc071e97206ea5c6adba68dd0cbf8897/stb_truetype.h|stb"
  "stb_vorbis|4e9b368|https://raw.githubusercontent.com/nothings/stb/4e9b368bfc071e97206ea5c6adba68dd0cbf8897/stb_vorbis.c|stb"
  "font-chakrapetch-regular|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-Regular.ttf|fonts"
  "font-chakrapetch-semibold|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-SemiBold.ttf|fonts"
  "font-chakrapetch-bold|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/ChakraPetch-Bold.ttf|fonts"
  "font-chakrapetch-ofl|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/chakrapetch/OFL.txt|fonts/ChakraPetch"
  "font-pressstart2p|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/pressstart2p/PressStart2P-Regular.ttf|fonts"
  "font-pressstart2p-ofl|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/pressstart2p/OFL.txt|fonts/PressStart2P"
  # Coustard (OFL), a Clarendon-style slab serif: Hydra Harmonics' text font in place of Superclarendon.
  "font-coustard|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/coustard/Coustard-Regular.ttf|fonts"
  "font-coustard-ofl|86aced9|https://raw.githubusercontent.com/google/fonts/86aced91de05cd4f96a06838124cca45be337b2b/ofl/coustard/OFL.txt|fonts/Coustard"
)

fetch() {
  local name="$1" version="$2" url="$3" dir="$4" sparse="${5:-}"
  if [[ "$url" == *.git#* && -n "$sparse" ]]; then
    local repo="${url%%#*}" commit="${url##*#}"
    if [[ ! -d "$SRC/$dir/.git" ]]; then
      echo "[$name] sparse checkout of $repo @ $commit"
      git init -q "$SRC/$dir"
      git -C "$SRC/$dir" remote add origin "$repo"
    fi
    # Re-applied on every run, so adding a path to the list fetches just that path.
    # shellcheck disable=SC2086
    git -C "$SRC/$dir" sparse-checkout set --no-cone $sparse
    git -C "$SRC/$dir" fetch -q --depth 1 --filter=blob:none origin "$commit"
    git -C "$SRC/$dir" -c advice.detachedHead=false checkout -q "$commit"
    grep -q "^$name $version " "$LOCK" || echo "$name $version $commit" >> "$LOCK"
    echo "[$name] checked out ($(git -C "$SRC/$dir" ls-files -t | grep -c "^H") files) to $SRC/$dir"
    return
  fi
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
    *) tar -xf "$tarball" -C "$SRC" ;;  # gz, xz and bz2 (bsdtar/GNU tar auto-detect)
  esac
  rm -f "$tarball"
  echo "[$name] extracted to $SRC/$dir"
}

want="${1:-}"
for d in "${DEPS[@]}"; do
  IFS='|' read -r name version url dir sparse <<<"$d"
  if [[ -z "$want" || "$want" == "$name" ]]; then fetch "$name" "$version" "$url" "$dir" "$sparse"; fi
done
