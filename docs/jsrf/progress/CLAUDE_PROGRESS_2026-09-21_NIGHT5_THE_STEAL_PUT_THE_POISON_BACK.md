# 21 September 2026, night — the steal put the poison back

`progress/CLAUDE_PROGRESS_2026-09-21_THE_LOCK_DID_NOT_SPAN_THE_CRITICAL_SECTION.md`
closed by naming what its runs had not tested:

> `contended=0` in both runs [...] The intro scene never has two threads in the
> ADX region at once, so the *title* runs have not exercised the interleave —
> only the unit test has. A cutscene is where the freeze was measured and is
> where the guard needs a player session.

**That session happened.** The player reached Beat's race challenge and the
title stopped responding. The log is
`~/Library/Application Support/JSRF/last-run-2026-09-21-SESSION10-BEAT-RACE-HUNG.log`
(57 MB, 1167.8 s, 49,889 flips), input recorded to
`padrec/graffiti-2026-09-21_1914.padrec`. It exercised the interleave —
`contended=121` — and the guard did not hold.

Supersedes section 1 of the progress note above, which stands as an account of
the *first* defect. It does not stand as an account of the fix being complete.

---

## 1. WHAT THE SESSION MEASURED

It did not hang. It fell to 0.4 fps at **t ≈ 996 s** and stayed there for the
remaining 171 s. Flips per five-second report window went `264 → 2` in one
window, with no ramp.

Not the renderer, and the log says so three ways:

| | |
|---|---|
| Metal waited ~25 ms per 5 s window | `sync cost by caller: external` differenced across the cliff |
| The pushbuffer never stopped | `runs` 537,091→537,700, fence `counter=1253989 word=1253987`, `[PB-ACK] ... (fence healthy)` |
| The time is outside the renderer | `[STAGE] ... rest=2496.25 ms of 2514.12` |

Frames took **exactly 5002–5005 ms**. A round 5000 is a timeout, and it is
`adx_guard`'s: `RECOMP_ADX_LOCK_TIMEOUT_MS`, default 5000.

From the cliff, every report window, never recovering:

    [ADX] lock_count=0 saved_priority=15 raised=15 idle=-15 mwidle_flag=0
    [ADX] saved_priority is 15 -- the unlock restores base 16, so the I/O
          guard cannot exit
    [ADX-GUARD] locks=206583 unlocks=206582 matched (+1 UNMATCHED) ... held=1
    [ADX-GUARD] 33 STEALS after 5000 ms, 0 releases by a thread already stolen

`saved_priority` held a healthy `1` for the whole run and flipped to `15` at
the cliff. `stolen_from=0` for 171 s: the holder never came back. Exactly
**one** unmatched unlock, and it appeared in the same window as the first
steal and the poisoning.

This fault is unique to this session. The nine earlier player sessions of
21 Sep and `last-run-previous.log` all read zero steals and zero poisoning.

## 2. THE STEAL IS WHAT BREAKS THE GUARANTEE

`adx_guard.h` enumerated the poisoning interleave and claimed the guard made
it unwritable because "B's lock blocks until A's unlock". That holds for B's
*lock*. It did not hold for B's *unlock*: `adx_guard_unlock_enter()` decided
`matched` from `t_depth > 0`, so an unmatched unlock — `sub_001437B0`'s I/O
guard calling the registered unlock on a spin pass — called `guard_acquire()`,
waited out the timeout, **stole**, and ran underneath the holder:

    A locks             count 0->1, A raised to 16, A's real priority (1)
                        saved. A holds the guard across its own file I/O.
    B unlocks UNMATCHED waits 5000 ms on A, STEALS, runs: count 1->0,
                        restoring 1 onto B.
    A locks (nested)    the count is 0 again so the `jne` no longer skips,
                        and A saves its own priority -- but A is still at
                        16, so GetThreadPriority reads 15.  POISONED.

Every subsequent frame is another waiter paying the full 5000 ms on a holder
that can no longer exit. That is the 0.4 fps.

**The lock path had the same hole**, and `adx_guard_test`'s own
`run_interleave` finds it the moment `RECOMP_ADX_LOCK_TIMEOUT_MS` is short
enough for the test's patience window. The 5000 ms default was hiding it, not
preventing it. Both paths were being reported green by a test that could only
pass because the timeout outran it.

## 3. THE FIX

