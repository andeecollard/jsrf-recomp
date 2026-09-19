# The freeze and the skating crash are the same bug: a switch arm the lifter could not place

**Read the root-cause section first if you read nothing else.** This note was
written front-to-back over one morning and the early sections record what was
believed at the time. Two of their conclusions are corrected later in the file,
in place, and both corrections are marked. The title it was filed under was
"the freeze is an unresolved indirect call", which is true and is not the
point.

19 Sep 2026, from the 782-run scripted corpus under
`build-macos/jsrf-first-fault/render-investigation/` and the player's
preserved logs in `~/Library/Application Support/JSRF/`. No new runs: every
number here comes from runs that had already completed.

Supersedes item 0.1 of
`HANDOVER_2026-09-19_DAY_THE_STUTTER_IS_THE_FRAME_RATE_AND_THE_FREEZE_IS_THE_GATE.txt`,
which names it "THE ADX FREEZE" and treats it as a sound-worker problem. The
sound worker is a victim, not the cause.

## What the handover says, and what is actually in the logs

The handover: *"Every unattended run this afternoon died at t=60-90 s with
'[ADX] tick STUCK': the guest stops flipping and the log grows by a million
lines with no frames."* It puts this first, because nothing else can be
measured until it is fixed. That part is right. The diagnosis is not.

`jsrf_adx_watch` (`diagnostics/jsrf_first_fault/main.c:1497`) is a watchdog on
a guest tick counter. It reports that the sound worker stopped. It does not
say why, and nothing downstream of it ever asked.

## What actually happens, in one run, in order

`render-investigation/base1/stderr.log`, lines 16074-16078:

```
  [PAD-SCRIPT] t=  60.01 fire #18 buttons=0010 a=0 b=0 lx=0 ly=0 poll=10046
  [GPU] draw #336600
  ...
[ICALL] Failed to resolve VA 0x00114B66 caller=0x1033ecac0 (total calls: 9444077)
[ICALL]   recent targets: 0014D080 00011C80 0014D030 000B3DE0 00153E30 ...
[ICALL] skipped not-code target 0x00000000 (1 times)
```

and then that last line again, and again, to
`(10863700000 times)`. Ten point eight **billion** indirect calls to NULL. The
run's log reaches 1,749,336 lines, 1,735,600 of them that one line.

Meanwhile the picture is gone and stays gone. From t=60.00 to the end of the
run at t=149.00, ninety consecutive `[FB]` reports read

```
  [FB] t= 149.00 0x005F0000 sum=1A9B999B nonzero=153372/153600 same | presented nonzero=305148
```

— the same checksum, ninety times, `same` every time. The guest is not slow.
It has stopped.

## It is the unresolved call, in every case

Classifying all 782 corpus runs by whether the NULL call *ran away*
(>= 1e6 calls) rather than merely occurred — a handful of skipped NULL calls
is ordinary background here and harms nothing:

| | runaway storm | trickle | none |
|---|---|---|---|
| a START press (`buttons=0010`) in the run | 15 | 8 | 564 |
| no START press | 2 | 12 | 181 |

**All 17 runaway storms have an unresolved indirect-call VA. No exceptions.**

* 15 of 17 are `0x00114B66`, at 2.2e9 to 23.4e9 calls.
* 2 of 17 are `0x0007E575`, at 2.1e6 and 5.3e6 — the same failure mode, two
  orders of magnitude smaller, and reached with no pad input at all.

The trigger is ordinary: `0x0010` is START, and the schedule presses it. In 14
freezing runs checked individually the unresolved VA and a preceding
`buttons=0010` are both present, 14 times out of 14. **But START is not
necessary** — the two `0x0007E575` runs have no pad events at all. The claim
that survives is the general one: *an unresolved indirect call can leave the
caller spinning on a NULL target forever.* `0x00114B66` is the instance the
pad schedule happens to reach.

It is the button and not the clock. Thirteen runs fired pad event #18 at
t=60.00 and froze at t=60.00; `guardon-1` fired event #6 at t=28.00 and froze
at t=28.00.

## The gameplay split, and what it does NOT mean

The handover puts this first because *"it caps every scripted measurement at
about a minute of live frames"*. That is not what the corpus says. Scoring
every run by the gameplay gate CLAUDE.md already defines — `[APU-VOICE] on=`
in the 148-453 band is gameplay, 4-12 is the attract loop:

| | runaway storm | no storm |
|---|---|---|
| reached gameplay (`on=` >= 148) | **0** | 217 |
| attract / story (`on=` 1-147) | 15 | 519 |
| no voice data | 2 | 29 |

