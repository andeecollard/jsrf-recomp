# JSRF goals — the probe that cannot miss, 21 September 2026 (night, second)

Supersedes `JSRF_GOALS_2026-09-19_EVENING_EVERY_EDGE_IS_COUNTED.md` for
ORDERING ONLY. That file keeps the full evidence for G20–G25 and none of it
is retracted here. This file records what the instrument error of 21 September
does to the evidence base, opens **G26**, and resets what gets worked first.

Built on `HANDOVER_2026-09-21_NIGHT2_THE_PICTURE_WE_WERE_READING_WAS_NEVER_THE_PICTURE.txt`,
which supersedes the two earlier handovers of the same day.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

G1–G25 carry forward. The render-target identity question takes **G26**.

> NUMBERING NOTE. `JSRF_GOALS_2026-09-05_PUSHBUFFER_RATE.md` also has a G26.
> That is a DEAD SERIES from a chain superseded a fortnight ago. The live
> series is G1–G25 out of the 17–19 September files. This G26 is the live one.

## WHAT CHANGED, AND IT IS AN EVIDENCE PROBLEM BEFORE IT IS A RENDERER PROBLEM

Every framebuffer picture this project had ever read was the LIVE surface,
sampled part way through composing a frame. `nv2a_pb_exec_dump_surface()` —
the function whose whole job is to write the PRESENTED frame — had no callers.
Proof is one instant, one sequence number: `snap031` is the complete JSRF
title screen; `report031` is the same instant as a solid black silhouette.

Three consequences, and they are goals-level, not notes-level:

1. **The "intro silhouettes" defect is not a defect.** Withdrawn.
2. **Every black-screen claim of the last week is UNVERIFIED, not refuted.**
   They were read off a half-composed frame. Each needs re-reading against
   `snapNNN` before it is spent or believed. §2 of the handover is one that
   was re-read and SURVIVED.
3. **The Load-menu defect changed shape.** It is not "the Load screen renders
   black". The menu renders perfectly (`snap037`, 306820/307200 nonzero),
   fades out correctly, and the screen that should follow never arrives —
   for thirty-three seconds, 0 stale repeats, frame sequence advancing
   3429 → 10766 the whole time.

**The rule this buys, and it is permanent:** a picture is evidence only if the
dump states its own freshness. `[SNAP]` now prints the pool frame sequence,
the source surface, and says outright when the sequence has not moved. A dump
without that line is not a measurement.

## G26 — DOES THE GUEST'S RENDER TARGET AGREE WITH OURS?

**The question, and it is binary.** During the Load-screen black, 28 batches a
frame are accepted, submitted and rasterised — `[TEXTURE] prepared=283142
rejected=0`, `[METAL] hw draws=283142 refusals=0`, `[VSH] 195870 accepted
0 refused`, triangles climbing ~38,000/s — and put no non-zero pixel on the
presented surface. Either:

    COVERAGE   they are not landing on the surface we present
    SHADING    they land on it and write zero

**The measurement.** Passive, every report, from any run that reaches the Load
screen. It cannot fail to reach its own condition, which is exactly what the
white test could not promise and why it burned two runs.

    device  = *(u32 *)(mem + 0x0019DCE0)      D3D8__D3D_g_pDevice
    rt      = *(u32 *)(mem + device + 0x2070) D3DDevice.m_RenderTarget
    data    = *(u32 *)(mem + rt     + 0x0004) D3DSurface.Data
    compare data (masked) against s_gpu.color_offset

**`D3DSurface.Data = +0x04` is DERIVED FROM THIS TITLE'S OWN XBE**, to the same
standard as every row of `jsrf-d3ddevice-offsets.tsv`, and it is new — the
handover asked for the surface to be "resolved to its guest offset" without
saying how, and nothing in the tree knew the layout. Evidence:
`D3DDevice_SetRenderTarget` at `0x0018D3E2` reads `mov ecx,[edi+4]` from the
incoming render target and at `0x0018D3EB` `mov edx,[ebp+4]` from the depth
stencil, both immediately before it builds the surface pushbuffer words at
`0x0018D417` (`mov edx,0x40100` / `mov ecx,0x40110`). The same function reads
`[edx+0x0C]` as Format (`shr 0x14; and 0xF` — the U/V size nibbles) and
`[edx+0x10]` as Size (`and 0xFFF; inc`), which is D3DPixelContainer exactly,
so the whole layout corroborates the one field we need.

