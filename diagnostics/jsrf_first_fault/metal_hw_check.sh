#!/bin/sh
# Does the hardware-state path draw the image as well as the software-state one?
#
# RECOMP_METAL_HW=1 moves blending, depth test and stencil out of the fragment
# shader and onto the blend unit, the depth unit and an MTLDepthStencilState.
# RECOMP_METAL_565=1 additionally makes the colour attachment the guest's own
# two-byte format instead of RGBA32Float.
#
# THE GATE IS AGREEMENT WITH THE SOFTWARE RASTERISER, NOT WITH THE OTHER METAL
# PATH, and getting that wrong cost an afternoon. The first version of this
# script demanded the two Metal paths be byte-identical, on the reasoning that
# the existing one is the reference. It is not. Both are approximations of the
# rasteriser in nv2a_texture_copy.c, which is the only thing here that can be
# called an oracle, and measured against it the hardware path is the BETTER
# approximation. So this scores both against the oracle and requires the
# hardware path to be no worse.
#
# WHAT THIS GATE MISSED, AND WHY IT NOW SCORES TWO PHASES AND THREE ARMS.
#
# On 15 Sep 2026 it PASSED on a configuration that renders a real mission
# wrong -- a flickering grid and black rectangles, reported by a person playing
# the game. Two holes, both in what was scored rather than in the rule:
#
#   1. It read the test's PHASE A numbers, and phase A never changes render
#      target. metal_batch_test's phase D does alternate two targets, but D is
#      compared batched-against-per-draw only; the oracle comparison did not
#      reach it. So nothing scored a surface swap, which is what the title does
#      between batches and the leading suspect for the wrong image.
#   2. It never set RECOMP_METAL_565. The broken configuration is HW=1 AND
#      565=1, and the 565 arm -- a different attachment format, a different
#      upload path and a different readback path -- was not scored at all.
#
# Phase F (three alternating surfaces, scored against the oracle) closes the
# first and the third arm below closes the second. Both were added by the
# review of 15 Sep; NEITHER REPRODUCES THE REPORTED BREAKAGE, so the swap
# hypothesis is not supported and the cause is still open. What this gate still
# does not exercise: register combiners, alpha test, stencil, multi-texture --
# all of which a real mission uses and none of which metal_batch_test sets.
#
# THE DEPTH RULE HAS A STATED ALLOWANCE, and the number it replaced was an
# artifact. This script used to report "software state 54091 depth bytes differ,
# hardware state 25" and conclude the hardware path was the better
# approximation. It was not measuring the paths. metal_batch_test built its
# state with memset(0), leaving z_clip_min == z_clip_max == 0, and the software
# path clamps every fragment into that range -- so all of its depth was zero.
# nv2a_texture_copy.c:76 normalises that pair back to [0, 16777215] when it
# decodes the guest registers, so no real draw could ever reach the backend with
# it. With a legal range the two paths land at 24 and 25 bytes of 262144,
# deterministically, and the accuracy gap disappears.
#
# One byte is a rasteriser tie-break, not a regression, so a strict "no worse"
# rule would fail forever on noise. The allowance is 0.1% of the depth buffer:
# large enough to absorb tie-breaks, three orders of magnitude below the
# thousands a real depth regression produces.
#
# WHAT IS SCORED IS MAGNITUDE, NOT COUNT. This project's stated acceptance rule
# for a GPU sink is that it "matches the CPU rasteriser to within one RGB565
# channel step" -- the rule the D3D11 backend was admitted under, with 48% of
# pixels differing on a single unblended draw and every one of them off by
# exactly one step. A bare count cannot tell that from a real error, and would
# fail the 565 attachment for 697 conforming pixels while passing a path that
# put one pixel catastrophically wrong. So the gate counts pixels that exceed
# one step, and reports the raw counts alongside for context.
#
# Needs a real GPU, which is why this is a script and not a CTest case.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_batch_test"

COPY="$BUILD/jsrf_metal_copy_test"

