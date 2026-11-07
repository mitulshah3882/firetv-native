#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/_src (git-ignored).
# Sources are verified against SHA-256 checksums recorded in third_party/deps.lock
# (first fetch records the checksum; later fetches must match).
#
#   tools/fetch-deps.sh            # fetch everything
#   tools/fetch-deps.sh sdl3       # fetch one dependency
#
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
)

fetch() {
  local name="$1" version="$2" url="$3" dir="$4"
  local tarball="$SRC/$(basename "$url")"
  if [[ -d "$SRC/$dir" ]]; then echo "[$name] already present: $SRC/$dir"; return; fi
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
  tar -xzf "$tarball" -C "$SRC"
  rm -f "$tarball"
  echo "[$name] extracted to $SRC/$dir"
}

want="${1:-}"
for d in "${DEPS[@]}"; do
  IFS='|' read -r name version url dir <<<"$d"
  if [[ -z "$want" || "$want" == "$name" ]]; then fetch "$name" "$version" "$url" "$dir"; fi
done
