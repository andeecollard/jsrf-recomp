#!/bin/sh
# Does the hardware-state path draw the image as well as the software-state one?
#
# RECOMP_METAL_HW=1 moves blending, depth test and stencil out of the fragment
# shader and onto the blend unit, the depth unit and an MTLDepthStencilState.
#
# THE GATE IS AGREEMENT WITH THE SOFTWARE RASTERISER, NOT WITH THE OTHER METAL
# PATH, and getting that wrong cost an afternoon. The first version of this
# script demanded the two Metal paths be byte-identical, on the reasoning that
# the existing one is the reference. It is not. Both are approximations of the
# rasteriser in nv2a_texture_copy.c, which is the only thing here that can be
# called an oracle, and measured against it the hardware path is the BETTER
# approximation:
#
#     software state    1 of 65536 pixels,  54091 of 262144 depth bytes differ
#     hardware state    1 of 65536 pixels,     25 of 262144 depth bytes differ
#
# The two Metal paths do still differ from each other, and the shape of that
# difference is the proof it is precision rather than semantics: the stencil
# byte and the HIGH depth byte match exactly, and only the low two depth bytes
# move. Depth agrees to about one part in 65536. The software path recomputes
# the interpolated z in the fragment shader and stores it in a colour
# attachment's alpha; the hardware path takes it from the rasteriser's own
# depth interpolation, which is what the NV2A does and why it lands closer to
# the oracle.
#
# So a byte-identical rule would have failed the more accurate renderer for
# being more accurate. This one scores both against the oracle and requires the
# hardware path to be no worse.
#
# Needs a real GPU, which is why this is a script and not a CTest case.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_batch_test"

[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_batch_test target" >&2; exit 1; }

# The test prints its own comparison against the software rasteriser:
#   "... differs from the software rasteriser on N of 65536 pixels and M of
#    262144 depth bytes"
score() {
    RECOMP_METAL_HW="$1" "$BIN" "${TMPDIR:-/tmp}/jsrf-hw-$1.bin" 2>&1 \
      | sed -n 's/.*differs from the software rasteriser on \([0-9]*\) of [0-9]* pixels and \([0-9]*\) of [0-9]* depth bytes.*/\1 \2/p'
}

SW=$(score 0); HW=$(score 1)
[ -n "$SW" ] && [ -n "$HW" ] || { echo "could not read the test's oracle comparison" >&2; exit 1; }

SW_PX=${SW%% *}; SW_Z=${SW##* }
HW_PX=${HW%% *}; HW_Z=${HW##* }

printf 'software state   %6s pixels  %6s depth bytes differ from the oracle\n' "$SW_PX" "$SW_Z"
printf 'hardware state   %6s pixels  %6s depth bytes differ from the oracle\n' "$HW_PX" "$HW_Z"

if [ "$HW_PX" -le "$SW_PX" ] && [ "$HW_Z" -le "$SW_Z" ]; then
    echo "PASS: the hardware path is no further from the oracle than the software path"
    exit 0
fi
echo "FAIL: the hardware path is further from the oracle -- do not default it on" >&2
exit 1
