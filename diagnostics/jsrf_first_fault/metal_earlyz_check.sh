#!/bin/sh
# G27b gate: early-Z must not change the image (byte-identical), and the known
# approximation (EARLY_Z_REF0) must, or the scene is blind. Needs a real GPU.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_earlyz_test"
OUT="${TMPDIR:-/tmp}/jsrf-earlyz-check.$$"
[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_earlyz_test target" >&2; exit 1; }
printf 'binary: %s (%s)\n' "$BIN" "$(date -r "$BIN" '+%Y-%m-%d %H:%M:%S')"
mkdir -p "$OUT" && trap 'rm -rf "$OUT"' EXIT INT TERM
run() { name=$1; shift; env "$@" "$BIN" "$OUT/$name.bin" 2>&1 | grep -E "depth test before|early-Z G27b|rejected" | sed "s/^/  [$name] /"; }
run late  RECOMP_METAL_EARLY_Z=0
run early RECOMP_METAL_EARLY_Z=1
run ref0  RECOMP_METAL_EARLY_Z=1 RECOMP_METAL_EARLY_Z_REF0=1
fail=0
if cmp -s "$OUT/late.bin" "$OUT/early.bin"; then echo "PASS: early-Z image is byte-identical to late-Z"
else echo "FAIL: early-Z changed the image ($(cmp -l "$OUT/late.bin" "$OUT/early.bin" | wc -l | tr -d ' ') bytes)"; fail=1; fi
if cmp -s "$OUT/late.bin" "$OUT/ref0.bin"; then echo "FAIL: positive control -- the inexact REF0 arm matched, so the scene cannot see a wrong early test"; fail=1
else echo "PASS: positive control -- REF0 differs ($(cmp -l "$OUT/late.bin" "$OUT/ref0.bin" | wc -l | tr -d ' ') bytes)"; fi
exit $fail
