#!/bin/sh
# Combiner specialisation must not change the image: byte-identical against
# the generic interpreter, under both texture samplers and both vertex paths
# (the fixed `vs`, and a guest vertex program on the GPU -- the player's
# default, whose pipelines come from a different cache). And a specialisation
# on deliberately WRONG words must change it, or the scene cannot see one.
# Each arm must also SAY what it did: the on arm must have built specialised
# pipelines and used them with no fallback, the off arm none, and a vsh arm
# must have drawn through the guest program -- otherwise "identical" could
# mean "the switch did nothing". Needs a real GPU.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_combiner_spec_test"
OUT="${TMPDIR:-/tmp}/jsrf-combiner-spec-check.$$"
[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_combiner_spec_test target" >&2; exit 1; }
printf 'binary: %s (%s)\n' "$BIN" "$(date -r "$BIN" '+%Y-%m-%d %H:%M:%S')"
mkdir -p "$OUT" && trap 'rm -rf "$OUT"' EXIT INT TERM
fail=0
# sh has no locals: run_name/run_mode, so the caller's loop variables survive.
run() {
    run_name=$1; run_mode=$2; shift 2
    if ! env "$@" "$BIN" "$OUT/$run_name.bin" $run_mode > "$OUT/$run_name.log" 2>&1; then
        echo "FAIL: arm $run_name exited nonzero"; sed 's/^/    /' "$OUT/$run_name.log" | tail -20; fail=1
    fi
    grep -E "combiner specialisation|draw rejected|FAIL" "$OUT/$run_name.log" | sed "s/^/  [$run_name] /"
}
field() { grep 'combiner specialisation' "$OUT/$1.log" | sed -n "s/.* $2=\([0-9]*\).*/\1/p" | head -1; }
gpu_draws() { grep 'vsh draws:' "$OUT/$1.log" | sed -n 's/.*vsh draws: \([0-9]*\) GPU.*/\1/p' | head -1; }
for mode in fixed vsh; do
  for tex in 0 1; do
    a=$mode$tex; m=; [ "$mode" = vsh ] && m=vsh
    echo "vertex path $mode, RECOMP_METAL_HW_TEX=$tex:"
    run off$a "$m" RECOMP_METAL_HW_TEX=$tex RECOMP_METAL_SPECIALISE_COMBINERS=0
    run on$a  "$m" RECOMP_METAL_HW_TEX=$tex
    run ctl$a "$m" RECOMP_METAL_HW_TEX=$tex RECOMP_METAL_SPECIALISE_COMBINERS_CONTROL=1
    if cmp -s "$OUT/off$a.bin" "$OUT/on$a.bin"; then
        echo "PASS: specialised image is byte-identical to the generic interpreter ($(wc -c < "$OUT/on$a.bin" | tr -d ' ') bytes)"
    else
        echo "FAIL: specialisation changed the image ($(cmp -l "$OUT/off$a.bin" "$OUT/on$a.bin" | wc -l | tr -d ' ') bytes)"
        diff "$OUT/off$a.log" "$OUT/on$a.log" | grep '^[<>] case' | sed 's/^/    /'
        fail=1
    fi
    if cmp -s "$OUT/off$a.bin" "$OUT/ctl$a.bin"; then
        echo "FAIL: positive control -- wrong words matched, so the scene cannot see a wrong specialisation"; fail=1
    else
        echo "PASS: positive control -- wrong words differ ($(cmp -l "$OUT/off$a.bin" "$OUT/ctl$a.bin" | wc -l | tr -d ' ') bytes)"
    fi
    b_on=$(field on$a built); f_on=$(field on$a fallbacks); b_off=$(field off$a built)
    b_ctl=$(field ctl$a built)
    if [ "${b_on:-0}" -gt 0 ] && [ "${f_on:-1}" -eq 0 ] && [ "${b_off:-1}" -eq 0 ] && [ "${b_ctl:-0}" -gt 0 ]; then
        echo "PASS: arms named themselves (on built $b_on, 0 fallbacks; off built 0; control built $b_ctl)"
    else
        echo "FAIL: arm counters wrong (on built=${b_on:-?} fallbacks=${f_on:-?}; off built=${b_off:-?}; control built=${b_ctl:-?})"; fail=1
    fi
    if [ "$mode" = vsh ]; then
        g_on=$(gpu_draws on$a); g_off=$(gpu_draws off$a)
        if [ "${g_on:-0}" -gt 0 ] && [ "${g_off:-0}" -gt 0 ]; then
            echo "PASS: both arms drew through the guest program ($g_off and $g_on GPU vsh draws)"
        else
            echo "FAIL: the vsh arms did not draw through the guest program (off ${g_off:-?}, on ${g_on:-?})"; fail=1
        fi
    fi
  done
done
exit $fail
