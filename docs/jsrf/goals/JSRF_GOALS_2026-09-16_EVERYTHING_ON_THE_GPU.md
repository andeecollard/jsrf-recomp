# JSRF goals — everything on the GPU, and the sound stays on, 16 September 2026

> Superseded, 16 September 2026 (night). The active list is
> `JSRF_GOALS_2026-09-16_NIGHT_THE_DEFECT_LIST.md`, which keeps the same
> target and replaces the goals with specific defects. Kept for the history.

Supersedes `JSRF_GOALS_2026-09-13_THE_TUTORIAL_IS_THE_LAST_GATE.md`. Evidence
for where we are is `handovers/HANDOVER_2026-09-16_NIGHT_THE_LATCH_THE_GLYPHS_
AND_EVERYTHING_ON_THE_GPU.txt` and the two 16 Sep handovers before it.

## The player's instruction, verbatim

> I want everything on the gpu

That is the goal state, set by the player on 16 Sep 2026. Every vertex the
title submits is transformed on the GPU (programmable path: done, default;
fixed-function path: built, gated, drawing two wrong glyphs), the frame is
presented from the GPU surface without a CPU readback (flip sync: designed,
staged, not implemented), and the sound never stops or breaks up.

## The measurable target, in the player's words (16 Sep, evening)

> We want to hit 60fps and for the player to move at the correct speed

On this title those are one number: a late vblank drops its period, so game
speed = 16.683 ms / mean frame time. "Correct speed" means p99 frame time under
16.68 ms in a mission, not just the mean. Current: player session mean
15.4 ms (fixed-function on) / 18.1 ms (off), p99 39-43 ms. The levers, in
order of size: G2 (6 ms, blocked on the glyphs), G3 (4-9 ms, the flip sync),
then the tail.

## Where we are

Player-confirmed on the 16:11 bundle: fences fixed, sound effects audible.
Player-reported on the same bundle: music died at ~20 s and effects cut off
(idle-trap edge on), two corrupt glyphs in tutorial text (GPU fixed-function
on). Both switches are off in paths.conf tonight; neither is where we want it.

Frame time is game speed on this title (a late vblank drops its period), so
every millisecond off the frame is speed the player feels.

## Goals, in the order they should be done

**G1. DONE (16 Sep, night). Music keeps playing and effects finish.**
Neither trap arm is right: level nags at 1500 Hz and breaks the music up,
edge tells the guest once and it never reclaims the voice, so the free list
empties and playing voices are stolen. Build the bounded re-raise
(`RECOMP_APU_IDLE_TRAP_REARM_MS`): keep the edge latch, re-raise a voice still
inactive-and-linked after one guest tick. Add `[APU-POOL]` (distinct handles
started per window, handles reused within a window) so voice theft is
measurable. First check whether the latch sets on an attempted or a delivered
raise.
*Measured:* scripted mission run, 150 s, live audio: trapped 3.6% (level arm
53%, pure edge 0.6% with the music dead), 2D heard 203,741 — the highest of
the three arms — reraise 1,387, rearm 70, starved 0, `[APU-POOL] on_active=0`.
Enabled in the player's paths.conf. *Still to do:* a 5+ minute player session
confirms it, then make it the tree default.

**G2. IN PROGRESS. Fixed-function T&L on the GPU draws the same picture.**
Two arms now differ by one known defect fewer: the GPU path skipped the CPU
path's clip-w batch refusal, and does not any more. The glyph corruption
itself is reproduced once in 24 captures and then absent from ~4,800 parked
frames with a per-frame detector on, so it is TIMING-dependent, not data-
dependent — the vertex data the CPU fetched never changed. Leading suspect is
the vertex staging ring, which wraps at different rates in the two arms
because they upload different-sized rows. Detectors are live in the player's
build.
Capture the tutorial text frame in both arms (frame_bisect.sh or
play_tutorial.sh), add a per-batch dump for the glyph batches
(key, texmat enables, attribute sizes/types, current registers, first
vertices, CPU oT0), diff good glyphs against bad, fix the field that differs,
add that batch as a state of `jsrf_ff_msl_diff_test`.
*Done when:* the player confirms clean text on a bundle with
`RECOMP_METAL_FF=1`, and a scripted pixel diff of the text frame between arms
is empty. Then default it.

**G3. The frame is presented without the readback — flip sync stages 0-2.**
Stage 0 (blit instrument with bilinear tolerance) and a per-frame sync
histogram first, so the p99 claim is testable. Then the surface-cache lookup
of the flipped address, then the presenter. Design and patches are staged in
the 16 Sep afternoon handover, section 4.
*Done when:* mean frame under 16.68 ms with p99 under 20 ms in a mission,
and the game runs at full speed by the frame-locked measure.

**G4. DONE (16 Sep, night). The harness cannot silently differ.**
play_scripted.sh prints its full switch set and diffs it against paths.conf
in the run log; every switch prints its state. Without this every A/B above
is suspect.

## Rules for this phase

- A player-facing default needs a picture or a listen, not a residual.
  G2's diff test scores positions; it did not catch the glyphs.
- When a report and a log disagree on time, the log is not the evidence:
  last-run.log is overwritten per launch. Copy it before relaunching.
- Do not re-enable `RECOMP_APU_IDLE_TRAP_EDGE` as it stands. Twice silenced
  the music, once with the log.
- `pgrep -x jsrf_first_fault` before touching `src/`.
