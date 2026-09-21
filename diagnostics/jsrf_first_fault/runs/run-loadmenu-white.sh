#!/bin/sh
# THE WHITE TEST, ARMED BY THE DEFECT INSTEAD OF BY A CLOCK.
#
# G26 answered the first half on 21 Sep 2026: across a 66-report player-driven
# run, including 35 seconds of black, the guest's render target and ours agreed
# every single time (66 MATCH, 0 COVERAGE, bound-ever=yes throughout, ring
# check never failed). So the batches are aimed at a surface we parse and
# present. What is left is whether they COVER any of it.
#
# RECOMP_FRAG_FORCE=3 replaces every fragment with white at the end of shade(),
# which both the fixed-function and the programmable path go through. If the 28
# batches a frame cover pixels on the presented surface, the black screen turns
# WHITE. If it stays black, they are covering nothing -- the geometry is being
# thrown away somewhere between acceptance and rasterisation.
#
# WHY THIS SCRIPT EXISTS IN THIS FORM. Mode 3 paints the MENU white too, so the
# run cannot be navigated while it is on. The first attempt (15:57) ran 66
# seconds pinned at 307200/307200 and never once reached the Load screen's 28
# batches per frame. The second was stopped before its force was due. Then
# RECOMP_FRAG_FORCE_AFTER=<seconds> was added -- and the player took 30 s to
# reach the Load screen in one run and 50 s in the next, so any fixed second is
# either early enough to blind the menu or late enough to miss the window.
#
# So this arms on RECOMP_FRAG_FORCE_ON_BLACK instead. AND BLACK ALONE IS NOT
# THE CONDITION -- the first version of that fired at t=18 in a real run,
# nowhere near the Load screen, because somebody who has not pressed START yet
# sits through black transitions. The Load screen is not "black": it is black
# WHILE THE GUEST IS STILL SUBMITTING A WHOLE SCENE, 28 draw calls a frame,
# every frame. So the arm requires N consecutive reports that are both
# all-zero and above 10 draws per flip. The boot black that fooled the first
# version drew nothing at all -- it stayed black for four reports AFTER
# arming, which is what gave it away. It latches once armed.
#
# Every report prints [FRAG-ARM] with the arm's own inputs beside its
# decision, so a run that does not arm says why.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Then sit still. The screen
# goes black, the force arms itself about three seconds later, and the next
# frames answer the question. Give it 20 s after the black before closing.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
BLACK="${1:-3}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-WHITE-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-WHITE-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START -> main menu -> LOAD -> SIT STILL. The force arms itself"
echo "  after $BLACK black reports. Give it 20 s, then close the window."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FRAG_FORCE=3 RECOMP_FRAG_FORCE_ON_BLACK="$BLACK" \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== did the run reach the Load screen? (black reports) ==="
grep -ac 'nonzero=0/307200' "$LOG"
echo "=== DID THE FORCE ARM? (if not, the experiment never ran) ==="
grep -a 'FRAG-FORCE' "$LOG"
echo "=== what the arm was looking at, around the decision ==="
grep -a '\[FRAG-ARM\]' "$LOG" | tail -12
echo "=== snapshot freshness -- both must be 0 ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo
echo "=== THE ANSWER: presented coverage across the arm ==="
echo "    black ... black ... ARMED ... then WHITE means the batches were"
echo "    covering the surface (SHADING); still black means they were not."
grep -a '\[SNAP\]' "$LOG" | sed -E 's/.* (t=[0-9.]+) .*nonzero=([0-9]+).*/  \1 nonzero=\2/' | tail -30
echo
echo "=== G26 stayed consistent? (should still be all MATCH) ==="
printf '  %-10s %s\n' MATCH    "$(grep -ac '| MATCH' "$LOG")"
printf '  %-10s %s\n' COVERAGE "$(grep -ac '| COVERAGE' "$LOG")"
echo
echo "log:  $LOG"
echo "dump: $DUMP"
