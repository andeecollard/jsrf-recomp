#!/bin/sh
# Play to the Corn tutorial with a real pad, and record what P2 needs.
#
# The synthetic pad cannot do this. Measured 2026-09-12: under RECOMP_FAKE_PAD
# the guest stops requesting USB reports at poll 175 with nonneutral=0 -- it is
# never handed a single button press, so it never leaves the menu. A real pad
# hits the same dead schedule, which is what RECOMP_PAD_INJECT exists to
# bypass: it writes the live report straight into the title's own copies at
# 60 Hz and the USB schedule stops mattering.
#
# What this records, all read-only and all opt-in:
#   [PAD-TRACE]   edge-triggered, so a press either appears or the read is dead
#   [FUNC-HIT]    the five exec-mode walkers, ActionExec and readInput
#   [JSRF-SCENE]  live= is the scene: 61 Corn cutscene, 65 gameplay, 70 paused
#   [JSRF-STATE]  +7930/+7934 are FFFFFFFF through the tutorial and 1/2 once it
#                 COMPLETES -- which is the pass/fail signal for the jump
#
# Usage: play_tutorial.sh [outdir]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="${1:-$ROOT/build-macos/jsrf-first-fault/render-investigation/play-tutorial}"
SCRATCH="${PLAY_SCRATCH:-/tmp/jsrf-play}"
STOCK="${JSRF_HDD_SRC:-$ROOT/../upstream_xboxrecomp_clean/build-windows-jsrf/emulated-hdd}"
[ -d "$STOCK" ] || { echo "no emulated-hdd at $STOCK -- set JSRF_HDD_SRC" >&2; exit 1; }
rm -rf "$OUT" "$SCRATCH/hdd"; mkdir -p "$OUT" "$SCRATCH"
cp -R "$STOCK" "$SCRATCH/hdd"
cd "$ROOT" || exit 1
echo "Plug in a controller, then press START and choose New Game."
echo "Log: $OUT/stderr.log"
RECOMP_PB_EXEC=1 RECOMP_METAL=1 \
RECOMP_OHCI_ATTACH=1 RECOMP_PAD_INJECT=1 RECOMP_PAD_TRACE=1 \
RECOMP_SCENE_REPORT=1 RECOMP_FUNC_HIT_TRACE=1 RECOMP_REPORT_MS=5000 \
RECOMP_HDD_ROOT="$SCRATCH/hdd" \
  "$ROOT/build-macos/jsrf-first-fault/build/jsrf_first_fault" \
  > "$OUT/stderr.log" 2>&1
echo "--- exec-mode walkers (0x112A0/0x114D0/0x11700/0x11930 absent means zero) ---"
grep '\[FUNC-HIT\]' "$OUT/stderr.log" | tail -9
echo "--- scene ---"
grep -E '\[JSRF-SCENE\]|\[JSRF-STATE\]' "$OUT/stderr.log" | tail -6
echo "--- pad ---"
grep -c 'PAD-TRACE' "$OUT/stderr.log"
grep '\[PAD-POLL\]' "$OUT/stderr.log" | tail -2
