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

### 0. m_bFatal is set DURING GAMEPLAY, and that is the new frontier

Found the moment the title stall stopped hiding it. A run that passes the
tutorial and reaches `live=70` runs healthy for about six reports and then:

    [ANIM-BLOCK] id=44 ... MOVED      <- still animating
    [ANIM-BLOCK] id=45 ... MOVED
    [JSRF-FATAL] m_bFatal=1           <- flips here
    ... 11 further reports, both players `static runs=9`

The poses are still moving when the flag flips, so the freeze is a CONSEQUENCE:
`CActMan::Idle()` runs `IdleSub()` while +0x24 is clear and
`readInput(); Sleep(0x10);` forever once it is set. The animation stopping is
the idle loop, not a second animation bug, and chasing the pose here would be
chasing a symptom for the second time in one day.

`CActMan::Fatal()` at 0x00012770 is NOT called -- that was measured on 12 Sep --
so one of the other writers of `[reg+0x24]` is responsible. There are **710**
stores to that offset in .text and none writes an immediate 1, so it arrives
through a register and static enumeration will not find it.

**Approach, in order of cost:**

  1. Characterise it first, because it is now cheap to reproduce. Is the flip
     at a fixed TIME, a fixed number of frames, or tied to an action? Several
     runs reaching gameplay and left alone will say, and "always at report 6"
     versus "when the player does X" point at completely different writers.
  2. `RECOMP_MEM_WATCH` on root+0x24 is the obvious instrument and is recorded
     as not working: arming it needs a regeneration that SIGBUSes, and on a
     working build it logs nothing, silently. Re-test it rather than trust the
     note -- the tree has changed a great deal since -- but do not spend a day
     on it.
  3. Failing that, narrow the 710 by reachability the way everything else was
     narrowed today, then by which candidates can hold the CActMan pointer, and
     put O(1) counters on what survives. That is exactly how the +0xE6C writer
     was found: two counters, one run, unambiguous.


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

## The title stall is SOLVED, and it was the build tree's location

Resolved the same day this was written, so the section below is kept only for
the reasoning that led here.

`build-macos` was inside iCloud Drive. Every regeneration and link handed
`cloudd`, `bird`, `fileproviderd` and Spotlight four gigabytes of freshly
written files, and the contention is what starved the guest. The tree now lives
at `/Users/andrewcollard/jsrf-build` with `build-macos` a symlink to it -- a
rename on the same APFS volume, so nothing was copied and every hardcoded path
still resolves.

    before:  logos end 15.5 - 31.3 s, bimodal with a clean gap, ~50% stall
    after:   7 runs, 7 reached the tutorial
             22.69 22.73 22.68 22.67 22.68 22.87 (and one at 17.40)

Six of seven within 0.2 s of each other, against a 16-second range before. A
variance collapse like that is the signature of removing a contending process,
not of a lucky sample -- which is what makes seven runs enough here.

It also retires the guesswork in the earlier sections: boot time DID predict the
stall, but boot time was itself the symptom. Two hypotheses died on the way and
are worth not re-running: there is no extra step in the slow path (identical
file, kernel, heap, GPU and notify counts, uniformly 1.6x slower), and it is not
process priority (a `nice 5` run booted in 22.3 s).

Do not "tidy" the symlink away, and do not move the exclusion into the tracked
`.gitignore`: that file has `build-macos/`, a directory pattern that does not
match a symlink, and it is upstream's. The exclusion lives in the repo-local
exclude file, which for this worktree is the COMMON dir's -- find it with
`git rev-parse --git-common-dir`.

Only the build output moved. The repo source is still in iCloud, and that was
evidently enough.

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
