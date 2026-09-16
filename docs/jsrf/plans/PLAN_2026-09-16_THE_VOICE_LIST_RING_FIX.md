# The voice-list ring: fix the insert, not the walk

Answers open item 3 of
`handovers/HANDOVER_2026-09-16_THE_FENCES_THE_RINGS_AND_A_BLACK_SCREEN_I_CAUSED.txt`.
Derived from counters in existing logs plus static reading; **nothing here has
been built or run.** Every number below is from a log that already existed.

## The headline, and two corrections to what we believed

**`RECOMP_APU_CYCLE_BREAK` is containment, not repair, and must not ship
alone.** The TOP insert at `src/apu/apu_vp.c:1335-1337` writes
`link(v) = regs[top]` over whatever `link(v)` held, and that field is the only
pointer to v's successor -- the voice register file lives in guest RAM. By the
time the walk sees the ring, the tail is already unreachable. Breaking the walk
stops the CPU burn and the re-render; it cannot bring those voices back.

**Correction 1 -- the damage is far worse than "2x".** `voice_process` is
called once per *visit* with no dedup (`apu_vp.c:3160`), bounded only by the
256-iteration cap at `:2929`, whose `break` logs through a `DPRINTF` that is
compiled out. Dividing `processed` by `se` between consecutive report lines:

| run       | clean windows | ringing windows        | cycles/subframe |
|-----------|---------------|------------------------|-----------------|
| apuclock2 | 4.00-5.24     | 12.2 -> **77.1**       | 0 -> 1.000      |
| idxfix3   | 5.00          | 10.9 -> **71.4**       | 0 -> 1.000      |
| reverted  | 4.73          | 13.2 -> **194.6**      | 0 -> 2.000      |

`reverted` also shows three consecutive 10 s windows at **0.00** voice_process
calls -- the silent mode, where the ring's reachable members are all inactive
and the list renders nothing. One run contains both failure modes minutes
apart. That is "music and sound effects glitch", not drift.

It is audible rather than merely expensive because each visit advances `cbo` by
another 32 samples, steps the amplitude and filter envelopes, and **adds** its
different 32 samples into the bin, which the EP then hard-clamps
(`apu_dsp.c:151-157`) -- so the output saturates into clipped square-wave
garbage. Voice 0's 14,016-sample ADPCM loop at N=70 completes in ~4 ms instead
of 292 ms: a ~240 Hz buzz where music should be.

**Correction 2 -- 83% of the rings are the head case, not the deep case.** The
handover reads `relink=209` as rings of length >= 2. The contradicting number is
on the line above it: `self_link=173`, which counts `link_after == v` and in the
TOP branch happens iff `regs[top] == v`. So 173 of 209 are length-1 (what
`REON_HEAD_NOP` already covers) and only 36 are length >= 2. `reverted`: 356 of
370. This is why both existing guards individually measured as nothing -- each
addresses one part and misses the other.

## What the driver actually does

- **FEAV is always `0x0001FFFF` or `0x0002FFFF`** in all 535 `[VOICE-LINK]`
  lines of `play/20260915-122828-human-gameplay/stderr.log` -- `ante=FFFF`. So
  the TOP branch is the branch the guest asked for; we are not discarding a real
  antecedent. `on_inherit=0` in every run. That hypothesis is dead.
- **The guest is an active, correct list maintainer.** It services
  `SE2FE_IDLE_VOICE` and writes the head itself: 145 3D head writes and 37 2D in
  one 280 s run.
- **The ring forms when the guest retriggers inside one 256-sample window**,
  before the idle trap can be raised -- `off-command`, `retire`, `on`, all at
  the same stamp with no `idle` line between. An ordinary DirectSound retrigger
  racing our trap latency, not a stale head.
- **The guest writes `link(v) = v` itself, at runtime**, on voices our TVL still
  names. Five direct captures, plus the REON A/B: with the head guard on, our
  code never writes that, yet `head_nop=104` against `self_link=99` -- 99 of 104
  found the sentinel already set by the guest.

## The fix: three changes, A/B'd together

