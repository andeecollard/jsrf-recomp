#!/bin/sh
# COVERAGE OR SHADING. One run, binary outcome.
#
# EARNED, NOT ASSUMED. The unforced runs at 15:52 and 15:55 established the
# defect and reproduced it twice, with fresh snapshots both times:
#   - the MAIN MENU renders perfectly, then fades out correctly
#   - the screen after it never arrives: 33 s of nonzero=0/307200
#   - 0 stale repeats, 0 never-published -- the snapshot is not lying
#   - [STAGE] 28 draw calls per frame THROUGHOUT, which is the evening
#     handover's own signature for being on the Load screen
#   - [TEXTURE] rejected=0, [METAL] hw refusals=0, and the triangle total
#     climbs by ~38,000 a second while the screen is black
#
# So 28 batches a frame are accepted, submitted and rasterised, and put no
# non-zero pixel on the presented surface. Two ways that happens and they need
# opposite fixes:
#   COVERAGE  they are not landing on the surface we present
#   SHADING   they land on it and write zero
#
# RECOMP_FRAG_FORCE=3 returns opaque white from shade() for every fragment,
# after the combiner, before the blend. Then:
#   WHITE SHAPES APPEAR -> the batches DO cover the presented surface, and
#                          their colour is the fault. A shading question.
#   STILL BLACK         -> they never reach it. A coverage question, and the
#                          next stop is D3D8__D3DDevice_SetRenderTarget
#                          (0x0018d0f0) and CMGameGL::setRenderTargetFromArray
#                          (0x0014d340) -- read the guest's own render target
#                          and compare it with ours.
#
# THE OBJECTION TO MODE 3 IS DEAD. Under a DST_COLOR/ZERO multiply, white is a
# no-op and this test would prove nothing -- which is why it was worth checking
# first. [BLEND-FADE] froze at 72 batches for the whole run, all rasterised,
# none rejected, and did not move during the black. No multiply blend is
# running on this screen, so white is a real signal.
#
# Renders incorrectly by construction. Snap dumps are on, so read snapNNN.
#
# DRIVE IT: title -> START -> main menu -> LOAD. Sit ~20 s, then close.
set -u
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-WHITE-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-WHITE-KEEP"
. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"
echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  DRIVE THE MENUS NORMALLY. The force holds off until t=${AFTER:-45}s."
echo "  START -> main menu -> LOAD, and BE SITTING ON THE BLACK SCREEN at t=${AFTER:-45}s."
echo "  Everything turns white then. Sit ~20 s more, then close the window."
echo "  Override with:  AFTER=60 ./run-loadmenu-white.sh"
echo
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FRAG_FORCE=3 \
RECOMP_FRAG_FORCE_AFTER=${AFTER:-45} \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1
echo
echo "=== the switch was read? ==="
grep -a 'FRAG-FORCE' "$LOG" | head -1
echo "=== presented coverage, last 12 ==="
grep -a '\[SNAP\]' "$LOG" | awk '{t="";n="";for(i=1;i<=NF;i++){if($i ~ /^t=/)t=$i; if($i ~ /^nonzero=/)n=$i} print t,n}' | tail -12
echo "=== freshness: repeats=$(grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG") nothing-published=$(grep -ac 'NOTHING PUBLISHED' "$LOG") ==="
echo "=== DID WE REACH THE LOAD SCREEN? 28 batches a frame is the signature ==="
grep -ac '(28 calls)' "$LOG"
grep -a '\[STAGE\] per frame' "$LOG" | grep -o '([0-9]* calls)' | sort -n -t'(' -k2 | uniq -c | tail -8
echo "=== lowest presented coverage reached ==="
grep -a '\[SNAP\]' "$LOG" | awk '{for(i=1;i<=NF;i++) if($i ~ /^nonzero=/){split($i,a,"[=/]"); print a[2]}}' | sort -n | head -3
echo
echo "log:  $LOG"
echo "dump: $DUMP"
