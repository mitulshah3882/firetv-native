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
T3D_RSP=$(ls -d "$ROOT"/third_party/_src/tiny3d-*/src/t3d/rsp 2>/dev/null | head -1)  # rsp_tiny3d.h for hle_t3d.c
[[ -n "$T3D_RSP" ]] && INC+=(-I"$T3D_RSP")
# Generated code: big, so compile in parallel; NDEBUG drops N64Recomp's per-instruction asserts.
pids=()
for f in "$GEN"/funcs_*.c "$GEN"/ignored_stubs.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  if [[ ! -f "$o" || "$f" -nt "$o" ]]; then
    cc -std=gnu11 -O2 -DNDEBUG -w "${INC[@]}" -c "$f" -o "$o" & pids+=($!)
  fi
done
for p in ${pids[@]+"${pids[@]}"}; do wait "$p"; done
c++ -std=c++20 -O2 -DNDEBUG "${INC[@]}" -c "$ROOT/n64/runtime/src/n64rt.cpp" -o "$OUT/obj/n64rt.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_libdragon.c" -o "$OUT/obj/hle.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_audio.c" -o "$OUT/obj/hle_audio.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_rdpq.c" -o "$OUT/obj/hle_rdpq.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_rspq.c" -o "$OUT/obj/hle_rspq.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/gfx_gl.c" -o "$OUT/obj/gfx_gl.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_t3d.c" -o "$OUT/obj/hle_t3d.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/hle_coro.c" -o "$OUT/obj/hle_coro.o"
cc -std=gnu11 -O2 -Wall "${INC[@]}" -c "$ROOT/n64/runtime/src/n64_main.c" -o "$OUT/obj/main.o"
# shellcheck disable=SC2086
c++ "$OUT"/obj/*.o $LIBS -o "$OUT/$NAME"
cp "$GEN/$NAME.bin" "$OUT/"
[[ -f "$GEN/$NAME.z64" ]] && cp "$GEN/$NAME.z64" "$OUT/"

cd "$OUT"
if [[ "${2:-}" == "--shot" ]]; then
  # A hidden window with a GL context (the dummy video driver has no GL); see n64_main.c.
  N64_ASSETS="$OUT/" N64_SHOT="$ROOT/build/n64/$NAME-host.png" \
    N64_SHOT_FRAMES="${3:-30}" "./$NAME"
else
  N64_ASSETS="$OUT/" "./$NAME"
fi
