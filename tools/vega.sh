#!/usr/bin/env bash
# Dev loop helper for pure-native Vega OS apps on a USB-attached Fire TV stick (developer mode).
#
#   tools/vega.sh build   <app-dir> [armv7|aarch64] [Debug|Release]
#   tools/vega.sh install <app-dir> [arch] [type]        # install the built .vpkg
#   tools/vega.sh launch  <component-id>                 # NB: force-kills a running instance first
#   tools/vega.sh resume  <component-id>                 # foreground an existing instance (like the launcher)
#   tools/vega.sh stop    <component-id>
#   tools/vega.sh run     <app-dir> <component-id> [arch] [type]   # build+install+launch
#   tools/vega.sh key     KEY_RIGHT [KEY_ENTER ...]      # inject remote/gamepad buttons (Linux key names)
#   tools/vega.sh hold    KEY_UP 800                      # hold a button for N ms
#   tools/vega.sh shot    <out.png> [max-width]          # screenshot the TV, pull to host, optional downscale
#   tools/vega.sh logs    <seconds> [grep-pattern]       # stream device logs for N seconds
#
# Requires the Vega SDK (`source ~/vega/env`). SPDX-License-Identifier: MIT
set -euo pipefail

# shellcheck disable=SC1090
source "${VEGA_ENV:-$HOME/vega/env}" >/dev/null 2>&1 || { echo "Vega SDK env not found" >&2; exit 1; }

cmd="${1:-}"; shift || true

pkg_path() {  # <app-dir> <arch> <type>
  local dir="$1" arch="$2" type; type="$(echo "$3" | tr '[:upper:]' '[:lower:]')"
  ls "$dir"/build/"$arch"-"$type"/local/packages/*.vpkg 2>/dev/null | head -1
}

# Stops an app by its exact package id. `vega device terminate-app --appName` matches packages by
# prefix: asked for com.firetvnative.supertuxkart.main it force-stops com.firetvnative.supertux and
# leaves SuperTuxKart running. vlcm on the device matches the package id exactly.
stop_app() {  # <component-id or package id>
  local pkg="${1%.main}"
  vega device run-cmd --command "vlcm terminate-app --pkg-id $pkg --force >/dev/null 2>&1; true" >/dev/null 2>&1 || true
}

case "$cmd" in
  build)
    dir="$1"; arch="${2:-armv7}"; type="${3:-Debug}"
    (cd "$dir" && vega build -t "$arch" -b "$type")
    ;;
  install)
    dir="$1"; arch="${2:-armv7}"; type="${3:-Debug}"
    p="$(pkg_path "$dir" "$arch" "$type")"; [[ -n "$p" ]] || { echo "no vpkg built in $dir" >&2; exit 1; }
    # In-place updates of a running app can fail ("Internal Error") and silently keep the OLD build,
    # so stop the app first and fall back to uninstall + install.
    comp="$(grep -m1 '^id = ' "$dir/manifest.toml" | sed -E 's/id = "(.*)"/\1/').main"
    stop_app "$comp"
    if ! vega device install-app --packagePath "$p" | tee /dev/stderr | grep -q success; then
      echo "update failed; reinstalling clean" >&2
      vega device uninstall-app --appName "$comp" >/dev/null 2>&1 || true
      vega device install-app --packagePath "$p"
    fi
    # install-app stages the package in the device's /tmp, a RAM-backed tmpfs, and leaves it
    # there: a 250 MB game package then costs 250 MB of the stick's 1 GB of RAM. Remove it.
    vega device run-cmd --command "rm -f /tmp/$(basename "$p")" >/dev/null 2>&1 || true
    ;;
  launch)
    vega device launch-app --appName "$1"
    ;;
  resume)
    vega device run-cmd --command "vlcm launch-app pkg://$1"
    ;;
  stop)
    stop_app "$1"
    ;;
  run)
    dir="$1"; comp="$2"; arch="${3:-armv7}"; type="${4:-Debug}"
    "$0" build "$dir" "$arch" "$type"
    "$0" install "$dir" "$arch" "$type"
    "$0" stop "$comp" >/dev/null 2>&1 || true
    "$0" launch "$comp"
    ;;
  key)
    cmds=""
    for k in "$@"; do cmds+="inputd-cli button_press $k >/dev/null 2>&1; sleep 0.15; "; done
    vega device run-cmd --command "$cmds"
    ;;
  hold)
    # NB: `inputd-cli button_press KEY 1 --holdDuration N` is INVALID (prints usage, rc=255);
    # the working form is `button_press KEY --holdDuration N`.
    vega device run-cmd --command "inputd-cli button_press $1 --holdDuration ${2:-500} >/dev/null 2>&1"
    ;;
  shot)
    out="$1"; maxw="${2:-}"
    remote="/tmp/vegashot_$$.png"
    # gwsi-tool-screenshooter works for app_user. (Plain `screenshooter` fails to allocate its
    # buffer and hangs holding ~220 MB, so always bound it with a timeout.)
    vega device run-cmd --command "timeout 20 gwsi-tool-screenshooter $remote >/dev/null 2>&1; pkill -9 -x gwsi-tool-screenshooter 2>/dev/null; true"
    vega device copy-from -s "$remote" -o "$out" >/dev/null
    vega device run-cmd --command "rm -f $remote" >/dev/null 2>&1 || true
    if [[ -n "$maxw" ]]; then sips -Z "$maxw" "$out" --out "$out" >/dev/null; fi
    echo "$out"
    ;;
  logs)
    secs="${1:-10}"; pat="${2:-}"
    tmp="$(mktemp)"
    ( timeout "$secs" vega device start-log-stream > "$tmp" 2>&1 || true )
    if [[ -n "$pat" ]]; then grep -E "$pat" "$tmp" || true; else cat "$tmp"; fi
    rm -f "$tmp"
    ;;
  *)
    sed -n '2,12p' "$0"; exit 1;;
esac
