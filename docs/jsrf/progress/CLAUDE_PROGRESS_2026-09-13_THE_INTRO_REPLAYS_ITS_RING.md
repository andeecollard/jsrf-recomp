# The intro replays a fifth of its music

2026-09-13, late. Follows `CLAUDE_HANDOVER_2026-09-13_THE_APU_ADDRESSED_HALF_THE_MACHINE.txt`.

## What the intro glitch actually is

Roughly **23% of the intro's music is the previous lap of the ring buffer,
played again**, in unbroken runs of 100-160 ms, about 2.7 times a second. Every
sample of it is valid decoded music, which is why delta distributions, silence
runs, autocorrelation, spectral alignment and block-repetition search all called
it clean.

Two independent instruments agree quantitatively:

| instrument | what it says |
|---|---|
| xemu differential (`lagtrack`/`winsweep`/`track` in scratchpad) | sample-identical for 2 s (r=1.00, flat lag), then 85 ms of forward progress lost per 200 ms |
| `RECOMP_VOICE_FRESH` (in-APU, this commit) | 22.8% of slots stale per pass, longest stale run 103-163 ms |

## What it is not

- **Not the buffer wrap.** Voice 68 wraps 82 times in 30.5 s, exactly the
  arithmetic for a 16384-sample ring at 44100 Hz. `starved_loops=0`, `short=0`,
  `dry=0`, `silent_frames=0`. The `d449575` counter has been run.
- **Not incoherent voice bounds.** `voice_desc_dump` runs at the top of the
  `VOICE_ON` case, ~50 lines before that case resets CBO. The
  `cbo=14740 ebo=32` lines were the previous life's cursor printed just before
  it was cleared. A clean run prints `cbo=0 ebo=16383 lbo=0`.
- **Not a mapping/SGE bug.** Per-slot histogram: 0 slots never stale, 0 always
  stale. The stale region moves.
- **Not a general speed deficit.** At the Corn tutorial the same voice on the
  same ring goes stale 4349 times early and then never again — frozen across
  three reports — at a *lower* frame rate than the intro.
- **Not the host output path.** SDL2 at 48000 Hz native (no resampling),
  `gen_hz=48003`, `starved=0`, `empty=0`, `min_queued=32 ms`,
  `max_submit_gap=7.5 ms`, `reprimes=0`.

## The open question

The onset is exact. Before the music the intro runs at **85 fps with 4 slow
frames in the whole run**. From the report window in which voice 68 starts,
`p90` goes to **80 ms**, a sixth of frames exceed 33 ms, and **more than half
the wall clock** is spent inside those stalls.

So starting the music and the frame-rate collapse coincide. Which causes which
is **not settled**. The device-lock hand-off counter argues against the obvious
lock-inversion answer — 84 hand-offs in 70 s against ~8 stalls a second — but
`g_apu_lock_handoffs` only fires at a frame boundary, so read what increments it
before trusting the zero (see the house rule about counters that have lied).

## How to reproduce

```sh
RECOMP_VOICE_FRESH=1 REPORT_MS=15000 \
  diagnostics/jsrf_first_fault/measure.sh intro_slots 75 \
  diagnostics/jsrf_first_fault/pad/intro.pad
grep -A5 'voice  68' <log> | grep FRESH
```

`RECOMP_VOICE_FRESH` implies `RECOMP_VOICE_RATES` (the report skips voices with
no counted frames, so arming freshness alone would print nothing). Voices with a
single-slot buffer — 64-67, `ebo=31` — never commit a slot and correctly report
all zeros; voice 68's 512 first-pass slots are the probe's positive control.

## Method note

The handover's A/B compared **two different passages**: ours and xemu's attract
clips correlate at 0.09 over a six-second window. Sweeping the window length
shows 0.98-0.99 from 5 ms to 400 ms, collapsing beyond a second. A single long
correlation window cannot distinguish "different music" from "same music, broken
continuity" — and a coarse ratio grid steps straight over a rate error and
reports "no match", which reads exactly like a finding. Sweep the window;
control every correlator against itself.

## Next

1. Find what the 80 ms stalls are, and whether they start because the music
   starts or the other way round. Per-window (not cumulative) stale and stall
   counters, timestamped together, would settle the direction in one run.
2. `g_apu_lock_handoffs` needs its trigger widened before its 84 can be trusted.
3. A longer xemu intro capture (the recipe is in
   `CLAUDE_HANDOVER_2026-09-10_XEMU_REFERENCE.txt`; `dvd_path` is already set)
   would extend the differential past the 30 s that `B_xemu_attract.wav` covers.
