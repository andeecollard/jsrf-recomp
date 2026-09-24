# JSRF goals — the 2D bin stops alone, 18 September 2026, night (5)

> Superseded, 19 September 2026, by
> `JSRF_GOALS_2026-09-19_ONE_ROOT_CAUSE_UNDER_TWO_ITEMS.md`. Kept for the record; the active list is
> the newest goals file in this folder.

Supersedes `JSRF_GOALS_2026-09-17_NIGHT2_TRACK_A_AND_A_COMPILER_THAT_LIES.md`.
The **target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

**G-numbers are unchanged and carried forward.** G1 is *rewritten*, not
extended — its founding question was answered "no" tonight. New findings take
G17–G19 rather than renumbering anything.

## What changed

The first player session with all five instruments armed produced a **matched
pair**: one healthy run and one broken run, same build, same switches. That
pair refuted the question G1 is named after, killed the IEN hypothesis, caught
a hang live with its stack, and showed the glyph trap missing the defect by 47
pixels.

## Where we actually are

| | measured | source |
|---|---|---|
| music | **2D bin freezes while 3D keeps climbing** | player, 22:30 |
| ...and the guest keeps submitting | `guest_methods` +2000–3300/window across it | player, 22:30 |
| a healthy control exists | same build, music fine for 156 s | player, 22:25 |
| hang | guest thread parked in self-suspend, **stack captured** | `sample`, x2 |
| IEN | **dead** — 3 writes, all before t=0.005 s | player, 22:30 |
| `[IRQ-LATENCY]` | **measures a dead path**; B0 unanswered | player, 22:30 |
| glyph trap | alive, `quiet=1546`, `min_outside=351` vs `budget=304` | player, 22:30 |
| APU submitters | **two** threads, not one | player, 22:30 |
| frame, flip read-back | 32% of frame, **not recoverable** (cost moves) | A/B, n=3+3 |
| frame, colour resolve | −1.36 ms in `sync`, not moved; frame verdict void | A/B, n=3+4 |
| method dispatch | ~0.4 ms of a 32.8 ms frame | `PB_STAGE_WALK` |
| live codegen defect | **6 branches ask the wrong question** | 9 gen chunks |
| tests | 67/67, audit 0 problems | `build-feav` |

---

## The order

0. ~~**Diff the matched pair.**~~ **DONE** — see the progress note. It killed
   four explanations, three of them written the same evening, and bottomed out
   in a missing instrument that turned out not to be missing.
1. **ONE MORE SESSION. Nothing needs building.** Five switches armed in
   paths.conf on the unchanged bundle: `VOICE_RATES`, `VOICE_FRESH`,
   `THREAD_TRACE`, and the glyph trap at `STILL_PPM=10000` with dumps. The
   next session answers G1, G17 and G2 together, and stays comparable to the
   22:25/22:30 pair because the engine is untouched.
3. **G1** — why the 2D bin alone. Blocked on a real 2D instrument (see G5).
4. **G18** — the cursor pin, unaffected by tonight and still the strongest audio arm.
5. **G3** — colour resolve needs 4 more trials; `recomp_gpu_own` on Metal.
6. **G19** — the six-branch clobber, in the batched regeneration.

---

## G1 — Why the **2D bin** stops contributing *(open, REFOUNDED 18 Sep night 5)*

### THE QUESTION THIS GOAL WAS NAMED AFTER IS ANSWERED, AND THE ANSWER IS NO

The old title was *"Why the guest stops issuing APU methods"*. In the broken
session it does not stop:

    2D heard = 70864 x 32 windows       frozen from window 7 (~t=35 s)
    3D heard = 225561 ... 414327        climbing throughout
    guest_methods = +2349 +3321 +2101   guest submitting normally
    on = 138, 146, 153                  voices still being started
    lost by bin: none                   nothing dropped in the mixdown

