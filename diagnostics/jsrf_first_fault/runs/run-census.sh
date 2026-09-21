#!/bin/sh
# THE LOAD-MENU BLACK SCREEN, SETTLED IN ONE ~40 SECOND RUN.
#
# TWO SYMPTOMS, ONE RUN. Both are screens the title draws while the main 3D
# scene is not being drawn, so they may be one mechanism:
#
#   A. boot -> main menu -> select Load -> wait ~15 s     (the black Load menu)
#   B. play on into a level transition                     (the missing
#                                                           "Now Loading" screen)
#
# Do A first because it is 40 seconds. Then, in the same run or a second one,
# do B. The stride covers ~400 s of play, so one session can hold both.
# Close the window when done.
#
# This is the bundle's own launcher plus two switches, so it plays exactly like
# a normal session and leaves paths.conf alone. Its log goes straight into
# preserved-logs under its own name -- it does NOT touch last-run.log.
#
# READ THE POSITIVE CONTROL FIRST. Reports at t=16-22, while the MAIN MENU is
# on screen and you can see it, must say `verdict: PICTURE-HELD`. If they do
# not, the instrument never looked and no later zero means anything.
#
#   GPU-BLACK     the draws produced no pixels; the loss is BEFORE write-back
#   OWED          the frame is in a Metal texture and guest RAM never got it
#   PICTURE-HELD  ...while the screen is black -> the loss is past the surface
#   NOTHING-HELD  the census read nothing. Not a finding.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-CENSUS-KEEP.log"
FBDIR="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-CENSUS-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$FBDIR" "$HOME/jsrf-build/preserved-logs"

echo "  engine:  $APP/Contents/MacOS/jsrf-engine"
echo "  log:     $LOG"
echo "  frames:  $FBDIR"
echo
echo "  Boot, main menu, press Load, wait ~15 s, then close the window."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_SURFACE_CENSUS=120:10 \
RECOMP_FB_DUMP_DRAW=1:26 \
RECOMP_FB_DUMP="$FBDIR/fb" \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== every census verdict, in order ==="
grep -a 'verdict:' "$LOG" | sed 's/^ *//'
echo
echo "full log: $LOG"