1. **`RECOMP_APU_LIST_MOVE_TO_FRONT` -- make the TOP insert a move-to-front.**
   Before splicing, walk from `regs[top]`; if v is found, unlink it first, then
   prepend. `voice_list_contains` (`apu_vp.c:916-928`) already does the bounded
   walk; it needs to return the predecessor instead of a bool. This is
   idempotent for the head case (so it **subsumes `REON_HEAD_NOP` exactly**) and
   lossless for the deep case (v moves, its successor stays attached to its old
   predecessor). The same change belongs in the INHERIT branch, which has the
   identical defect -- free correctness, since `on_inherit=0` here.
2. **`RECOMP_APU_SELFLINK_END` -- honour `link(v) == v` as end-of-list.** Not
   redundant with #1: the guest writes that sentinel asynchronously, to a voice
   our TVL still names, and no insert-side change can prevent it. Terminating
   loses nothing -- if `link(v)` reads `v`, whatever was behind v is already
   unreachable. The existing A/B tested this **alone**, against a storm it
   cannot stop by itself; re-measure it with #1.
3. **Keep the revisit detector; demote `CYCLE_BREAK` to an assertion**, and give
   the 256-cap break at `apu_vp.c:2929-2936` a real `fprintf` -- it currently
   reports through a compiled-out `DPRINTF`, which is why this went unseen for
   the life of the file.

## The measurement, and its positive control

> **`walks_with_a_cycle` must go to 0 while `relink` stays non-zero.**

One line already prints both. The two counters sit on opposite sides of the
fix: `relink` is incremented *before* the insert and measures the guest's
behaviour, which must not change; `walks_with_a_cycle` measures our list state
and must go to zero. **If `relink` collapses to 0 too, the run never reached
churn and the zero means nothing.**

Positive control: **`on_top`**, incremented one line before `relink` on the same
event and unaffected by all three changes. A report reading
`relink=0 (of 4 top inserts)` is the parked-player signature and must be
discarded, not counted as a clean arm.

**This is exactly what happened to the only run ever taken with
`CYCLE_BREAK` on.** `render-investigation/cyclebrk1/stderr.log` reads
`relink=0 (of 4 top inserts) walks_with_a_cycle=0 broken=0` with `on=4` and no
churn. `broken=0` there means "no ring occurred", not "the break does nothing".
**`RECOMP_APU_CYCLE_BREAK` has never actually been measured.**

Second control, and the one that tracks what the player hears:
**`processed / se` must return to the clean band of 4-5 per subframe** -- a
ratio with a known healthy value from the same runs, so it cannot be confounded
by scene length.

Five clean runs per arm minimum. That rule was paid for by the lock guard.

## Second mechanisms, ranked, for after the ring

- **The VOICE_ON / frame-thread data race.** `voice_lock()` takes `d->lock` and
  releases it immediately, while the frame thread holds it across the whole of
  `se_frame`. So the entire VOICE_ON body runs concurrently with
  `mcpx_apu_vp_frame`, doing non-atomic read-modify-write on shared guest-RAM
  dwords. `PAR_OFFSET` holds CBO *and* EALVL in one dword: a lost `cbo = 0`
  restarts a voice mid-buffer. `PAR_STATE` holds NEW_VOICE/ACTIVE_VOICE/EACUR/
  EFCUR: a lost `ACTIVE=1` leaves a voice the guest just started silent and
  idle-trapping. Uninstrumented; a per-voice generation counter sampled at the
  top and bottom of `voice_get_samples` would catch a torn render.
- **ADPCM decode failure emits stack garbage.** `adpcm_decode_block` returns 0
  without writing anything on a bad header; the caller (`apu_vp.c:2242`) ignores
  the return and `adpcm_decoded[]` is an uninitialised local, and the block
  index is cached anyway. JSRF does use ADPCM (9 of 57 descriptor dumps).
- **HALTED subframes punch 0.67 ms silence holes** -- but `light` co-onsets
  exactly with `walks_with_a_cycle`, so this is probably a consequence of the
  ring. Re-check after, before spending an A/B on it.
- **`if (cbo >= ebo)` wraps one sample early** (`:2313` against the `cbo <= ebo`
  loop at `:2220`). Real, tiny, inherited from xemu.
- **The stream/SSL descriptor path is broken and completely dormant here** --
  all 57 `[VOICE-DESC]` dumps say `buffer`, none say `STREAM`. So the handover's
  "a streaming voice drains at 2x and starves" does not describe JSRF at all;
  the damage lands on buffered looping voices. Fix for correctness, expect no
  audible change, and keep it out of the ring change.
