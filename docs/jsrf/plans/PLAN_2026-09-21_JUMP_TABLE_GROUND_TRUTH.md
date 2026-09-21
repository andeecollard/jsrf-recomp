# 21 September 2026 — step 0b measured: the 405 dropped rows are NOT G22's answer

Closes step **0b** of `PLAN_2026-09-21_NIGHT_ONE_RUN_ANSWERS_THREE.md`. The
filter is fixed and the rows are recovered. The premise that they feed G22 is
**not supported by the data**, and this file records the measurement so nobody
re-derives it.

Measured at HEAD `8d765ed`, against gen `/Users/andrewcollard/jsrf-build/jsrf-first-fault/gen`
and `control_flow_baseline.json` (gen `46bb115cdb053f4e`, head `3b9d2fc`).
The gate holds at that baseline as of this measurement.

## What the 405 rows are

`merge_symbols.py:34` kept decomp rows with `len(f) > 6`. All 1,347 `func` rows
have 8 fields or more; all 405 `data` rows have exactly 5, so every one was
dropped and nothing said so. Broken down:

| n | what | control flow? |
|---:|---|---|
| 90 | C++ vtables (`CActBase::\`vftable'`, `CopSpawnView::vtable`, …) | already `itail_vtable`, runtime-resolved |
| 89 | scalar globals (floats, `g_cameraSpeed`, `epsilon_pos`) | no |
| 57 | typed arrays (`voiceLineMapping[623]`, `musicMapping[56]`) | no |
| 52 | pointer / function-pointer globals, incl. 4× `SwitcherMethod *[119]`, `ActSequenceMethod *[64]`, `MovementStateFunc *[27]` | already `icall`, runtime-resolved |
| 51 | strings and string-pointer tables (`g_soundBanks[57]`) | no |
| 40 | MSVC EH tables (`_unwindmap`, `_funcinfo`) | no |
| **9** | **jump tables (7 `__jumptargets` + 2 `__jumptable`)** | **the reason 0b existed** |
| 6 | COM IIDs | no |
| 6 | CRT init/term markers (`__xi_a`, `__xc_z`) | no |
| 5 | XDK globals (`D3D8::D3D__RenderState`, …) | no |
| **405** | | |

The **9 jump-table rows are the only 405-row members inside `.text`**
(0x11000–0x18CB30). The other 396 sit in `.rdata`, `.data` or `D3D` — sections
the lifter never sweeps as code. 402 of the 405 addresses are new to
`jsrf-symbols-merged.tsv`; 3 are already in it mislabelled `func`, which is
step 0c's point, not this one.

## THE NUMBER: 1 of 62

`RECOMP_ITAIL(MEM32(reg * 4 + 0x…))` appears 152 times over **62 distinct
tables** in the current gen — the baseline's `switch_unresolved` /
`switch_unresolved_tables`, reproduced exactly. Against the decomp's 9:

**Exactly one** decomp jump table is one of the 62 unresolved tables:

    0x0007C9C0  void *[5]   CActSequence::ReturnFromFullRoboyMenu__jumptargets
    0x0007C9D4  byte[12]    CActSequence::ReturnFromFullRoboyMenu__jumptable

The other **8 are already resolved statically**, and the lifter's own emitted
comment agrees with the decomp's element count on every one of them:

| table | decomp says | lifter emitted |
|---|---|---|
| 0x39BC8 / 0x39C5C / 0x3AE88 | `void *[4]` | `switch: 4 entries, 4 targets` |
| 0x3A734 / 0x3B928 | `void *[4]` | `switch: 4 entries, 2 targets` |
| 0x7BE08 | `void *[2]` | `switch: 2 entries, 2 targets` |

That is a clean independent confirmation of the table walker and **zero new
information**.

## And the one match would not be fixed by the data row either

The size is not what we get wrong. Read straight out of the XBE, 0x7C9C0 holds
five `.text` pointers and a sixth word (0x02010400) that is not `.text`, so the
gate's existing "walk until the value leaves `.text`" already stops at exactly
5 — matching `void *[5]` — and the byte table at 0x7C9D4 is bounded by the
`cmp eax, 0xB` two instructions above the dispatch, matching `byte[12]`.
**Both numbers are already recoverable statically.**

