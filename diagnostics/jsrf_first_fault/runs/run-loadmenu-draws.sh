#!/bin/sh
# ONE LOAD-SCREEN FRAME, ONE DRAW AT A TIME.
#
# Our Load-screen frame is 20 untextured alpha-blended draws, 7 DXT3-textured
# draws and 1 composite (RECOMP_DRAW_MIX, 21 Sep 2026), and the render target
# is black on the GPU at the flip. This captures the render target after each
# of 24 consecutive draws, armed by the black condition itself (all-zero
# presented frame with >=10 draws a flip for six reports -- two armed on the intro at t=18), so it lands on the
# Load screen however fast the player gets there.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit ~20 s. Close the window.
#
# READ: [DRAW-CAP] ARMED must appear. Then diff fbdrawNNN.bmp in sequence:
# which draws change the surface, and what they write. The DRAW-MIX rows say
# whether the DXT3 bytes were even nonzero in guest RAM.
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
TAG="LOADMENU-DRAWS-KEEP"
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_${TAG}.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-${TAG}"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"
echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START at the title -> main menu -> LOAD -> sit ~20 s -> close the window."
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_DRAW_MIX=1 RECOMP_CLIP_AUDIT=1 \
RECOMP_FB_DUMP_DRAW=1:100000 RECOMP_FB_DUMP_DRAW_ON_BLACK=6 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1
echo
echo "=== did the capture arm, and where? ==="
grep -a 'DRAW-CAP' "$LOG" | head -30
echo "=== the draw mix in the last window ==="
awk '/^\[DRAW-MIX\] [0-9]+ distinct/{buf=""} /^\[DRAW-MIX\]/{buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG"
echo "=== clip audit, last two reports ==="
grep -a "audit" "$LOG" | tail -12
echo "=== last snapshots ==="
grep -a '\[SNAP\]' "$LOG" | tail -5
echo "log:  $LOG"
echo "dump: $DUMP"
