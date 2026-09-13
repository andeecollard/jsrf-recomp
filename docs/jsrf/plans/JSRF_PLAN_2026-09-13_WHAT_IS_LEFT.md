# What is left in the pipeline, after a day of auditing it

13 Sep 2026, written after the comiss fix, the flag-dataflow work and rcl/rcr.
Supersedes the priority list in JSRF_PLAN_2026-09-13_PIPELINE_SEMANTICS_AUDIT.md,
which is still the right description of the bug CLASS.

## The method that found everything today

Every bug this session had the same shape and the same tell.

**Shape:** an x86 behaviour emitted as the nearest C construct, where C differs
at one edge, or emitted as nothing at all. Ordinary inputs pass either way, so
tests and casual play never see it.

**Tell:** split the population by REACHABILITY first. Every raw count in this
pipeline is dominated by functions nothing calls -- data swept as code -- and
reading the raw count sends you to the wrong component:

    dead branches         90 total  ->   4 reachable  (86 unreachable)
    unimplemented insns  165 total  ->   4 reachable  (161 unreachable)

Both raw numbers barely moved today. Both reachable numbers fell by ~78%.
Report the split, always.

## Closed today, with evidence

  * `comiss` on NaN. ZF/PF/CF all set; C's `==` false and `!=` true. Froze the
    Corn tutorial for two days by making a ray-plane test report a hit on a
    zero-length segment, which death-warped both CPlayers every frame.
  * `cvtss2si` emitted as truncation; float->int of NaN/inf/range was UB.
  * Variable shift counts unmasked (339 sites); ROL32/ROR32 undefined at count 0.
  * Flag dataflow swept once in address order, so any join whose predecessor sat
    at a higher address was abandoned before the merge was called.
  * Width-mismatched snapshot joins refused outright, when ZF is still knowable.
  * `rcl`/`rcr` emitted as nothing, inside MSVC's 64-bit shift.

  * **Overlapping functions are NOT truncation.** All 1599 overlapping pairs
    were checked: in every one the outer function's body emits code covering the
    interior range. The aliasing duplicates, it does not steal. This had been
    open since the vtable-seed work and only one case had ever been hand-checked.

## What is left, in priority order

### 1. The four reachable unimplemented instructions
`stmxcsr`, `in`, `wbinvd`, `out` -- one each. `stmxcsr` is the interesting one:
it writes the SSE control word to memory and emitting nothing leaves the
destination holding whatever was there, so any subsequent test of the rounding
mode or exception mask reads garbage. The other three are port I/O and cache
control, plausibly ignorable on this host, but they should SAY so rather than
be a comment that reads like ordinary output.

### 2. The four reachable dead branches
`sub_00130FD0` (1), `sub_001237A0` (2, `jg`), `sub_00015130` (1, `jne`),
`sub_000A0F10` (1, `je`). The `jg` pair needs OF, which no merge currently
answers across a join. `sub_00015130`'s survivor joins seven predecessors.

### 3. Re-baseline the ratchet
It is set to 76 and the tree is at 90, so `jsrf_unresolved_flags_ratchet` fails
regardless of anything anyone does -- a gate that cannot pass teaches nothing.
Decide whether it counts the total or the reachable split (now reported by
`audit_unresolved_flags.py`) and set it to today's number.

### 4. Retire the obsolete backports
Six `backport_*` checks fail because the fix they hand-apply now lives in the
lifter; `jsrf_challenge_flag_merge` started passing by itself when the
fixed-point change landed. They are 6 of the 11 ctest failures and they hide
whether anything real breaks.

### 5. The disassembler, which is where the 161 + 86 actually live
Everything unreachable is one finding, not two hundred: the linear sweep walks
into data. `in`, `out`, `bound`, `arpl`, `hlt`, `sti`, the BCD family and
`lcall` are not in MSVC output for an Xbox title. Bounding the swept regions --
or marking unreachable functions so the counts exclude them by construction --
would retire most of both lists at once.

### 6. Runtime model gaps, unaudited
x87 is modelled as 8 doubles; real x87 is 80-bit with tags. ARM's default NaN
is 0x7FC00000 where x86's indefinite is 0xFFC00000, so anything testing a NaN's
sign diverges. Our heap hands back dirty memory where the Xbox's was zeroed.
None has a known symptom; all are real differences.

### 7. One UBSan run
`-fsanitize=undefined` over a gameplay run would have caught the shift and
float-cast UB directly rather than by reading code. Cheap, and it covers classes
nobody has thought to look for.

## Verification is the bottleneck now, not the changes

Each translator change needs a regeneration (~40 s), a build (~4 min) and a
playthrough to `+7930/+7934 == 1/2`. The playthrough is the expensive part,
because roughly half of runs never leave the title.

**The title stall is therefore the highest-leverage thing in this document**,
even though it is not a translator bug. What is known: boot time predicts it
(logos ending before 25 s reached the tutorial 7 times in 11; after 29 s, 2 in
9) and the distribution is bimodal with a clean gap. The slow mode does
IDENTICAL work -- 2441 vs 2469 file opens, same kernel, heap, GPU and notify
counts -- just 1.6x slower throughout, and the guest drops to ~54 controller
polls per second against a healthy ~177.

Two hypotheses are dead: there is no extra step in the slow path, and it is not
process priority (a `nice 5` run booted in 22.3 s). Fast boot is not sufficient
either -- a 23.07 s boot still stalled. What remains is contention: the slow
runs cluster after builds and regenerations, when iCloud and Spotlight are
churning over a freshly written gen tree that lives inside iCloud Drive. That
fits "same work, uniformly slower" and would fit the bimodality if the
contention is effectively on or off.

**Do first:** move the build tree out of iCloud's path, or exclude it, and
re-measure the stall rate. If it drops, every future verification gets cheaper
and the rest of this document gets easier.
