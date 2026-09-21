#!/bin/sh
# OUR SIDE OF THE XEMU DIFFERENTIAL -- the title screen, unattended.
#
# The xemu side is diagnostics/jsrf_first_fault/xemu_capture.sh. The title
# screen is the matched scene because BOTH sides reach it with NO INPUT: xemu
# boots straight through to it and ours arrives in ~20 s, so nothing about the
# comparison depends on a person driving two emulators the same way.
#
# RECOMP_PB_EXEC_TOP=400 IS NOT OPTIONAL. The [GPU] 0xMMMM xN list is top TEN
# by frequency by default, so a once-per-frame method can never appear in it
# and its absence from the diff would mean nothing at all.
#
# Silent, because the player can hear a harness run and the counters cannot.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${1:-75}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_OURS-TITLE-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-OURS-TITLE-KEEP"

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
RECOMP_PB_SCAN=1 RECOMP_COMBINER_TRACE=1 RECOMP_PB_EXEC_TOP=400 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=5000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCH=$!
wait "$PID" 2>/dev/null
kill "$WATCH" 2>/dev/null

echo
echo "=== did we reach the title? (snapshot coverage) ==="
grep -a '\[SNAP\]' "$LOG" | sed -E 's/.* (t=[0-9.]+) .*nonzero=([0-9]+).*/  \1 nonzero=\2/' | tail -8
echo "=== freshness -- both must be 0 ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo "=== method rows for the diff ==="
grep -ac '^\[PB\]' "$LOG"
echo
echo "log:  $LOG"
