#!/bin/sh
# THE LOAD SCREEN SAMPLES THE SURFACE IT IS RENDERING INTO. Does syncing it
# first bring the screen back?
#
# xemu, last 800 flips of the Load-screen trace: 21,573 draws into the render
# target, 15,162 of them binding that same surface as TEXTURE0. Nineteen a
# flip. Our texture path uploads from guest RAM, which only holds the surface
# as of the last sync, so every one of those reads saw a stale frame.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit ~20 s. Close the window.
#
#   run-loadmenu-feedback.sh on     RECOMP_METAL_FEEDBACK_SYNC=1  the candidate
#   run-loadmenu-feedback.sh off    counter only, the control
#
# READ: [SNAP] freshness first, then the snapNNN images, then the [METAL]
# feedback line -- the draw count must be ~19 x flips on the Load screen for
# either arm to mean anything, and "synced" must be 0 on the off arm.
set -u
ARM="${1:-on}"
case "$ARM" in on) SYNC=1;; off) SYNC=0;; *) echo "on|off"; exit 2;; esac
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
TAG="LOADMENU-FEEDBACK-$(echo "$ARM" | tr a-z A-Z)-KEEP"
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_${TAG}.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-${TAG}"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  arm:  feedback_sync=$SYNC"
echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START at the title -> main menu -> LOAD -> sit ~20 s -> close the window."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_METAL_FEEDBACK_SYNC=$SYNC RECOMP_DRAW_MIX=1 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== snapshot freshness: repeats / never-published (both should be 0) ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo "=== the feedback counter, per report ==="
grep -a 'feedback reads' "$LOG" | tail -8
echo "=== what the draws target and sample, last window ==="
awk '/^\[DRAW-MIX\] [0-9]+ distinct/{buf=""} /^\[DRAW-MIX\]/{buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG" | sort -t' ' -k2 -rn | head -14
echo "=== draws per flip (28.0 is the Load screen) ==="
grep -a 'FRAG-ARM\|draws/flip' "$LOG" | tail -4
echo "=== the last 25 snapshots: is the frame black? ==="
grep -a '\[SNAP\]' "$LOG" | tail -25
echo
echo "log:  $LOG"
echo "dump: $DUMP"
