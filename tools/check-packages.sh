#!/usr/bin/env bash
# Checks the built Release packages for things that broke silently before (run after building):
#   - the launcher icon is a 512x512 PNG (otherwise the app never shows in "Your Apps & Channels")
#   - the licences and NOTICE ship under assets/licenses/
#   - games carry the loading splash (assets/image/splash.astc) and Vega's system splash
#     (assets/raw/SplashScreenImages.zip)
#   - the shared library needs only libapmf, libc, libm and the loader (anything else fails launch)
#   - no GL function pointer is exported (it would bind to the driver's symbol; decision 45)
#   - recompiled N64 programs link libdragon's recompiled wav64_vadpcm_preload, not a stub (the
#     silent-sound bug of decision 51)
#
#   tools/check-packages.sh [app ...]      (default: the apps the arcade ships)
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
GAMES=" doom quake supertux supertuxkart gravity-choir prism-spire chroma-orbit n64-brew-volley n64-landgrab n64-hydraharmonics n64-swordstrike n64-tohubohu "
fail=0
bad() { echo "  FAIL: $*"; fail=1; }

apps=("$@")
[[ ${#apps[@]} -gt 0 ]] || apps=(arcade sdl-demo $GAMES)

for a in "${apps[@]}"; do
  pkg=$(ls apps/"$a"/build/armv7-release/local/packages/*.vpkg 2>/dev/null | head -1)
  [[ -n "$pkg" ]] || { echo "$a: no Release package (tools/vega.sh build apps/$a armv7 Release)"; fail=1; continue; }
  echo "$a: $(basename "$pkg") ($(( $(stat -f %z "$pkg") / 1048576 )) MB)"
  list=$(tar tf "$pkg" 2>/dev/null)
  has() { grep -qx "$1" <<<"$list"; }

  # Icon: the manifest's @image/icon.png, exactly 512x512.
  if has assets/image/icon.png; then
    tmp=$(mktemp -d)
    tar xf "$pkg" -C "$tmp" assets/image/icon.png 2>/dev/null
    size=$(sips -g pixelWidth -g pixelHeight "$tmp/assets/image/icon.png" 2>/dev/null | awk '/pixel/{print $2}' | paste -sd x -)
    [[ "$size" == "512x512" ]] || bad "icon is $size, not 512x512"
    rm -rf "$tmp"
  else
    bad "no assets/image/icon.png"
  fi

  has assets/licenses/NOTICE.txt || bad "no assets/licenses/NOTICE.txt"
  if [[ "$GAMES" == *" $a "* ]]; then
    has assets/image/splash.astc || bad "no assets/image/splash.astc"
    has assets/raw/SplashScreenImages.zip || bad "no assets/raw/SplashScreenImages.zip"
  fi

  # The shared library: dependencies, exported GL symbols, the VADPCM preload.
  so=$(ls apps/"$a"/build/armv7-release/lib/*.entry_point_factory.so 2>/dev/null | head -1)
  tc=$(ls -d apps/"$a"/build/private/toolchain/armv7/mac/bin 2>/dev/null | head -1)
  if [[ -n "$so" && -n "$tc" ]]; then
    needed=$("$tc/llvm-readelf" -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | paste -sd ' ' -)
    for lib in $needed; do
      case "$lib" in libapmf*|libc.so*|libm.so*|ld-linux*) ;; *) bad "needs $lib (launch fails within ms)";; esac
    done
    gl=$("$tc/llvm-nm" -D --defined-only "$so" 2>/dev/null | awk '$3 ~ /^gl[A-Z]/' | head -3)
    [[ -z "$gl" ]] || bad "exports GL symbols: $(echo $gl | cut -c1-80)"
    dbg="$so.debug"
    if [[ "$a" == n64-* && -f "$dbg" ]]; then
      sz=$("$tc/llvm-nm" -S "$dbg" 2>/dev/null | awk '$4 == "n64_wav64_vadpcm_preload" {print $2}' | head -1)
      if [[ -n "$sz" ]] && (( 16#$sz < 256 )); then
        bad "n64_wav64_vadpcm_preload is a $((16#$sz))-byte stub, not the recompiled function"
      fi
    fi
  fi
done
[[ $fail -eq 0 ]] && echo "all checks passed" || echo "some checks failed"
exit $fail
