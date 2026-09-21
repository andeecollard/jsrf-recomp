#!/bin/sh
# WHAT SHADES THE LOAD SCREEN TO BLACK. The run that should name the fix.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit on the black/fading
# screen for ~15 s, then close. Reaching Load is the whole point -- the last
# three runs never left the attract loop and measured nothing.
#
# WHAT WE ALREADY KNOW, measured today:
#   - Cleared to magenta, the Load screen FADES magenta in and out. So it is
#     covered, and something drives it to zero.
#   - One of the blend states in use is src=0x306 dst=0x0 -- GL_DST_COLOR,
#     GL_ZERO -- a MULTIPLY. Multiply by white is a no-op; multiply by black
#     is permanent black. Which one happens is decided by the COLOUR the
#     combiner produces for that quad, and nothing has ever printed it.
#   - The intro silhouettes write ZERO over magenta: covered, shaded to
#     nothing. Same shape, and possibly the same cause.
#   - This is the class of the CD/AB nibble bug fixed today: a MISSING WRITE
#     rather than a wrong colour, which is why it survived two handovers.
#
# RECOMP_PB_EXEC_TOP=400 is REQUIRED, not optional: the [GPU] unhandled list
# is top-ten-by-frequency by default, so a once-per-frame method can never
# appear in it and its absence would mean nothing at all.
#
# No magenta: it did its job (Now Loading uncovered, Load screen covered and
# driven to black) and it makes the menus hard to read. Normal colours now.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_COMBINER-LOADSCREEN-KEEP.log"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs"

echo "  log: $LOG"
echo "  START at the title -> main menu -> LOAD -> sit 15 s -> close."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_COMBINER_TRACE=1 \
RECOMP_PB_EXEC_TOP=400 \
RECOMP_REPORT_MS=5000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== did we reach the Load menu? (on= 4-12 is the attract loop) ==="
grep -ao '\[APU-VOICE\] on=[0-9]*' "$LOG" | tail -1
grep -ao 'vsh=[0-9.]* ms ([0-9]* calls)' "$LOG" | awk -F'[()]' '{print $2}' | uniq -c | tail -6
echo "=== raster state per window ==="
grep -a '\[RASTER-WIN\]' "$LOG" | tail -4
echo "=== combiner configurations seen ==="
grep -ac '\[COMBINER\]' "$LOG"
echo
echo "log: $LOG"
