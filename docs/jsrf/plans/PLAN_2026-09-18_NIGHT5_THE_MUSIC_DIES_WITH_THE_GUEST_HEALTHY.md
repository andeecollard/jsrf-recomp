# The music dies with the guest healthy — 18 September 2026, night (5)

Supersedes `PLAN_2026-09-18_NIGHT4_THE_LEVERS_THAT_SURVIVED_MEASUREMENT.md`,
which is four hours old and predates the first player session with the five
instruments armed. That plan's graphics and recompiler findings stand. Its
audio section is obsolete: **G1's founding question is the wrong question.**

## What the 22:25 and 22:30 sessions established

Two player sessions, same build, same five instruments, **one healthy and one
broken**. That pair is the best audio evidence this project has had.

### 1. G1 IS MIS-FRAMED. The guest does not stop.

G1 is titled *"Why the guest stops issuing APU methods"*. In the broken
session it does not stop:

    2D heard = 70864 x 32 windows          <- MUSIC FROZEN at window 7 (~t=35 s)
    3D heard = 225561 ... 414327           <- effects climbing throughout
    guest_methods = +2349 +3321 +2101 ...  <- guest submitting normally
    on = 138, 146, 153                     <- voices still being started
    lost by bin: none                      <- nothing dropped in the mixdown

The music stops; the guest, the effects and the mixer carry on. The 17:55
session — where `guest_methods` *did* freeze dead — is therefore either a
**different failure** or a later consequence, and the two have been conflated
for days. Seven hypotheses died assuming the guest going quiet was the defect.

**The new question is: why does the 2D bin alone stop contributing, while 3D
does not?**

### 2. The IEN hypothesis is dead, and not on the counter it was meant to die on

    [APU-IEN] value=00000AFB writes=3 last_written_at=0.005 s
              | raises allowed=137 SUPPRESSED BY IEN=0

`SUPPRESSED BY IEN=0` was the stated kill condition. The stronger fact is
`writes=3, last_written_at=0.005 s`: **the guest writes IEN three times in the
first five milliseconds and never touches it again.** The hypothesis required
a write that silences us at the freeze. That write does not exist, so this
refutation does not depend on the session reaching the freeze point.

### 3. `[IRQ-LATENCY]` measures a path that does not exist

    [IRQ-LATENCY] 0 delivered, 31 lines dropped undelivered, mean 0 us, max 0 us

Not "the interrupt is never delivered". `pci_irq_assert` is
`{ (void)d; recomp_irq_latency_raise(); }` — a no-op whose only effect is this
hook. The APU never delivers through `bridge_run_isr`, so raise and deliver
can never pair and `g_lost` counts a line nothing listens to.

**B0 is still unanswered and the poll byte (B1.1) is still ungated.** The
instrument must time the path the guest actually takes, not the PCI line.

### 4. A hang caught live, with the stack

The session hung — process alive, not crashed — and two `sample` runs 30 s
apart agree:

    bridge_thread_main -> sub_00147EBB -> sub_0013B2A0 -> sub_00147DAC
      -> kernel_thunk_dispatch -> bridge_NtSuspendThread
        -> xbox_NtSuspendThread -> SuspendThread (win32_compat.c:761)
          -> pthread_cond_wait -> __psynch_cvwait

A guest thread is parked in the **self-suspend** path, waiting for
`suspend_count` to reach zero. `win32_compat.c`'s own comment names the frame
below it: *"JSRF's XAPI worker (`sub_00147DAC`…) did precisely that."*
Self-suspension is normal here and that worker does it constantly
(`tib=0x009A4000`, 14,258 calls, `last_ordinal=231` = `NtSuspendThread`, in
**both** sessions). **The defect is that nothing resumed it.**

Why is unknown, because `g_w32_parked` and `g_w32_suspends` are incremented
and **never printed anywhere**, and nothing counts `NtResumeThread` at all.

