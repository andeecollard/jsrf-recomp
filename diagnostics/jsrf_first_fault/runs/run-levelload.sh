#!/bin/sh
# THE LEVEL LOAD, CENSUSED. The measurement nobody has ever taken.
#
# DRIVE IT: boot -> let the logos play -> title -> START -> get into a LEVEL
# LOAD as directly as you can (New Game is fine) -> cross the transition ->
# play ~20 s on the other side -> close the window.
#
# WHAT IT IS ASKING. From the Roboy log, a level-load window is ~1 draw per
# flip, 322-829 triangles per flip, ~400 fps, and lasts about 2 seconds, with
# guest RAM completely black throughout. So the guest IS submitting an
# ordinary batch -- the same triangles-per-draw as the gameplay either side --
# and getting no pixels. Three explanations survive and the census separates
# them:
#     GPU-BLACK     the draws made no pixels; loss is BEFORE the write-back
#     OWED          it is in a Metal texture and guest RAM never got it
#     PICTURE-HELD  ...while you see black -> loss is past the surface
#     NOTHING-HELD  the census read nothing. Not a finding.
#
# AND [CLEAR-WIN], new in this build, settles the other half in one line:
#     cleared to BLACK, nothing drew over it   -> the draws are at fault
#     cleared to a COLOUR, screen came out 0   -> the clear or the surface is
# The old trace printed only the first sighting of each colour, so all eight
# of its lines land in the first minute and it is mute by the time anything
# goes black. This one reports per window, including "no colour clear".
#
# READ THE POSITIVE CONTROLS FIRST, and they are built into the schedule:
#   the logos at t~11-15    must read PICTURE-HELD (guest_nonzero ~42,000)
#   the main menu t~32-43   must read PICTURE-HELD (guest_nonzero ~306,000)
# If those do not fire, the instrument never looked and no later zero counts.
#
# Stride 200 from t=8: ~16 reports over the logos, ~33 over the menus, ~4
# inside the load window itself, and the 200-report cap leaves roughly ten
# minutes of hand-driven play to reach one.
#
# IGNORE THE AUDIO IN THIS RUN. Each census report drains the GPU on purpose,
# which chops the sound. The clean audio baseline is already taken.
set -u

SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
STAMP=$(date +%Y-%m-%d_%H%M)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_LEVELLOAD-CENSUS-KEEP.log"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs"

echo "  log: $LOG"
echo "  Boot -> title -> START -> get into a LEVEL LOAD -> play 20 s -> close."
echo

RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_SURFACE_CENSUS=200:8 \
RECOMP_REPORT_MS=5000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1

echo
echo "=== positive controls (must include PICTURE-HELD before any black) ==="
grep -a 'verdict:' "$LOG" | sed 's/^ *//' | cut -c1-58 | uniq -c
echo "=== clear colours per window, last 12 ==="
grep -a '\[CLEAR-WIN\]' "$LOG" | tail -12
echo
echo "log: $LOG"
