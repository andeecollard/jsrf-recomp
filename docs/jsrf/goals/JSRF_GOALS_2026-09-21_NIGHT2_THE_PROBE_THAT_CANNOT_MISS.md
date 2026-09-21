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

1. **G26.** The render-target probe. Closes the last binary question on a
   defect the player can see.
2. **FRAG_FORCE on the Load screen**, with `AFTER` set — only on the SHADING
   branch, only after G26.
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
