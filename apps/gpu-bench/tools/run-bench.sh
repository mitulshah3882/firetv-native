#!/usr/bin/env bash
# Runs gpu-bench on the attached Fire TV stick and collects its results from the device log.
#
#   apps/gpu-bench/tools/run-bench.sh [-q 'only=fill,alu&soak=120'] [-t timeout-s] <out-dir>
#
# Writes <out-dir>/log.txt (the raw log stream), <out-dir>/results.psv (one BENCH|... line per
# measurement), <out-dir>/caps.txt and <out-dir>/free.txt (memory before and after). Stops our
# other com.firetvnative.* apps first so they don't compete for memory or the GPU. Needs the app
# installed (tools/vega.sh install apps/gpu-bench armv7 Release) and `source ~/vega/env`.
#
# SPDX-FileCopyrightText: 2026 Mitul Shah
# SPDX-License-Identifier: MIT
set -uo pipefail
query=""; timeout=900
while getopts "q:t:" o; do
  case "$o" in q) query="$OPTARG" ;; t) timeout="$OPTARG" ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
out="${1:?usage: run-bench.sh [-q query] [-t timeout] <out-dir>}"
mkdir -p "$out"
# shellcheck disable=SC1090
source "${VEGA_ENV:-$HOME/vega/env}" >/dev/null 2>&1
comp=com.firetvnative.gpubench.main

# `vega device start-log-stream` is a wrapper over python and node: stop the whole process tree,
# or the stream keeps running and the next one fails (the device allows one at a time).
kill_tree() {
  local child
  for child in $(pgrep -P "$1"); do kill_tree "$child"; done
  kill "$1" 2>/dev/null
}

for app in $(vega device running-apps 2>/dev/null | grep '^com.firetvnative\.' ); do
  echo "stopping $app"
  vega device terminate-app --appName "$app" >/dev/null 2>&1
done
sleep 3
vega device run-cmd --command 'free -m; uptime' > "$out/free.txt" 2>&1

vega device start-log-stream > "$out/log.txt" 2>&1 &
streamer=$!
sleep 3
uri="pkg://$comp"
[[ -n "$query" ]] && uri="$uri?$query"
start=$(date +%s)
# Wake the stick / dismiss the screensaver first: it otherwise reclaims the foreground ~5 s after launch.
vega device run-cmd --command "inputd-cli button_press KEY_LEFT >/dev/null 2>&1; sleep 1; inputd-cli button_press KEY_LEFT >/dev/null 2>&1" >/dev/null 2>&1
sleep 2
vega device run-cmd --command "vlcm launch-app '$uri'" >/dev/null 2>&1
echo "launched $uri"
backgrounds=0
results=0; last_result=$(date +%s)
# The screensaver takes the foreground after ~5 minutes without input (which parks the app's render
# thread): press a key the app ignores every minute.
last_key=$(date +%s)
while :; do
  sleep 5
  if (( $(date +%s) - last_key >= 60 )); then
    vega device run-cmd --command "inputd-cli button_press KEY_LEFT >/dev/null 2>&1" >/dev/null 2>&1
    last_key=$(date +%s)
  fi
  if grep -q "BENCH DONE" "$out/log.txt"; then break; fi
  # If something took the foreground anyway, bring the app back (a resume, not a relaunch) and say
  # so: the measurement running at that moment is invalid.
  n=$(grep "user.info" "$out/log.txt" | grep -c "gpubench.*lifecycle: background")
  if (( n > backgrounds )); then
    backgrounds=$n
    echo "WARNING: app was backgrounded at $(date +%T); resuming (discard the test running then)"
    vega device run-cmd --command "inputd-cli button_press KEY_LEFT >/dev/null 2>&1; vlcm launch-app pkg://$comp" >/dev/null 2>&1
  fi
  if grep -qE "memory-critical: request to terminate pid|gpubench.*(crash|SIGSEGV)" "$out/log.txt"; then
    echo "app killed or crashed; see $out/log.txt"; break
  fi
  if (( $(date +%s) - start > timeout )); then echo "timeout"; break; fi
  # Watchdog: no test takes minutes; a long silence means the app or the GPU is stuck.
  n=$(grep -cE "BENCH\||SOAK\|" "$out/log.txt")
  if (( n > results )); then results=$n; last_result=$(date +%s); fi
  if (( $(date +%s) - last_result > 150 )); then
    echo "STALLED: no result for 150 s after: $(grep "user.info" "$out/log.txt" | grep -o "BENCH|.*" | tail -1 | cut -c1-120)"
    break
  fi
done
echo "elapsed $(( $(date +%s) - start )) s"
vega device run-cmd --command 'free -m; uptime' >> "$out/free.txt" 2>&1
kill_tree "$streamer"; wait "$streamer" 2>/dev/null
# vegaplat lines appear twice in the stream (two log facilities): keep the user.info copy.
grep "user.info" "$out/log.txt" | grep -o "BENCH|.*" > "$out/results.psv"
grep "user.info" "$out/log.txt" | grep -oE "(CAPS|SOAK)\|.*" > "$out/caps.txt"
grep "user.info" "$out/log.txt" | grep -E "GL error|failed|incomplete|shader |program |unavailable" | grep -vE "BENCH\||CAPS\|" > "$out/errors.txt"
echo "$(wc -l < "$out/results.psv") results, $(wc -l < "$out/errors.txt") error lines -> $out"
