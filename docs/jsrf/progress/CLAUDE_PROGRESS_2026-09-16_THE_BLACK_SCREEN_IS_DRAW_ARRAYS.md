# The black screen is DRAW_ARRAYS, and the other seven fixes never ran

> **READ THIS FIRST -- TWO CONCLUSIONS IN THIS FILE ARE RETRACTED.**
>
> **1. BATCH TRUNCATION IS NOT THE FENCE BUG. CLOSED BY MEASUREMENT.** The
> index cap was raised 4096 -> 16384 and it demonstrably works: a post-fix
> gameplay run reports `no-room=0 (e16=0 e32=0); biggest batch asked for 9681
> of 16384`. Nine thousand six hundred and eighty-one indices requested, zero
> capacity failures -- and the fences still disappear. The handover's section 4
> ("THE FENCES -- CAUSE MEASURED, FIX APPLIED") is wrong about the cause. The
> cap raise is a real fix for a real defect; it is not this one.
>
> **2. DRAW_ARRAYS IS FAR TOO SMALL TO BE THE FENCE.** Measured across four
> scene-matched gameplay runs that had the decode compiled in: ~0.9% of batches
> and **0.01% of indices**, under one batch and under eight vertices per frame.
> A mesh canopy cannot be in there.
>
> **WHERE THE FENCE BUG ACTUALLY IS.** The fence's batches are submitted,
> accepted and drawn, and then produce no visible fragments. Every "was
> something rejected?" counter reads clean because nothing is rejecting
> anything: `[TEXTURE] prepared=894355 rejected=0`, `[VSH] rejected=141` (all
> "fixed-function clip W"), `[METAL] refusals=0`, `0 software fallbacks`, and
> the same presence-only in the player's own live session. Alpha-test and blend
> state are measured clean in two independent gameplay runs two days apart.
> The texture cache is structurally incapable of it -- a hit requires a full
> memcmp of the texture every time.
>
> The question is no longer "where did the fence go?" but **"which stage turns
> a known-submitted fence draw into zero visible fragments?"** The path to
> instrument, in order, is **mip selection -> texture sampling -> alpha test**.
> The leading hypothesis is mip/LOD on an alpha-cutout weave texture: the
> minified mip averages the holes into a sampled alpha below the test
> reference, every fragment discards, and the geometry and state are valid
> throughout -- which is precisely why no refusal counter can see it.
>
> The rest of this file remains accurate about the BLACK SCREEN, which is a
> different bug from the fence. Note its own later correction too.


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

## A latent defect found beside it, deliberately not fixed yet

`src/kernel/nv2a_pb_exec.c`, in the `SET_VERTEX_DATA_ARRAY_FORMAT` decode:

```c
a->stride = (param >> 8) & 0xFF;
```

`NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE` is `0xFFFFFF00` -- a **24-bit**
field. Masking to 8 bits wraps any stride of 256 or more: a stride of 260
becomes 4, and every vertex after the first is fetched from inside its
predecessor. A stride of exactly 256 becomes 0, which `fetch_attr` refuses, so
that case at least fails loudly.

**It is latent, not a live cause.** Every attribute stride observed in any run
log in this tree is 32 (`stride=32`, four dumps; no other value appears), so
nothing measured here has crossed 256. That is why it has not been fixed in the
same change: the correct mask is obvious, but this is exactly the "obviously
safe" reasoning that put the black screen in, and it deserves its own run.

## CORRECTION, same day: the 4/4 separation above compared two different scenes

The framebuffer-census separation in section 4 is CONFOUNDED and must not be
relied on. `caponly1` and `reverted` (the "working" arm) are GAMEPLAY runs --
`[APU-VOICE] on=316` and `on=453`. `bisectA` and `idxcap` (the "black" arm) are
ATTRACT-LOOP runs -- `on=4`. The two arms differed in scene as well as in the
decode, which is the shortcut CLAUDE.md lists under known-bad reasoning:
"comparing log sizes across scenes". The exact arithmetic in section 3 closed
because the populations were never comparable, not because the model was right.

The honest table, all eleven 16-Sep runs, black = `[FB] nonzero=0` samples:

    scene      decode IN                          decode OUT
    attract    idxfix1 192/200, bisectA 35/44,    nv2aalias 0/200, cyclebrk1
               idxcap 232/241      -> 3/3 black   0/203, apuclock1 0/225,
                                                  e32_2 0/280  -> 0/4 black
    gameplay   idxfix2 6/200, idxfix3 5/200       caponly1 0/260,
               -> render fine                     reverted 0/150 -> fine

WHAT SURVIVES: within the attract scene the association is real -- 3/3 black
with the decode against 0/4 without. Fisher one-tailed on n=7 gives p = 1/35 =
0.029. That is suggestive, not established.

WHAT DOES NOT SURVIVE: "DRAW_ARRAYS is the black screen" as a general claim.
In GAMEPLAY the decode is measured harmless: idxfix2 and idxfix3 drew 59,008
and 99,422 DRAW_ARRAYS indices with the picture correct for the whole run, and
idxsplit1 drew 64,596 over 10,719 calls. The defect, whatever it is, is
specific to the title/attract scene.

ALSO WRONG ABOVE: the "sticky attr" mechanism is not supported. In the black
runs `draw_arrays_calls` FREEZES at 1,201 from the second report onward while
`[GPU] draws` climbs to 119,413 and the framebuffer stays at nonzero=0 -- so
the black frames contain no DRAW_ARRAYS batches at all. Stickiness cannot
explain frames that never had one. The texture-cache latch (99.3% hits) is a
consequence of the screen already being black, not a cause.

AND THE STRIDE CLAIM ABOVE IS UNVERIFIABLE, not merely latent. The only stride
evidence in the tree is four `[ALPHA-IN] attr[3] decl: ... stride=32` lines,
and that print reads `s_gpu.attr[3].stride` -- the value AFTER the 8-bit mask.
A true stride of 288 prints as 32. "No observed stride exceeds 255" cannot be
concluded from any existing log; the field has to be measured at full width.

The attract loop is itself a pre-existing intermittent guest-progress failure
that happens with and without the decode, and it has been silently
contaminating render A/Bs. It needs its own investigation before any further
render comparison is trusted: every arm must be scene-matched.
