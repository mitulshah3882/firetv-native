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
make -C "$JAM" -f "$ROOT/n64/jam24/Makefile" -j"$(getconf _NPROCESSORS_ONLN)" TARGET="$TARGET" MINIGAMES="$*"
echo "ELF: $JAM/build-$TARGET/$TARGET.elf"
echo "ROM: $JAM/$TARGET.z64"
