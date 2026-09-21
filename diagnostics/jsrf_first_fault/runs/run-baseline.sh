#!/bin/sh
# THE CLEAN BASELINE. NOTHING EXPENSIVE IS ARMED.
#
# No RECOMP_SURFACE_CENSUS (it drains the GPU every stride-th flip by design),
# no RECOMP_FB_DUMP_DRAW, no traces. This is exactly what double-clicking
# JSRF.app does, plus a log kept under its own name instead of last-run.log.
#
# WHAT IT IS FOR, in order:
#   1. Audio and frame timing with no instrument in the way. Every run today
#      had the census armed, so "choppy intro music" has never been heard
#      without it. If the intro is smooth here, the census was the cause.
#   2. The post-regeneration binary's first outing. The [ITAIL] section below
#      is the Roboy fix: 0x00075EB3 now has a body AND a table row, so an
#      unresolved tail jump to it must not appear.
#   3. A control the next instrumented run can be scored against.
#
# PLAY IT NORMALLY. Let the intro animation run so the music can be judged,
# then play as you like. Close the window when you are done.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_BASELINE-NO-CENSUS-KEEP.log"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs"

echo "  log: $LOG"
echo "  Nothing expensive is armed. Let the intro play, then play normally."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== the census must be ABSENT (0 = clean baseline) ==="
grep -ac 'SURFACE-CENSUS' "$LOG"
echo "=== audio ==="
grep -a '\[APU-SDL2\] depth buckets' "$LOG" | tail -1
grep -a '\[APU-SDL2\] max_submit_gap' "$LOG" | tail -1
echo "=== frame timing ==="
grep -a '\[STAGE\] per frame' "$LOG" | tail -1
echo "=== the Roboy fix: this must print NOTHING ==="
grep -a '\[ITAIL\]' "$LOG" | head -5
echo "=== (end) ==="
echo "log: $LOG"
