#!/bin/sh
# G33c: two silenced tutorial arms on the census build, then the report.
#   arm off: RECOMP_D3D8_CENSUS unset, no tally  -- the wrappers must change nothing
#   arm on : RECOMP_D3D8_CENSUS=1, RECOMP_MEM_WATCH_TALLY=ring
# Usage: run_d3d8_census.sh <census-binary> [seconds]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="${1:?usage: run_d3d8_census.sh <census-binary> [seconds]}"
SECS="${2:-150}"
ICL="$HOME/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future"
export JSRF_GAME_DIR="${JSRF_GAME_DIR:-$ICL/Jet Set Radio Future (US)}"
export SDL_AUDIODRIVER=no_such_driver        # silenced: the player hears harness runs
export JSRF_BIN="$BIN"
M="$ROOT/diagnostics/jsrf_first_fault/measure.sh"
OUT="$ROOT/build-macos/jsrf-first-fault/measure"

env -u RECOMP_D3D8_CENSUS -u RECOMP_MEM_WATCH_TALLY sh "$M" d3d8census-off "$SECS" || exit $?
RECOMP_D3D8_CENSUS=1 RECOMP_MEM_WATCH_TALLY=ring sh "$M" d3d8census-on "$SECS" || exit $?

/usr/bin/python3 "$ROOT/experiments/d3d8_boundary/census_report.py" \
    --off "$OUT/d3d8census-off/stderr.log" --on "$OUT/d3d8census-on/stderr.log" \
    --entries "$ROOT/experiments/d3d8_boundary/entry_points.json"