**THE PROBE MUST VALIDATE ITSELF.** It is a guest-VA dereference through
`xbox_GetMemoryOffset()` and a null or stale device pointer would read as a
mismatch — that is, as the answer. So every probe line also reads `m_pPut` at
`device+0x00` and checks it lies inside the ring `[device+0x24, device+0x28]`
(`m_PushBufferStart`, `m_PushBufferEnd`, both "certain"). If that holds, the
address-space assumption is proven in the same line that uses it. If it does
not hold, the line says NOT TRUSTED and reports no verdict at all.

**The decision rule, written before the run:**

    THEY MATCH    -> SHADING. The guest and we agree on the target and the
                     batches land there. RECOMP_FRAG_FORCE finally earns its
                     run, with RECOMP_FRAG_FORCE_AFTER set so the menus can
                     be driven normally first.
    THEY DIVERGE  -> COVERAGE. The guest pointed somewhere we never followed.
                     That is the bug, named and located. Cxbx-Reloaded's HLE
                     D3D8 then becomes worth pulling — it is the authority on
                     what SetRenderTarget does to the pushbuffer — and it is
                     the ONLY outside project worth a session, on this branch
                     only.
    NOT TRUSTED   -> fix the probe, not the renderer.

## G26 IS ANSWERED, AND SO IS THE STEP AFTER IT — 21 September, 16:35

**SHADING, twice over, and the second measurement is stronger than the
first.**

The passive probe, across a player-driven run with 35 seconds of black:
**66 MATCH, 0 COVERAGE, bound-ever=yes throughout**, device never missing,
ring check never failed, 0 stale snapshots, frame sequence 7711 → 10585. The
guest's `m_RenderTarget->Data` and the offset our parser latched are the same
number, byte for byte, while the screen is black.

Then the white test, armed by the defect itself:

| t | presented nonzero | |
| --- | --- | --- |
| 33–35 | 306195 → 306506 | the Load menu, rendering |
| 36–37 | 279022 → 99034 | the fade, behaving correctly |
| 38–40 | **0** | the defect. 28.0 draws/flip throughout |
| 41+ | **307200 / 307200** | the force armed, and every pixel lit |

**THE 28 BATCHES A FRAME COVER THE ENTIRE 640×480 SURFACE.** Not part of it,
not a sliver — all 307,200 pixels, on the frame we present, while the screen
is black. Nothing is missing, misaimed or clipped away. The geometry is
exactly where it should be and the fragment shading resolves to zero.

That closes the binary question this file was written around, and it kills a
whole family of theories with it: missing geometry, a wrong render target, a
viewport or clip problem, a surface we never followed. None of them survive a
measurement that lights every pixel of the same frame.

**What it leaves is narrow and cheap to test.** `shade()`'s inputs are
already switchable and the arm now costs nothing to reuse:

    RECOMP_FRAG_FORCE=1   raw TEXTURE0      black here -> the texture is black
    RECOMP_FRAG_FORCE=2   PRIMARY_COLOR     black here -> the vertex colour is
    RECOMP_FRAG_FORCE=4   TEXCOORD0         shows the coordinates outright

Three player-driven runs, each self-arming, each answering a different half.
The night handover's six arms all read these same questions off half-composed
frames and every one of them is UNVERIFIED rather than refuted — they can now
be re-asked properly.

## G27 — THE LOAD SCREEN IS RENDER-TO-TEXTURE, 21 September, 17:00

**Found with the xemu differential, which is the first one this project has
ever run.** Driven to the Load screen: 119 s, 23,269,892 trace lines, 1.79 GB,
3,671 flips. The player confirms xemu ANIMATES that screen where ours is
black, so the two sides are matched on the thing that matters.

### The correction first, because it changes what three runs meant

Mode 4 reported TEXCOORD0 as flat and out of range, and that was read as the
collapsed-q defect. **It is not.** The Load screen's stage-0 texture format is
`0x11129`, and format `0x11` is `LU_IMAGE_R5G6B5` — **linear** — with its size
coming from `SET_TEXTURE_IMAGE_RECT = 0x028001e0`, which is **640×480**, not
from the log2 fields. Linear textures take **unnormalised** coordinates.

Mode 4's `oor` flag tests against `[0,1]`, so **blue everywhere is the correct
result for a linear texture** and indicates nothing. And `fract(q) = 0.5`
uniformly is precisely a 1:1 full-screen blit with half-texel centring: at
pixel *x* the coordinate is *x+0.5*, so `fract` is 0.5 at every pixel. The
coordinates were right all along, and our own code already knew it —
`nv2a_metal.m:674` clamps `uv` to `[0,w]×[0,h]` when linear.

