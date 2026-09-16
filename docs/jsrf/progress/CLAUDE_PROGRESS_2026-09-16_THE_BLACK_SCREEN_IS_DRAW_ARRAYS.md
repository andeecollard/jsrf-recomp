# The black screen is DRAW_ARRAYS, and the other seven fixes never ran

Answers open item 1 of
`handovers/HANDOVER_2026-09-16_THE_FENCES_THE_RINGS_AND_A_BLACK_SCREEN_I_CAUSED.txt`.
Static reading plus arithmetic on run logs that already existed; no new run was
taken for this. The confirming run is item 1 of "what is still owed" below.

## The claim

Of the eight fixes in `attic/2026-09-16_renderer_audit_fixes_REVERTED.patch`,
the black screen is caused by **one**: the new `case NV097_DRAW_ARRAYS:` block
in `src/kernel/nv2a_pb_exec.c`. The decisive line is

```c
s_gpu.idx[s_gpu.idx_count++] = (uint16_t)v;
```

The other seven are either measured inert in the failing scene or cannot emit
black. **The index-cap raise remains innocent**, as the original bisect said.

## Why -- three independent checks, all against logs in
## `build-macos/jsrf-first-fault/render-investigation/`

**1. Only two of the eight changes execute at all in the failing run.**
`bisectA` (the black arm) reports:

    [RASTER] 0 batches + 0 triangles on the CPU; 1320305 triangles total
    [METAL] 119413 batches native, 0 software fallbacks
    [METAL] MIXED draws (software tail while hw on)=0
    [VSH] executed batches=119413 rejected=0

So `nv2a_texture_copy.c` -- both the LOD-scale fix and the
`depth_test && depth_write` gate -- never runs, and the `goto vsh_rejected`
path is never taken (`rejected=0`, and the label's body is a no-op unless
`RECOMP_VSH_REUSE` is on, which it is not). Those three are exonerated by
measurement, not by argument.

**2. Of the two decode cases, ARRAY_ELEMENT32 never fires in this scene.**
Every black run reports `elem32=0/0 seen`. 0x1808 is a gameplay-only method
here; the 164,101/run quoted in the handover came from a gameplay capture, not
from the boot sequence where the screen goes black. That leaves DRAW_ARRAYS.

**3. The index arithmetic closes exactly, on four runs.** `[GPU] draws N, M
indices` at the first report, modelling the changed builds as "the same
population, plus 1,079 four-index DRAW_ARRAYS batches" (`draw_arrays=4316/1079
seen (1079 calls)`, exactly 4.0 indices per call):

| run      | decode | model                                   | reported |
|----------|--------|-----------------------------------------|----------|
| caponly1 | out    | 5584x5 - 20 = 27900                     | 27900    |
| reverted | out    | 5677x5 - 20 = 28365                     | 28365    |
| bisectA  | in     | (5961-1079)x5 + 1079x4 - 20 = 28706     | 28706    |
| idxcap   | in     | (5955-1079)x5 + 1079x4 - 20 = 28676     | 28676    |

Exact in all four, with the same constant 20-index offset in every build --
including the two that do not have the decode, which is what makes the offset a
fixed accounting artefact rather than a fitted parameter. The unhandled-method
histogram is otherwise the same mix at the same per-draw ratios, so the guest is
issuing an identical command stream: the changed build simply **draws 1,079
four-vertex quads that the old build silently dropped.**

**4. The framebuffer census separates the two groups.** `[FB] ... nonzero=N`,
one sample per second through boot. Working builds (`caponly1`, `reverted`)
paint the 9,758-pixel logo screen; the changed builds (`bisectA`, `idxcap`,
`idxfix1`) never produce that sample at all and sit at `nonzero=0`.

## Why the quads are opaque garbage, and why nothing complains

`fetch_vertex -> fetch_attr(&s_gpu.attr[a], index, ...)`, and `s_gpu.attr[]` is
**sticky across batches** (`src/kernel/nv2a_pb_exec.c:1205-1215`). It refuses
only when `offset|size|stride` is zero, which they never are once any earlier
batch has declared them. So a DRAW_ARRAYS batch that did not re-declare its
arrays fetches positions out of the *previous* batch's array and draws a
full-screen quad -- with no reject, no `[VSH]` reject, no `[TEXTURE]` reject and
a clean `hw draws` count. That is exactly the "every health counter reads clean"
signature the handover describes.

It latches because the title screen feeds its own surface back as a texture:
once a frame is painted black the read-back writes zeros into the guest RAM the
next frame samples. `[METAL] texture buffers` shows 99.3% cache hits in the
black run (482 uploads) against 31,202 uploads in the working one, and the
cache is memcmp-verified -- the sampled texture is byte-identical for the rest
of the run.

## What is still owed

1. **The confirming run.** Apply the patch minus the `case NV097_DRAW_ARRAYS:`
   block and take a gameplay run; the picture should render. That single arm is
   what turns this from a closed argument into a measured result, and it has
   not been taken.
2. Then reapply the remaining seven **one at a time with a run each**, per
   section 8 of the handover. Three of them are measured inert in the boot
   scene, which means the boot run cannot validate them either -- they need a
   scene that exercises the CPU rasteriser.
3. When DRAW_ARRAYS is eventually decoded for real, the fix is not the decode
   but the sticky `s_gpu.attr[]`: a batch must not inherit another batch's
   vertex arrays. Decoding the method without that is what produced this.

## Corrections to the reverted patch itself, for whoever reapplies it

- The patch is internally inconsistent: its long `NV_MAX_INDICES` comment
  describes the raise to 16384, but the `#define` in that hunk is context at
  4096 and `NV2A_METAL_MAX_VERTICES` is defined as 4096. Applied as-is it keeps
  the old cap while documenting the new one. The working tree is the correct
  version; do not let the patch overwrite it.
- The patch's `static unsigned indices[...],n=0; n=0;` makes `n` static too.
  Harmless as written, but the working tree's split form is the one to keep.
