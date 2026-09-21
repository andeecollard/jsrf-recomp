#!/bin/sh
# WHAT HAPPENS TO THE 28 BATCHES. Still normal rendering; counters only.
#
# CHOSEN FROM THE 15:52 RESULT, not from a theory. That run established, with
# fresh snapshots and no forcing:
#   t=38  snap037  the MAIN MENU, rendered perfectly, Load Game highlighted
#   t=39  snap038  faded almost to black -- a correct, ordinary fade
#   t=41+ 33 seconds of nonzero=0/307200, every frame FRESH, 0 repeats
#   throughout: [STAGE] 28 draw calls per frame, submitted, never dropping
#   and the flip rate goes from ~30/s to ~230/s at the transition
#
# So the menu is not failing to draw. It fades out correctly and what should
# follow never appears, while 28 batches a frame keep being submitted and put
# no non-zero pixel anywhere. That is the evening handover's Load-screen
# signature, CONFIRMED rather than retracted.
#
# THE QUESTION THIS ASKS. The state on that screen includes a DST_COLOR/ZERO
# blend -- a MULTIPLY. Multiply is an absorbing operation: once the surface is
# exactly zero, every later multiply keeps it zero, forever, whatever colour
# the fragment shader produces. If the fade drives the surface to black and
# the batches that should WRITE afterwards are being dropped, the screen can
# never come back. [BLEND-FADE] is the stage-by-stage fate of every batch drawn
# under a multiply -- submitted, short, vsh-rejected, prepare-rejected,
# rasterised -- and it has never been armed on this screen.
#
# RECOMP_BLEND_TRACE also brings [ALPHA], [ALPHA-IN] and the clear histogram.
# It changes NO rendering: it counts.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit ~20 s, then close.
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-BLEND-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-BLEND-KEEP"
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
RECOMP_BLEND_TRACE=1 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1
echo
echo "=== when did the presented frame go black? ==="
grep -a '\[SNAP\]' "$LOG" | awk '{t="";n="";for(i=1;i<=NF;i++){if($i ~ /^t=/)t=$i; if($i ~ /^nonzero=/)n=$i} print t,n}' | awk 'NR>1' | uniq -f1 | tail -6
echo "=== fate of the batches drawn under a MULTIPLY blend ==="
grep -a '\[BLEND-FADE\]' "$LOG" | tail -4
echo "=== the clear the guest asks for ==="
grep -a '\[CLEAR' "$LOG" | tail -6
echo "=== freshness: repeats=$(grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG") nothing-published=$(grep -ac 'NOTHING PUBLISHED' "$LOG") ==="
echo
echo "log:  $LOG"
echo "dump: $DUMP"
