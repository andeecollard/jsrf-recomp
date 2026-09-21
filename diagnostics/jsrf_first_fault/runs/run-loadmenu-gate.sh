#!/bin/sh
# WHICH PER-FRAGMENT GATE EATS THE LOAD SCREEN?
#
# 21 Sep 2026, RECOMP_DRAW_MIX + RECOMP_CLIP_AUDIT on the black Load screen:
# every triangle of every draw is accepted by the backend, nothing culled or
# degenerate, and 24 consecutive draws still leave the render target black.
# The draws that vanish all run with depth test AND alpha test on; the only
# draws that visibly work there, the composites, run with both off.
#
#   run-loadmenu-gate.sh depth     RECOMP_METAL_HW_DEPTH_ALWAYS=1  (draws over everything)
#   run-loadmenu-gate.sh alpha     RECOMP_METAL_NO_ALPHA_TEST=1
#   run-loadmenu-gate.sh stencil   RECOMP_METAL_HW_NO_STENCIL=1
#
# Each arm renders WRONGLY by construction; the only question is whether the
# Load screen comes back. DRIVE IT: title -> START -> main menu -> LOAD, sit
# ~20 s, close the window.
set -u
ARM="${1:-depth}"
case "$ARM" in
  depth)   GATE="RECOMP_METAL_HW_DEPTH_ALWAYS=1";;
  alpha)   GATE="RECOMP_METAL_NO_ALPHA_TEST=1";;
  stencil) GATE="RECOMP_METAL_HW_NO_STENCIL=1";;
  *) echo "depth|alpha|stencil"; exit 2;;
esac
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
TAG="LOADMENU-GATE-$(echo "$ARM" | tr a-z A-Z)-KEEP"
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_${TAG}.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-${TAG}"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"
echo "  arm:  $GATE"
echo "  log:  $LOG"
echo "  dump: $DUMP"
env $GATE \
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_DRAW_MIX=1 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1
echo
echo "=== freshness (must be 0) ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
echo "=== black-with-scene reports (FRAG-ARM black=yes with >=10 draws/flip) ==="
grep -a 'FRAG-ARM' "$LOG" | grep -ac 'black=yes.*counting'
echo "=== last 20 snapshots ==="
grep -a '\[SNAP\] snap' "$LOG" | tail -20 | sed -E 's/.*(t=[0-9.]+).*nonzero=([0-9]+).*/\1 nz=\2/' | tr '\n' ';'; echo
echo "log:  $LOG"
