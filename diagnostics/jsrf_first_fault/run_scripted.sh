#!/bin/sh
# An unattended playthrough: drive the title with RECOMP_PAD_SCRIPT and report
# where its own state machine got to.
#
# Reaching the freeze used to need a person to select New Game, which made
# every measurement past the title screen cost a human playthrough -- and an
# expensive measurement is what tempts you into inferring one instead. Two
# things replace the person:
#
#   RECOMP_PAD_SCRIPT   a timed list of presses rather than a blind pulse
#   RECOMP_SEQ_TRACE    CActSequence::m_dwNextMethod, named, on every change
#
# The second is what makes the first tunable: the log says "WaitEndTitle ->
# SwitchOnGlobal at t=31.4", so the next press goes where it is needed instead
# of where it was guessed.
#
# Usage:  run_scripted.sh <outname> "<schedule>" [seconds]
#         run_scripted.sh <outname> @path/to/file [seconds]
#
# Check `pgrep -x jsrf_first_fault` first -- two instances fight over the
# controller and the pad, and it looks like a dead schedule.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NAME="${1:?usage: run_scripted.sh <outname> <schedule> [seconds]}"
SCHED="${2:?usage: run_scripted.sh <outname> <schedule> [seconds]}"
LIMIT="${3:-180}"
OUT="$ROOT/build-macos/jsrf-first-fault/render-investigation/$NAME"
SCRATCH="${PLAY_SCRATCH:-/tmp/jsrf-scripted-$NAME}"
# Which binary. Overridable so a regenerated tree can be A/B-ed against
# an archived one without rebuilding either: the pair of gen trees is
# the experiment, and swapping RECOMP_GEN_DIR under one build dir would
# destroy whichever binary you are not testing.
BIN="${JSRF_BIN:-$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault}"

. "$ROOT/diagnostics/jsrf_first_fault/run_common.sh"
jsrf_require_current_binary
jsrf_require_idle
jsrf_require_game

rm -rf "$OUT" "$SCRATCH"; mkdir -p "$OUT" "$SCRATCH"
jsrf_stage_hdd

echo "schedule: $SCHED"
echo "binary:   $BIN"
echo "log:      $OUT/stderr.log   (${LIMIT}s)"
cd "$ROOT" || exit 1
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_PAD_INJECT=1 RECOMP_PAD_TRACE=1 \
RECOMP_PAD_SCRIPT="$SCHED" RECOMP_SEQ_TRACE=1 RECOMP_FUNC_HIT_TRACE=1 \
RECOMP_SCENE_REPORT=1 RECOMP_REPORT_MS="${REPORT_MS:-10000}" \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$BIN" \
  > "$OUT/stderr.log" 2>&1 &
PID=$!
( sleep "$LIMIT"; kill -TERM $PID 2>/dev/null; sleep 5; kill -9 $PID 2>/dev/null ) &
WATCH=$!
wait $PID
kill $WATCH 2>/dev/null
rm -rf "$SCRATCH"

echo "=== sequence (the title's own state machine) ==="
grep '\[JSRF-SEQ\]' "$OUT/stderr.log"
echo "=== presses fired ==="
grep '\[PAD-SCRIPT\] t=' "$OUT/stderr.log" | tail -20
echo "=== scene / fatal ==="
grep -E '\[JSRF-SCENE\] root=|\[JSRF-FATAL\]' "$OUT/stderr.log" | tail -6
# The open count is the boot and disc-cache path, and NOTHING about which
# screen the run reached. It read 131 at the title, in the VS menu and in
# gameplay on 15 Sep 2026 -- the same 22 paths with the same multiplicities --
# because every staged emulated-hdd now carries a complete Media/Cache and the
# title skips StartBuildCache. The 1342/1408 gates it used to carry were timing
# the cache build. The sequence block at the top of this output is the scene
# verdict; this is disc traffic and is labelled as such.
echo "=== files opened (disc traffic only -- NOT a scene marker) ==="
printf 'NtOpenFile=%s distinct=%s\n' \
    "$(grep -c 'NtOpenFile' "$OUT/stderr.log")" \
    "$(grep -o 'path=.*' "$OUT/stderr.log" | sort -u | wc -l | tr -d ' ')"
