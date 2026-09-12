#!/bin/sh
set -eu

PYTHON="${PYTHON:-python3}"

XBE="../Jet Set Radio Future (US)/default.xbe"
OUT=build-macos/jsrf-first-fault
ACCUM="$OUT/vtable_seeds_accum.json"

# Indirect-call targets observed at runtime. icall_feedback.py maintains this
# cumulatively; without it discovery sees only the 22 hand-curated entries in
# icall_seed.json, and a target the title actually calls -- but that static
# analysis cannot see -- is never translated. The call then fails to resolve at
# runtime and the caller proceeds on uninitialised registers.
ICALL_DB="tools/recomp/output/icall_targets.json"

# Close the loop here, rather than leaving it to whoever remembers. The feedback
# loop has four steps -- run, merge, seed, regenerate -- and the runtime already
# does the first (RECOMP_ICALL_FEEDBACK_DUMP fires from the periodic report, so
# even a watchdog-killed run leaves a current dump) while this script does the
# last two. The merge in the middle was the only manual link, and skipping it
# fails silently: regeneration re-seeds from the previous cycle's database and
# looks exactly like a loop that has converged.
#
# The dump is written to the *process* working directory, so it lands wherever
# the title was launched from: the repo root for a hand-run binary, the build
# tree for a scripted one. Merge every one we can find rather than choosing.
# The database only ever ORs flags together, so re-merging a dump already
# merged costs a line of output and nothing else, while missing a new one
# stalls the convergence.
ICALL_DUMPS="${ICALL_DUMPS:-icall_targets.dump $OUT/icall_targets.dump $OUT/build/icall_targets.dump}"

ICALL_FOUND=""
for dump in $ICALL_DUMPS; do
    if [ -f "$dump" ]; then
        ICALL_FOUND="$ICALL_FOUND $dump"
    fi
done

if [ -n "$ICALL_FOUND" ]; then
    echo "=== merging runtime icall observations ==="
    # Cross-referenced against the previous cycle's functions.json, which is
    # what makes the "NOT a known function start" list meaningful: those are
    # the targets this regeneration exists to pick up. Absent on a first run,
    # and the tool says so rather than failing.
    #
    # merge exits 1 when the dumps held no observations at all. That is worth
    # reporting and is not a reason to abandon a regeneration, so it must not
    # reach set -e.
    if ! "$PYTHON" -m tools.recomp.icall_feedback \
        --db "$ICALL_DB" \
        --functions "$OUT/disasm/functions.json" \
        merge $ICALL_FOUND
    then
        echo "WARNING: no observations merged; the database is unchanged." >&2
        echo "         The dumps exist but are empty -- check the title was" >&2
        echo "         built with RECOMP_ICALL_FEEDBACK and reached its report." >&2
    fi
else
    echo "WARNING: no runtime icall dump found. Looked for:" >&2
    for dump in $ICALL_DUMPS; do echo "           $dump" >&2; done
    echo "         Regenerating against the database as it stands. A run's newly" >&2
    echo "         observed call targets reach discovery only once merged, so if" >&2
    echo "         you have just run the title, find its dump and pass the path" >&2
    echo "         in ICALL_DUMPS." >&2
fi

# A missing database is survivable but is never what you want: discovery falls
# back to the 22 hand-curated seeds, and every target that only a run can find
# goes untranslated again. Because output/ is gitignored, a fresh clone or a
# new worktree starts without one -- so this is the expected first-run state,
# and worth saying out loud instead of silently omitting the seed.
if [ ! -f "$ICALL_DB" ]; then
    echo "WARNING: $ICALL_DB does not exist." >&2
    echo "         Discovery will see only the hand-curated seeds in" >&2
    echo "         icall_seed.json. Run the title and re-run this script to" >&2
    echo "         build the database; it converges over several cycles, one" >&2
    echo "         blocker at a time, rather than in one pass." >&2
    ICALL_DB=""
fi

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
        ${ICALL_DB:+--seed-functions "$ICALL_DB"} \
        --function-bounds diagnostics/jsrf_first_fault/function_bounds.json \
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

# Mandatory post-pass. An entry discovery missed is emitted as a stub whose
# whole body is `g_esp += 4`; it consumes the return address and nothing else,
# so the caller restores callee-saved registers from the wrong stack slots.
# Skipping this leaves ~200 such stubs and the title faults during stage load.
"$PYTHON" diagnostics/jsrf_first_fault/recover_midfunction_entries.py \
    --output "$OUT"
