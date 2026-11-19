#!/usr/bin/env bash
# Host regression tests (macOS; no Fire TV involved). Builds into build/tests and runs:
#   unit    tests/unit, against the real runtime sources: VADPCM decoding (vs libdragon's reference
#           codec and the source WAVs), mono/outline font layers, address masking and memory
#           lanes in the generated recomp.h, the portable 64-bit multiply
#   elf     decision 51 on the device toolchain: no runtime object defines a weak n64_* symbol,
#           and an optional recompiled function is a weak reference (needs the Vega SDK's clang, else skipped)
#   <game>  each recompiled game in build/n64 (helloworld, brew_volley, jam_landgrab, jam_hydraharmonics, jam_swordstrike, jam_tohubohu, gj25_cathode_quest; skipped if
#           not generated): its sound effects decoded through its own recompiled wav64 loader vs
#           the source WAVs, and a headless run that must boot and draw a non-black frame
#   chroma_rules         Chroma Orbit's rules (tests/originals): clusters, rings, chains, flares and
#                        a balance band for the greedy AI
#   <game>_audio         each original game's synthesizer rendered offline (its tools/audio-test.sh):
#                        its own checks pass and nothing reaches full scale
#
#   tools/test.sh [-v]        (-v: measurements and logs)
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build/tests"
VERBOSE=""; [[ "${1:-}" == "-v" ]] && VERBOSE=-v
SRC="$ROOT/third_party/_src"
SDL="$ROOT/build/host-sdl3/install"
RT="$ROOT/n64/runtime/src"
FIX="$ROOT/tests/unit/fixture"
T0=$(date +%s)
mkdir -p "$B/include" "$B/unit"

npass=0; nfail=0; nskip=0; failed=()
pass() { echo "PASS $1"; npass=$((npass + 1)); }
fail() { echo "FAIL $1${2:+: $2}"; nfail=$((nfail + 1)); failed+=("$1"); }
skip() { echo "SKIP $1: $2"; nskip=$((nskip + 1)); }
# Runs a command, showing its output on failure (always with -v). Output goes to $B/<log>.
run_logged() {
  local log="$B/$1"; shift
  "$@" >"$log" 2>&1; local rc=$?
  [[ $rc -ne 0 || -n "$VERBOSE" ]] && sed 's/^/  | /' "$log"
  return $rc
}
# Compiles in the background; `wait_all` fails if any compile failed.
pids=()
bg() { "$@" & pids+=($!); }
wait_all() { local ok=0; for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || ok=1; done; pids=(); return $ok; }

LIBDRAGON=$(ls -d "$SRC"/libdragon-*/ 2>/dev/null | head -1); LIBDRAGON=${LIBDRAGON%/}
JAM=$(ls -d "$SRC"/N64brew-GameJam2024-*/ 2>/dev/null | head -1); JAM=${JAM%/}
[[ -n "$LIBDRAGON" ]] || { echo "libdragon source missing: run tools/fetch-deps.sh"; exit 1; }
[[ -f "$SDL/lib/libSDL3.a" ]] || "$ROOT/apps/arcade/host-preview.sh" --frames 1 >/dev/null
[[ -f "$SDL/lib/libSDL3.a" ]] || { echo "no host SDL3 (apps/arcade/host-preview.sh builds it)"; exit 1; }
SDL_LIBS=$(sed -n 's/^Libs: //p' "$SDL/lib/pkgconfig/sdl3.pc" | sed "s#\${libdir}#$SDL/lib#")

# ---- recomp.h, regenerated with tools/n64-recomp.sh's own transform (so a change there is tested
# here without recompiling a game) ----
GEN_PY="$B/gen-recomp-h.py"
awk '/include\/recomp\.h" <<.PY.$/ {on = 1; next} on && /^PY$/ {exit} on' "$ROOT/tools/n64-recomp.sh" > "$GEN_PY"
RECOMP_SRC=$(sed -n 's#^python3 -I - "\$ROOT/\(third_party/_src/N64Recomp-[^/]*/include/recomp\.h\)".*#\1#p' "$ROOT/tools/n64-recomp.sh" | head -1)
if [[ ! -s "$GEN_PY" || -z "$RECOMP_SRC" ]]; then
  echo "cannot find recomp.h's generator in tools/n64-recomp.sh"; exit 1