- **Unmatched unlock never steals.** `adx_guard_unlock_enter()` returns `-1`
  when another thread is inside; `sub_0013B0E0` drops the body without
  touching either shared word. On hardware that spin pass could not have run
  at all while the holder was elevated, so skipping it is the faithful answer,
  and it never waits, so it cannot deadlock. Counted as `SKIPPED`.
- **Lock path waits instead of stealing**, printing every timeout period who
  it is waiting for. `RECOMP_ADX_LOCK_STEAL=1` restores the old behaviour for
  an A/B.

That trades a livelock for a possible deadlock. The evidence says it is the
right trade: **85 contended acquisitions against 206,583 locks in the healthy
996 s, and not one reached even a second.** Every steal in the run happened
after the state was already poisoned. A steal has never been observed to
rescue a run; it has now been observed to wreck one.

`adx_guard_test` gains `run_unmatched_contended`, which drives the session-10
sequence. Its unguarded arm is a positive control — the same sequence must
still reach `saved=15`, or the test has stopped modelling the defect. Verified
to have teeth by reverting the fix in a scratch copy, which fails with the
session-10 signature (`THE POISON FORMED UNDER THE GUARD: saved=15`,
`an unmatched unlock stole the guard`, `A was not restored, base=16`), and by
`RECOMP_ADX_LOCK_STEAL=1`, which reproduces the same failure on the fixed
tree. C suite 110/112, the two failures the known host ones
(`jsrf_input_hotplug` with a controller attached, `jsrf_switch_audit`'s
pre-existing six-over ratchet — audit re-run with the change stashed and it
reads identically, `211 switches (63 via the helper, 150 hand-rolled)`).

No regeneration needed: `jsrf_manual_overrides.c` changed only the body of an
already-excluded function.

### What this has NOT tested

> **SUPERSEDED THE SAME NIGHT — READ SECTION 6.** The fix got two player
> sessions and froze both of them hard at ~620 s. It did prevent the
> poisoning; it also cost the player more playtime than it saved. The trade
> argued for above was made on incomplete evidence.

The same gap as before, one level up: the fix had not had a player session
when this was written. The unit test drives the interleave; the title had not,
on the fixed tree.

## 4. TWO CORRECTIONS TO `STATUS.md`

Both found while checking a performance claim against this log, not by
reading.

**NV2A vertex programs are NOT interpreted on the CPU.** STATUS.md's "Not
working" section says they are, at "~7.5 ms of a ~32 ms frame", and that the
MSL emitter "is not wired into the renderer and must not be until its outputs
are compared against the interpreter". On this tree `vsh_gpu_on()`
(`nv2a_metal.m:971`) **defaults to 1**, the player's `paths.conf` does not set
it, and the run reads `[METAL] vsh: guest programs on the GPU (metal_vsh on)`
with `vsh draws: 7646170 GPU, 161143 CPU` — 98% of draws. The CPU interpreter
is now the fallback and the control arm, which is what the code comment at
that line already says.

What *is* still on the CPU is vertex **marshalling**, not vertex
**interpretation**: on the GPU path `prepare_vertices()` still fills a
256-byte `inputs` array per vertex and copies it into `s_outputs[]`
(`nv2a_pb_exec.c:4355`). That is the cost, and it is a different fix from the
one STATUS.md describes.

**There is no `clear_surface` sync caller.** STATUS.md attributes 443,738 ms
of drain and 125,914 ms of readback to `clear_surface`. The current instrument
has four callers — `SYNC_WHO_EXTERNAL`, `SWAP`, `INVALIDATE`, `FRAME_END`
(`nv2a_metal.m:2603`) — and none is named that. The figure predates the
current naming.

## 5. WHERE THE DRAIN ACTUALLY IS

Measured in this run, and it re-aims the standing performance item:

| caller | calls | wait | readback | per flip |
|---|---|---|---|---|
| **external** | 50,121 | **346.4 s** | 30.2 s | **6.94 ms** |
| swap | 100,075 | 33.6 s | 29.3 s | 0.67 ms |
| invalidate | 441 | 0.0 s | 0.4 s | ~0 |

**`external` is 91% of all drain waiting**, at 50,121 calls against 49,889
flips — one per flip, the flip readback, not the diagnostics (`[FB-WATCH]` 232
lines, `[FLIP-SNAP]` 232, over the whole run). Surface swaps drain twice as
often but ten times more cheaply, because by the time a swap happens the GPU
has usually caught up; the flip readback drains right after the frame's work
was submitted, so it eats the whole outstanding frame.

