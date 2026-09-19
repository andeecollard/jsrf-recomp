# JSRF goals — every edge is counted, 19 September 2026 (evening)

Supersedes `JSRF_GOALS_2026-09-19_ONE_ROOT_CAUSE_UNDER_TWO_ITEMS.md`. That
file keeps the full evidence for G20 (the switch arm) and G21 (the texture
path); this one records what happened to G20, adds **G22**, and resets the
order.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

G1–G21 carry forward. The control-flow completeness work takes **G22**.

## What changed since the midday file

**G20 is regenerated, built and played.** The merged tree was regenerated at
12:45 (gen `c4400dbb0025f56d`, head `2bf7c1d`), built at 14:13, and the
player ran it for 1209 seconds with zero `[ITAIL]` failures, no black frame,
no guest fault and no ADX freeze (`2cf2d9e`), against 307 seconds for the
morning's build. A pad recording of that session exists:

    ~/Library/Application Support/JSRF/padrec/graffiti-2026-09-19_1309.padrec   (+ .KEEP)

Its header reads `#!gen c4400dbb0025f56d`. **That is the translator hash**, and
it is the whole of what the replay check compares (`xinput_device.c:1427`,
stamped from `JSRF_GEN_TRANSLATOR`, which comes from the gen's manifest). Any
change under `tools/recomp` changes it at the next regeneration, and every
binary built on that regeneration refuses this recording.

**What the played session opened**, from the same commit and `c6ed54a`: the
text defect has a second symptom (variable leading-character loss, a line
flush against the window's left edge), the corruption is animated rather than
stable, the string in guest memory is whole (measured 29 times) so what is
wrong is *which span* of it we draw; and graffiti does not plant while the
sound fires and the can count falls. Those are G2's and G21's evidence and
stay in their files.

## THE ORDERING CONSTRAINT, restated for G22

G22b below changed `tools/recomp/lifter.py`. It is **not regenerated**, on
purpose. The current gen, the 14:13 binary and the 13:09 recording all still
agree on `c4400dbb0025f56d`.

The runtime half of G22b lives in `recomp_types.h` and `recomp_manual.c`,
which are compiled from the repo. The header was **hand-copied into the gen
tree** (the legitimate move `jsrf_gen_header_current`'s comment describes) so
that test stays green and the binary carries `recomp_unimpl` — which nothing
in the current gen calls yet. Two configure-time warnings now fire and are
expected until the next regeneration:

    STALE GENERATED TREE   -- tools/recomp changed (the lifter), gen did not
    STALE RUNTIME TYPES    -- the manifest's header sha predates the hand copy

**Do not regenerate to silence them.** Regenerate when the 13:09 recording has
been spent — replayed for what it was taken for, or the player decides a new
session is cheaper than the old one — and bundle every pending translator
change into that one regeneration, as the midday file's rule says.

## G22 — control-flow completeness: every edge accounted for, and gated

The midday file named two gates that worked, coverage and the unresolved-table
count, and both lived in prose. Neither was a test. A regeneration that
stranded a hundred more arms would have built green. And the lifter's answer
to an instruction it cannot translate was a bare C comment: the instruction
vanished, and nothing at runtime could say the site had even been reached.

### G22a — the gate — DONE

`diagnostics/jsrf_first_fault/control_flow_gate.py`, registered as
`jsrf_control_flow_gate`, classifies **every** transfer the gen tree emits and
holds each unresolved or silent count at or below
`control_flow_baseline.json`, coverage and resolved switches at or above it. A
shape it does not know fails the run (it found one on its first pass:
`RECOMP_ITAIL(MEM32(eax))`, a slot jump with no displacement, now classified).
Its switch-arm numbers reproduce `switch_arm_audit.py` exactly.

The baseline, on gen `c4400dbb0025f56d`:

| class | count | reading |
|---|---:|---|
| direct calls / direct tail jumps | 43,170 / 3,578 | translated |
| gotos, of which switch arms | 65,878 / 3,757 | translated |
| switches lifted to gotos | 487 | translated |
| indirect calls through the dispatch table | 7,418 | runtime-resolved |
| tail jumps through a slot / global / register | 291 / 13 / 13 | runtime-resolved |
| **unresolved switch dispatches / tables** | **152 / 62** | G20's shape |
| **switch arms within the window, with no body** | **571 / 487** | the damage |
| call targets that reach a stub | 168 | detector never defined them |
| **untranslated instructions, all silent** | **122 / 122** | G22b |
| conditional branches with no target / bare `jmp` with none | 0 / 0 | |
| functions / unique bytes covered | 8,846 / 1,613,483 | union of extents |

Accept a deliberate change with `--write-baseline`, in a commit that says why.
The gate cannot see a branch translated to the *wrong* place, only to nowhere,
and cannot say whether an edge is ever taken; those are G22c and G22d.

### G22b — no silent omission — DONE IN THE LIFTER, PENDING REGENERATION

Eleven exits in `lifter.py` returned `/* TODO: mnemonic */`; three recorded the
site in the translator's tally and eight did not. All eleven now go through
one helper that records the site and emits

    RECOMP_UNIMPL("mnemonic ops", 0xVAu); /* TODO: mnemonic ops */

`recomp_unimpl` logs the guest address, hit count and eight registers on the
ICALL cadence, and under **`RECOMP_UNIMPL_TRAP=1`** aborts at the first hit —
at the cause, not downstream. Default off; a healthy run reaches none of these
and prints nothing. Deliberately ignored instructions (`RECOMP-IGNORED-PRIV`,
cache hints) carry no marker; `tools/recomp/test_lifter_unimpl.py` pins both
sides. The 122 sites are `bound`, `arpl`, `hlt`, `outsb`, `sti`, `enter`,
`daa`… — what a linear sweep reads over data — so most are probably dead.
*Probably* is the word this goal exists to remove.

**Done when:** after the next regeneration `todo_silent` reads 0 in the gate,
and one scripted run with `RECOMP_UNIMPL_TRAP=1` either completes or names a
live site by address. Either answer is a result.

**Two things the TODO list says that need a look, no run needed:** `popfd`
has 4 sites although the handover says upstream #72 (popfd) was applied
locally — either the applied form does not cover these operands or the gen
predates it; and `hlt` has 7, which the lifter deliberately refuses to no-op
because it would turn a wait into a spin.

### G22c — the runtime side, per run — OPEN

The static classes above have runtime counterparts: `[ITAIL]`, `[ICALL]`,
`[UNIMPL]`. Nothing joins them. A per-run summary — which unresolved sites
were *reached*, how often, in which class — beside the gate's static counts
would say which of the 152 dispatches and 122 sites the title actually
touches. The corpus tooling that tallied 17 freezes is the place to add it.

### G22d — the reference execution — OPEN, and bounded

There is no native 32-bit x86 on this machine and Rosetta cannot provide
one, so "compare guest and reference at the first divergence" cannot mean a
whole-program trace. What exists is `tools/conformance/fuzz_unicorn.py`:
register-only integer instructions against Unicorn, no memory, no FPU, no
SSE. Widening it to memory operands and the x87 stack is the only route to a
second opinion on translation *correctness* (as opposed to completeness), and
every adjudicated mismatch becomes a permanent case. Unicorn is a model, not
silicon; a disagreement is a lead.

### G22e — the 76 parked tables — OPEN, carried

Unchanged from the midday file: the remaining unresolved tables need a
*retraction* rule in the detector, and the thread to pull is
`_find_function_end`'s stopping rule on alias bodies. Route (b), loosening
the lifter's ≥2-arms rule, was measured unnecessary. Do not reopen it without
a number.

### G22f — explain the drop from 84 to 62 — OPEN

The midday file counted **84** unresolved tables, **607** arms and **8,866**
starts and quoted coverage **1,614,033**. The 12:45 regeneration reads **62 /
487 / 8,846 / 1,613,483**. The two detector fixes account for four or five
tables by their own commit messages, not twenty-two. The discovery loop is
stateful (`vtable_seeds_accum.json`, `icall_seed.json`), so a regeneration is
not bit-stable and the earlier numbers were taken on the shared gen and on
worktree trees, not this one. Until someone diffs the two `functions.json`
files, **do not describe 84→62 as "22 tables fixed".** The gate pins the
current numbers so the next drift is at least visible.

## The order

1. **Replay the 13:09 recording once, before anything regenerates.** It is
   the only recording, it is bound to the current gen, and the four questions
   in the midday file's §3 are answered by it or not at all. This costs no
   player time.
2. **G22b's regeneration rides with the next deliberate one.** Not before.
   When it happens: run the gate (expect `todo_silent` 0, nothing else
   rising), then one scripted run under `RECOMP_UNIMPL_TRAP=1`.
3. **G21, the texture path** — the largest win and the "everything on the
   GPU" goal's real content. First step is unchanged: a run with
   `RECOMP_METAL_EARLY_Z=1`, because the early/late counter reads nothing
   with the switch off.
4. **G2, the text** — now known to be a wrong *span* of a whole string, moving
   over time. Aim the next capture at the speech box directly; the timing is
   known.
5. G22c, G22d, G22f as background work that needs no player.

## Rules added today

- **A gate in prose is not a gate.** Two numbers that decided whether a tree
  was good sat in a goals file for a day with nothing enforcing them.
- **A comment is a no-op.** An instruction the translator cannot handle must
  leave something the runtime can report, or its failure will be found from
  the wrong end.
- **Numbers from different gens do not compare.** The manifest says which gen
  a number came from; quote it with the number, or the next reader will
  subtract two measurements of two different trees and call it progress.
- **A recording is bound to a translator hash.** Editing `tools/recomp` is
  free; regenerating is what spends the recording.
