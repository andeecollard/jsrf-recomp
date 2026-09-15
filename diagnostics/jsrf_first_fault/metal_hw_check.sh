#!/bin/sh
# Does the hardware-state path draw the same image as the software-state one?
#
# RECOMP_METAL_HW=1 moves blending, depth test and stencil out of the fragment
# shader and onto the blend unit, the depth unit and an MTLDepthStencilState.
# That is only worth having if the image is unchanged, and "unchanged" has to
# mean byte-for-byte, because a blend factor translated to the wrong Metal enum
# produces a plausible frame that is wrong in one mode -- the exact bug the
# _Static_asserts in nv2a_metal.m caught at compile time, and the exact bug a
# screenshot will not catch at runtime.
#
# The gate is the two Metal paths against EACH OTHER, not against the software
# rasteriser: the per-draw path already differs from that rasteriser on a
# handful of pixels, and failing the hardware path for an older difference
# would be blaming this change for someone else's. Same reasoning as
# metal_batch_check.sh, which compares per-draw against batched.
#
# Needs a real GPU, which is why this is a script and not a CTest case.
#
# CURRENT STATUS, 15 Sep 2026: IT FAILS, and that is why the switch is off.
#     software state   1 of 65536 pixels differ from the software rasteriser
#     hardware state   23096 of 65536
# So the hardware path is not yet equivalent. The leading suspect is DEPTH
# OWNERSHIP: with RECOMP_METAL_HW the real depth attachment is populated once,
# at surface creation, while the surface upload ALSO still writes depth into
# the colour texture's alpha and nv2a_metal_sync still reads it back from
# there. Two stores for one quantity, updated on different schedules, will
# drift exactly like this. The fix is to make depth ownership exclusive per
# path rather than to adjust anything in the translation tables.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_batch_test"
OUT="${TMPDIR:-/tmp}/jsrf-hw-check.$$"

[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_batch_test target" >&2; exit 1; }
mkdir -p "$OUT" || exit 1
trap 'rm -rf "$OUT"' EXIT INT TERM

echo "software state (blend/depth/stencil in the fragment shader):"
RECOMP_METAL_HW=0 "$BIN" "$OUT/sw.bin" || exit 1
echo "hardware state (blend unit, depth unit, MTLDepthStencilState):"
RECOMP_METAL_HW=1 "$BIN" "$OUT/hw.bin" || exit 1

if cmp -s "$OUT/sw.bin" "$OUT/hw.bin"; then
    echo "IDENTICAL: the hardware path draws the same image"
    exit 0
fi
echo "DIFFERENT: $(cmp -l "$OUT/sw.bin" "$OUT/hw.bin" | wc -l | tr -d ' ')" \
     "of $(wc -c < "$OUT/sw.bin" | tr -d ' ') bytes" >&2
echo "  Do NOT default the switch on until this prints IDENTICAL." >&2
exit 1
