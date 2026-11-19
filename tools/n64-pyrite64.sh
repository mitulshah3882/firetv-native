#!/usr/bin/env bash
# Builds a Pyrite64 project (Cathode Quest 64 is Pyrite64's n64/examples/jam25) into a libdragon
# ELF for tools/n64-recomp.sh, against our libdragon and tiny3d pins.
#
#   tools/n64-pyrite64.sh jam25     # -> build/n64/p64-jam25/<romName>.elf
#
# Pyrite64 (MIT, HailToDodongo) is an editor plus a C++ engine; a project is built by the editor,
# which converts the assets, writes scene and asset tables, generates the Makefile and runs make.
# So this script:
#   1. fetches Pyrite64 at the pinned commit into third_party/_src: a sparse checkout without the
#      docs and other examples, the editor's submodules shallow, and the Git LFS files the build
#      reads from GitHub's media endpoint (tools/lfs-fetch.py checks each against its pointer);
#   2. builds the editor once (AppleClang; only its CLI is used);
#   3. copies the project to build/n64/p64-<name>, applies n64/pyrite64/patches/<name>.patch if
#      there is one, and runs `pyrite64 --cli --cmd build` with N64_INST set to a staging prefix:
#      our toolchain plus tiny3d's t3d.mk (the generated Makefiles include $(N64_INST)/include/t3d.mk).
#      The editor copies its engine into the project from n64/engine under its working directory,
#      so it runs from build/n64/p64-work: its data/ plus a copy of the engine with
#      n64/pyrite64/patches/engine.patch applied.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: $0 <example-name, e.g. jam25>}"
export N64_INST_REAL="${N64_INST:-$HOME/n64-toolchain}"

P64_REPO=https://github.com/HailToDodongo/pyrite64.git
P64_GH=HailToDodongo/pyrite64
P64_COMMIT=ec98ba1b028f83e1a09feecd1fe60d39f2c0a278
P64="$ROOT/third_party/_src/pyrite64-${P64_COMMIT:0:7}"
T3D="$(ls -d "$ROOT"/third_party/_src/tiny3d-* 2>/dev/null | head -1)"
[[ -n "$T3D" ]] || { "$ROOT/tools/fetch-deps.sh" tiny3d; T3D="$(ls -d "$ROOT"/third_party/_src/tiny3d-* | head -1)"; }

# The editor's submodules (doctest is only for its tests; tiny3d is linked to ours, below).
# SDL_shadercross vendors SPIR-V tools; its DirectX compiler stays out (the editor builds with
# SDLSHADERCROSS_DXC off).
SUBMODULES=(IconFontCppHeaders ImGuizmo ImNodeFlow ImViewGuizmo SDL SDL_image SDL_shadercross SHA256
            argparse glm imgui libdragon minimp3 quickjs-ng tiny-regex-c)

# ---- 1. fetch --------------------------------------------------------------------------------
if [[ ! -f "$P64/.fetched" ]]; then
  echo "[pyrite64] sparse checkout of $P64_REPO @ ${P64_COMMIT:0:7}"
  [[ -d "$P64/.git" ]] || { git init -q "$P64"; git -C "$P64" remote add origin "$P64_REPO"; }
  git -C "$P64" sparse-checkout set --no-cone '/*' '!/docs/' '!/n64/examples/' "/n64/examples/$NAME/" '!*.blend' '!*.xcf'
  git -C "$P64" fetch -q --depth 1 --filter=blob:none origin "$P64_COMMIT"
  git -C "$P64" -c advice.detachedHead=false checkout -q "$P64_COMMIT"
  for m in "${SUBMODULES[@]}"; do
    git -C "$P64" submodule update -q --init --depth 1 "vendored/$m"
  done
  # vendored/tiny3d is the tiny3d we already pin, at the same commit: link it, don't fetch it twice.
  t3d_pin="$(git -C "$P64" ls-tree HEAD vendored/tiny3d | awk '{print $3}')"
  [[ "$(basename "$T3D")" == "tiny3d-$t3d_pin" ]] || { echo "error: Pyrite64 pins tiny3d $t3d_pin, we pin $(basename "$T3D")" >&2; exit 1; }
  rm -rf "${P64:?}/vendored/tiny3d"
  ln -s "$T3D" "$P64/vendored/tiny3d"
  git -C "$P64/vendored/SDL_shadercross" submodule update -q --init --depth 1 \
    external/SPIRV-Cross external/SPIRV-Headers external/SPIRV-Tools
  # shadercross checks that every vendored project exists, DXC included, before it looks at
  # SDLSHADERCROSS_DXC; an empty stand-in satisfies the check and is never added.
  mkdir -p "$P64/vendored/SDL_shadercross/external/DirectXShaderCompiler"
  echo "# stand-in: SDLSHADERCROSS_DXC is off" > "$P64/vendored/SDL_shadercross/external/DirectXShaderCompiler/CMakeLists.txt"
  python3 -I "$ROOT/tools/lfs-fetch.py" "$P64" "$P64_GH" "$P64_COMMIT" data "n64/examples/$NAME"
  grep -q "^pyrite64 " "$ROOT/third_party/deps.lock" || echo "pyrite64 ${P64_COMMIT:0:7} $P64_COMMIT" >> "$ROOT/third_party/deps.lock"
  touch "$P64/.fetched"
