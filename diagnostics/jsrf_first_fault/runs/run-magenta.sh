#!/bin/sh
# EVERY COLOUR CLEAR BECOMES MAGENTA. One glance decides the fix.
#
# gpu_nonzero counts NONZERO pixels, so every instrument we have reads
# "nothing covered these pixels" and "something covered them and wrote black"
# identically. They need opposite fixes. This separates them by eye:
#
#   a black area comes back MAGENTA  -> no draw ever covered it
#                                       -> the fault is in the TRANSFORM
#                                          (geometry off-screen or zero-area)
#   a black area STAYS BLACK         -> draws covered it and wrote zero
#                                       -> the fault is in the SHADING
#
# 0xF81F is R5G6B5 magenta: red and blue full, green zero. Nothing in this
# title is that colour, so anything magenta on screen is bare cleared surface.
#
# WHAT TO LOOK AT, in order:
#   1. THE LOAD MENU. Main menu -> Load. Magenta or black?
#   2. A LEVEL LOAD, if you get that far. Same question in that 2 s window.
#   3. Anywhere else that is normally black -- the big silhouettes in the
#      intro animation are worth a glance too.
#
# Nothing expensive is armed: no census, no frame dumps. The audio should be
# as good as the clean baseline, and [CLEAR-WIN] still reports what the GUEST
# asked for, not the override, so the instrument is not measuring itself.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_MAGENTA-CLEAR-KEEP.log"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs"

echo "  log: $LOG"
echo "  Main menu -> Load. Is the screen MAGENTA or BLACK? That is the whole test."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_CLEAR_COLOR_FORCE=0xF81F \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== the override must have been read ==="
grep -a 'FORCING every colour clear' "$LOG" | head -1
echo "=== what the GUEST asked for (unaffected by the override) ==="
grep -a '\[CLEAR-WIN\]' "$LOG" | grep -v FORCING | tail -6
echo
echo "log: $LOG"
