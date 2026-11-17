#!/usr/bin/env bash
# Renders Chroma Orbit's Appstore images with the Mac host preview, which runs the same renderer and
# shaders as the TV build. Scripted shots are deterministic (fixed seeds, a fixed 60 Hz step), so the
# same frames come out every run.
#
#   apps/chroma-orbit/store/make-kit.sh
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(dirname "$HERE")"
SHOTS="$(cd "$APP/../.." && pwd)/build/host-$(basename "$APP")"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# frame <script> <frame> <out>: render the script up to that frame; JPEG for .jpg, opaque PNG for .png
frame() {
  "$APP/host-preview.sh" shots "$1" "$2" 1 >/dev/null 2>&1
  local src="$SHOTS/shots-$1/shot-$(printf %04d "$2").png"
  case "$3" in
    *.jpg) sips -s format jpeg -s formatOptions 90 "$src" --out "$HERE/$3" >/dev/null ;;
    *.png) cp "$src" "$HERE/$3" ;;
  esac
  echo "$3 <- $1, frame $2"
}

frame title 1800 screenshot-1.jpg   # the title: the menu, and the attract game turning on the right
frame tour 6960 screenshot-2.jpg    # world 4, Ember: a full board and a solar flare
frame tour 2760 screenshot-3.jpg    # world 2, Aurora
frame tour 7680 screenshot-4.jpg    # a supernova: the star swallows the rings
frame tour 8400 screenshot-5.jpg    # world 5, Night City
frame keyart 60 background-1920x1080.jpg
frame logo 60 "logo.png"
sips -z 720 1280 "$HERE/logo.png" --out "$HERE/app-icon-1280x720.png" >/dev/null && rm "$HERE/logo.png"
echo "app-icon-1280x720.png <- logo, frame 60"
sips -s format jpeg -s formatOptions 100 "$APP/icon/icon.png" --out "$TMP/icon.jpg" >/dev/null   # flatten the alpha
sips -s format png -z 114 114 "$TMP/icon.jpg" --out "$HERE/icon-114.png" >/dev/null
echo "icon-114.png <- icon/icon.png"
