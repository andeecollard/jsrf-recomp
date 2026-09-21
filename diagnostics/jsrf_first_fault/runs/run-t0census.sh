#!/bin/sh
# WHICH DRAWS CARRY ONE TEXTURE COORDINATE. Unattended, ~45 s, no controller.
# See the note on g_t0_flat in nv2a_metal.m. Nothing is forced: this is a
# measurement of the NORMAL picture, so its numbers describe the real bug and
# not a diagnostic arm.
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${1:-45}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_T0-CENSUS-KEEP.log"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs"
echo "  log: $LOG"
SDL_AUDIODRIVER=no_such_driver \
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_T0_CENSUS=30 \
RECOMP_T0_CENSUS_MINAREA=20000 \
RECOMP_REPORT_MS=5000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
W=$!
wait "$PID" 2>/dev/null
kill "$W" 2>/dev/null
echo
grep -a '^\[T0\]' "$LOG" | tail -40
echo "log: $LOG"
