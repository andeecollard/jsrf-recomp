#!/bin/sh
# WHICH OF shade()'s INPUTS IS THE BLACK ONE?
#
# The white test (run-loadmenu-white.sh, mode 3) settled the question above
# this one on 21 Sep 2026: the Load screen's 28 batches a frame cover the
# ENTIRE 640x480 presented surface -- 307200 of 307200 pixels -- while the
# screen is black. The geometry is where it should be. The shading resolves to
# zero. So the remaining question is which input to shade() is already black
# when it arrives.
#
#   mode 1   the raw TEXTURE0 sample      black -> the texture we sampled is black
#   mode 2   PRIMARY_COLOR                black -> the vertex colour is black
#   mode 4   TEXCOORD0 after the divide   not a yes/no: LOOK AT THE PICTURE.
#                                         Flat colour = one coordinate over the
#                                         whole object. Blue = out of range.
#   mode 3   white                        the positive control. Already run.
#
# EVERY MODE ARMS ITSELF off the defect: it holds until the presented frame has
# been all-zero for N consecutive reports AND the guest is still submitting at
# 28 draws a flip. A fixed wall-clock second could not do this -- the player
# reached the Load screen at t=30 in one run and t=50 in the next -- and plain
# black could not either, because a boot transition is black with nothing
# behind it and armed a run at t=18. See [FRAG-ARM], which prints the arm's
# inputs beside its decision every report.
#
# READ IT IN THIS ORDER, and the order matters:
#   1. did it ARM, and on a window at ~28 draws/flip?
#   2. the [SNAP] freshness lines -- a stale frame is a healthy-looking lie
#   3. the presented nonzero series across the arm
#   4. THE IMAGES, and snapNNN ONLY. reportNNN is the live surface caught part
#      way through composing a frame; reading those is what cost this project
#      a week.
#
# DRIVE IT: title -> START -> main menu -> LOAD -> SIT STILL for ~20 s.
set -u

MODE="${1:?usage: run-loadmenu-frag.sh <1|2|3|4> [black-reports]}"
BLACK="${2:-3}"
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LOADMENU-FRAG${MODE}-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-LOADMENU-FRAG${MODE}-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  mode: $MODE   arms after $BLACK black reports at >=10 draws/flip"
echo "  log:  $LOG"
echo "  dump: $DUMP"
echo
echo "  START -> main menu -> LOAD -> SIT STILL. ~20 s, then close."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FRAG_FORCE="$MODE" RECOMP_FRAG_FORCE_ON_BLACK="$BLACK" \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== 1. DID IT ARM? (no arm = the experiment never ran) ==="
grep -a 'FRAG-FORCE' "$LOG"
echo "--- the arm's own reasoning, around the decision ---"
grep -a '\[FRAG-ARM\]' "$LOG" | grep -n -B4 -A4 'run=3/\|run=2/' | tail -14
echo
echo "=== 2. SNAPSHOT FRESHNESS -- both must be 0 ==="
grep -ac 'SAME FRAME AS THE LAST DUMP' "$LOG"
grep -ac 'NOTHING PUBLISHED' "$LOG"
echo
echo "=== 3. PRESENTED COVERAGE ACROSS THE ARM ==="
grep -a '\[SNAP\]' "$LOG" | sed -E 's/.* (t=[0-9.]+) .*nonzero=([0-9]+).*/  \1 nonzero=\2/' | tail -24
echo
echo "=== 4. THE IMAGES -- snapNNN ONLY, never reportNNN ==="
ls "$DUMP" | grep -c '^snap'
echo "  $DUMP"
echo
echo "=== did G26 stay consistent? ==="
printf '  MATCH %s   COVERAGE %s\n' "$(grep -ac '| MATCH' "$LOG")" "$(grep -ac '| COVERAGE' "$LOG")"
echo
echo "log:  $LOG"
echo "dump: $DUMP"
