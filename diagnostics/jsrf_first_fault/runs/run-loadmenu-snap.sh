#!/bin/sh
# THE LOAD MENU, WITH NORMAL RENDERING AND PRESENTED-FRAME CAPTURES.
#
# NOTHING IS FORCED. No FRAG_FORCE, no magenta clear, no TEXMODE arm. The only
# question this run asks is whether the defect the PLAYER SEES is still there,
# and what the frames the player actually saw look like.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit on the black/fading screen
# for ~20 s, then close the window. Reaching Load is the whole point; three
# runs on 21 Sep never left the attract loop and measured nothing.
#
# WHAT IS NEW. snapNNN is the FLIP_STALL copy -- the only image the display
# path ever shows -- written beside reportNNN, which is the live surface part
# way through composing a frame. Until tonight only the live surface was ever
# dumped, and at the title screen the two disagree completely.
#
# AND THE SNAPSHOT IS CHECKED FOR FRESHNESS, because it can lie the other way:
# s_snap is republished at the flip, so if the guest STOPS FLIPPING the pool
# keeps handing out the last good frame and the files look healthy while the
# screen is black. Every dump prints [SNAP] with the pool's frame sequence and
# the source surface, and says outright when the sequence has not moved.
#
# READ IT IN THIS ORDER:
#   1. [SNAP] lines -- is each file a fresh frame, a REPEAT, or nothing at all?
#   2. the images, snapNNN first
#   3. only then the counters
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-SNAP-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-SNAP-KEEP"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START at the title -> main menu -> LOAD -> sit ~20 s -> close the window."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== did we reach the Load menu? (on= 4-12 is the attract loop) ==="
grep -ao '\[APU-VOICE\] on=[0-9]*' "$LOG" | tail -1
echo "=== snapshot freshness: fresh frames vs repeats ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo "=== the last 25 snapshots ==="
grep -a '\[SNAP\]' "$LOG" | tail -25
echo
echo "log:  $LOG"
echo "dump: $DUMP"