### 5. The glyph trap missed by 47 pixels

    comparisons=5182 still=0 moving=1133 quiet=1546
    min_outside=351   budget=304

`quiet=1546` is the positive control: the threshold works. But the smallest
outside-change ever seen *alongside a region change* was 351 pixels against a
budget of 304, so `still` can never fire in this title's cutscenes. The defect
was on screen — photographed — and the trap could not see it.

### 6. Two threads submit APU methods, not one

    tib=0x00001000  vp=11295   kernel calls=501,375
    tib=0x00982000  vp=11525   kernel calls=332,774

Both are also the busiest kernel-callers, so they have constant opportunities
for wait-call-driven delivery — which **weakens** the "spinning uninterruptibly"
shape for them. `0x00982000` is the thread that dropped 35% at t=270 in the
17:55 session.

---

## THE ORDER

### 0. Read the matched pair. Free, no runs, available now.

Two complete logs, same build, same instruments, one healthy and one broken:

    last-run-2026-09-18_2225-player-FIVE-INSTRUMENTS.log        healthy
    last-run-2026-09-18_2230-player-MUSIC-DIED-THEN-CRASH.log   broken

Diff every counter across them and find what differs at the window the 2D bin
stops. This is the cheapest thing on the list and nothing else should start
first. **Done when:** we know which counters separate the two sessions.

### 1. Wire the suspend/resume census *(tiny `src/`, no regeneration)*

Print `g_w32_parked` and `g_w32_suspends`, add a resume counter, and record
**which handle was suspended and by whom**. Tonight's evidence is a one-off
stack sample; this turns it into a standing measurement that any session
produces.

**Refutation:** `parked` returning to 0 across the music death means the park
is not the mechanism. **Positive control:** `suspends` must climb — it is
14,258 calls a session, so a zero means the counter is dead.

### 2. Raise the glyph threshold and spend the BMPs *(one line, NO rebuild)*

`RECOMP_FB_WATCH_STILL_PPM=10000` admits the whole `upto10x` bucket — 148
candidate events at ~24 KB per rectangle, about 3.5 MB. Re-arm
`RECOMP_FB_WATCH_STILL_DUMP`. Replaying the Gum cutscene should capture the
corrupt glyph as an image, which G2 has never had.

The player's `paths.conf` reads this at launch, so it costs an edit and a
relaunch, not a build.

### 3. Why the 2D bin alone

The new G1. Per-voice: which voices feed the 2D bin, what state are they in at
the freeze window, and does the guest stop starting them or do they stop being
rendered? `[APU-POOL] on_2d` cannot answer it — G5 already records that it
counts starts and flatlines during healthy playback, silence and death alike.
That counter needs replacing before this question can be asked properly.

### 4. The cursor pin *(unchanged, still strong)*

Proven reachable from the recompiled guest driver, and the branch it removes
is inside `sub_001A2E2E`, the function named by 40 of 66 faults. Unaffected by
tonight's reframing.

### 5. Graphics, unchanged and unblocked

Colour resolve needs 4 more trials (mechanism confirmed, frame verdict void).
`recomp_gpu_own` on Metal is still the missing instrument.

### 6. The batched regeneration

Now carries the six-branch correctness fix as well as the performance items.
Gated on the dead-flag filter experiment, which can be settled on a filtered
copy of the existing gen tree without spending the regeneration.

---

## Rules this session added

- **A counter that is never printed is not an instrument.** `g_w32_parked` has
  been incremented since the file was written and has never appeared in a log.
  Tonight it would have answered the question in one line.
- **Sample a hang before killing it.** The process stayed up and two samples
  gave the exact stack. Every previous hang in this project was killed first
  and diagnosed from counters afterwards.
- **A dead instrument reads like data.** `[IRQ-LATENCY] 0 delivered` looks like
  a dramatic finding and means the wiring is wrong. Read the trigger.
- **The control session is worth as much as the broken one**, and they must be
  captured on the same build with the same switches or the pair is worthless.
