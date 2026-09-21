#!/bin/sh
# G26 -- DOES THE GUEST AGREE WITH US ABOUT WHERE IT IS DRAWING?
#
# Reads the guest's own D3DDevice every report and prints [RT]:
#
#   D3D_g_pDevice 0x0019DCE0 -> device -> +0x2070 m_RenderTarget
#                            -> that D3DSurface -> +0x04 Data
#
# against s_gpu.color_offset, which is the payload our parser latched from
# NV097_SET_SURFACE_COLOR_OFFSET. The guest writes ->Data into that method
# VERBATIM (see d3d8_ring.h), so the two are comparable byte for byte.
#
# THIS SCRIPT IS THE POSITIVE CONTROL, NOT THE EXPERIMENT. It boots
# unattended into the attract loop, where the title demonstrably renders, and
# asks only whether the PROBE works: is the device found, does the ring
# self-check hold, and does the verdict read MATCH on a screen we can see?
#
# A probe that reads DIVERGE here is broken, because the attract loop is
# visibly on screen. Prove it on a working screen before believing it on a
# black one.
#
# The experiment is the player-driven run to the Load menu -- run-loadmenu-snap.sh
# with this build. Do that second.
#
# Silent: the player can hear a harness run and the counters cannot.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${1:-45}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_RTPROBE-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-RTPROBE-KEEP"

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
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCH=$!
wait "$PID" 2>/dev/null
kill "$WATCH" 2>/dev/null

echo
echo "=== did the probe find a device at all? ==="
grep -ac 'NO VERDICT -- D3D_g_pDevice is still empty' "$LOG"
echo "=== did the ring self-check ever fail? (must be 0) ==="
grep -ac 'FIX THE PROBE, NOT THE RENDERER' "$LOG"
echo "=== the verdicts ==="
grep -a '^\[RT\] guest target' "$LOG" | tail -20
echo "=== verdict tally -- THIS IS THE READING, NOT ANY SINGLE LINE ==="
printf '  %-18s %s\n' MATCH    "$(grep -ac '| MATCH' "$LOG")"
printf '  %-18s %s\n' COVERAGE "$(grep -ac '| COVERAGE' "$LOG")"
printf '  %-18s %s\n' ALIAS    "$(grep -ac '| ALIAS' "$LOG")"
printf '  %-18s %s\n' off-target "$(grep -ac 'sampled off-target' "$LOG")"
echo "  (off-target is expected and harmless: this title rotates three"
echo "   surfaces and the report samples at an arbitrary instant. What"
echo "   matters is bound-ever=NO, which is what COVERAGE counts.)"
echo "=== was the guest's target ever a surface we parsed? ==="
grep -ao 'bound-ever=[a-zA-Z]*' "$LOG" | sort | uniq -c
echo "=== is anything actually on screen? (snapshot freshness first) ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -a '\[SNAP\]' "$LOG" | tail -6
echo "=== where the boot got to (on= 4-12 is the attract loop) ==="
grep -ao '\[APU-VOICE\] on=[0-9]*' "$LOG" | tail -1
echo
echo "log:  $LOG"
echo "dump: $DUMP"