So "find which surface swaps force GPU drains" is aimed at the 9% slice. The
91% is the flip readback: drain, copy the colour surface to guest RAM, upload
it back to present — a round trip for data already on the GPU.
`no_flip_sync=1` is not the fix and the code says why; presenting from the
resident surface, and reading back only when the guest actually reads, is the
shape of one.

Separately and additively, the per-draw cost is **~37 µs** (`vsh` + `submit`,
dead linear in draw count: 60 draws → 12.75 ms/frame, 507 draws → 34.92 ms).
That is what the player felt as "slow with the traffic on screen". Encoder-
per-draw was checked and ruled out — batching is on and coalescing ~52 draws
per encoder.

**The 27–30 fps figure in STATUS.md is not corrected here.** This session read
43–53 fps in ordinary play and 28–34 with heavy traffic, but those are not the
scene STATUS.md measured, and scene-matching every comparison is the rule this
tree learned the hard way. It needs a `measure.sh` run on the same scene
before anyone moves the number.

---

## 6. THE FIX GOT ITS PLAYER SESSIONS THE SAME NIGHT, AND IT REGRESSED THEM

Two sessions on the fixed build, both wedged. **Read this before section 3.**

|  | session 11 | session 12 |
|---|---|---|
| froze at | ~620 s | ~623 s, on a cutscene |
| `saved_priority` | 1 | 1 |
| poison / steals / UNMATCHED | 0 / 0 / 0 | 0 / 0 / 0 |
| `held` | 1 | 1 |
| `STALLED` reports | 6 | 6 |
| flips | **0** | **0** |

Logs `last-run-2026-09-21-SESSION11-FROZE.log` and `...SESSION12-CUTSCENE-HANG.log`;
samples `hang-2026-09-21_session11-adx-holder.sample.gz` and
`...session12-cutscene-adx-holder.sample.gz`.

**What the fix achieved:** the poisoning never formed. `saved_priority` stayed
1 through 169 contended acquisitions, no steal, no unmatched unlock. Section 2
is not retracted.

**What it cost:** a hard freeze instead of a 0.4 fps crawl, twice, at ~620 s
instead of session 10's ~996 s. The player got less playtime than before the
fix. The trade in section 3 was made on the evidence and the evidence was
incomplete: contention never reaching a second in one healthy run did not mean
a holder never stops.

**What the samples say**, and this is the new fact:

- main thread **562 of 562 samples** in `adx_guard_lock_enter` — a dead stop
- the ADX spinner `sub_0013B180` **562 of 562 samples** burning a core
- **no thread anywhere in the process is inside the guest critical section**

The spinner is the tell. The guest's lock *resumes* it and the unlock
*suspends* it, so a spinner at 100% means the lock was taken and the matching
unlock never ran — with the slot unpoisoned, so by a different mechanism than
session 10.

**Hypothesis, NOT tested:** the guard keys ownership on the HOST thread
(`__thread t_id`), but this runtime runs guest code on more than one host
thread — `bridge_run_dpc` under `bridge_deliver_isr_ex` is doing exactly that
in the session 12 sample. A guest thread that takes the lock on one host
thread and returns on another would leave the guard held by a thread that is
never coming back, which is what both samples look like. If that is right, a
host-thread-keyed recursive mutex is the wrong primitive and no amount of
timeout tuning fixes it.

**The experiment that splits it**, one env var, one session:
`RECOMP_ADX_LOCK_STEAL=1`.

- recovers and keeps playing → the holder is merely slow, chase what it waits on
- `saved_priority` reaches 15 again → the holder never returns, and the
  ownership model above is the bug

### Also measured, unrelated to the lock

The glitching audio the player reported is the same fault one severity down.
Output is clean — `out_hz=48000`, `starved=0`, `[APU-ADPCM] fail=0`,
`reprimes=0` — but the final per-voice block (indexed by voice, not `tail`)
shows high-traffic voices unrefilled by the guest: voice 0 at 99.4% stale with
a 525 ms gap, voice 13 at 99.7% / 929 ms, voice 42 at 100% / 1088 ms, against
voice 68 — the busiest — clean at 0.2%. Sub-second refill gaps are audible.
Same contention, not yet fatal.

Separately, `getenv` appears uncached on the PB-ACK thread's hot loop (which
runs ~1M iterations/s) in both samples, contending the libc environ lock:
`getenv("RECOMP_OHCI_TRANSFER_TRACE")` at `xbox_usb_ohci.c:817`. Worth fixing
on its own merits.
