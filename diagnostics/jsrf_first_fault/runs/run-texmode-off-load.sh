#!/bin/sh
# THE A/B NOBODY HAS EVER RUN: RECOMP_TEXMODE_APPROX=0.
#
# paths.conf sets RECOMP_TEXMODE_APPROX=1, so every run in this project's
# history has been the `on` arm. Until commit 6cc7b22 today the switch was a
# presence test, so `=0` ALSO turned it on and this arm could not be expressed.
#
# THE SCENE IS THE TITLE SCREEN AND YOU DO NOT TOUCH THE CONTROLLER.
# Boot, let the title animation play over the street scene, and the run kills
# itself at t=55 so the two arms are scene-matched by construction.
#
# WHAT IT SCORES. In the `on` arm the title animation's ellipse and bars are
# drawn with correct coverage and correct occlusion of the city behind them,
# and shaded to EXACTLY (0,0,0) for 23 consecutive frames while the scene
# behind them varies normally. If this arm renders them in colour, the
# approximation is the defect. If they are still exact zero, it is not, and
# that is just as useful.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_TEXMODE-OFF-LOAD-KEEP.log"
FBDIR="$HOME/jsrf-build/fbdump-${STAMP}-TEXMODE-OFF-LOAD-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$FBDIR" "$HOME/jsrf-build/preserved-logs"

echo "  log:    $LOG"
echo "  frames: $FBDIR"
echo "  DRIVE IT: main menu -> Load -> wait 15 s -> close the window."

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_TEXMODE_APPROX=0 \
RECOMP_SURFACE_CENSUS=120:10 \
RECOMP_FB_DUMP_DRAW=1:40 \
RECOMP_FB_DUMP="$FBDIR/fb" \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1
wait

echo
echo "=== the switch state the MODEL read (must say off) ==="
grep -a 'TEXMODE_APPROX' "$LOG" | sort -u
echo "=== census verdicts ==="
grep -a 'verdict:' "$LOG" | sed 's/^ *//' | cut -c1-60 | uniq -c
