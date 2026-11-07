#!/usr/bin/env bash
# Builds a recompiled N64 program for the host (macOS/Linux) against the host SDL3 build, and runs
# it. With --shot, renders headlessly and saves a PNG instead of opening a window.
#
#   n64/host-run.sh helloworld            # window
#   n64/host-run.sh helloworld --shot     # build/n64/helloworld-host.png
#
# Run tools/n64-recomp.sh n64/recomp/<name>.toml first.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="$1"
GEN="$ROOT/build/n64/$NAME"
OUT="$ROOT/build/n64/host-$NAME"
SDL="$ROOT/build/host-sdl3/install"
[[ -f "$SDL/lib/libSDL3.a" ]] || "$ROOT/apps/arcade/host-preview.sh" --frames 1 >/dev/null
[[ -f "$GEN/funcs_0.c" ]] || { echo "run tools/n64-recomp.sh n64/recomp/$NAME.toml first" >&2; exit 1; }
mkdir -p "$OUT/obj"
LIBS=$(sed -n 's/^Libs: //p' "$SDL/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL/lib#")
INC=(-I"$ROOT/build/n64/include" -I"$GEN" -I"$ROOT/n64/runtime/include" -I"$SDL/include")
# Generated code: big, so compile in parallel; NDEBUG drops N64Recomp's per-instruction asserts.
pids=()
for f in "$GEN"/funcs_*.c "$GEN"/ignored_stubs.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  if [[ ! -f "$o" || "$f" -nt "$o" ]]; then
    cc -std=gnu11 -O2 -DNDEBUG -w "${INC[@]}" -c "$f" -o "$o" & pids+=($!)
  fi
done
for p in "${pids[@]}"; do wait "$p"; done
c++ -std=c++20 -O2 -DNDEBUG "${INC[@]}" -c "$ROOT/n64/runtime/src/n64rt.cpp" -o "$OUT/obj/n64rt.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_libdragon.c" -o "$OUT/obj/hle.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/n64_main.c" -o "$OUT/obj/main.o"
# shellcheck disable=SC2086
c++ "$OUT"/obj/*.o $LIBS -o "$OUT/$NAME"
cp "$GEN/$NAME.bin" "$OUT/"

cd "$OUT"
if [[ "${2:-}" == "--shot" ]]; then
  SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software N64_ASSETS="$OUT/" N64_SHOT="$ROOT/build/n64/$NAME-host.png" \
    N64_SHOT_FRAMES="${3:-30}" "./$NAME"
else
  N64_ASSETS="$OUT/" "./$NAME"
fi
