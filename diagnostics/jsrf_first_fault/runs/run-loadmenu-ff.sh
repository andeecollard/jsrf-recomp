#!/bin/sh
# THE DRAWS THAT VANISH ON THE LOAD SCREEN ARE ALL FIXED-FUNCTION.
#
# RECOMP_DRAW_MIX, 21 Sep 2026, per flip on the black Load screen: 20
# untextured and 5 DXT3-textured draws with vmode 0 (fixed function), no
# diffuse stream and a white current colour, plus 2 programmable draws and
# the composite. Every draw on the VISIBLE main menu is a vertex program.
# Twenty-four consecutive Load-screen draws leave the render target black.
#
#   run-loadmenu-ff.sh cpu    RECOMP_METAL_FF=0  fixed function on the CPU
#   run-loadmenu-ff.sh gpu    RECOMP_METAL_FF=1  the default, the control
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit ~20 s. Close the window.
# If the cpu arm shows the Load screen, the GPU fixed-function path is the
# defect and the [VSH] fixed-function line names how many batches it owns.
set -u
ARM="${1:-cpu}"
case "$ARM" in cpu) FF=0;; gpu) FF=1;; *) echo "cpu|gpu"; exit 2;; esac
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
TAG="LOADMENU-FF-$(echo "$ARM" | tr a-z A-Z)-KEEP"
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_${TAG}.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-${TAG}"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  arm:  metal_ff=$FF"
echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START at the title -> main menu -> LOAD -> sit ~20 s -> close the window."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_METAL_FF=$FF RECOMP_DRAW_MIX=1 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== snapshot freshness: repeats / never-published (both should be 0) ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo "=== which arm ran ==="
grep -a 'fixed-function on the GPU\|fixed-function on the CPU\|metal_ff' "$LOG" | tail -2
echo "=== what the draws target and sample, last window ==="
awk '/^\[DRAW-MIX\] [0-9]+ distinct/{buf=""} /^\[DRAW-MIX\]/{buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG" | sort -t' ' -k2 -rn | head -14
echo "=== draws per flip (28.0 is the Load screen) ==="
grep -a 'FRAG-ARM\|draws/flip' "$LOG" | tail -4
echo "=== the last 25 snapshots: is the frame black? ==="
grep -a '\[SNAP\]' "$LOG" | tail -25
echo
echo "log:  $LOG"
echo "dump: $DUMP"