[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_batch_test target" >&2; exit 1; }
[ -x "$COPY" ] || { echo "no $COPY -- build the jsrf_metal_copy_test target" >&2; exit 1; }
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
say_build "$BIN"

# The test prints one line per scored phase:
#   "... differs from the software rasteriser on N of M pixels and Z of Y depth
#    bytes; worst channel error W step(s), K of the differing pixels are within
#    one"
# Emitted as "N Z W K" for the phase whose line matches $2.
read_phase() {
    sed -n "s/^metal $2.*differs from the software rasteriser on \([0-9]*\) of [0-9]* pixels and \([0-9]*\) of [0-9]* depth bytes; worst channel error \([0-9]*\) step(s), \([0-9]*\) .*/\1 \2 \3 \4/p" "$1"
}

# One process per arm: every switch here is read once and cached.
run_arm() {
    RECOMP_METAL_HW="$1" RECOMP_METAL_565="$2" \
        "$BIN" "${TMPDIR:-/tmp}/jsrf-hw-$1-$2.bin" >"$3" 2>/dev/null
}

TMP="${TMPDIR:-/tmp}"
run_arm 0 0 "$TMP/jsrf-hw-sw.txt"    || { echo "software arm failed" >&2; exit 1; }
run_arm 1 0 "$TMP/jsrf-hw-hw.txt"    || { echo "hardware arm failed" >&2; exit 1; }
run_arm 1 1 "$TMP/jsrf-hw-565.txt"   || { echo "565 arm failed" >&2; exit 1; }

status=0
# Per arm and phase: pixels off by MORE than one step, which is the rule, and
# the raw count, which is only context.
report() {   # $1 label  $2 file  $3 phase-key  $4 baseline-over  (empty = is baseline)
    set -- "$1" "$2" "$3" "${4:-}"
    vals=$(read_phase "$2" "$3")
    [ -n "$vals" ] || { echo "could not read $3 from $2" >&2; exit 1; }
    px=${vals%% *}; rest=${vals#* }; zb=${rest%% *}
    rest=${rest#* }; worst=${rest%% *}; one=${rest##* }
    over=$((px - one))
    printf '  %-22s %6s pixels differ, %5s by more than one step (worst %2s), %6s depth bytes\n' \
        "$1" "$px" "$over" "$worst" "$zb"
    OVER=$over; ZB=$zb
}

for phase in "batch:phase A, one surface" "swap:phase F, three surfaces"; do
    key=${phase%%:*}; name=${phase#*:}
    echo "$name"
    report "software state" "$TMP/jsrf-hw-sw.txt"  "$key"; base_over=$OVER; base_z=$ZB
    report "hardware state" "$TMP/jsrf-hw-hw.txt"  "$key"; hw_over=$OVER;   hw_z=$ZB
    report "hardware + 565"  "$TMP/jsrf-hw-565.txt" "$key"; f_over=$OVER;   f_z=$ZB
    # 0.1% of the depth buffer the phase scored: 262144 bytes for one surface,
    # three times that for three.
    case $key in swap) z_allow=786 ;; *) z_allow=262 ;; esac
    for arm in "hardware state:$hw_over:$hw_z" "hardware + 565:$f_over:$f_z"; do
        label=${arm%%:*}; rest=${arm#*:}; over=${rest%%:*}; z=${rest##*:}
        if [ "$over" -gt "$base_over" ]; then
            echo "  FAIL: $label puts $over pixels beyond one channel step against the software path's $base_over" >&2
            status=1
        fi
        if [ "$z" -gt "$((base_z + z_allow))" ]; then
            echo "  FAIL: $label differs on $z depth bytes against the software path's $base_z (allowance $z_allow)" >&2
            status=1
        fi
    done
done

# THE STRICTER ORACLE, WHICH NOTHING WAS RUNNING.
#
# metal_copy_test compares the Metal backend against the rasteriser with an
# exact memcmp -- no tolerance at all -- across the states a mission actually
# uses and metal_batch_test never sets: register combiners (one stage and two),
# alpha test, stencil with a real func and real ops, multi-texture, DXT1, DXT3,
# mip selection, untextured draws and dither. It is built on Apple but
# registered with nothing: it needs a real GPU, so it is out of ctest, and no
# check script ran it. So the broadest state coverage in the tree was sitting
# unexecuted while the narrowest was gating the renderer.
#
# Run across the same three arms it answers the question metal_batch_test
# cannot: both hardware arms pass byte-exactly, and the software arm fails at
# its line 49 -- a depth readback mismatch after a depth-tested draw, known and
# tracked, and the reason this is not a plain "all arms must pass".
#
# The rule is a ratchet on the hardware arms: they pass today, so a change that
# breaks combiners, stencil or multi-texture on them fails here in a second
# rather than surviving to a mission.
echo "exact-match state coverage (metal_copy_test)"
copy_arm() {
    if RECOMP_METAL_HW="$2" RECOMP_METAL_565="$3" "$COPY" >"${TMPDIR:-/tmp}/jsrf-copy-$2-$3.txt" 2>&1; then
        printf '  %-22s exact match against the rasteriser\n' "$1"
        return 0
    fi
    printf '  %-22s FAILED: %s\n' "$1" \
        "$(grep -v 'native raster' "${TMPDIR:-/tmp}/jsrf-copy-$2-$3.txt" | head -1)"
    return 1
}
copy_arm "software state" 0 0 || echo "  (known: the software path's depth readback, tracked separately)"
copy_arm "hardware state" 1 0 || status=1
copy_arm "hardware + 565"  1 1 || status=1

[ "$status" -eq 0 ] && echo "PASS: no arm is further from the oracle than the software path"
exit $status