fi
if [[ ! -d "$P64/n64/examples/$NAME" ]]; then
  git -C "$P64" sparse-checkout add "/n64/examples/$NAME/"
  python3 -I "$ROOT/tools/lfs-fetch.py" "$P64" "$P64_GH" "$P64_COMMIT" "n64/examples/$NAME"
fi

# ---- 2. the editor -----------------------------------------------------------------------------
EDITOR_BUILD="$ROOT/build/pyrite64-editor"
EDITOR="$P64/pyrite64"  # its CMakeLists puts the binary in the source directory
if [[ ! -x "$EDITOR" ]]; then
  echo "[pyrite64] building the editor (once, a few minutes)"
  cmake -S "$P64" -B "$EDITOR_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPYRITE64_BUILD_TESTS=OFF >/dev/null
  ninja -C "$EDITOR_BUILD" pyrite64 >/dev/null
fi

# ---- 3. the project ----------------------------------------------------------------------------
STAGE="$ROOT/build/n64/p64-inst"
mkdir -p "$STAGE/include"
for d in bin lib libexec mips64-elf share; do ln -sfn "$N64_INST_REAL/$d" "$STAGE/$d"; done
ln -sfn "$N64_INST_REAL/include/n64.mk" "$STAGE/include/n64.mk"
ln -sfn "$T3D/t3d.mk" "$STAGE/include/t3d.mk"
[[ -f "$T3D/build/libt3d.a" ]] || make -C "$T3D" -j"$(getconf _NPROCESSORS_ONLN)" N64_INST="$N64_INST_REAL" >/dev/null

OUT="$ROOT/build/n64/p64-$NAME"
PATCH="$ROOT/n64/pyrite64/patches/$NAME.patch"
STAMP="$OUT/.source"
WANT="$P64_COMMIT $( [[ -f "$PATCH" ]] && shasum -a 256 "$PATCH" | cut -d" " -f1 || true )"
if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$WANT" ]]; then
  echo "[pyrite64] fresh copy of n64/examples/$NAME"
  rm -rf "${OUT:?}"
  cp -R "$P64/n64/examples/$NAME" "$OUT"
  if [[ -f "$PATCH" ]]; then
    patch -s -f -F 0 -p1 -d "$OUT" -i "$PATCH" </dev/null || { echo "error: $PATCH no longer applies" >&2; exit 1; }
  fi
  echo "$WANT" > "$STAMP"
fi
# The editor reads its templates (data/...) and copies its engine (n64/engine) from the directory
# its executable is in (when both are there), so a copy of it runs from a work directory with its
# data/ and our patched engine.
WORK="$ROOT/build/n64/p64-work"
EPATCH="$ROOT/n64/pyrite64/patches/engine.patch"
if [[ ! -f "$WORK/.engine" || "$(cat "$WORK/.engine")" != "$P64_COMMIT $(shasum -a 256 "$EPATCH" | cut -d' ' -f1)" ]]; then
  rm -rf "${WORK:?}"
  mkdir -p "$WORK/n64"
  ln -s "$P64/data" "$WORK/data"
  cp -R "$P64/n64/engine" "$WORK/n64/engine"
  patch -s -f -F 0 -p1 -d "$WORK/n64/engine" -i "$EPATCH" </dev/null || { echo "error: $EPATCH no longer applies" >&2; exit 1; }
  echo "$P64_COMMIT $(shasum -a 256 "$EPATCH" | cut -d' ' -f1)" > "$WORK/.engine"
fi
cmp -s "$EDITOR" "$WORK/pyrite64" || cp "$EDITOR" "$WORK/pyrite64"
(cd "$WORK" && N64_INST="$STAGE" ./pyrite64 --cli --cmd build "$OUT/project.p64proj")
ELF="$(ls "$OUT"/build/*.elf | head -1)"
echo "ELF: $ELF"
