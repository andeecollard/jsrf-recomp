#!/bin/sh
# Boot the title once, with play.sh's environment rather than the scripted
# harness's, and leave a log that can be scored.
#
#   boot_trial.sh <name> <0|1 batching> [seconds]
#   RECOMP_SEQ_REPORT=1 boot_trial.sh <name> <0|1 batching> [seconds]
#
# THE NtOpenFile SCORE IS RETIRED. This header used to say "1342 is the title
# screen, 1408 is New Game", and that is no longer true of any run taken in this
# tree. Those numbers were counting the DISC CACHE BUILD: the run that produced
# 1410 wrote Media/Cache into the staged HDD, and every emulated-hdd staged
# since carries a complete cache plus JSRF_CACHE_COMPLETE.CMP, so the title
# skips StartBuildCache and the count collapses to 131 in EVERY scene --
# measured 15 Sep 2026 at the title, in the VS menu and in gameplay, the same
# 22 distinct paths with the same multiplicities in all three. The gate reports
# the opposite of the truth, so it is gone rather than merely recalibrated.
#
# THE GATE THAT WORKS is the title's own top-level state,
# CActSequence::m_dwNextMethod, via RECOMP_SEQ_REPORT. It is OPT-IN here and
# deliberately not switched on by default, because it starts a 100 Hz sampler
# thread and the whole point of this script is to boot under play.sh's weight
# rather than the scripted harness's -- see below. So:
#
#   * for the hang this script exists to chase (an interactive session stuck on
#     the SEGA screen), run it WITHOUT the sampler and read the free signals in
#     the tail below: a title that is still presenting frames and still issuing
#     draws did not hang, whatever state it is in.
#   * to say WHICH screen it reached, pass RECOMP_SEQ_REPORT=1 and accept that
#     the sampler is now part of what you measured.
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
RECOMP_OHCI_ATTACH=1 RECOMP_PAD_INJECT=1 \
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

# Did it hang, and if the sampler was on, where did it get to?
#
# "Still presenting" is the signal that costs nothing and answers the question
# this script was written for. A title stuck on the SEGA screen stops advancing
# the presented-frame counter; one sitting on a screen legitimately does not.
echo
echo "=== boot trial: $NAME (RECOMP_METAL_BATCH=$BATCH) ==="
printf '  frames presented: %s\n' \
    "$(grep -o 'presented frame [0-9]*' "$OUT/stderr.log" | tail -1 | awk '{print $3}')"
printf '  draws issued:     %s\n' \
    "$(grep -o 'draw #[0-9]*' "$OUT/stderr.log" | tail -1 | tr -d 'draw #')"
printf '  guest faults:     %s\n' "$(grep -c 'FIRST GUEST FAULT' "$OUT/stderr.log")"
SEQ=$(grep '\[JSRF-SEQ\] now=' "$OUT/stderr.log" | tail -1)
if [ -n "$SEQ" ]; then
    printf '%s\n' "$SEQ" | sed 's/^ */  /'
else
    echo "  SCENE: not measured -- re-run with RECOMP_SEQ_REPORT=1 if you need"
    echo "         to know which screen. The open count in this log is the boot"
    echo "         and cache path only and cannot tell one screen from another."
fi
