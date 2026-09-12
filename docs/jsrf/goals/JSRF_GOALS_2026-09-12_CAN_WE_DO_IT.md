# Goals — 12 Sep 2026: can this be decompiled and recompiled, and in what order

Supersedes `JSRF_GOALS_2026-09-12_MAC_IS_THE_TARGET.md` on ordering; its finding
that Windows is an instrument rather than a gate still stands.

## The question, answered separately for the two halves

They are different projects and conflating them has been costing us clarity.

**Recomp — yes, and it is close.** macOS reaches gameplay in ~30 s, GPU
rasterised, with audio and a real controller; a human played to the Corn
tutorial today. What remains is one gate (the tutorial jump) and a renderer
residency step. That is weeks of focused work, not years.

**Matching decomp — no, not as a from-scratch effort, and we should not try.**
KeybadeBlox's JSRF-Decompilation has been at it with a delinking-first method
and stands at **2.28% delinked, 0.38% of the XBE matched**. A matching decomp of
a 2.7 MB `.text` commercial title is a multi-year, multi-person undertaking.
Starting a second one would duplicate that work and finish later.

**The middle path is the one worth taking: a *named* recomp.** The mechanical
lift already runs. Adding names, types and structure to it gives most of what
people actually want from a decomp — readable, patchable, debuggable code with a
behavioural oracle — without requiring byte-matching object files. We already
have 1,721 function names and an object map to hang it on.

So: finish the recomp, name it as we go, and treat the matching decomp as
someone else's project we can learn from and should not duplicate.

## Where the evidence currently points

Today established, by measurement:

* 131 functions return with `ebx`/`esi`/`edi` changed at gameplay
  (`RECOMP_ABI_CHECK`, never previously run past startup).
* 96% of them are `seed_vtable_thunk` — the vtable-thunk feedback loop carving
  real functions into prologue-less fragments.
* Of 1,070 interior seeds, **895 are speculative and safe to drop** (dropping
  them reaches the same runtime anchor as the control) and **175 are addresses
  the title was measured branching to**.
* Dropping the 895 changes the ABI count not at all. **All 131 violations are
  the measured ones.**
* An external delinking map put one function nine bytes past its translation
  unit; `--function-bounds` now clamps it and the database has zero boundary
  straddles.

## The plan

### P1. Secondary entries — the live blocker *(next)*

77 addresses are branch targets in the middle of a function. Expressed as a
separate function they violate the ABI; as an alias they SIGBUS; dropped they
would be unresolved. All three fail because each expresses a secondary entry as
something else.

What is needed is one C function per owner — single prologue, single epilogue —
entered at a label:

```c
void sub_OWNER(uint32_t entry) {
    switch (entry) { case 1: goto loc_X; case 2: goto loc_Y; default: break; }
    ...
```

with the dispatch table built from `disasm/interior_seeds.json`, which already
lists every one with its owner extent. Callers reaching a secondary entry call
the owner with the selector.

**Done when:** the ABI offender count falls from 131 toward the three expected
`__SEH_prolog`/`__chkstk` false positives, and the build still reaches the
control's anchor. Both halves required — a reduced count on a corpse is not a
result, which this session learned the hard way.

### P2. The tutorial jump — the gate on "playable"

Unchanged and still the only thing between here and a Mac build someone can
play. Split input from animation with `RECOMP_PAD_TRACE` (edge-triggered;
`nonneutral` counts polls and cannot answer it). If input arrives, it is the
CPlayer animation gate and its 330 dwords of bone data.

P1 may or may not touch this. `CActBase::recursiveExec1Default` is the one
gameplay-class ABI offender and is a cascade victim rather than a carved
fragment, so it should resolve with P1 — or stand alone as a real second bug,
which is itself the answer.

### P3. The differential oracle

`dplewis/jsrf` patches a 6-byte `push imm32; ret` over a function in an
otherwise-retail XBE, and xemu has a GDB stub. For call counts and state at
entry no patch is needed at all — breakpoints on retail JSRF under `xemu -s`
answer it. `readInput` at `0x000659C0` is triple-confirmed (their kb.json, the
Ghidra table, our database) and sits on the input path.

### P4. Naming, continuously

Fold the 1,721 known names into the generated C's banners and diagnostics so
every future trace reads in JSRF's own vocabulary rather than in `sub_XXXXXXXX`.
This is the decomp half, done incrementally and without a matching requirement.

### P5. Merge and boundary guards

The upstream comparison that found the lost lock instruments was manual. The
object-boundary audit that found the straddle was manual. Both should be
commands that fail the build.

## What we deliberately are not doing

* A matching decompilation.
* Windows OHCI enumeration, or Windows performance.
* Resident depth clears.
* Any further attempt to express a secondary entry as a function, an alias, or
  a deletion.

## On the other projects

Read them; do not vendor from them. JSRF-Decompilation has **no licence file**
and its readme disallows LLM use within that repository — we take its addresses
and ranges as facts about the binary, as we already do with the symbol table,
and we do not copy its source or send it generated contributions.