What is actually wrong is a **function boundary**. `tools/disasm/functions.py`
requires `func_start <= arm < func_end` (see its note at :1356), and:

    dispatch site 0x0007C8B3   ->  our func 0x0007C600..0x0007C8C1
    all 5 arms 0x7C8C1..0x7C9B5 ->  our func 0x0007C800..0x0007C9BE

Two overlapping extents, the arms all in the *other* one, so the first arm
fails the test and the switch stays unresolved — and the same dispatch is
emitted into three different function bodies (`sub_0007C600`, `sub_0007C720`,
`sub_0007C7E0`), which is why one table accounts for 3 of the 152.

The decomp's **`func`** rows say what the boundaries should be:

    0x0007C600  CActSequence::LoadFullRoboyMenu
    0x0007C720  CActSequence::StartFullRoboyMenu
    0x0007C7E0  CActSequence::WaitEndFullRoboyMenu
    0x0007C800  CActSequence::ReturnFromFullRoboyMenu   <- owns the dispatch
    0x0007C9E0  CActSequence::PrepareStoryOrVsMission

Our 0x7C600..0x7C8C1 swallows three of them. **That is a `func`-row finding,
and `func` rows were never dropped** — they have been in
`jsrf-symbols-merged.tsv` the whole time.

## Verdict: do not build a jump-table feed

A lifter path that consumes decomp jump-table extents would cost a translator
change, a regeneration (invalidating the 13:09 pad recording and every
`c4400dbb0025f56d` binary), and a new external-data dependency the licence
says must live outside the repo — to move `switch_unresolved_tables` from 62
to **at best 61**, on a menu transition, and only if the boundary bug is fixed
first, at which point the decomp data is redundant. It does not pay.

## What to do instead, when G22 is next picked up

1. **The decomp's 1,347 `func` starts are a function-EXTENT oracle, and it is
   already sized.** Measured against `disasm/functions.json`:

   - 1,333 of them land exactly on one of our 8,846 function starts (99.0%),
     4 land strictly inside one of our extents, 10 in none.
     **Starts are not the problem.** Ends are.
   - 1,592 of our 8,846 extents (18%) run past the next function's start.
   - Of the 1,333 our-functions the decomp also names, **107 have an `end`
     that runs past the decomp's next function start** — an over-long extent
     the decomp can adjudicate. `0x0007C600` is one of them, and its
     over-reach to 0x7C8C1 is exactly what strands the 0x7C9C0 arms.

   107 is a candidate set, not 107 bugs: some are the detector deliberately
   extending to a shared tail (the 0x4AD10–0x4B134 chain has eight starts
   ending at one address, which looks intentional). Triaging 107 rows by hand
   is a session; it needs no run, no regeneration and no new data source, and
   the licence allows it because it is a *comparison*, not a copy.
2. Only then re-measure `switch_unresolved`. If the extent oracle closes the
   0x7C9C0 case, the jump-table question answers itself.
3. 4 of the 5 arms at 0x7C9C0 have no translated body, so this *is* a live
   `[ITAIL]` gap — but the 1,209-second session on 19 Sep saw zero `[ITAIL]`
   failures, so the path is cold. Treat it as a correctness debt, not a bug
   with a symptom.

## What changed in the tree

`diagnostics/jsrf_first_fault/merge_symbols.py` only.

- `jsrf-symbols-merged.tsv` is **byte-identical** to before (verified by diff).
  `symbolize.py` rejects any row whose second field is not `func`
  (`symbolize.py:55`), so the data rows would have been dead weight there.
- The 405 `data` rows now go verbatim, in the decomp's own 5-field shape, to
  `~/jsrf-build/jsrf-symbols-data.tsv` — outside this repo, beside the merged
  TSV, per the licence constraint.
- Every row is counted by kind and the script prints `seen` / `kept` /
  `DROPPED`, with a reason and an example for anything it routes nowhere, and
  exits non-zero if the three do not add up. A future silent drop is now a
  line of output.

```
symboltable.tsv: 1752 rows
    func                 seen  1347
    data                 seen   405
    data -> sidecar      kept   405
    func -> merged       kept  1347
    DROPPED                       0
360 XDK + 1347 decompilation func -> 1695 rows in .../jsrf-symbols-merged.tsv
405 decompilation data rows -> .../jsrf-symbols-data.tsv
```