fi
python3 -I "$GEN_PY" "$ROOT/$RECOMP_SRC" "$B/include/recomp.h.new" || { echo "recomp.h generation failed"; exit 1; }
# Replaced only when it changed, so the games' compiled objects stay valid.
cmp -s "$B/include/recomp.h.new" "$B/include/recomp.h" && rm "$B/include/recomp.h.new" || mv "$B/include/recomp.h.new" "$B/include/recomp.h"

# ---- unit ----
U="$B/unit"
CODEC="$LIBDRAGON/tools/audioconv64"
INC=(-I"$B/include" -I"$ROOT/n64/runtime/include" -I"$SDL/include")
T3D_RSP=$(ls -d "$SRC"/tiny3d-*/src/t3d/rsp 2>/dev/null | head -1)  # rsp_tiny3d.h for hle_t3d.c
[[ -n "$T3D_RSP" ]] && INC+=(-I"$T3D_RSP")
UFLAGS=(-g -O1 -fsanitize=address -fno-omit-frame-pointer -DTEST_REPO_ROOT="\"$ROOT\"" -DTEST_BUILD_DIR="\"$U\""
        -I"$ROOT/tests" -I"$ROOT/tests/unit" -I"$FIX" -I"$RT" "${INC[@]}")
bg c++ -std=c++20 "${UFLAGS[@]}" -w -c "$RT/n64rt.cpp" -o "$U/n64rt.o"
bg cc -std=gnu11 "${UFLAGS[@]}" -Wall -c "$RT/hle_libdragon.c" -o "$U/hle_libdragon.o"
bg cc -std=gnu11 "${UFLAGS[@]}" -Wall -c "$RT/hle_rdpq.c" -o "$U/hle_rdpq.o"
bg cc -std=gnu11 "${UFLAGS[@]}" -Wall -c "$RT/hle_rspq.c" -o "$U/hle_rspq.o"
bg cc -std=gnu11 "${UFLAGS[@]}" -Wall -c "$RT/gfx_gl.c" -o "$U/gfx_gl.o"
bg cc -std=gnu11 "${UFLAGS[@]}" -Wall -c "$RT/hle_t3d.c" -o "$U/hle_t3d.o"
bg cc -std=gnu11 -O2 -w -I"$CODEC" -I"$CODEC/vadpcm" -c "$CODEC/libvadpcm.c" -o "$U/libvadpcm.o"
for t in main test_vadpcm test_font test_memory test_dmult fixture_stubs; do
  f="$ROOT/tests/unit/$t.c"; [[ $t == fixture_stubs ]] && f="$FIX/stubs.c"
  bg cc -std=gnu11 "${UFLAGS[@]}" -I"$CODEC/vadpcm" -Wall -Wextra -c "$f" -o "$U/$t.o"
done
if wait_all && c++ -fsanitize=address "$U"/*.o $SDL_LIBS -o "$U/n64_unit"; then
  if run_logged unit.log "$U/n64_unit" $VERBOSE; then pass unit; else fail unit "see the checks above"; fi
else
  fail unit "build failed"
fi

# ---- elf: decision 51 with the device's compiler ----
VEGA_TC=$(ls -d "$HOME"/vega/sdk/vega-sdk/main/*/workspace/env/VodkaClangArmv7-*/runtime 2>/dev/null | tail -1)
if [[ -z "$VEGA_TC" || ! -x "$VEGA_TC/mac/bin/clang" ]]; then
  skip elf "no Vega SDK armv7 toolchain under ~/vega/sdk (tools/check-packages.sh checks built packages)"
