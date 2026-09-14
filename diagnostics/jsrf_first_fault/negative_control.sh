#!/bin/sh
# Run a test's negative control, without the step everyone forgets.
#
# A test that has never been seen to FAIL is not evidence. The pattern is
# always the same: break the thing under test, rebuild, confirm the test fails,
# restore, rebuild, confirm it passes. The rebuild is the step that gets
# skipped, and skipping it makes the control silently pass against a stale
# object -- which happened three times in one session here, each time producing
# a confident and wrong "the test has teeth".
#
# This does the whole cycle and refuses to report success unless the control
# genuinely failed.
#
# Two things stand between it and the stale-object trap it exists to avoid, and
# the first one alone was not enough. `touch` before each build, because make
# compares mtimes -- but a touch that lands in the SAME SECOND as the build it
# is trying to invalidate is not newer, and make skips the rebuild. That is not
# a hypothetical: the first version of this script left a broken
# jsrf_vsh_reuse_test binary installed after restoring its source, and ctest
# reported that test failing for an hour against source that was correct. So
# the product is DELETED before every build as well; nothing can then be stale,
# whatever the clock says. And the restore VERIFIES the test passes again,
# which is the check whose absence let the broken binary out.
#
# Usage:
#   negative_control.sh <source> <target> <sed-expression> [command]
#
# [command] is what decides pass or fail, defaulting to running the target
# binary.  Give one when the check is not a single executable -- a script that
# runs a binary twice and compares the two outputs, say, which is how the
# batching change is gated.
# e.g.
#   negative_control.sh src/kernel/nv2a_pb_exec.c jsrf_vsh_reuse_test \
#       's/seen\[v >> 3\] &= /seen[v >> 3] |= /'
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC="${1:?usage: negative_control.sh <source> <target> <sed-expression>}"
TGT="${2:?usage: negative_control.sh <source> <target> <sed-expression>}"
EXPR="${3:?usage: negative_control.sh <source> <target> <sed-expression>}"
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build}"
BIN="$BUILD/$TGT"
RUN="${4:-$BIN}"
BAK=$(mktemp)

cd "$ROOT" || exit 1
cp "$SRC" "$BAK" || exit 1
# Delete the OBJECTS, not just the binary. Deleting only the binary makes make
# relink -- from the object it already has, which is the stale one, so a whole
# cycle of break/rebuild/run reports "STILL PASSES" for a defect the test does
# in fact catch. Seen twice in one session, once per level: first the installed
# test binary, then its object. There is no third level below this one.
rebuild() {
    # The object files only: the .dir also holds build.make and DependInfo,
    # and removing those leaves make unable to build the target at all.
    find "$BUILD" -type d -name "$TGT.dir" \
      -exec sh -c 'find "$1" -name "*.o" -delete' _ {} \; 2>/dev/null
    # And the object for THIS source wherever it lives, which is usually not
    # under the target's directory at all: a source in a library compiles into
    # the library's .dir, so clearing only the target's left the edited
    # nv2a_metal.m object in place and the control reported STILL PASSES for a
    # defect it had never actually built. CMake names objects <source>.o.
    find "$BUILD" -name "$(basename "$SRC").o" -delete 2>/dev/null
    rm -f "$BIN"; touch "$SRC"
    cmake --build "$BUILD" --target "$TGT" -j 6 >/dev/null 2>&1
}
restore() {
    cp "$BAK" "$SRC"; rm -f "$BAK"
    if ! rebuild; then
        echo "RESTORE BUILD FAILED: $SRC is back but $BIN is not rebuilt" >&2
        return
    fi
    if sh -c "$RUN" >/dev/null 2>&1; then
        echo "restored: the test passes again"
    else
        echo "RESTORE LEFT THE TEST FAILING -- diff $SRC against git before trusting anything" >&2
    fi
}
trap 'restore' EXIT INT TERM

printf 'baseline: '
if ! rebuild; then
    echo "BUILD FAILED before any change"; exit 1
fi
if sh -c "$RUN" >/dev/null 2>&1; then echo "passes"; else
    echo "FAILS ALREADY -- fix the test before controlling it"; exit 1; fi

printf 'with the defect injected: '
sed -i '' "$EXPR" "$SRC" || { echo "sed failed"; exit 1; }
if cmp -s "$SRC" "$BAK"; then
    echo "SED CHANGED NOTHING -- the expression did not match, so this proves nothing"
    exit 1
fi
if ! rebuild; then
    echo "build failed (counts as caught, but check it is for the right reason)"
    exit 0
fi
if sh -c "$RUN" >/dev/null 2>&1; then
    echo "STILL PASSES -- the test does NOT cover this defect"
    exit 1
fi
echo "fails, as it must"
echo "negative control OK: the test detects this defect"
