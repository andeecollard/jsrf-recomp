# The frame goes to the CPU and back — 18 September 2026, night (3)

Written during the `RECOMP_METAL_NO_FLIP_SYNC` A/B. Everything below is measured
from **completed** runs; the A/B's own verdict is not in this file.

## The headline

**Every frame makes a GPU → CPU → GPU round trip, and it is 32% of the frame.**

```
1. Metal renders the scene
2. NV097_FLIP_STALL -> snapshot_surface() -> nv2a_metal_sync_range():
   WAIT for the GPU, then read the whole colour surface back into guest RAM
3. d3d8_gl.c: convert it, glTexSubImage2D it back ONTO the GPU
4. draw a quad, SDL_GL_SwapWindow
```

`nv2a_metal.m` contains no `presentDrawable`, no `CAMetalLayer`, no `MTKView`.
The frame crosses **Metal → CPU → OpenGL** to reach the screen. Step 2 is timed
on its own as `[STAGE] sync`, one call per frame:

| run | `[STAGE] sync` | frame | share |
|---|---|---|---|
| player session 17:55, no instrumentation | **5.19 ms** | 16.43 ms | **32%** |
| scripted `walkattr2`, heavier scene, walk armed | 11.89 ms | 32.82 ms | 36% |

Step 3 does not even appear there — the re-upload is inside `rest` (11.12 ms).
And across all Metal syncs the wait dominates the copy:

    [METAL] sync 25340 calls: 69144.1 ms draining the GPU,
                              14860.7 ms reading back and converting

**82% is the drain.** This is precisely what `ms-fusion-nv2a-translator.md` §8
records Microsoft refusing to do — their semaphore handler appends to a
1,024-entry ring and never blocks, and *"everything else in the design is
arranged so that the pushbuffer consumer never has to block"*.

It is also the literal obstacle to the player's standing goal, "I want
everything on the gpu".

## THE A/B: SKIPPING THE READBACK DOES NOT BUY THE TIME. IT MOVES IT.

`RECOMP_METAL_NO_FLIP_SYNC`, 3 trials per arm, 240 s, ABBA, binary pinned by
sha256, arms verified distinct.

    =0  frame 27.04 28.46 30.76 ms   (mean 28.75, n=3)
    =1  frame 27.48 27.97 32.00 ms   (mean 29.15, n=3)
    THE RANGES OVERLAP. 0.40 ms of difference inside the run-to-run spread.

**The switch worked perfectly and the frame did not move**, and the per-stage
numbers say exactly why:

    =0   submit= 4.52   sync= 9.92        =1   submit=15.01   sync=0.01
    =0   submit= 5.58   sync=10.68        =1   submit=15.27   sync=0.01
    =0   submit= 4.85   sync=10.81        =1   submit=15.79   sync=0.01

`sync` goes to zero in all three runs and `submit` absorbs almost exactly the
same time. **The wait is the GPU finishing the frame, not an artificial
CPU-side serialisation.** Not waiting at the flip only defers the same wait to
the next submit.

**What this costs the direct-Metal-present idea.** Presenting from a
`CAMetalLayer` would still remove the 4.9 MB read-back, the format conversion
and the `glTexSubImage2D` re-upload -- all real. It will **not** recover the
5.19 ms, because that is GPU time. Anyone quoting the 32% as the prize for
that work is quoting this A/B's refutation. Size it from the copy and the
upload, not from `[STAGE] sync`.

**Harness defect found and fixed by this A/B.** The first scoring said "arms
NOT verified distinct: no report names RECOMP_METAL_NO_FLIP_SYNC" although the
token was registered in `SWITCH_TOKEN` and printed in both arms -- because
`ab_score.py` only harvested the token from lines beginning `[METAL]`, and this
counter prints under `[FLIP-SYNC]`. That is the third VOID check this
restriction has silently skipped (`defer_swap` was the first). The harvester
now scans every line; the token form is specific enough, and an unrecognised
token is inert because `switch_state_for` only matches names in `SWITCH_TOKEN`.
Re-scoring the archived logs with the fix reads "arms verified distinct", with
no new runs.

## THE COLOUR RESOLVE: THE SAVING IS REAL AND IS NOT MOVED. NOT YET SEPARATED.

`RECOMP_METAL_NO_COLOUR_SYNC`, 4 trials per arm, 240 s, ABBA, binary pinned
(same sha256 as the flip-sync A/B), arms verified distinct, one run excluded
for never reaching a mission.

    =0  frame 23.63 22.04 23.72 ms        (mean 23.13, n=3)
    =1  frame 21.77 20.75 21.82 22.97 ms  (mean 21.83, n=4)
    ranges overlap on one run -- VOID by the non-overlap bar

**The stage data is what makes this worth keeping.** Unlike the flip
read-back, the cost is removed rather than relocated:

    sync    =0  9.60 9.45 8.97          =1  7.73 7.91 8.22 8.07
    submit  =0  4.85 4.72 4.02          =1  4.08 4.11 4.16 4.11

`sync` separates non-overlappingly -- max(=1) 8.22 < min(=0) 8.97 -- for a mean
delta of **1.36 ms**, and `submit` does not absorb it. The frame mean moves
**1.30 ms**, which matches the stage saving to within 0.06 ms. Mechanism and
effect agree, which is stronger than either number alone.

Compare the flip read-back, where `sync` went to 0.01 and `submit` rose by
almost exactly as much. That is the difference between a resolve that is never
performed and a wait that is merely deferred.

**Owed:** four more trials. Depth took 8 usable runs across two A/Bs to
separate; this has 7, and 3 of the 4 `=1` values already sit below every `=0`
value.