### What is actually happening

Last 500 flips of the xemu trace:

    render_to_texture : 500        exactly once per frame
    surface_download  : 0
    surface_upload    : 0

    nv2a_pgraph_surface_render_to_texture
        Rendering surface 0x03ec4000 to texture (640x480)

`0x03ec4000` is one of the four `SET_TEXTURE_OFFSET[0]` values bound on that
screen. **The Load screen composites itself by sampling a surface it has just
rendered into**, once per frame, through a full-screen linear texture.

That accounts for every measurement taken today, with nothing left over:

| measurement | explained by |
| --- | --- |
| coverage 307200/307200 | a full-screen quad — what a composite is |
| PRIMARY_COLOR white | modulate, so the texture carries the whole image |
| TEXCOORD0 0..640 / 0..480 | a 1:1 blit, correct |
| **TEXTURE0 black** | **the source surface never reached the sampler** |
| the menu renders | drawn directly, no round trip |
| the fade is correct | still direct |
| then black | the first frame composited rather than drawn |

### Why this is a lead and not a rewrite

`nv2a_metal.m:4879` already calls `surface_pay_debt_for_range` before
uploading each stage's bytes, and its comment names this exact case: *"a
TEXTURE whose guest bytes are a surface this title has rendered into and not
yet handed back … which is render-to-texture silently sampling last frame."*
It also notes the walk "costs nothing when nothing is owed — which, with the
deferral off, is always."

**The next measurement is `RECOMP_SURFACE_CENSUS`**, which exists for exactly
this question: it reads back every held surface and prints what the GPU holds
beside what guest RAM holds, and "where they disagree is where the frame goes
missing". GPU has the picture and guest RAM is black → the write-back is the
defect, and `g_debt_paid_on_read` is the counter that says so.

**NOT YET VERIFIED ON OUR SIDE:** that our Load screen binds a texture whose
offset is one of our own surfaces. The guest code is identical and the title
streams agreed method for method, so it should — but the next run carries
`RECOMP_PB_SCAN=1` alongside the census and shows it rather than assuming it.

## WHAT IS EXPLICITLY NOT THE NEXT MOVE

- **The white test first.** Player-driven, depends on being on the right
  screen at the right second, has already burned two runs. `RECOMP_FRAG_FORCE=3`
  paints the MENU white too, so the run cannot be navigated to its own
  condition. It comes after G26, not before.
- **The FF batch watchers on the Load screen.** All four call sites are in the
  fixed-function branches; this title draws through guest vertex programs
  (317,384 GPU vsh draws against 0 CPU in a 45 s boot). They would have come
  back empty and been read as "the texture is fine".
- **The multiply-blend absorbing state.** Dead. `[BLEND-FADE] batches under
  DST_COLOR=72` frozen across every report and not moving during the black.

## ORDER OF WORK

1. ~~**G26.** The render-target probe.~~ **DONE, 16:30. SHADING.**
2. ~~**FRAG_FORCE on the Load screen.**~~ **DONE, 16:35. Full coverage,
   307200/307200.** Next is `FRAG_FORCE=1`, then `=2`, then `=4`, to find
   which of `shade()`'s inputs is the black one.
3. **`snapNNN` becomes the default for every run**, and `reportNNN` gets a name
   that cannot be mistaken for the frame. The name is what caused the error,
   six arms deep, twice.
4. **Re-read the week's black-screen claims against `snapNNN`** as unverified.
5. **A preserved playthrough**, under a preserving script, with snap dumps at a
   small stride across a real level transition. Settles "NOW LOADING" (which is
   probably not broken — the player saw it last week because the game was STUCK
   on it at 10.2 Hz) and banks the playthrough evidence in the same run. The
   player completed a level on 21 September and NO LOG OF IT EXISTS.
6. Everything from the evening handover's §10 item 2 onward — Roboy, vocal
   choppiness, the APU stall, text defect mode B, duplicate surface slots, the
   OWED verdict, `unresolved_stubs`, Windows — untouched and still standing.

## THE INSTRUMENTS BELONG IN THE REPOSITORY

The snap dump is the finding, and as of tonight it exists only as an
uncommitted diff. `jsrf-d3ddevice-offsets.tsv` and every `run-*.sh` live in
`~/jsrf-build`, outside git entirely. Losing them costs the week they bought.
They get committed before G26 is run, not after.
