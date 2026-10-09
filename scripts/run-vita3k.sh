#!/usr/bin/env bash
# Run a VPK in a source-built Vita3K under Xvfb + Mesa lavapipe and screenshot it.
# usage: run-vita3k.sh <app.vpk> <out_prefix> [seconds_before_shot] [total_seconds]
set -u
vpk="$1" out="$2" shot="${3:-30}" total="${4:-60}"
V3K="${VITA3K:-/home/user/deps/vita3k-preqt/build/linux-ninja-clang/bin/Release/Vita3K}"
export DISPLAY="${DISPLAY:-:99}" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/vita3k-xdg}"
mkdir -p "$XDG_RUNTIME_DIR"
pgrep -x Xvfb >/dev/null || { Xvfb "$DISPLAY" -screen 0 1280x800x24 >/dev/null 2>&1 & sleep 2; }
pkill -x Vita3K 2>/dev/null; sleep 1
timeout "$total" "$V3K" ${V3K_ARGS:-} ${vpk:+"$vpk"} > "$out.log" 2>&1 &
pid=$!
sleep "$shot"
import -window root "$out.png"
wait "$pid"
echo "vita3k exit=$? log=$out.log shot=$out.png"
