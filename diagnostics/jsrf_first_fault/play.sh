#!/bin/sh
# Just play it. No pad script, no tracing, no measurement.
#
# run_scripted.sh exists to drive the title unattended, and it always sets
# RECOMP_PAD_SCRIPT. Past the title screen START is the PAUSE button, and that
# schedule keeps firing START until t=69.5 -- long after the game has reached
# gameplay around t=47 -- so six synthetic STARTs land in play and pause the
# game under you. It reads exactly like a dead controller. Do not use that
# script to play; use this one.
#
# Also omitted deliberately: RECOMP_PAD_TRACE, RECOMP_SEQ_TRACE,
# RECOMP_FUNC_HIT_TRACE, RECOMP_SCENE_REPORT. Heavy instrumentation
# destabilises this title -- it provokes the input-poll stall, which then reads
# as a controller fault. Add probes back one at a time when measuring.
#
# Usage:  play.sh [seconds]        (default: until you quit)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LIMIT="${1:-0}"
BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"

# Which binary, and is it current? Five scripts here defaulted to
# build-macos/jsrf-first-fault/build for a session while every measurement was
# taken against a different tree, so a run could silently be the old -O0 build.
# The binary now prints its own optimisation level at startup ([BUILD]); this
# checks the other half, that it is not simply stale.
if [ ! -x "$BIN" ]; then
    echo "no binary at $BIN" >&2
    echo "  build it:  cmake -S diagnostics/jsrf_first_fault -B ${BIN%/*} && cmake --build ${BIN%/*} -j 6" >&2
    exit 1
fi
NEWER=$(find "$ROOT/src" "$ROOT/diagnostics/jsrf_first_fault" -name '*.c' -o -name '*.h' -o -name '*.m' 2>/dev/null \
        | grep -v ' 2\.c$' | while read -r f; do [ "$f" -nt "$BIN" ] && echo "$f"; done | head -3)
if [ -n "$NEWER" ]; then
    echo "WARNING: $BIN is older than these sources -- rebuild, or you are measuring the previous build:" >&2
    echo "$NEWER" | sed 's/^/    /' >&2
    [ -n "${JSRF_ALLOW_STALE:-}" ] || { echo "  (set JSRF_ALLOW_STALE=1 to run anyway)" >&2; exit 1; }
fi
SCRATCH="${PLAY_SCRATCH:-/tmp/jsrf-play}"
STOCK="$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd"

if pgrep -x jsrf_first_fault >/dev/null 2>&1; then
    echo "REFUSING: jsrf_first_fault is already running (pid $(pgrep -x jsrf_first_fault | tr '\n' ' '))" >&2
    exit 2
fi
[ -d "$STOCK" ] || { echo "no emulated-hdd at $STOCK" >&2; exit 1; }

# A disposable copy: the guest writes saves, and the stock tree is a baseline.
rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
cp -R "$STOCK" "$SCRATCH/hdd"

echo "binary: $BIN"
echo "log:    $SCRATCH/stderr.log"
echo "Click the game window so it has focus, then press START to begin."
cd "$ROOT" || exit 1
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_USB=1 RECOMP_PAD_INJECT=1 \
RECOMP_REPORT_MS="${REPORT_MS:-30000}" \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$BIN" > "$SCRATCH/stderr.log" 2>&1 &
PID=$!
if [ "$LIMIT" -gt 0 ] 2>/dev/null; then
    ( sleep "$LIMIT"; kill -TERM $PID 2>/dev/null; sleep 5; kill -9 $PID 2>/dev/null ) &
    WATCH=$!
    wait $PID; kill $WATCH 2>/dev/null
else
    wait $PID
fi