**Not one of the 217 runs that reached real gameplay has ever frozen.** Every
storm with voice data happened in the attract or story state.

`base1`, the run dissected above, reads `on=12` throughout and sits in
`WaitEndStoryOrVsMission` for the whole run — four `[JSRF-SEQ]` reports, dwell
climbing 420 -> 2976 ticks, never leaving. By the handover's own gating rule in
its section 8 that run is VOID as a gameplay measurement. Handover section 8
even says so of this schedule: *"new_game.pad stops at t=73 and parks in
WaitEndStoryOrVsMission with on=12"*.

**I first read this as "the bug does not reach gameplay, so it is less
urgent". That reading is wrong and is retracted.** The root-cause section below
shows why: the same defect produces a *spin* when it lands in a polling loop
(the attract/story state, these 15 runs) and a *corrupted stack unwind* when it
lands in a tree walk (real play — the player's four skating crashes). Zero
gameplay runs storming is not immunity; it is a different presentation of one
defect. What the table above actually establishes is narrower: the RUNAWAY SPIN
form is confined to the parked state, so the freeze is not what limits gameplay
measurement. The defect certainly is.

Two further things this kills:
- **It is not "START during a mission".** An earlier draft of this note said
  so; `base1` was never in a mission.
- **It is not the clock.** Twelve of fourteen freeze on the same pad event
  (fire #18, t=60.00), but the delay since reaching state 30 varies from 30.0
  to 36.0 s, `guardon-1` froze on fire #6 at t=28.00, and `regen5` on fire #45
  at t=135.00. The determinant is which press, in which parked state — not
  elapsed time and not the count of presses.

## The address has been known and unfixed the whole time

`0x00114B66` is already in the player's feedback database
`~/Library/Application Support/JSRF/icall_targets.dump` as

```
00114B66 2
```

where flag 2 is `SEEN_UNRESOLVED` — observed, never once resolved.
`sub_00114B66` is not in the shared gen. Its nearest translated neighbours are
`sub_00114A80` and `sub_00114CB8`, so there is a gap in translated code across
it.

So the run-merge-seed-regenerate feedback loop that
`diagnostics/jsrf_first_fault/regenerate.sh` documents at length either has not
been run since the dump learned this address, or its seed filters
(`tools/recomp/icall_feedback.py:225`, an interior-of-known-body drop and a
decode/16-alignment probe — and `0x00114B66` is not 16-aligned) drop it on
every pass, in which case the loop cannot converge on the class of target it
exists to catch. **Which of those two it is matters more than this one
address.** Under investigation.

## The skating crash may be the same root cause

Not established — stated here because it is cheap to test and nobody has.

Every one of the player's four distinct crash logs carries
`[ICALL] Failed to resolve VA 0x000A5B8C` between 6 and 13 log lines before
the guest fault:

| log | VA | gap | fault |
|---|---|---|---|
| `MERGE-PLAYER-CRASH-ON-SKATE` | 0x000A5B8C | 13 lines | `sub_00011D00 +0x244` |
| `0817-WILDPTR-ANSWER` | 0x000A5B8C | 8 lines | `sub_00011D00 +0x244` |
| `082017-CRASH` | 0x000A5B8C | 7 lines | `sub_00011D00 +0x244` |
| `092100-CRASH` | 0x000A5B8C | 6 lines | `sub_00011D00 +0xA20` |

(`0817`/`081755` and `082017`/`0825-CRASH3` are each one log saved under two
names. Four crashes, not six.)

Proximity is not causation, but this is not a periodic line landing nearby by
chance: `Failed to resolve` prints **twice in the whole 19,000-line log**, so
it is effectively a first-occurrence print, and it lands next to the fault four
times out of four.

And the instrument has already recorded the descent that handover section 3
retracted as unestablished. `0817-WILDPTR-ANSWER.log:19154`:

```
[ICALL]   recent targets: 00011C90 00011C90 00011C90 00011C90 040004E0 04000508 04000530 04000558 04000610 8D30247C 0000A000 FFFFFFFF
```

`040004E0`, `04000508`, `04000530`, `04000558` are each **exactly 0x28 apart**,
and `0x28` is the walker's own child offset. Section 3 says the walk "marched
0x28 bytes at a time through a list-head array until it hit the sentinel" and
that it could not establish which real node handed it that array. Here the
march is a measured sequence of live call targets rather than a reconstruction
from a spilled stack slot, and the last valid target before the garbage run is
`00011C90` — a translated function, flagged resolved in the dump.

The hypothesis this supports: a skipped indirect call means whatever it was
supposed to build in the scene graph never gets built, and the walker then
descends into an uninitialised list-head array and reads the allocator sentinel
`0xFFEEFFEE`. Under it, section 3's finding that `sub_00011D00`'s translation
is byte-exact is both true and beside the point — correct code walking a graph
a skipped call left unbuilt.

## What this is NOT

The blackscreen / full-guest-stop session carries an unresolved
`0x001063CF` but only **one** skipped NULL call. It is a different failure and
belongs to G17's self-suspend park. Do not fold it in.

Also note that `BLACKSCREEN-FULL-GUEST-STOP`, `hang-THREADTRACE` and
`0420-pre-merge-player` are one session saved three times — identical line
number, caller and call total. One data point, not three.


## TWO DIFFERENT FAILURES SHARE THE `[ADX] tick STUCK` LINE

Found by joining this analysis with a parallel one that was classifying runs by
their own `switches.txt`. The watchdog line everyone has been reading as one
bug is two, and they have different causes and different fixes.

Restricted to 16-19 Sep, the window in which `RECOMP_NV2A_PMC_UPMIRROR` exists
at all — before 16 Sep no run has it, so the whole-corpus comparison is
confounded with date and binary and must not be used:

| | clean | `ADX tick STUCK`, no storm | NULL-call storm | n |
|---|---|---|---|---|
| `NV2A_PMC_UPMIRROR=1` | 86 | **0** | 1 | 87 |
| not set | 117 | **70** | 13 | 200 |

**The common failure — 70 runs, 35% of the unarmed arm — is eliminated
entirely by `RECOMP_NV2A_PMC_UPMIRROR`, 0 of 87.** It is a vblank-delivery
problem and it has nothing to do with indirect calls. Its own A/B (`4111a6b`,
two scene-matched 180 s runs) measured 10.2 Hz / 8,266 unacked skips / worker
frozen against 58.0 Hz / 2 skips / never froze.

**The rare failure — the NULL-call storm this note is about — is not fixed by
it**: 1 of 87 armed still stormed, against 13 of 200 unarmed, and that
difference is small enough to be the parking effect rather than a real one.
Different cause, different fix.

This is still observational — the arms differ in more than one switch — but
restricting to the common window removes the date and binary confound, and
0 of 87 against 70 of 200 is a large effect.

The practical consequence: **the harness has been running without the one
switch that removes the commonest freeze**, which is item 0.4's argument
("the harness has never run the player's game") arriving as a concrete cost
rather than a principle.

## The known unresolved targets, all of them

From the scripted corpus and the player's logs together:

```
                scripted   player
                runs       logs
0x00114B66        16          -    the runaway freeze; untranslated; dump flag 2
0x0006D7A8         4          -
0x000960C3         3          -
0x0007E575         2          -    the other two runaway storms
0x000A5B8C         2          4    beside all four player crashes
0x001063CF         -          1    the blackscreen session (1 session, 3 filenames)
0x000FDB9F         1          -
0x000F8B54         1          -
0x001D2B28         1          -
0xFFFFFFFF         1          1    garbage, not a target
```

Counted per RUN containing the address, not per occurrence.


## THE ROOT CAUSE: a switch arm the lifter could not place

This unifies handover items 0.1 and 0.3 into one defect. Established by the
agent working item 0.3; the load-bearing parts independently re-checked here.

`0x000A5B85` is `jmp dword ptr [eax*4 + 0x000A60A4]` — a C switch. Its table
holds five arms, the first of which is `0x000A5B8C`. The owning function
`sub_000A5B60` is carved with **`end` set to the dispatching jump itself**, so
every arm lies outside it. `lifter.py:_analyze_switch_table`
(tools/recomp/lifter.py:2745-2786) needs at least two arms inside
`[start, end)`, finds none, and the dispatch falls through to the generic path:
`RECOMP_ITAIL` through the table.

`RECOMP_ITAIL`'s failure path, `templates/runtime/recomp_types.h:1380`:

```c
else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED);
       recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; }
```

`g_esp += 4` is **correct for a genuine tail jump** — the jumping function has
run its epilogue and esp sits on the return address. It is **wrong for a
misidentified switch**, where esp still holds the whole frame. `sub_000A5B60`
has 0x50 of locals and four pushes, so it returns with the guest stack short by
**0x60 bytes and no epilogue run**. ebx/esi/edi are globals in this ABI, so
every caller up the chain then pops them from the wrong slots.

The arithmetic closes against the player's crashes. The innermost
pre-corruption walker frame base is `0050FE5C`, so resumption should be at
`0050FE5C - 0x60 = 0050FDFC`:

| log | ESP | `0050FDFC - ESP` | reads as |
|---|---|---|---|
| `MERGE-PLAYER-CRASH-ON-SKATE` | 0050FD6C | 0x90 | 6 walker frames |
| `0817-WILDPTR-ANSWER` | 0050FDB4 | 0x48 | 3 walker frames |
| `082017-CRASH` | 0050FDB4 | 0x48 | 3 walker frames |
| `092100-CRASH` | 0050FDF8 | 0x04 | the `push 1` before `call [edx]` |

`0x00114B66` — the freeze — is the same shape: `0x00114B5F` is
`jmp dword ptr [eax*4 + 0x00114F90]`, `sub_00114A80` is carved
`end=0x00114D34` (verified directly in `disasm/functions.json`), arm 0 is
inside but arms 1 and 2 are outside, so `inside` has length 1, below the
two-arm threshold. Same root cause, same runtime primitive. It presents
differently only because the generated function returns `eax = 0` into a state
machine whose caller loops until the state advances — returning 0 forever is
the spin.

### The scope, which is the number to lead with

Measured by `diagnostics/jsrf_first_fault/switch_arm_audit.py`, re-run here
against the 19 Sep shared gen and reproducing exactly:

```
dispatch sites (RECOMP_ITAIL through a scaled table): 196
distinct tables: 84   distinct owning functions: 191
arms, tight bound (within 0x2000 of the owner): 711, of which 607 have NO translated body
tables with at least one unreachable arm: 76 of 84
```

484 switch dispatches *are* recognised and lifted to gotos, so this is a
minority case — but **607 unreachable arms across 76 tables** is a systemic
translation gap, and every one of them is a latent guest-stack corruption of a
computable size.

### Consequences

- **Do not seed these addresses.** `0x00114B66` is interior to `sub_00114A80`,
  and `icall_feedback.py`'s interior filter is right to drop it — its docstring
  already explains that seeding an interior address truncates the containing
  function and corrupts every caller. That answers the question posed above:
  the feedback loop cannot converge on these, correctly, because they are not
  function starts. The loop is not broken; it is being asked for something
  outside its remit.
- **The fix is the carve.** Extending
  `diagnostics/jsrf_first_fault/function_bounds.json` so owners include their
  arms is the low-risk route, and `regenerate.sh` already passes
  `--function-bounds`. Changing `_analyze_switch_table`'s rule is the
  higher-risk route and puts the working 484 at stake.
- **The repair is verifiable without running the game**, because the audit
  measures the gen and not a run. 607 falling is the ratchet.

### Two corrections to the earlier sections of this note

- **`sub_00011C90` is one byte, `c3`** (verified: 0x11c90-0x11c91). It is the
  empty default virtual method. The claim above that it is "a named candidate
  for the node that handed the walk a list-head array" does not hold — four
  calls to it mean four real nodes whose vtable slot 2 is the do-nothing
  default.
- **The 0x28 march is stronger than stated.** The garbage targets are
  `node + 8`, because the walker's virtual call at `0x11D67` is `call [eax+8]`
  with `eax = MEM32(edi)` and the region is self-linked. So `040004E0`,
  `04000508`, `04000530`, `04000558` decode to nodes `040004D8`, `04000500`,
  `04000528`, `04000550` — exactly the saved-edi values in the live frames. Two
  independent instruments measure the same march.
- And the "a real node handed the walk a bad child" hypothesis is refuted as a
  *cause* and survives only as a *symptom*: under the stack-deficit reading the
  frame that appeared to show it is simply stale. Reviving it needs a crash
  with this walker signature and **no** preceding unresolved indirect branch.
  Zero of six so far.

## Rules this paid for

- **A watchdog names a symptom, not a cause.** `[ADX] tick STUCK` was read as
  an audio fault for weeks because nothing asked what the guest did in the
  seconds before it.
- **A log that grows without frames is telling you what the guest is doing.**
  1.7M lines of one message was treated as noise to be scrolled past. It was
  the diagnosis, printed 1.7 million times.
- **Classify a counter by magnitude, not by presence.** A handful of skipped
  NULL calls is background; a billion is the bug. The first cross-tab I ran
  used presence and showed 14 counter-examples that all evaporated under the
  right threshold.
- **"It does not reproduce in configuration X" is not "it does not happen in
  configuration X".** The freeze never appears in gameplay because in gameplay
  the same defect crashes instead. A symptom-shaped search finds symptom-shaped
  answers.
- **A failure path that is correct for what it says it is can still be wrong
  for what actually reaches it.** `g_esp += 4` is right for a tail jump and
  catastrophic for a switch arm wearing a tail jump's clothes.
