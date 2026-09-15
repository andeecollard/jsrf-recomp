#!/bin/sh
# Does batching change the image?  Run the same draws both ways and compare.
#
# The gate is per-draw output against batched output, byte for byte, because
# the software rasteriser is not a perfect oracle here -- the per-draw path
# already differs from it on a handful of pixels, and failing the batched run
# for that would be blaming this change for an older one.  See the header of
# metal_batch_test.c.
#
# Needs a real GPU, which is why this is a script and not a CTest case: the
# same reason jsrf_metal_copy_test is built but not registered.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_batch_test"
OUT="${TMPDIR:-/tmp}/jsrf-batch-check.$$"

# WHICH BUILD DID THIS SCORE? Say so, and default to the same one as its
# siblings. metal_batch_check.sh defaulted to .../build while
# metal_hw_check.sh defaulted to .../build-feav, so the two gates over the same
# binary read different build trees -- and during the 15 Sep review the first
# scored a day-old binary and reported a retracted figure as current. Stating
# the path and its timestamp makes that visible in the output instead of
# invisible in a default.
say_build() {
    printf 'build: %s\n' "$BUILD"
    printf 'binary: %s (%s)\n' "$1" "$(date -r "$1" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo 'unknown mtime')"
}

[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_batch_test target" >&2; exit 1; }
say_build "$BIN"
mkdir -p "$OUT" || exit 1
trap 'rm -rf "$OUT"' EXIT INT TERM

# Batching is the default now, so the per-draw arm is the one that needs the
# switch. Setting it to 1 for the other arm is redundant but says which is which.
echo "per-draw:"
RECOMP_METAL_BATCH=0   "$BIN" "$OUT/per-draw.bin" || exit 1
echo "batched:"
RECOMP_METAL_BATCH=1   "$BIN" "$OUT/batched.bin"  || exit 1

if cmp -s "$OUT/per-draw.bin" "$OUT/batched.bin"; then
    echo "IDENTICAL: batching does not change the image or the depth buffer"
else
    echo "DIFFERENT: batching changed the output -- do not measure it, fix it" >&2
    cmp -l "$OUT/per-draw.bin" "$OUT/batched.bin" | head -20 >&2
    exit 1
fi
