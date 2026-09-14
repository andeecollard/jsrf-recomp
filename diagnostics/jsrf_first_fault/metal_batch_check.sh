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
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build}"
BIN="$BUILD/jsrf_metal_batch_test"
OUT="${TMPDIR:-/tmp}/jsrf-batch-check.$$"

[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_batch_test target" >&2; exit 1; }
mkdir -p "$OUT" || exit 1
trap 'rm -rf "$OUT"' EXIT INT TERM

echo "per-draw:"
                       "$BIN" "$OUT/per-draw.bin" || exit 1
echo "batched:"
RECOMP_METAL_BATCH=1   "$BIN" "$OUT/batched.bin"  || exit 1

if cmp -s "$OUT/per-draw.bin" "$OUT/batched.bin"; then
    echo "IDENTICAL: batching does not change the image or the depth buffer"
else
    echo "DIFFERENT: batching changed the output -- do not measure it, fix it" >&2
    cmp -l "$OUT/per-draw.bin" "$OUT/batched.bin" | head -20 >&2
    exit 1
fi