The music stops; the guest, the effects and the mixer carry on. **Do not write
another hypothesis that assumes the guest going quiet is the defect.** The
17:55 session, where `guest_methods` did freeze dead, is either a different
failure or a later consequence — and the two have been conflated for days.

### THE NEW QUESTION

Why does the **2D bin alone** stop contributing while 3D does not?

Three shapes, and the instruments cannot yet separate them:
- the guest stops *starting* 2D voices;
- 2D voices are started but never become ACTIVE;
- 2D voices are ACTIVE and render silence.

**Done when:** we know which of those three it is.

**The instrument for this ALSO already existed and was never armed.**
`RECOMP_VOICE_RATES` / `RECOMP_VOICE_FRESH` (`apu_vp.c:3079`) tracks, per
voice: `off_frames`, `silent_frames` ("fetched samples were all ~zero"),
`energy`, `fresh_slots`/`stale_slots`/`max_stale_run`, `w_refill_edges` as a
cadence in Hz, `starved_loops`, and min/max rate. That separates all three
shapes above. Its own header comment describes the failure we then measured,
before we measured it: *"a voice played at the wrong speed drains its buffer
at the wrong speed — fine in steady state, wrong at the moment a stream is
switched or refilled."*

Armed 18 Sep night 5. Read voices 64–67 only. No rebuild.

`[APU-POOL] on_2d` remains useless for this (G5), but it is no longer the
blocker it was described as an hour ago.

### What is dead, and must not be re-derived

- **The IEN gate.** `writes=3, last_written_at=0.005 s`. The guest writes IEN
  three times in the first five milliseconds and never again, so the write the
  hypothesis needs at the freeze does not exist. `SUPPRESSED BY IEN=0` agrees.
- **The idle-trap latch shutting.** `reraise` climbs (7814 → 8307) with
  `rearm=102`. The 16:2x explanation does not apply to this failure.
- **DSP recognition.** `gp=7 ep=8` in three sessions now.
- **The FECTL HALTED divergence.** Real, 124,443 frames, but no step at the
  freeze.

---

## G17 — The suspend/resume census *(ALREADY BUILT. It was never armed.)*

**CORRECTED before a line of it was written.** This goal said "wire the
census". The census exists: `w32_thread_trace_report()` at
`win32_compat.c:625`, called from `xinput_device.c:614`, compiled into the
shipped bundle, gated on `RECOMP_THREAD_TRACE` — **which has never been in
paths.conf**, so `[THREAD]` appears zero times in every log this project has.

It already carries the exact discriminator, and its own comment states the
test: *"a resume arriving while the count is already zero is a NO-OP, so if
the producer signals before the worker parks, the wakeup is LOST and the
worker sleeps for the life of the process. `lost_resumes` counts exactly that
case. A stall that begins in the same report as a lost resume is the
mechanism; a stall with `lost_resumes` flat is not."*

Armed 18 Sep night 5. No rebuild.


The 22:30 session hung with the process **alive**, and two `sample` runs 30 s
apart agree on the stack:

    sub_00147DAC -> kernel_thunk_dispatch -> bridge_NtSuspendThread
      -> xbox_NtSuspendThread -> SuspendThread (win32_compat.c:761)
        -> pthread_cond_wait -> __psynch_cvwait

A guest thread is parked in the **self-suspend** path waiting for
`suspend_count` to reach zero. `win32_compat.c`'s own comment names the frame
below it: *"JSRF's XAPI worker (`sub_00147DAC`…) did precisely that."* Self
suspension is normal and that worker does it constantly — `tib=0x009A4000`,
14,258 calls, `last_ordinal=231` (`NtSuspendThread`), in **both** sessions.

**The defect is that nothing resumed it**, and we cannot say why because
`g_w32_parked` and `g_w32_suspends` are incremented and **never printed
anywhere**, and nothing counts `NtResumeThread` at all.

