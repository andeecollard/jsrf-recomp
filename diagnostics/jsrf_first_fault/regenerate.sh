#!/bin/sh
set -eu

PYTHON="${PYTHON:-python3}"

XBE="../Jet Set Radio Future (US)/default.xbe"
OUT=build-macos/jsrf-first-fault
ACCUM="$OUT/vtable_seeds_accum.json"

# func_id's vtable scanner and disasm's function detector feed each other: the
# scanner finds thunks the detector missed, and seeding those back gives the
# detector real boundaries so they enter functions.json -- and therefore the
# recompiler's function database. A thunk that is discovered but never seeded
# back exists only as a classification in identified_functions.json and is
# never translated, so an indirect call to it fails to resolve at runtime.
#
# Each func_id run writes only the thunks that were NEW relative to the
# functions.json it was given, so a fixed two passes stops wherever the second
# pass happened to land: JSRF's second pass still discovered 60 more, one of
# which (0x0007BE30) the game calls indirectly. Iterate to an actual fixpoint
# instead, accumulating the discoveries across rounds since the per-run file is
# overwritten each time.
MAX_ROUNDS=8

echo "[]" > "$ACCUM"

round=1
while [ "$round" -le "$MAX_ROUNDS" ]; do
    echo "=== discovery round $round ==="

    # Disassemble every executable code section, not just .text: JSRF's CRT
    # initializer tables call into named XDK sections (D3D/DSOUND/XPP)
    # indirectly, so restricting the pass to .text omits valid guest functions
    # before func_id can inspect them.
    "$PYTHON" -m tools.disasm \
        "$XBE" \
        --analysis-json "$OUT/jsrf_analysis.json" \
        --output "$OUT/disasm" \
        --seed-functions diagnostics/jsrf_first_fault/thread_start_seed.json \
        --seed-functions diagnostics/jsrf_first_fault/icall_seed.json \
        --seed-functions diagnostics/jsrf_first_fault/startup_entries.json \
        --seed-functions "$ACCUM" \
        --force \
        --verbose

    "$PYTHON" -m tools.func_id \
        "$XBE" \
        --functions "$OUT/disasm/functions.json" \
        --strings "$OUT/disasm/strings.json" \
        --xrefs "$OUT/disasm/xrefs.json" \
        --output "$OUT/func-id" \
        --verbose

    # Merge this round's discoveries into the accumulator. Exits 0 when the
    # round added something new, 1 once the loop has converged.
    if "$PYTHON" - "$ACCUM" "$OUT/func-id/vtable_thunk_seeds.json" <<'PY'
import json, sys

accum_path, new_path = sys.argv[1], sys.argv[2]
accum = json.load(open(accum_path))
try:
    found = json.load(open(new_path))
except FileNotFoundError:
    found = []

seen = {e["start"].lower() for e in accum}
added = [e for e in found if e["start"].lower() not in seen]
if added:
    accum.extend(added)
    json.dump(accum, open(accum_path, "w"), indent=2)

print(f"  round added {len(added)} thunk(s); {len(accum)} accumulated")
sys.exit(0 if added else 1)
PY
    then
        round=$((round + 1))
    else
        echo "=== converged after $round round(s) ==="
        break
    fi
done

if [ "$round" -gt "$MAX_ROUNDS" ]; then
    echo "WARNING: vtable thunk discovery did not converge in $MAX_ROUNDS rounds." >&2
    echo "         Translating the function set as of the last round." >&2
fi

# --exclude-manual keeps the hand-written guest bodies in
# jsrf_manual_overrides.c authoritative: the recompiler skips those addresses
# instead of emitting a generated body that would collide at link time.
"$PYTHON" -m tools.recomp \
    "$XBE" \
    --all \
    --split 1000 \
    --disasm-dir "$OUT/disasm" \
    --func-id-dir "$OUT/func-id" \
    --gen-dir "$OUT/gen" \
    --output-dir "$OUT/recomp-summary" \
    --trace-functions diagnostics/jsrf_first_fault/reach_trace.json \
    --exclude-manual diagnostics/jsrf_first_fault/jsrf_manual_overrides.c \
    --verbose

# Generated C includes this local copy first. Keep its ABI synchronized with
# the lifter (MMX unions, FS accessors and non-local jump helpers).
cp templates/runtime/recomp_types.h "$OUT/gen/recomp_types.h"
