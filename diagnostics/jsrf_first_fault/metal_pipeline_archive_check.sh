#!/bin/sh
# The Metal pipeline archive (RECOMP_METAL_PIPELINE_ARCHIVE, nv2a_metal.m) must
# let a SECOND PROCESS build every pipeline without compiling, and must never
# change a pixel. Uses jsrf_metal_combiner_spec_test through a guest vertex
# program (the player's configuration) with the default asynchronous
# compilation, in a private archive directory:
#
#   first    empty directory: every pipeline misses, is compiled and recorded,
#            and a shard is written
#   second   the same directory: every pipeline is FOUND -- the lookup uses
#            MTLPipelineOptionFailOnBinaryArchiveMiss, so hits=N misses=0 is
#            Metal saying it built them from the archive, not a timing guess
#   off      RECOMP_METAL_PIPELINE_ARCHIVE=0: no lookups at all (control)
#   shader   a changed MSL (RECOMP_METAL_SHADER_NONCE=1, pixel-neutral): a
#            different key directory, so nothing is found -- a shader edit
#            cannot be served stale binaries (control)
#   corrupt  a garbage shard beside the good one: discarded, deleted, and the
#            good shard still serves every pipeline
#
# Every arm's image must equal the first's. Needs a real GPU.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="${JSRF_BUILD:-$ROOT/build-macos/jsrf-first-fault/build-feav}"
BIN="$BUILD/jsrf_metal_combiner_spec_test"
OUT="${TMPDIR:-/tmp}/jsrf-pipeline-archive-check.$$"
[ -x "$BIN" ] || { echo "no $BIN -- build the jsrf_metal_combiner_spec_test target" >&2; exit 1; }
printf 'binary: %s (%s)\n' "$BIN" "$(date -r "$BIN" '+%Y-%m-%d %H:%M:%S')"
mkdir -p "$OUT" && trap 'rm -rf "$OUT"' EXIT INT TERM
export RECOMP_METAL_PIPELINE_ARCHIVE_DIR="$OUT/archive"
fail=0
run() {
    run_name=$1; shift
    if ! env "$@" "$BIN" "$OUT/$run_name.bin" vsh > "$OUT/$run_name.log" 2>&1; then
        echo "FAIL: arm $run_name exited nonzero"; sed 's/^/    /' "$OUT/$run_name.log" | tail -20; fail=1
    fi
    grep -E "pipeline archive|pipeline compiles|FAIL" "$OUT/$run_name.log" | sed "s/^/  [$run_name] /"
}
field() { grep '\[METAL\] pipeline archive:' "$OUT/$1.log" | sed -n "s/.* $2=\([0-9]*\).*/\1/p" | head -1; }
same() {
    if cmp -s "$OUT/first.bin" "$OUT/$1.bin"; then echo "PASS: $1 -- image identical to the first run"
    else echo "FAIL: $1 -- image differs from the first run"; fail=1; fi
}

run first
m1=$(field first misses); a1=$(field first added); s1=$(field first saves); h1=$(field first hits)
nshard=$(ls "$OUT"/archive/*/shard-*.bin 2>/dev/null | wc -l | tr -d ' ')
if [ "${h1:-1}" -eq 0 ] && [ "${m1:-0}" -gt 0 ] && [ "${a1:-0}" -eq "${m1:-x}" ] && [ "${s1:-0}" -gt 0 ] && [ "$nshard" -eq 1 ]; then
    echo "PASS: first run compiled and recorded every pipeline ($m1 misses, $a1 added, $s1 saves, 1 shard)"
else
    echo "FAIL: first run (hits ${h1:-?}, misses ${m1:-?}, added ${a1:-?}, saves ${s1:-?}, shards on disk $nshard)"; fail=1
fi

run second
h2=$(field second hits); m2=$(field second misses); a2=$(field second added); sh2=$(field second shards)
# hits is not compared with the first run's misses: two threads may both miss
# on one descriptor there (the background warm-up and a draw), which records
# it twice and finds it once.
if [ "${sh2:-0}" -eq 1 ] && [ "${m2:-1}" -eq 0 ] && [ "${a2:-1}" -eq 0 ] && [ "${h2:-0}" -gt 0 ]; then
    echo "PASS: second process built all $h2 pipelines from the archive, 0 compiled (first run compiled $m1)"
else
    echo "FAIL: second run (shards ${sh2:-?}, hits ${h2:-?}, misses ${m2:-?}, added ${a2:-?})"; fail=1
fi
same second

run off RECOMP_METAL_PIPELINE_ARCHIVE=0
h3=$(field off hits); m3=$(field off misses)
if [ "${h3:-1}" -eq 0 ] && [ "${m3:-1}" -eq 0 ] && grep -q 'metal_pipeline_archive OFF' "$OUT/off.log"; then
    echo "PASS: control -- archive off: no lookups, and the arm says OFF"
else
    echo "FAIL: control -- archive off still looked up (hits ${h3:-?}, misses ${m3:-?})"; fail=1
fi
same off

run shader RECOMP_METAL_SHADER_NONCE=1
h4=$(field shader hits); m4=$(field shader misses); sh4=$(field shader shards)
if [ "${h4:-1}" -eq 0 ] && [ "${sh4:-1}" -eq 0 ] && [ "${m4:-0}" -gt 0 ]; then
    echo "PASS: control -- a changed shader finds no shard and no pipeline ($m4 misses)"
else
    echo "FAIL: control -- a changed shader was served from the archive (shards ${sh4:-?}, hits ${h4:-?})"; fail=1
fi
same shader

# The first run's key directory, as its own report names it (the shader arm
# has made a second one beside it).
key=$(sed -n 's/.*pipeline archive: on, empty \([^;]*\);.*/\1/p' "$OUT/first.log" | head -1)
[ -d "$key" ] || { echo "FAIL: cannot find the first run's archive directory ('$key')"; exit 1; }
printf 'this is not a Metal binary archive\n' > "$key/shard-0000000000-0.bin"
run corrupt
d5=$(field corrupt discarded); h5=$(field corrupt hits); m5=$(field corrupt misses)
if [ "${d5:-0}" -eq 1 ] && [ ! -e "$key/shard-0000000000-0.bin" ] && [ "${m5:-1}" -eq 0 ] && [ "${h5:-0}" -eq "${h2:-x}" ]; then
    echo "PASS: a corrupt shard was discarded and deleted; the good one still served all $h5 pipelines"
else
    echo "FAIL: corrupt shard (discarded ${d5:-?}, still on disk: $([ -e "$key/shard-0000000000-0.bin" ] && echo yes || echo no), hits ${h5:-?}, misses ${m5:-?})"; fail=1
fi
same corrupt
exit $fail
