#!/usr/bin/env bash
# Builds N64brew Game Jam 2024 minigames (pinned in third_party/deps.lock) as one static libdragon
# ELF + ROM, ready for tools/n64-recomp.sh. See n64/jam24/Makefile for how the DSO-based harness
# is linked statically.
#
#   tools/n64-jam24.sh landgrab            # -> <jam>/build-jam_landgrab/jam_landgrab.elf, jam_landgrab.z64
#   tools/n64-jam24.sh landgrab mallard    # both minigames in one program: jam_landgrab_mallard
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export N64_INST="${N64_INST:-$HOME/n64-toolchain}"
export PATH="$N64_INST/bin:$PATH"
[[ $# -ge 1 ]] || { echo "usage: $0 <minigame>..." >&2; exit 2; }
JAM="$(ls -d "$ROOT"/third_party/_src/N64brew-GameJam2024-* 2>/dev/null | head -1)"
[[ -n "$JAM" ]] || { "$ROOT/tools/fetch-deps.sh" n64jam2024; JAM="$(ls -d "$ROOT"/third_party/_src/N64brew-GameJam2024-* | head -1)"; }
for m in "$@"; do
  [[ -d "$JAM/code/$m" ]] || { echo "minigame '$m' is not in the sparse checkout: add /code/$m/ and /assets/$m/ to n64jam2024 in tools/fetch-deps.sh (check its licences first)" >&2; exit 1; }
done
TARGET="jam_$(IFS=_; echo "$*")"
# Minigames that include tiny3d get it built (once) and linked: libt3d.a with our libdragon, and the
# host glTF -> .t3dm converter.
T3D_ARGS=()
if grep -qs '#include <t3d/' $(for m in "$@"; do echo "$JAM/code/$m/"*.c "$JAM/code/$m/"*.h; done); then
  T3D="$(ls -d "$ROOT"/third_party/_src/tiny3d-* 2>/dev/null | head -1)"
  [[ -n "$T3D" ]] || { "$ROOT/tools/fetch-deps.sh" tiny3d; T3D="$(ls -d "$ROOT"/third_party/_src/tiny3d-* | head -1)"; }
  [[ -f "$T3D/build/libt3d.a" ]] || make -C "$T3D" -j"$(getconf _NPROCESSORS_ONLN)" >/dev/null
  [[ -x "$T3D/tools/gltf_importer/gltf_to_t3d" ]] || make -C "$T3D/tools/gltf_importer" -j"$(getconf _NPROCESSORS_ONLN)" CXX=c++ >/dev/null
  T3D_ARGS=(USE_T3D=1 T3D_DIR="$T3D")
fi
make -C "$JAM" -f "$ROOT/n64/jam24/Makefile" -j"$(getconf _NPROCESSORS_ONLN)" TARGET="$TARGET" MINIGAMES="$*" "${T3D_ARGS[@]+"${T3D_ARGS[@]}"}"
echo "ELF: $JAM/build-$TARGET/$TARGET.elf"
echo "ROM: $JAM/$TARGET.z64"
