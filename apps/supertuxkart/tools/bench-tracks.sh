#!/usr/bin/env bash
# Frame-rate and memory benchmark of SuperTuxKart on the attached Fire TV stick: each track is raced
# by AI karts in STK's profile mode (--profile-time, the camera on one kart), and vegaplat's
# VP_FPS_LOG lines (frame rate, the game thread's busy share), the 3D view's final scale and the
# system's `free -m` are summarised per track.
#
#   apps/supertuxkart/tools/bench-tracks.sh [-s seconds] [-k karts] [-t tune] [track ...] > results.md
#   (BENCH_LOGS=<dir> keeps the raw logs.)
#
# Default: every race track, 30 s each after loading, 4 karts. -t passes the launch URI's tune=
# option (e.g. scale:0.5). Needs the app installed and `source ~/vega/env`.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail
secs=30; karts=4; tune=""
while getopts "s:k:t:" o; do
  case "$o" in s) secs="$OPTARG" ;; k) karts="$OPTARG" ;; t) tune="$OPTARG" ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
tracks=("$@")
[[ ${#tracks[@]} -gt 0 ]] || tracks=(abyss black_forest candela_city cocoa_temple cornfield_crossing
  fortmagma gran_paradiso_island hacienda lighthouse mines minigolf olivermath ravenbridge_mansion
  sandtrack scotland snowmountain snowtuxpeak stk_enterprise volcano_island xr591 zengarden)
comp=com.firetvnative.supertuxkart.main
# BENCH_LOGS=<dir> keeps each track's raw log stream there.
tmp="${BENCH_LOGS:-}"
if [[ -n "$tmp" ]]; then mkdir -p "$tmp"; else tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT; fi

# `vega device start-log-stream` is a wrapper script over python and node: stop the whole tree, or
# the stream keeps running and the next one fails (the device allows one at a time).
kill_tree() {
  local child
  for child in $(pgrep -P "$1"); do kill_tree "$child"; done
  kill "$1" 2>/dev/null
}

echo "| Track | fps (mean) | fps (lowest 10 s) | worst frame (ms) | game thread busy | 3D scale (end) | load (s) | GPU shared (MB) | available (MB) | swap used (MB) |"
echo "|---|---|---|---|---|---|---|---|---|---|"
# Waits (up to 30 s) until STK is no longer running: a launch sent while the previous instance is
# still shutting down (after its race, profile mode exits by itself) is dropped.
wait_stopped() {
  local _
  for _ in $(seq 1 15); do
    vega device running-apps 2>/dev/null | grep -qx "$comp" || return 0
    sleep 2
  done
}

for t in "${tracks[@]}"; do
  vega device run-cmd --command "vlcm terminate-app --pkg-id ${comp%.main} --force >/dev/null 2>&1; true" >/dev/null 2>&1  # exact match (see tools/vega.sh stop_app)
  wait_stopped
  sleep 1
  log="$tmp/$t.log"
  vega device start-log-stream > "$log" 2>&1 &
  streamer=$!
  sleep 2
  query="args=--track=$t,--profile-time=$secs,--numkarts=$karts"
  [[ -n "$tune" ]] && query="tune=$tune&$query"
  start=$(date +%s)
  # An idle stick's screensaver takes the foreground a couple of seconds after a launch (and STK
  # then waits, parked, for the whole run): wake it first.
  vega device run-cmd --command "inputd-cli button_press KEY_LEFT >/dev/null 2>&1; sleep 1; vlcm launch-app 'pkg://$comp?$query'" >/dev/null 2>&1
  mem=""; first=""; resumed=0
  for _ in $(seq 1 60); do
    sleep 5
    # Sent to the background before the race started (screensaver)? Bring the same instance back.
    bg=$(grep -c "supertuxkart.main)<F>: Trigger event BACKGROUND in state VISIBLE. Completed: true" "$log")
    if [[ -z "$first" && $bg -gt $resumed && $resumed -lt 3 ]]; then
      resumed=$bg
      echo "  ($t: backgrounded before the race, resuming)" >&2
      vega device run-cmd --command "inputd-cli button_press KEY_LEFT >/dev/null 2>&1; sleep 1; vlcm launch-app pkg://$comp" >/dev/null 2>&1
    fi
    if [[ -z "$first" ]] && grep -q "\] fps " "$log"; then first=$(( $(date +%s) - start )); fi
    # Memory snapshot once the race has run ~15 s.
    if [[ -n "$first" && -z "$mem" && $(( $(date +%s) - start - first )) -ge 15 ]]; then
      mem=$(vega device run-cmd --command 'free -m' 2>/dev/null | awk '/^Mem:/{s=$5; a=$7} /^Swap:/{w=$3} END{print s" "a" "w}')
    fi
    grep -q "profile: Number of frames" "$log" && break
  done
  kill_tree "$streamer"; wait "$streamer" 2>/dev/null
  # vegaplat lines appear twice in the stream (two log facilities): keep one copy.
  grep "user.info" "$log" | grep "\] fps " | sed 's/.*\] fps //' > "$tmp/$t.fps"
  read -r shared avail swap <<< "${mem:-- - -}"
  # The 3D view's scale at the end (dynamic resolution logs every change).
  scale=$(grep "user.info" "$log" | grep -o "3D view scale[^(]*" | tail -1 | sed "s/ *$//" | grep -o "[0-9.]*$")
  awk -v t="$t" -v load="${first:-?}" -v sh="$shared" -v av="$avail" -v sw="$swap" -v sc="${scale:--}" '
    { gsub(",", ""); gsub("%", ""); fps = $1; worst = $3; busy = ""
      for (i = 1; i <= NF; i++) if ($i == "busy") busy = $(i+1)
      n++; if (n == 1) next                       # the first interval includes the start countdown
      m++; sum += fps; if (m == 1 || fps < lo) lo = fps; if (worst > w) w = worst; bs += busy }
    END { if (m == 0) { printf "| %s | no data | | | | | %s | %s | %s | %s |\n", t, load, sh, av, sw; exit }
          printf "| %s | %.1f | %.1f | %.1f | %.0f%% | %s | %s | %s | %s | %s |\n", t, sum/m, lo, w, bs/m, sc, load, sh, av, sw }' "$tmp/$t.fps"
done
vega device run-cmd --command "vlcm terminate-app --pkg-id ${comp%.main} --force >/dev/null 2>&1; true" >/dev/null 2>&1  # exact match (see tools/vega.sh stop_app)
