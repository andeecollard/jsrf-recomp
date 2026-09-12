# Dropping interior seeds is safe, changes nothing, and names the real problem

Date: 2026-09-12 (Europe/London)

Third repair attempt on the carve from
`CLAUDE_PROGRESS_2026-09-12_VTABLE_SEEDS_SHRED_FUNCTIONS.md`. Aliasing an
interior seed regenerated cleanly and SIGBUSed before the first frame.
Dropping it instead runs, and the result refutes the hypothesis it was built on.

## The hypothesis, and what happened to it

*Most of the healed fragments are false interior function seeds; removing the
false boundary preserves the owner's prologue/epilogue and avoids the stack
corruption an aliased mid-body entry causes.*

Half right. Of 1,070 interior seeds, **895 are false and safe to drop** — and
**none of them was ever the problem**.

Classified by what actually references the address:

| class | count | action |
|---|---|---|
| `speculative` (no xref at all, or branch-only from inside the owner) | 895 | dropped |
| `indirect_target` (measured in `icall_targets.json`, or address taken as data) | 175 | kept |
| `direct_call` | 0 | — |
| `entry_point` / `declared_bound` | 0 | — |

778 of the 895 had **no incoming reference of any kind**; the rest were reached
only by a conditional or unconditional branch from inside their own owner,
which is what an interior label looks like.

## Runtime, against the same control

```
run            [RASTER]   ABI   triangles     outcome
control          25       131   171,605,623   SIGSEGV at the end
alias repair      0         3             0   SIGBUS immediately
drop repair      25       131   169,356,768   SIGSEGV at the end
```

Dropping restores a running build that reaches the same anchor as the control.
The database improves too: functions 10,292 -> 9,397, carved fragments
1,334 -> 495.

And the ABI offender set is **identical** — not merely the same size, the same
131 addresses. Of them:

* **0** were among the 895 dropped
* **77** are kept interior seeds, i.e. measured branch targets
* 54 are neither (49 `seed_vtable_thunk` the walk did not classify as interior,
  5 `call_target`)

So the speculative seeds were harmless all along, and every violation that
matters is an address **the title really branches to in the middle of a
function**.

## `readStageObj` is the counterexample, not the example

It was chosen as the sanity check on the assumption its interior seeds were
guesses. They are not: `0x00037550`, `0x00037587`, `0x00037604` and
`0x00037620` are all in `icall_targets.json`, the record of addresses the title
was *measured* branching to. The function stays carved after the drop pass
because keeping the first interior seed makes it the owner of the next, and so
on down the run.

These are genuine secondary entries into one function. Not a carve, not an
alias, not a drop.

## What the other projects say

The **Xbox Dashboard** is the direct precedent and it points the same way. Its
README records the function count going *down*, 6,323 -> 3,873:

> That earlier figure came from a hand-rolled "split-tail" scan that
> manufactured entry points; most were not functions, and 2,204 of them were
> hand-stubbed to `return 0`. The current number is what the disassembler
> actually finds, **plus the vtable targets the title is measured reaching**.

They retired the scan that manufactured entries and kept the measured ones —
exactly the split above. It is worth being precise about the direction: that
project is not an example of successful split-tail recovery, it is an example
of throwing split-tail recovery away.

**XenonRecomp** supplies the other half: explicit extents where analysis cannot
decide, which is what `--function-bounds` now is here.

And upstream's own `--seed-functions` is where the conflation lives. A seed
means *"explore here"*. A function means *"this address owns an independent ABI
frame"*. `seed_vtable_thunk` has been asserting the second while only ever
having evidence for the first.

## What is actually needed

A measured branch to a mid-function address is a **secondary entry**: one C
function for the owner, its single prologue and epilogue intact, entered at a
chosen label. An entry selector and a dispatch to labels inside the existing
body would do it. All three things tried so far fail because they each try to
express a secondary entry as something else — a separate function (carve, ABI
violations), a body sharing the owner's tail (alias, SIGBUS), or nothing at all
(drop, unresolved).

77 addresses need it. That is the whole remaining problem.

## Reproducing

```sh
RECOMP_SEED_INTERIOR=1 sh diagnostics/jsrf_first_fault/regenerate.sh
```

Off by default. The pass writes `disasm/interior_seeds.json` listing every
dropped and kept seed with its owner, provenance and xref classes. Declared
bounds and the entry point are never dropped.
