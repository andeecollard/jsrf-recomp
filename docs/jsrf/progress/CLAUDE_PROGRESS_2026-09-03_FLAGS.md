# A branch that could only go one way, 110 more that still can

3 September 2026. Reviews Codex's cross-block flag-merge work and adds an audit
of the class it belongs to. Continues `CLAUDE_PROGRESS_2026-09-03_HEAP.md`.

## Codex's fix

`tools/recomp/translator.py` required the flag state on every incoming CFG edge
to be *identical* before a joined `jcc` could use it. Two predecessors that both
`cmp` but name different registers produced different states, so the state was
discarded and the branch fell back to `_flags` — a function-local the translator
initialises to zero and only the rep-string and xadd paths ever write. For a
`jcc` it is a constant zero, so **the branch is never taken**.

At `0x0013D3F3` that is a `jle` in the loader. Never taking it made the title
record loader status -1 and open `JSRF_FATAL.ERR` after the anti-graffiti
screen.

`_merge_predecessor_flag_states` now merges states that differ only in which
registers they named, when the setter kind (`cmp`/`test`/`bsf`/`bsr`) and the
operand width agree. That is sound for the right reason: the lifter snapshots
those operands into the same function-local `_fa/_fb/_fas/_fbs` at runtime, so
the joined `jcc` reads whichever predecessor actually executed. Width has to
match because `js`/`jns` cast back to the original operand width. Covered by
`tools/recomp/test_lifter_flag_merge.py`, backported to the preserved tree by
`backport_flag_merge.py`, gated as the `jsrf_flag_merge_backport` ctest.

This is the third recompiler bug of the same shape, after FCMOVcc: a branch on
a control path that silently went one way.

## What it changed, measured

Fresh HDD, `RECOMP_PB_EXEC=1`, `RECOMP_REPORT_MS=1200`, 100 s
(`claude-flagmerge-32`), against the identical run before it
(`claude-lowbase-31`):

| | before | after |
|---|---|---|
| `JSRF_FATAL.ERR` | written | **not written** |
| failed allocations | 207 | 207 |
| screens reached | 4 | 4 (the SEGA logo also sampled, so 5 of 5 known) |
| live-set curve | monotonic to the wall | fluctuates: 42.4 → 43.5 → 42.4 → 50.3 → 55.4 MB |

The disc-error path is gone: the title no longer decides it has a bad disc. It
does not yet get further — it sits on black after the anti-graffiti notice — and
the heap still fills.

**The contiguous leak is untouched**, which is the honest reading of G9:

```
export 166  MmAllocateContiguousMemoryEx  830 allocs / 32,483,736 bytes
export 171  MmFreeContiguousMemory        227 frees  /  2,203,648 bytes
export 184  NtAllocateVirtualMemory       583 allocs / 152,546,992 bytes
export 199  NtFreeVirtualMemory           260 frees  / 133,521,456 bytes
```

Same ratio as before the fix. Virtual memory still churns healthily; contiguous
memory still takes about 30 MB and never gives it back. The failing requests are
still 144 to 2,880 bytes from `ordinal 166 ra=0x00199789`.

So Codex removed the *consequence* — the title giving up and blaming the disc —
without removing the cause. Both were real; they were not the same bug.

## The class, counted

The merge fixes the shape it can prove. The fallback still exists for everything
else, and it is silent. `diagnostics/jsrf_first_fault/audit_unresolved_flags.py`
counts what survives in the preserved tree:

```
_flags branches with a real preceding write (legitimate): 3
dead fallbacks -- condition is constant zero:            110  in 78 functions
by condition: je=41, jne=17, jnp=11, jbe=8, jl=5, jge=5, jae=5, ja=4,
              jb=3, jg=3, loopne=3, jle=2, jp=2, loop=1
```

The 3 legitimate ones are `rep cmpsb`/`scas`/`xadd`, which do write `_flags`.
The other 110 are branches that can only go one way.

**Calibration, because this is easy to oversell.** Eleven of the 110 fall in the
render and texture-cache range 0x00145000–0x00156000. Checked individually, all
but `sub_0014B536` have no static call site and no dispatch-table entry, so they
are very likely cold. Nothing here says JSRF executes any of the remaining 110.
What it does say is that the one which mattered cost a boot, and that the tree
cannot currently tell you whether the others matter.

Two changes so it cannot hide again:

- `tools/recomp/lifter.py` marks the fallback `UNRESOLVED FLAGS, branch never
  taken` (and the setcc/cmovcc equivalents). The emitted code is unchanged —
  there is no correct conservative answer for a branch — but it is greppable
  instead of reading like ordinary output.
  `tools/recomp/test_lifter_unresolved_flags.py` covers it, including that a
  *resolved* branch carries no marker.
- `jsrf_unresolved_flags_ratchet` is a ctest pinned at 110. Not a gate — the
  tree does not pass a zero threshold — but the number cannot grow unnoticed,
  and it should be lowered whenever a fix brings it down.

## Where G9 stands

Unchanged in substance: roughly 30 MB of contiguous memory is allocated and
never freed, through a release path that demonstrably works, and enlarging the
arena does not help (the four-arena table in
`CLAUDE_PROGRESS_2026-09-03_HEAP.md`).

The next step is still the one in the handover: bracket `sub_00192830` and the
refcount test above it. Codex has already installed the probes for exactly this
in `instrument_startup.py` — `jsrf_resource_probe` sites on the allocator
vector, the canonical Release helper at `0x00192990`, the binding/lock release
at `0x00192A10`, and each of the destructor's three free paths — plus a texture
cache probe and an error-dialog probe. Those are in place and unexercised in the
runs recorded here; running them is the obvious next move.

## Validation

`ctest` 12/12 (adds `jsrf_flag_merge_backport` and
`jsrf_unresolved_flags_ratchet`). `test_combiner_trace.py` OK. `tools/recomp`
179 passed / 37 subtests, with the one pre-existing `test_config` ordering
failure that fails identically without any of these changes.

## Runs

- `claude-lowbase-31` — before the flag-merge fix. Writes `JSRF_FATAL.ERR`.
- `claude-flagmerge-32` — after. No fatal marker, same screens, same leak.
