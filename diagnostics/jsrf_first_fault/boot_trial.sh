#!/bin/sh
# Boot the title once, with play.sh's environment rather than the scripted
# harness's, and leave a log that can be scored.
#
#   boot_trial.sh <name> <0|1 batching> [seconds]
#
# Score it on how many NtOpenFile lines the log holds: 1342 is the title
# screen, 1408 is New Game. A run that stalls on the logos has far fewer.
#
# The scripted harness turns on PAD_TRACE, SEQ_TRACE, FUNC_HIT_TRACE and
# SCENE_REPORT and reports every 10 s; play.sh turns on none of them and
# reports every 30 s. This project already knows instrumentation weight changes
# what the title does, so a boot under the scripted harness is not evidence
# about a boot under play.sh. Only the pad script is added, because the run has
# to be unattended, and it is the lightest of the four.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
. "$ROOT/diagnostics/jsrf_first_fault/run_common.sh"
NAME=$1; BATCH=$2; SECS=${3:-90}
BIN="$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault"
OUT="$ROOT/build-macos/jsrf-first-fault/boot/$NAME"
SCRATCH="/tmp/jsrf-boot-$NAME"
jsrf_require_current_binary; jsrf_require_idle; jsrf_require_game
rm -rf "$OUT" "$SCRATCH"; mkdir -p "$OUT" "$SCRATCH"
jsrf_stage_hdd >/dev/null
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_USB=1 RECOMP_PAD_INJECT=1 \
RECOMP_PAD_SCRIPT="@diagnostics/jsrf_first_fault/pad/new_game.pad" \
RECOMP_REPORT_MS=30000 \
RECOMP_METAL_BATCH="$BATCH" \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$BIN" > "$OUT/stderr.log" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM $PID 2>/dev/null; sleep 4; kill -9 $PID 2>/dev/null ) &
W=$!
wait $PID; kill $W 2>/dev/null
rm -rf "$SCRATCH"