**A methodological failure worth recording, because it cost a whole A/B.** The
first colour A/B was run while this session was grinding Python over the 50 MB
generated tree for the dead-flag and clobber scans -- exactly what the three
subagents had been instructed not to do, for exactly this reason. Its frame
spread was 20-44 ms against 20.75-23.72 on the quiet re-run. The stage numbers
survived contamination because they are per-frame averages within a run, but
the frame-time arm was worthless. **A frame-time A/B owns the machine.**

## A1 IS NOT THE FRAME-TIME LEVER. Measured, and the plan is wrong.

`PLAN_2026-09-18_NIGHT3` ranks the NV2A descriptor table as "the biggest lever
on both frame time and the glyphs". It is not a frame-time lever at all.

`RECOMP_PB_STAGE_WALK=1` brackets every method, and `FLIP_STALL` and
`CLEAR_SURFACE` are methods, so `walk` **contains every other stage**:

    walk 24.80   vsh 3.92 + prepare 0.05 + submit 5.63 + sync 11.89 + clear 0.21
                                                                      = 21.70
    walk - nested                                                     =  3.10 ms
    instrumentation  64,617 calls x 42 ns (this file's own figure)     =  2.71 ms
    net dispatch + every non-draw method body                          ~  0.4 ms

Essentially all of `walk`'s own time is the timer measuring it. A 2,048-entry
descriptor table can buy **at most ~0.4 ms of a 32.8 ms frame**, which is below
the resolution of the instrument that measured it.

Items 1 and 2 of the NV2A research (replace the switch, make unknown methods
free) remain real engineering improvements — the dirty groups and parameter
counts becoming table data is worth having. They are **not** worth a
regeneration-sized effort on frame-time grounds, and the plan's ordering should
not survive this.

Also corrected: the plan calls it "a ~900-line `switch`". It is 895 lines with
**23 case labels**, fronted by 7 range tests and tailed by 3 more. The shape is
a comparison chain, not a wide switch, and the chain costs ~6 ns per method.

## The DSP recognition idea cannot help this title

`MS_MCPX_APU_MODEL.md` §8/§10.7 is the purest "translation not emulation" item
in the audio research: Microsoft fingerprints the `.scr` effect program the
guest downloads against a 103-entry versioned catalogue and runs a **native**
implementation, rather than emulating the GP/EP DSPs.

JSRF never uses them. From two completed sessions, identical and constant from
startup:

    [APU-WRITE] main=516397 vp=30026 gp=7 ep=8 other=0     (17:55)
    [APU-WRITE] main=119045 vp=39529 gp=7 ep=8 other=0     (11:47)

Fifteen DSP register writes in a 25-minute session, all at init. Real
xboxrecomp improvement for other titles; **cannot** affect JSRF's music.

## The IEN gate is xemu's, character for character

The night-3 handover's headline is *"Microsoft's own APU model never reads IEN,
ours gates on it"*. True — but "ours" is not a local invention. `src/apu` heads
with *"Standalone extraction from xemu"*, `apu_regs.h` is **366 lines, all 366
identical** to xemu's, and `update_irq`'s gate is character-identical:

```c
if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
    ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) & d->regs[NV_PAPU_IEN])) {
```

This does not refute the hypothesis — the divergence from Microsoft is real and
`[APU-IEN]` is still one number worth reading. It lowers the prior: this is the
gate every xemu user runs, not something we invented. **Caveat that must travel
with it:** nobody has checked whether xemu itself keeps JSRF's music alive past
t=270, so "xemu has it too" is weaker evidence than it sounds.

## A real divergence from xemu, found and killed the same night

`FECTL`'s trap test differs, and ours is the stricter one:

```c
xemu:  if (d->regs[FECTL] & FEMETHMODE_TRAPPED)             /* 0x80 & 0xE0 -> true for HALTED */
ours:  if ((d->regs[FECTL] & FEMETHMODE) == FEMETHMODE_TRAPPED)  /* TRAPPED only */
```

HALTED is 0x80, TRAPPED is 0xE0, mask 0xE0 — confirmed identical in both trees.
So in HALTED xemu sets `FETINTSTS` and we do not: **124,443 frames** of it in
the 17:55 session, and `total − se = 124,443` exactly, so every one is a frame
the sound engine skipped. Fewer interrupts than the reference, on the path whose
failure mode is "the model stops telling the guest".

**It is not the freeze.** Per 5 s window across the freeze:

    t=264  halted+361  trapped+2506  guest_methods+61
    t=269  halted+265  trapped+2671  guest_methods+26
    t=274  halted+396  trapped+2514  guest_methods+0    <- freeze
    t=279  halted+335  trapped+2596  guest_methods+0

`guest_methods` stops dead; `halted` and `trapped` carry on unchanged. Across
the whole run `halted` drifts slowly from ~275 to ~570 per window with **no step
at the freeze**. A first pass comparing a short early window against a long late
one read that drift as a 43% jump; it is not one.

## The colour A/B is sized

    [METAL] colour write-backs: 16883 taken, 0 skipped (no_colour_sync OFF)
    [METAL] depth  write-backs:     0 taken, 16872 skipped (no_depth_sync on)

Colour has the same order of write-backs as depth, and depth won 9.1%. Worth
running — after the flip read-back, which is the larger object.

## Tree state

67/67 ctest (was 66/66; `jsrf_flip_sync` added). Switch audit: 167 switches,
0 problems. `RECOMP_METAL_NO_FLIP_SYNC` is default OFF, counted in both arms,
registered with `ab_score.py` as `no_flip_sync`, and its test is verified by
injected fault — a bare `getenv` makes the `=0` arm alone go red.