**Done when:** a report line prints suspends, resumes, currently-parked, and
which handle was suspended by whom. **Refutation:** `parked` returning to 0
across the music death means the park is not the mechanism. **Positive
control:** `suspends` must climb; it is 14,258 a session, so zero means dead.

---

## G2 — The glyph index error *(open, and the trap missed by 47 pixels)*

**The defect was photographed tonight** — `y` → `§` and both `w` → `W` in a
Gum speech box, rest of the string clean. The substitutions are not
ASCII-adjacent, so any index error is in **font-atlas order, not character
order**, which is a sharper claim than G2 has ever had.

The trap was armed and could not see it:

    comparisons=5182 still=0 moving=1133 quiet=1546
    min_outside=351   budget=304

`quiet=1546` is the positive control — the threshold works. But the smallest
outside-change ever seen *alongside a region change* was 351 pixels against a
304-pixel budget, so `still` can never fire in this title's cutscenes.

**Next:** `RECOMP_FB_WATCH_STILL_PPM=10000` admits the `upto10x` bucket — 148
events at ~24 KB per rectangle dump, about 3.5 MB. **This is a `paths.conf`
edit and a relaunch, not a build.** Replaying the Gum cutscene should capture
the corrupt glyph as an image.

---

## G18 — The cursor pin *(promoted; unaffected by tonight)*

`sub_001A2E2E` — the crash function, 40 of 66 guest faults — reads `CVL`/`NVL`
at every removal and branches on them. Handles are 0…255, so pinned at
`0xFFFF` the compare can never match and the guest takes a two-instruction
skip; the unlink happens earlier and unconditionally. Our model never reads
those registers. Microsoft's touches them **five times in the entire module,
all in reset**, and drops guest writes.

83 of 87 removals took the repair branch against us; Microsoft's titles take it
on zero. **Proof it took:** `[VOICE-TOP-RING] on-trapped-voice` 83 → 0, needing
no new code. **Both halves required:** stop republishing *and* drop guest
writes, or the pin silently un-pins itself.

---

## G19 — Six branches ask the wrong question *(new, live in the build)*

The result-setter family (`and`/`or`/`xor`/`add`/`sub`/`neg`/shifts) rebuilds
conditions at the consumer by re-reading a destination `mov` may have
clobbered. Six live sites in six real functions; `sub_000A6510` emits an
**unconditional branch** where the guest tested `ebp+ecx == 0`. Same bug class
that hung JSRF's ADX loop when `inc`/`dec` did it.

Fix is the snapshot `cmp`/`test` already take, so it needs a regeneration.
**Not established:** that any of the six execute.

---

## G3 — The frame tail *(open; two levers measured, one refuted)*

- **The flip read-back is 32% of the frame and not recoverable.** `sync` → 0.01
  and `submit` absorbs it one for one. The wait is the GPU finishing the frame.
  Direct Metal presentation still removes the copy, the conversion and the
  re-upload — **size it from those, never from `[STAGE] sync`**.
- **The colour resolve removes cost rather than moving it.** `sync` −1.36 ms
  non-overlapping, `submit` unchanged, frame −1.30 ms. Frame verdict void at
  n=3 vs 4; four more trials owed.
- **The descriptor table is not a frame-time lever**: ~0.4 ms of 32.8 ms.

## Carried forward unchanged

G4 (`SYNC_HIST` halt), **G5 (`on_2d` — now load-bearing for G1, not cosmetic)**,
G6–G9, G10–G13, G14–G16 (upstream debt; G15 and G14 sent as PR #70 and #71,
`popfd` as #72).

## Rules added tonight

- **A counter that is never printed is not an instrument.**
- **Sample a hang before killing it.** Every previous hang here was killed first.
- **A dead instrument reads like data.** `[IRQ-LATENCY] 0 delivered` is wiring.
- **The control session is worth as much as the broken one.**
- **A stage's size is not a lever's size.**
- **A frame-time A/B owns the machine** — this session polluted one of its own.
