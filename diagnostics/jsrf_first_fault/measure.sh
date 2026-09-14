#!/bin/sh
# One timed, comparable run. For A/B-ing a change against a baseline.
#
# Not run_scripted.sh, deliberately. That script turns on PAD_TRACE, SEQ_TRACE
# and FUNC_HIT_TRACE because it exists to answer "where did the state machine
# get to", and probe weight is already recorded as something that destabilises
# this title -- it provokes the input-poll stall, which then reads as a
# controller fault. A performance number taken under those probes is a number
# about the probes. This turns on the cheapest thing that still says how far
# the run got (SCENE_REPORT) and nothing else.
#
# Not play.sh either: that takes no schedule, so it needs a person.
#
# What to read out of the log, in order of how much it means:
#   [FRAME]        flips, mean, p50/p90/p99/max, over-33ms -- frame pacing
#   [APU-PACE]     gen_hz, starved, empty, min_queued     -- audio starvation
#   [APU-OUT]      drops: full/inactive/failed            -- audio backpressure
#   [JSRF-SCENE]   live= -- 61 is the Corn tutorial. A run that did not get
#                  there is NOT comparable with one that did, whatever its
#                  frame count says.
#
# Usage: measure.sh <outname> [seconds] [pad-file]
#        JSRF_BIN=<path> measure.sh ...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: measure.sh <outname> [seconds] [pad]}"
LIMIT="${2:-150}"
PAD="${3:-$ROOT/diagnostics/jsrf_first_fault/pad/measure.pad}"
OUT="$ROOT/build-macos/jsrf-first-fault/measure/$NAME"
SCRATCH="/tmp/jsrf-measure-$NAME"
BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"

# Which binary, and is it current, and is the disc image the one we think?
# Five scripts here defaulted to build-macos/jsrf-first-fault/build for a
# session while every measurement was taken against a different tree, so a run
# could silently be the old -O0 build. The binary prints its own optimisation
# level at startup ([BUILD]); run_common.sh checks the other half.
. "$ROOT/diagnostics/jsrf_first_fault/run_common.sh"
jsrf_require_current_binary
jsrf_require_idle
jsrf_require_game
rm -rf "$OUT" "$SCRATCH"; mkdir -p "$OUT" "$SCRATCH"
jsrf_stage_hdd

echo "binary: $BIN"
echo "log:    $OUT/stderr.log  (${LIMIT}s)"
cd "$ROOT" || exit 1
# The guest image, resolved from the repo root rather than baked into the
# binary. CLAUDE.md puts the game directory beside the repo root; override
# JSRF_GAME_DIR to point somewhere else.
if [ ! -f "$GAME_DIR/default.xbe" ]; then
    echo "no default.xbe under $GAME_DIR" >&2
    echo "  set JSRF_GAME_DIR to the directory holding your own dump" >&2
    exit 1
fi

RECOMP_XBE_PATH="$GAME_DIR/default.xbe" RECOMP_GAME_DIR="$GAME_DIR" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_USB=1 RECOMP_PAD_INJECT=1 \
RECOMP_PAD_SCRIPT="@$PAD" RECOMP_SCENE_REPORT=1 \
RECOMP_REPORT_MS="${REPORT_MS:-10000}" \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$BIN" > "$OUT/stderr.log" 2>&1 &
PID=$!
( sleep "$LIMIT"; kill -TERM $PID 2>/dev/null; sleep 5; kill -9 $PID 2>/dev/null ) &
WATCH=$!
wait $PID
kill $WATCH 2>/dev/null
rm -rf "$SCRATCH"

echo "--- frame pacing (cumulative, includes boot) ---"
grep '\[FRAME\]' "$OUT/stderr.log" | tail -3
echo "--- audio ---"
grep -E '\[APU-PACE\]|\[APU-OUT\]' "$OUT/stderr.log" | tail -5
echo "--- how far it got ---"
grep -E '\[JSRF-SCENE\] root=' "$OUT/stderr.log" | tail -2
# A real fault prints "FIRST GUEST FAULT" and a symbolised host PC. Do NOT
# count [JSRF-FATAL] lines for this: that is a periodic status line reporting
# the guest's m_bFatal flag, and it prints m_bFatal=0 when all is well, so
# counting it reports nine "fatal lines" for a run that finished cleanly.
printf 'guest faults: %s\n' "$(grep -c 'FIRST GUEST FAULT' "$OUT/stderr.log")"
grep -A 4 'FIRST GUEST FAULT' "$OUT/stderr.log" | grep -E 'SIGNAL|HOST PC' | head -4
