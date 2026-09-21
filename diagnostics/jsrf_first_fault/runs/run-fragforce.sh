#!/bin/sh
# WHICH INPUT TO THE FRAGMENT IS BLACK. Unattended, no controller, ~45 s.
#
# THE REPRODUCER IS THE ATTRACT LOOP, not the Load menu. The evening handover
# ordered the Load screen first and deferred the intro silhouettes to item 5,
# but the silhouettes need NOBODY: last-run_2026-09-21_1453_MAGENTA-CLEAR
# reads [FB] nonzero=96603/153600 at t=12..16 with on=4-5 and 36 pad lines in
# the whole run. 56,997 pixels written as zero over a magenta clear, reached
# by booting and waiting. The handover itself says the two are probably the
# same cause, so this is the same question asked where it costs nothing.
#
# WHAT THE FRAMES SHOW, in fbdump-2026-09-21_1400-LOADMENU-CENSUS-KEEP: the
# sky, the monorail and the distant city are CORRECT AND TEXTURED, and the
# near geometry -- the Rokkaku-dai disc, the mullions, the ground -- is solid
# black in the same frame. This is not a black screen. It is a subset of the
# draws shading to zero while their neighbours shade correctly.
#
# RECOMP_FRAG_FORCE replaces every fragment with one of shade()'s own inputs,
# so each arm answers one question with a picture:
#   1  TEXTURE0      silhouettes still black -> the texture sampled black
#   2  PRIMARY_COLOR silhouettes still black -> the diffuse was black
#   4  TEXCOORD0/w   a flat colour over a whole object -> the q collapse
#   0  control       taken in THIS build, THIS session, for the scene match
#
# MATCH THE SCENE, NOT THE CLOCK. The attract camera pans, and scoring a fixed
# rectangle across two runs of it is the measurement error that killed the
# RECOMP_TEXMODE_APPROX theory on 21 Sep. One picture per second for 45 s, and
# the frames are compared by what is IN them -- the disc is unmistakable.
#
# Silent: SDL_AUDIODRIVER=no_such_driver. The player can hear a harness run and
# the counters cannot, so an unattended run has no reason to make noise. It
# also makes the boot advance without the APU's real-time throttle, which is
# why this captures 45 frames rather than aiming at t=12.
set -u

MODE="${1:?usage: run-fragforce.sh <0|1|2|3|4>}"
SUPPORT="$HOME/Library/Application Support/JSRF"
APP="$HOME/jsrf-build/JSRF.app"
SECS="${2:-45}"
STAMP=$(date +%Y-%m-%d_%H%M%S)
LOG="$HOME/jsrf-build/preserved-logs/last-run_${STAMP}_FRAGFORCE-${MODE}-KEEP.log"
DUMP="$HOME/jsrf-build/fbdump-${STAMP}-FRAGFORCE-${MODE}-KEEP"

. "$SUPPORT/paths.conf"
: "${JSRF_HDD_ROOT:=$SUPPORT/hdd}"
mkdir -p "$HOME/jsrf-build/preserved-logs" "$DUMP"

echo "  mode: $MODE   log: $LOG"
echo "  dump: $DUMP"

SDL_AUDIODRIVER=no_such_driver \
RECOMP_XBE_PATH="$JSRF_GAME_DIR/default.xbe" \
RECOMP_GAME_DIR="$JSRF_GAME_DIR" \
RECOMP_HDD_ROOT="$JSRF_HDD_ROOT" \
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 \
RECOMP_FRAG_FORCE="$MODE" \
RECOMP_FB_DUMP="$DUMP/" \
RECOMP_REPORT_MS=1000 \
"$APP/Contents/MacOS/jsrf-engine" >"$LOG" 2>&1 &
PID=$!
( sleep "$SECS"; kill -TERM "$PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCH=$!
wait "$PID" 2>/dev/null
kill "$WATCH" 2>/dev/null

echo
echo "=== did the shader take the switch? ==="
grep -a 'FRAG-FORCE' "$LOG" | head -2
echo "=== did the Metal library compile? ==="
grep -ai 'shader\|newLibrary\|compil' "$LOG" | head -5
echo "=== frames ==="
ls "$DUMP" | wc -l
echo "=== where the boot got to (on= 4-12 is the attract loop) ==="
grep -ao '\[APU-VOICE\] on=[0-9]*' "$LOG" | tail -1
echo "=== coverage over time ==="
grep -a '\[FB\]' "$LOG" | tail -20
