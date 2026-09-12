# The vtable-thunk seed loop is carving real functions into fragments

Date: 2026-09-12 (Europe/London)

Found by pointing `RECOMP_ABI_CHECK` at gameplay for the first time (see
`CLAUDE_PROGRESS_2026-09-12_ABI_CHECK_AT_GAMEPLAY.md`) and then asking where its
131 offenders came from. The clue that made it findable came from the Xbox
Dashboard project's README, which documents the same failure with a different
cause.

## The Dashboard's warning, and why it is only half our problem

> **Do not seed from `tools/func_id/output/identified_functions.json`.** The
> disassembler's own help suggests it; it contains addresses inside existing
> function bodies, and each one truncates the function containing it. That is
> how `__heap_init` lost its epilogue.

We already follow that: `regenerate.sh` seeds `thread_start_seed.json`,
`icall_seed.json`, `startup_entries.json`, `vtable_seeds_accum.json` and the
measured icall database -- never `identified_functions.json`. Tested directly:
of 131 ABI offenders, exactly **one** appears in any of those seed files.

The mechanism transfers anyway. It is not the file that matters, it is that
*any* seed landing inside an existing body truncates the container.

## What the function database says

`detection_method` in `disasm/functions.json` is unambiguous:

| | offenders | whole database |
|---|---|---|
| `seed_vtable_thunk` | **126 of 131 (96%)** | 2,428 of 10,292 (23%) |
| `call_target` | 5 | 4,052 |

A four-fold enrichment. And the shape is visible directly --
`FileManager::readStageObj` is one function to Ghidra, starting at `0x00037550`.
Our database holds it as:

```
0x00037550..0x00037587   55 bytes   seed_vtable_thunk
0x00037587..0x00037604  125 bytes   seed_vtable_thunk
0x00037604..0x00037620   28 bytes   seed_vtable_thunk
0x00037620..0x0003767E   94 bytes   seed_vtable_thunk
0x0003767E..              60 bytes   seed_vtable_thunk
```

Back-to-back fragments, each ending exactly where the next begins, none with a
prologue. That is one function carved at every seeded address inside it. Across
the database, **1,334 of the 2,428 `seed_vtable_thunk` entries (54%) end exactly
where the next function begins**, which is what a carve looks like and not what
a real thunk looks like.

The consequence is the ABI violation: a fragment starting mid-body has no
prologue to match the epilogue it ends before, so it returns without restoring
`ebx`/`esi`/`edi` and with `esp` wrong.

## It accumulates

`regenerate.sh` writes `vtable_seeds_accum.json` -- the name is the problem.
The vtable scanner's discoveries are merged into an accumulating file and fed
back on the next regeneration, so a bad seed is permanent and each cycle can
only add more. That also explains why regeneration is not bit-stable in a way
that gets worse rather than randomly.

## What this does and does not explain

**Does:** 126 of the 131 measured ABI violations, concentrated in
`FileManager`'s asset readers and the `Opening` sequence.

**Does not, yet:** `CActBase::recursiveExec1Default` at `0x00011D00`, which is
`call_target`, not a seed. Its start matches Ghidra's and its end (`0x00011D9D`)
is exactly a `pop edi / pop ebp / pop ecx / ret` past its last block, so it is
not truncated. Its violation is most likely a *cascade*: it calls
`sub_00011B90` and three indirect targets, and it reads a spilled `ebx` back
from `[esp+0x10]`, which only survives if every callee balanced `esp`. A
violation in a callee therefore reappears as a violation in the caller.

That cascade is the reason the count should not be read as 131 independent
bugs. `RECOMP_ICALL_SAFE` restores `esp` on an *unresolved* target, so the leak
comes from resolved callees -- which the fragments are.

## Next

1. Do not regenerate against the current `vtable_seeds_accum.json`. It is the
   carrier.
2. Filter the seed merge: reject any candidate that falls strictly inside an
   already-detected function body, which is the Dashboard's rule applied to the
   file we actually use. `regenerate.sh` already has the merge step at the
   `vtable_thunk_seeds.json` -> `ACCUM` boundary.
3. Regenerate and re-run the ABI check. The prediction is that the 126 collapse
   and `recursiveExec1Default` either resolves with them or stands alone as a
   real second bug -- which is the measurement that decides whether any of this
   reaches the tutorial jump.
