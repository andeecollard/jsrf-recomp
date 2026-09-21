#!/bin/sh
# THE CPU INTERPRETER ARM. Two questions in one unattended run.
#
# 1. DOES THE DEFECT SURVIVE IT? RECOMP_METAL_VSH=0 runs the guest's vertex
#    programs through nv2a_vsh_execute instead of the emitted MSL. It is the
#    oracle nv2a_vsh_msl.c was verified against. If the Rokkaku-dai disc and
#    the mullions come back TEXTURED here, the defect is in the emitter; if
#    they are still flat, the emitter is innocent and the coordinate was
#    already wrong before it.
#
# 2. IT MAKES THE [T0] CENSUS MEASURE THE RIGHT THING. With the programs on
#    the GPU, nv2a_pb_exec hands the backend the program's INPUTS -- see the
#    "THE GPU PATH STOPS HERE" comment -- so a census at nv2a_metal_draw reads
#    v9, not oT0, and its screen boxes are pre-transform. That is what the
#    15:29 run measured and why its biggest flat draw was 100 px. On this arm
#    s_outputs carries real outputs and the same census reads oT0.
#
# Slower than the GPU arm, so it gets longer to reach the same scene.
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${1:-90}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_VSH-CPU-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-VSH-CPU-KEEP"
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
RECOMP_METAL_VSH=0 \
RECOMP_T0_CENSUS=20 \
RECOMP_T0_CENSUS_MINAREA=5000 \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=2000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
W=$!
wait "$PID" 2>/dev/null
kill "$W" 2>/dev/null
echo
echo "=== is the CPU interpreter actually in use? ==="
grep -a '\[METAL\] vsh:' "$LOG" | tail -1
grep -a '\[METAL\] vsh draws' "$LOG" | tail -1
echo "=== flat-coordinate census, now reading oT0 ==="
grep -a '^\[T0\]' "$LOG" | head -12
echo "=== frames: $(ls "$DUMP" | wc -l)"
