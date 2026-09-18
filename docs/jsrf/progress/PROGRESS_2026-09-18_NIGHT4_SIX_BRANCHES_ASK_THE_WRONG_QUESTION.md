# Six branches ask the wrong question — 18 September 2026, night (4)

A live control-flow defect in the player's build, found while scoping the
upstream flag work. Not latent, unlike the three PRs sent tonight.

## The defect

A `jcc` can only be resolved from whatever last set the flags. For
`cmp`/`test` the lifter snapshots the operands at the comparison
(`_snapshot_flags`, `lifter.py:2164`). For the **result-setter** family --
`and`, `or`, `xor`, `add`, `sub`, `neg`, and the shifts -- it emits no flag
code at all and instead rebuilds the condition at the consumer by
**re-reading the destination operand** (`lifter.py:436-438`).

Nothing checks that the destination still holds the result. `mov` is in
`_EFLAGS_PRESERVE`, so flag tracking walks straight over it:

    and eax, 3        ->  eax = eax & 3;
    mov eax, [esi]    ->  eax = MEM32(esi);
    je  target        ->  if ((eax == 0)) ...     <- tests the LOADED value

This is the same bug class that was already fixed once here. `inc`/`dec` were
moved off re-reading for exactly it, and the comment at `lifter.py:761-763`
records why: *"JSRF's ADX loop reloads ECX with its input pointer after DEC
ECX; testing live ECX made that sixteen-iteration loop run indefinitely."*
The reasoning was never applied to the rest of the family.

Reproduced on all six: `and`, `or`, `xor`, `add`, `sub`, `shl`.

## Six live sites, in real code

Scanned all nine generated chunks for a `jcc` naming a bare register whose
nearest preceding write is not a flag-setting form. **789 bare-register
conditions, 6 clobbered (0.9%)**, each in a different function:

| function | the clobber | the branch asks | it should ask |
|---|---|---|---|
| `sub_000A2200` | `eax = MEM32(ecx + 0xC70)` | loaded dword == 0 | `eax - 4 == 0` |
| `sub_000A6510` | `eax = 0` (`mov eax, 0`) | **0 == 0, always taken** | `ebp + ecx == 0` |
| `sub_00157E20` | `edx = MEM32(eax + 8)` | loaded dword == 0 | `edx - 2 == 0` |
| `sub_00158030` | `edx = MEM32(eax + 8)` | same shape | same |
| `sub_0015B050` | `edx = MEM32(eax + 8)` | same shape | same |
| `sub_0017CA70` | `ecx = MEM32(esp + 0xC)` | loaded dword != 0 | `ecx \| eax != 0` |

`sub_00157E20` in full, and it is the clearest:

    loc_00157E2A: ;
        edx = edx - 2;                        /* sub edx, 2 -- the flag setter */
        edx = MEM32(eax);                     /* a 12-byte struct copy through edx */
        RECOMP_MEM_WRITE32(..., ecx, edx);
        edx = MEM32(eax + 4);
        RECOMP_MEM_WRITE32(..., ecx + 4, edx);
        edx = MEM32(eax + 8);
        RECOMP_MEM_WRITE32(..., ecx + 8, edx);
        if ((edx == 0)) goto loc_00157E44;    /* the third loaded dword */

Three of the six are that same struct-copy shape in adjacent functions, so it
is probably one inlined routine repeated.

**These are in real code.** Zero `hlt`/`outsb`/`in`/`out` decodes within ±60
lines of any of them -- the marker that distinguishes genuine functions from
the linear sweep running out of phase over data. Compare the six `popfd`
sites, which are all surrounded by exactly that garbage and are latent for
that reason.

## What is NOT established

**That any of these execute.** "In a real function" is not "on a hot path",
and there is no profile in this tree. The honest claim is: six branches in six
real functions ask a different question from the one the guest asked, and one
of them is unconditional where the guest wanted a test.

## The positive control, because a 0.9% hit rate on a heuristic needs one

The scanner flags a condition when the nearest write to the register it names
is not a flag-setting form. Run against a hand-written file containing one
clobbered and one clean case it reports exactly 1 of 2. Two false positives
were found and removed while calibrating it, and both are worth recording:

- `eax = 0; /* xor self */` **is** a flag setter (`xor eax, eax` sets ZF), and
  is distinguishable only by the emitted comment. A bare `eax = 0;` is
  `mov eax, 0` and is a genuine clobber -- that distinction is what makes
  `sub_000A6510` a real hit rather than a miscount.
- `edi = (uint32_t)(-(int32_t)edi);` is `neg`, which is in `FLAG_SETTERS` and
  writes its own destination, so re-reading it is correct.

## The fix, and what it costs

Snapshot the result at the setter, as `cmp`/`test` already do and as
`inc`/`dec` were moved to. It changes generated code for every result-setter
that feeds a `jcc`, so for this tree it needs a **regeneration** and belongs
in the batched one.

That raises what the batched regeneration is worth: it would carry a live
correctness fix rather than only the performance items, and the performance
items have both shrunk this week (the descriptor table to ~0.4 ms, dead-flag
elision to ~1% of guest loads).

Upstream has the identical code, so it is also the fourth thing we owe them --
and the only one of the four that is not latent for somebody.
