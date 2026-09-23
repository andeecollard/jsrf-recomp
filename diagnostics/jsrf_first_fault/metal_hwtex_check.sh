#!/bin/sh
# G27 exit criterion 2: does hardware texture sampling draw what the software
# sampler draws? Needs a real GPU, so a script rather than a CTest case.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_hwtex_test"
OUT="${TMPDIR:-/tmp}/jsrf-hwtex-check.$$"
[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_hwtex_test target" >&2; exit 1; }
printf 'binary: %s (%s)\n' "$BIN" "$(date -r "$BIN" '+%Y-%m-%d %H:%M:%S')"
mkdir -p "$OUT" && trap 'rm -rf "$OUT"' EXIT INT TERM
echo "software sampler:"; RECOMP_METAL_HW_TEX=0 "$BIN" "$OUT/off.bin" || exit 1
echo "hardware sampler:"; RECOMP_METAL_HW_TEX=1 "$BIN" "$OUT/on.bin"  || exit 1
if cmp -s "$OUT/off.bin" "$OUT/on.bin"; then
    echo "IDENTICAL -- suspicious: check the on arm really sampled in hardware" >&2; exit 1
fi
python3 "$ROOT/diagnostics/jsrf_first_fault/hwtex_compare.py" "$OUT/off.bin" "$OUT/on.bin" "$@"
