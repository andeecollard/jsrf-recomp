#!/bin/sh
# WHICH SURFACES DOES THE GUEST BIND ON OUR SIDE, AND WHAT DO THEY HOLD?
#
# The xemu title trace names FOUR colour targets beyond the swap chain --
# 0x03628000 (1280x480), two 1024x512 ones and 0x032A4000 -- and renders
# three of them to texture. Our census has only ever listed the three
# swap-chain addresses. This run asks the same question of our side without
# a player: the title screen is unattended.
#
#   [FLIPTRACE]   surfaces: ...   every distinct SET_SURFACE_COLOR_OFFSET the
#                                 guest has issued (up to 8), with nonzero
#                                 counts in guest RAM
#   [SURFACE-CENSUS]              what the GPU holds beside guest RAM
#   [SNAP]                        freshness, read first
#
# Silent: the player can hear a harness run and the counters cannot.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${1:-75}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_TITLE-SURFACES-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-TITLE-SURFACES-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  log:  $LOG"
echo "  dump: $DUMP"

SDL_AUDIODRIVER=no_such_driver \
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FLIP_TRACE=100 RECOMP_SURFACE_CENSUS=240:10 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=5000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCH=$!
wait "$PID" 2>/dev/null
kill "$WATCH" 2>/dev/null

echo
echo "=== freshness -- both must be 0 ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo "=== every distinct surface the guest bound, last listing ==="
grep -a 'FLIPTRACE\]   surfaces' "$LOG" | tail -3
echo "=== census verdicts ==="
grep -a 'verdict:' "$LOG" | sed -E 's/ -- .*//' | sort | uniq -c
echo "=== census surfaces, last report ==="
grep -a 'SURFACE-CENSUS\]   surface' "$LOG" | tail -5
echo
echo "log:  $LOG"
