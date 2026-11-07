#!/usr/bin/env bash
# Builds libdragon (pinned in third_party/deps.lock) with the N64 MIPS toolchain, installs it into
# $N64_INST, and builds its example programs (the ELFs we statically recompile).
#
#   N64_INST=$HOME/n64-toolchain tools/n64-libdragon.sh
#
# The toolchain itself comes from libdragon's tools/build-toolchain.sh (GCC 16 + binutils + newlib,
# about 30-60 minutes, Homebrew deps on macOS). Never uses sudo: N64_INST must be user-writable.
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export N64_INST="${N64_INST:-$HOME/n64-toolchain}"
export PATH="$N64_INST/bin:$PATH"
LD="$(ls -d "$ROOT"/third_party/_src/libdragon-*/ 2>/dev/null | head -1)"
[[ -n "$LD" ]] || { "$ROOT/tools/fetch-deps.sh" libdragon; LD="$(ls -d "$ROOT"/third_party/_src/libdragon-*/ | head -1)"; }
command -v mips64-elf-gcc >/dev/null || { echo "no toolchain in $N64_INST (see libdragon's tools/build-toolchain.sh)" >&2; exit 1; }
J="$(getconf _NPROCESSORS_ONLN)"
cd "$LD"
make install-mk
make clobber
make -j"$J" libdragon tools
make install tools-install
make clobber
# videoplayer needs ffmpeg and a downloaded video; everything else should build.
make -j"$J" -k examples || true
find examples -name '*.elf' | sort