else
  E="$B/elf"; mkdir -p "$E"
  EFLAGS=(--target=arm-linux-gnueabihf --sysroot="$VEGA_TC/sysroot-armv7" -march=armv7-a -mfloat-abi=hard -O2 -w
          -I"$FIX" "${INC[@]}")
  LIBCXX=$(ls -d "${VEGA_TC%/VodkaClangArmv7-*}"/VodkaSysrootLibcpp*/runtime/sysroot-armv7/usr/include/c++/v1 2>/dev/null | tail -1)
  rm -f "$E"/*.o
  for f in "$RT"/*.c "$RT"/*.cpp; do
    std=(-std=gnu11); [[ $f == *.cpp ]] && std=(-std=c++20 -nostdinc++ -isystem "$LIBCXX")
    [[ $f == *.cpp && -z "$LIBCXX" ]] && continue  # no libc++ headers: the C files still say most
    bg "$VEGA_TC/mac/bin/clang" "${std[@]}" "${EFLAGS[@]}" -c "$f" -o "$E/$(basename "${f%.*}").o"
  done
  if ! wait_all; then
    fail elf "the runtime doesn't compile for armv7"
  else
    bad=$(for o in "$E"/*.o; do "$VEGA_TC/mac/bin/llvm-nm" "$o" | awk -v o="$(basename "$o")" '$(NF-1) ~ /^[WV]$/ && $NF ~ /^n64_/ {print o ": " $NF}'; done)
    ref=$("$VEGA_TC/mac/bin/llvm-nm" "$E/hle_rdpq.o" | awk '$NF == "n64_sprite_get_palette" {print $(NF-1)}')
    if [[ -n "$bad" ]]; then
      fail elf "weak definitions of recompiled functions tie with N64Recomp's weak RECOMP_FUNC: $(echo $bad)"
    elif [[ "$ref" != "w" ]]; then
      fail elf "hle_rdpq.o should reference n64_sprite_get_palette weakly (nm 'w'), got '${ref:-nothing}'"
    else
      pass elf
    fi
  fi
fi

# ---- recompiled games ----
# <name> <headless frames> <min non-black, in 1/10000 of the frame> [<rom path>=<source wav> ...]
game() {
  local name=$1 frames=$2 min=$3; shift 3
  local GEN="$ROOT/build/n64/$name" O="$B/n64/$name"
  if [[ ! -f "$GEN/funcs_0.c" ]]; then
    skip "$name" "not recompiled (tools/n64-recomp.sh n64/recomp/$name.toml)"; return
  fi
  mkdir -p "$O/obj"
  local GINC=(-I"$GEN" "${INC[@]}")
  # The generated code is big: compile it once (and again only when regenerated), as host-run.sh does.
  for f in "$GEN"/funcs_*.c "$GEN"/ignored_stubs.c; do
    local o="$O/obj/$(basename "$f" .c).o"
    [[ -f "$o" && "$o" -nt "$f" && "$o" -nt "$B/include/recomp.h" ]] || bg cc -std=gnu11 -O2 -fno-strict-aliasing -DNDEBUG -w "${GINC[@]}" -c "$f" -o "$o"
  done
  bg c++ -std=c++20 -O2 -fno-strict-aliasing -DNDEBUG "${GINC[@]}" -c "$RT/n64rt.cpp" -o "$O/n64rt.o"
  for s in hle_libdragon hle_rdpq hle_rspq hle_audio gfx_gl hle_t3d hle_coro n64_main; do bg cc -std=gnu11 -O2 -fno-strict-aliasing -Wall "${GINC[@]}" -c "$RT/$s.c" -o "$O/$s.o"; done
  # game_check's wav mode needs the program's wav64 loader (helloworld has none).
  local wavload=""; grep -q 'n64_wav64_open(' "$GEN/funcs.h" || wavload=-DGAME_CHECK_WAV64_LOAD
  bg cc -std=gnu11 -O2 -Wall "${GINC[@]}" -I"$RT" ${1:+-DGAME_CHECK_WAV} $wavload -c "$ROOT/tests/n64/game_check.c" -o "$O/game_check.o"
  if ! wait_all; then fail "$name" "build failed"; return; fi
  # Link order as in host-run.sh: the recompiled objects first (on Mach-O the first of two weak
  # definitions wins; decision 51).
  local runtime=("$O/n64rt.o" "$O/hle_libdragon.o" "$O/hle_rdpq.o" "$O/hle_rspq.o" "$O/gfx_gl.o" "$O/hle_t3d.o" "$O/hle_coro.o")
  if ! c++ "$O"/obj/*.o "${runtime[@]}" "$O/hle_audio.o" "$O/n64_main.o" $SDL_LIBS -o "$O/$name" ||
     ! c++ "$O"/obj/*.o "${runtime[@]}" "$O/game_check.o" $SDL_LIBS -o "$O/game_check"; then
    fail "$name" "link failed"; return
  fi
  local ok=1 why=""
  if [[ $# -gt 0 ]]; then
    run_logged "n64/$name/wav.log" env SDL_AUDIO_DRIVER=dummy "$O/game_check" "$GEN/" wav "$@" || { ok=0; why="sound effects"; }
  fi
  # Headless run: boots, plays, saves frame <frames> and exits; nothing fatal on the way.
  rm -f "$O/shot.png"
  local errs
  # (video: a hidden window with a GL context, as n64_main.c makes for N64_SHOT; the dummy driver has no GL)
  if ! run_logged "n64/$name/run.log" env SDL_AUDIO_DRIVER=dummy \
       N64_ASSETS="$GEN/" N64_SHOT="$O/shot.png" N64_SHOT_FRAMES="$frames" "$O/$name"; then
    ok=0; why="${why:+$why, }headless run exited non-zero"
  elif errs=$(grep -E 'FATAL|ASSERTION|VADPCM needs|unsupported VADPCM|not a wav64' "$B/n64/$name/run.log" | head -3) &&
       [[ -n "$errs" ]]; then
    sed 's/^/  | /' <<<"$errs"
    ok=0; why="${why:+$why, }errors in the run log"
  elif ! run_logged "n64/$name/png.log" "$O/game_check" png "$O/shot.png" "$min"; then
    ok=0; why="${why:+$why, }black frame"
  fi
  if [[ $ok == 1 ]]; then pass "$name"; else fail "$name" "$why"; fi
}

game helloworld 30 5
wavs=()
for w in halt hit win; do wavs+=("rom:/$w.wav64=$LIBDRAGON/examples/brew-volley/assets/$w.wav"); done
game brew_volley 120 3000 "${wavs[@]}"
if [[ -n "$JAM" ]]; then
  wavs=()
  for w in landgrab/buzz landgrab/click landgrab/pop core/Start core/Countdown core/Stop core/Winner; do
    wavs+=("rom:/$w.wav64=$JAM/assets/$w.wav")
  done
  game jam_landgrab 300 3000 "${wavs[@]}"
  # Hydra Harmonics: frame 300 is its instructions panel (CI4 surfaces, sub-rectangle uploads and
  # palettes, the replacement font, our remote icons).
  wavs=()
  for w in aah gulp hit-0 slide-whistle-up drumroll; do
    wavs+=("rom:/hydraharmonics/$w.wav64=$JAM/assets/hydraharmonics/$w.wav")
  done
  game jam_hydraharmonics 300 3000 "${wavs[@]}"
  # Sword Strike: the first tiny3d game (the rspq tap, the host microcode's untextured lit room). Its two
  # sounds are our own, synthesized by n64/jam24/overrides/swordstrike-sfx.py into the build dir.
  wavs=("rom:/core/Start.wav64=$JAM/assets/core/Start.wav")
  for w in wilhelm_scream challengers; do wavs+=("rom:/swordstrike/$w.wav64=$JAM/build-jam_swordstrike/swordstrike-sfx/$w.wav"); done
  game jam_swordstrike 300 3000 "${wavs[@]}"
  # Le Tohu-bohu: textured, skinned, animated tiny3d models (streamed .sdata), depth past the far
  # plane (decision 69), and a stereo song whose 404 KB decode buffer once overwrote the stack
  # (decision 70). Its own sounds are mp3s, which game_check can't read; the harness's WAV is checked.
  game jam_tohubohu 300 3000 "rom:/core/Start.wav64=$JAM/assets/core/Start.wav"
else
  skip jam_landgrab "no N64brew Game Jam 2024 checkout (tools/fetch-deps.sh n64jam2024)"
  skip jam_hydraharmonics "no N64brew Game Jam 2024 checkout (tools/fetch-deps.sh n64jam2024)"
  skip jam_swordstrike "no N64brew Game Jam 2024 checkout (tools/fetch-deps.sh n64jam2024)"
  skip jam_tohubohu "no N64brew Game Jam 2024 checkout (tools/fetch-deps.sh n64jam2024)"
fi

# Cathode Quest 64 (Pyrite64): its swap chain, draw-layer queues, coroutines (the boot logos run in
# one) and BigTex title; frame 1380 is the title screen. Its sound effects are our own
# (n64/pyrite64/jam25-sfx.py, written into the project copy by tools/n64-pyrite64.sh).
P64SFX="$ROOT/build/n64/p64-jam25/assets/sfx"
if [[ -f "$P64SFX/CoinGet.wav" ]]; then
  wavs=()
  for w in CoinGet UiOk PlayerJump00; do wavs+=("rom:/sfx/$w.wav64=$P64SFX/$w.wav"); done
  game gj25_cathode_quest 1380 3000 "${wavs[@]}"
else
  skip gj25_cathode_quest "not built (tools/n64-pyrite64.sh jam25)"
fi

# ---- the original games: rules and synthesizers, no device ----
mkdir -p "$B/originals"
if cc -std=gnu11 -g -O1 -fsanitize=address,undefined -Wall -Wextra -I"$ROOT/tests" -I"$ROOT/apps/chroma-orbit/src" \
     "$ROOT/tests/originals/runner.c" "$ROOT/tests/originals/test_chroma_board.c" \
     "$ROOT/apps/chroma-orbit/src/board.c" "$ROOT/apps/chroma-orbit/src/ai.c" -o "$B/originals/originals_unit" \
     >"$B/originals/build.log" 2>&1; then
  if run_logged originals/unit.log "$B/originals/originals_unit" $VERBOSE; then pass chroma_rules; else fail chroma_rules "see the checks above"; fi
else
  sed 's/^/  | /' "$B/originals/build.log"
  fail chroma_rules "build failed"
fi
synth() {  # <test name> <audio-test script>
  local name=$1 script=$2 pk
  if ! run_logged "originals/$name.log" "$script"; then fail "$name" "its audio test failed"; return; fi
  pk=$(grep -o 'peak [0-9.]*' "$B/originals/$name.log" | awk '$2 > m {m = $2} END {print m + 0}')
  if awk -v p="$pk" 'BEGIN {exit !(p > 0.95)}'; then fail "$name" "peak $pk"; else pass "$name"; fi
}
synth gravity_choir_audio "$ROOT/apps/gravity-choir/tools/audio-test.sh"
synth prism_spire_audio "$ROOT/apps/prism-spire/tools/audio-test.sh"
synth chroma_orbit_audio "$ROOT/apps/chroma-orbit/tools/audio-test.sh"

echo "$npass passed, $nfail failed, $nskip skipped in $(( $(date +%s) - T0 )) s${failed[0]+ (failed: ${failed[*]})}"
[[ $nfail -eq 0 ]]
