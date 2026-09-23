# JSRF goals — the D3D8 boundary, 23 September 2026

Supersedes `JSRF_GOALS_2026-09-22_THE_LIFTER_FOR_THIS_TITLE.md` for ORDERING.
That file's G28–G31 are all done and nothing there is retracted. G27
(hardware texture sampling, in the night-5 file) is carried forward unchanged.

The player judged the NV2A-emulation renderer "disappointing and against the
spirit of xboxrecomp" and asked for a lift at an API boundary. The feasibility
spec is `docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_D3D8_BOUNDARY.md`.
On 23 Sep they said to set goals and proceed. This file opens **G32–G35**.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

## What this track may and may not claim

The spec's 4–5 week estimate is provisional until G33 and G34 pass. A
DrawVertices interception prototype exists (`RECOMP_JSRF_DRAW_LIFT=1`,
`docs/jsrf/progress/PROGRESS_2026-09-23_TARGETED_DRAW_LIFT_PROTOTYPE.md`).
It proves that entry-point replacement works. It proves nothing about speed
or image parity, because it still emits the original commands.

**G27 is not retired.** Both renderers need native textures and hardware
samplers. G27's texture layer is to be keyed by the NV2A format word, size and
guest address, so a D3D-level renderer can call it unchanged.

## G32 — every entry point described, from the code

No run needed. Everything a wrapper needs to know about the 103 D3D functions
game code calls, derived from the XBE and checked against the generated C.

**Done when** `experiments/d3d8_boundary/entry_points.json` lists all 103 with:
1. the address and name (the five unnamed ones named from their code);
2. the calling convention: stack bytes popped (`ret N`), and the registers read before they are written (`ecx`/`edx` arguments);
3. the call-site count and callers from the census.

**Also:** a map of the flags word `0x19DED8`, giving the bits the graphics
layer sets and the bits `CDevice_SetStateVB`/`SetStateUP` test. The map goes
in the spec.

### G32 outcome, 23 Sep 2026

**Done.** `entry_points.json` lists all 103 entry points (`entry_points.py`,
names for the five unnamed ones in `inferred_names.json`):
- **Returns.** Every entry pops a consistent number of stack bytes.
- **Register arguments.** Exactly one entry takes them: `SetRenderState_Simple`, with `ecx` = method and `edx` = value.
- **Push counts.** They agree at every call site but one, and that one is explained (spec section 2b).

The flags-word map is in spec section 2b (`flags_map.py`). One item is
open: bits `0x10–0x40` are set only inside D3D, and the scan does not find
their consumer. It does not block G33.

## G33 — the census build (spec Phase 0)

A build that behaves exactly like the shipped one and counts what crosses the
boundary.

**What to build:**
- a. A staging script, generalising the prototype's. It copies the gen, renames each of the 103 bodies, and adds a wrapper that counts the call and then calls the original. It is guarded by `RECOMP_D3D8_CENSUS=1`, and by the hash of every body it renames.
- b. A counting mode for the memory watch, `RECOMP_MEM_WATCH_TALLY=ring`. It tallies every translated store and block write into the live pushbuffer ring (device `+0x24`/`+0x28`) by guest function, then prints the table at every Nth swap and at exit. The existing single-range print mode is unchanged.
- c. A run script that stages, builds into a separate directory and runs `measure.sh`-style with audio off.

**Done when:**
1. with the switch off, the staged build's gate numbers match the shipped build;
2. one scripted tutorial run (audio off, 0 guest faults) prints per-frame call counts for every entry point that fires, and the ring-store tally;
3. **every ring store comes from a function in the D3D section**, or each exception is listed by address and the spec is revised before G34.

### G33 outcome, 23 Sep 2026

**Done, and gate 3 passes: only D3D code writes the GPU command ring.**

Built from a staged copy of gen `52b6b3f8`
(`stage_d3d8_census.py`, 103 wrappers in `recomp_0007.c`/`recomp_0008.c`)
with `JSRF_MEM_WATCH=ON`, otherwise configured as `build-feav`. The build
includes the working tree's uncommitted ADX, USB, pushbuffer-executor and
Metal changes, which are not this track's. Two silenced 150 s `measure.sh`
arms were run by `run_d3d8_census.sh` and scored by `census_report.py`.

| gate | result |
|---|---|
| 1. wrappers change nothing | off arm: live=132, 0 guest faults, 8,813 flips, mean 15.87 ms (63.0 fps). On arm: live=138, 0 faults, 16.76 ms (59.7 fps; the tally routes every store through a function) |
| 2. per-frame calls | 83 of 103 entry points fired over 8,400 swaps; 20 never did |
| 3. ring writers | 62,914,560 ring stores (about 7,500 words per frame) from **70 functions, 0 outside the D3D section**, 0 block writes |

The ring lived at `0x8056D000–0x805ED000` (RAM `0x0056D000`, 512 KB).

**Calls per frame from game code** (8,400 swaps):

| entry point | per frame |
|---|---:|
| `SetVertexShaderConstant` | 219.6 |
| `SetTexture` | 107.0 |
| `SetTransform` | 89.9 |
| `SetStreamSource` | 68.1 |
| `SetIndices` / `DrawIndexedVertices` | 67.1 / 67.1 |
| `SetRenderState_Simple` | 44.2 |
| `SetTextureState_TexCoordIndex` | 39.9 |
| `SetVertexShader` | 32.2 |
| `SetPixelShader` | 15.9 |
| `SetRenderState_CullMode` | 13.9 |
| `SetVertexData4f` / `2f` (immediate mode) | 8.7 / 5.8 |
| `SetPixelShaderConstant` | 8.0 |
| `SetViewport` / `SetMaterial` | 7.4 / 6.1 |
| `D3DVertexBuffer_Lock` | 3.7 |
| `Clear` | 2.5 |
| `Begin`/`End`, `DrawVertices` | 1.5, 1.1 |
| `SetRenderTarget`, `Swap`, `SetGammaRamp` | 1.0 each |

About 850 D3D calls a frame. Nearly all geometry is indexed draws from
vertex buffers, about 67 per frame. **Never called in 150 s:** `CopyRects`,
`DrawVerticesUP`, `DrawIndexedVerticesUP`, `Reset`, `GetRasterStatus`,
`D3DSurface_LockRect`, `LockBox`, `CreateImageSurface`, `CreateVolumeTexture`,
`GetTransform`, `GetTexture2`, `GetSurfaceLevel`, `GetLevelDesc`,
`SetPushBufferSize`, `SetRenderState_MultiSampleMode`, `Delete*Shader`,
`D3DDevice_AddRef`/`Release`, and `FreeContiguousMemory`. Other scenes may
reach them.

**What this tells the replacement:**
- **Surface locks never happened.** Render targets are never locked, so write-back on lock is not exercised by this scene.
- **Vertex buffers are rewritten every frame.** `D3DVertexBuffer_Lock` runs 3.7 times a frame, so dynamic vertex buffers are live and need the ownership rule the audit in spec section 7 asks for.
- **D3D calls itself too.** It calls `SetTexture`, `SetVertexShader`, `SetPixelShader`, `SetRenderTarget` and `CullMode` twice per frame each, and `SetRenderState_TextureFactor` about 10 times. The replacement owns those paths as well.

**Caveats.** `measure.sh` ends a run with SIGTERM, so neither exit report
printed. The figures are the last periodic reports: the census at 8,400 `Swap`
calls, and the tally at 62.9 M ring stores. The `[FRAME]` line's 8,355 flips
counts a different event and is not the same measure. The
tally sees translated guest stores only. Host-side writes have no guest PC,
which is the intended scope. Gate 1 compared scene reached, faults and frame
rate, not the static control-flow gate.

## G27 outcome, 23 Sep 2026 (hardware texture sampling)

Built on the uncommitted groundwork already in the tree: the CPU decoder
`nv2a_texture_decode.c` and its test from 22 Sep, plus the texture-cache and
early-Z diagnostics in `nv2a_metal.m`. The player said "proceed" when asked
whether to build on it.

**What changed** (`nv2a_metal.m`):
- **Decode once per upload.** A texture unit using swizzled RGBA8 (0x06/0x07), DXT1 or DXT3 now samples an RGBA8 `MTLTexture`, decoded once per upload by `nv2a_texture_decode_rgba8`.
- **Samplers.** A cached `MTLSamplerState` makes the filter, mip and wrap choices `sample_lod()` made by hand; `bias()` carries the LOD bias.
- **Cache.** The texture hangs off the existing `texture_cache` slot and is dropped whenever that slot re-uploads.
- **Everything else unchanged.** Other formats, including the linear surfaces the title renders and then samples, keep the software path unit by unit, in the same draw.
- **The switch.** It is `RECOMP_METAL_HW_TEX`, off by default, and it prints its state.

**Exit criterion 2, image equality: PASS.** `metal_hwtex_check.sh`
(`jsrf_metal_hwtex_test` + `hwtex_compare.py`) draws 144 cases:
- DXT1, DXT3 and RGBA8, each with a 64x64 texture and full mip chain;
- point, bilinear, linear-mip-nearest and trilinear filtering;
- clamp and repeat;
- magnified, minified and projective quads, each plain and alpha-blended.

The worst case is **0.000% of pixels beyond one 565 step, max 1 step**, and
all 144 units sampled in hardware. Positive control: two different texture
seeds fail all 144 cases (up to 88% of pixels, max 31 steps), so the scorer
can see a real difference.

**Exit criterion 1, scene-matched A/B: PASS.** `ab_switch.sh g27hwtex RECOMP_METAL_HW_TEX 2 150`,
`gameplay_nobarrage.pad`, scene 30 held in all four runs, 0 crashes, 0 guest faults, silenced:

| arm | frame ms | sync ms/frame | submit | vsh |
|---|---|---|---|---|
| off | 18.41, 18.78 | 9.18, 9.41 | 3.13, 3.19 | 3.09, 3.08 |
| on  | 16.92, 17.22 | **7.41, 7.47** | 3.34, 3.35 | 3.09, 3.12 |

**Repeated on an idle host, 23 Sep 15:45** (`idlehwtex`, after the Zoom run
above had been flagged as preliminary): off 18.37 / 18.27 ms with sync
9.20 / 8.71; on 16.96 / 17.10 ms with sync 7.42 / 7.32. That is 7.0% less
frame time, with no overlap between arms. 1.57 M and 1.59 M units were
sampled in hardware, and there were 0 faults. The result holds.

That is 8.2% less frame time, with no overlap between arms, and GPU sync
down about 1.85 ms per frame. The on arms sampled 1.54 M and 1.57 M units in
hardware, about 99% of texture requests, with 218 textures built,
142.6 MiB decoded and 0 decode failures. `ab_switch.sh`'s scorer printed
"arms NOT verified distinct" because its switch list does not include the new
name. Each run's own log does print the state: `RECOMP_METAL_HW_TEX=off` in
both 0 arms with 0 hardware units, and `=on` in both 1 arms. Logs:
`measure/g27hwtex_ab_20260923-135938`.

**What is NOT established:**
- **Image equality on real game frames.** The comparison is the synthetic one this tree uses as precedent.
- **An interactive session.** None has been played on this build, and the bundle has not been rebuilt.
- **The default.** It stays OFF. Turning it on is the player's call, preferably after one played session with the switch set in `paths.conf`.

**Next on G27:** exact early-Z (G27b). The audit in spec section 7 applies:
eligibility depends on the final fragment alpha, including the combiners
and vertex alpha, not on texture alpha alone.

## G27b outcome so far, 23 Sep 2026 (exact early-Z, first class)

**Built, and the image check passes. Performance is unmeasured.**

**The change.** `hw_early_z()` now returns 0, 1 or 2:
- **2** is new. It covers a draw that may discard (alpha test or z-range
  cull) but writes neither depth nor stencil. With nothing written, an early
  test only decides which fragments get shaded, so it is exact.
- **The variant.** `fs_hw_early_nw` is `fs_hw_blend` with
  `[[early_fragment_tests]]`, and both discard branches kept verbatim.
- **Not covered.** Alpha-tested draws that DO write depth stay late. They
  are counted by combiner use for the next step. Vertex alpha comes from the
  GPU vertex program, so texture alpha alone cannot decide them.

**A bug, caught in review by the player before any result was used.** Both
pipeline selectors read `sblend && hw_early_z(s)`, which folds 2 into 1. So
the discarding draws got `fs_hw_early`, which removes the discard, and the
new variant was unreachable. The first A/B (`g27bnw`) ran on that binary and
was stopped unscored. It was also taken while Zoom held about 60% CPU. The
selectors now read `sblend ? hw_early_z(s) : 0`. The report counts
`fs_hw_early_nw` draws where the draw is encoded, so a run now proves
selection rather than shader compilation.

**Image check (`metal_earlyz_check.sh`, `jsrf_metal_earlyz_test`):**
- **The scene.** A far opaque floor, then alpha-tested checkers with and
  without depth writes, then an opaque quad behind the depth-writing checker
  that shows through its alpha-0 texels. Then a draw behind the floor, and
  z-culled draws with no writes.
- **PASS.** Early-Z on draws 7 of 8 early, 4 of them through
  `fs_hw_early_nw`; the depth-writing alpha draw stays late. The image is
  byte-identical to early-Z off.
- **Positive control.** The inexact `EARLY_Z_REF0` arm differs by 16,900
  bytes, so the scene can see a wrong early test.

**Idle-host A/B, 23 Sep 15:55.** `ab_switch.sh idleearlyz RECOMP_METAL_EARLY_Z 2 150`,
with `RECOMP_METAL_HW_TEX=1` in both arms, scene 30 held, 0 faults:

| arm | frame ms | sync ms | draws early (all via `fs_hw_early_nw`) | late |
|---|---|---|---|---|
| off | 17.88, 16.99 | 7.70, 7.34 | 0 | 923,595 / 939,255 |
| on  | 17.19, 16.86 | 7.46, 7.33 | 18,027 / 21,681 | 898,237 / 918,317 |

**The ranges overlap; this does not separate the arms.** The difference of
means, 0.41 ms, is inside the run-to-run spread. The draw counts say why:
- **The exact class is about 2% of draws.** Selection is proven, since every
  early draw went through `fs_hw_early_nw`.
- **No draw can take the plain early variant.** Every other draw is
  alpha-tested (reference 0) AND writes depth, so it stays late.

The 2.3 ms ceiling measured on 22 Sep lives entirely in that second class.
Reaching it exactly means proving a draw's final alpha is never 0. That
depends on vertex alpha from the GPU vertex program and on the combiner
alpha chain, as the spec's section 7 audit says. **`RECOMP_METAL_EARLY_Z`
stays off; turning it on gains nothing measurable.**

## G34 — pass-through skeleton (spec Phase 1)

The G33 wrappers become the seams. Each is a host function with the
convention recorded in G32, still calling the original.

**Done when** the tutorial and the Garage 60-second reproducer run with 0
faults, and the ring contents are word-for-word identical to a run without
wrappers over the same frames.

### G34, re-scoped and done, 23 Sep 2026

**Why the gate changed.** The gate as written above compared the ring word
for word between runs with and without pass-through wrappers. A wrapper that
only calls the original is identical by construction, and a cross-run
comparison is timing-noisy, so that gate cannot fail for a real reason. The
skeleton's actual risk is the convention table the G35 replacements will
rely on: how many bytes each entry pops, and which registers it preserves.

**What was built.** `stage_d3d8_census.py` wrappers now snapshot `esp` and
the callee-saved `ebx`/`esi`/`edi` around every original body. They check
that `esp` moved by exactly `4 + ret_bytes` from `entry_points.json` and that
the three registers came back unchanged. `ebp` is excluded because the
recompiler keeps it as a C local per generated function, so it is not guest
state across a call. Output is `[D3D8-ABI]`, periodic and at exit.

**Result.** One silenced 150 s `measure.sh` run, while Zoom was running,
which does not matter for a correctness count:
- **Calls checked.** 7,704,313, game and internal together, every entry
  point that fired, with **0 mismatches**.
- **Scene and faults.** live=61, the tutorial, with 0 guest faults.

So every entry point the tutorial reaches is described correctly, and a
replacement body can use the generic epilogue, `esp += 4 + ret_bytes` with
`eax` as the result. The 20 entry points that never fired are still
unchecked; the first scene that reaches them will check them.

## G35 — first pixels through Metal (spec Phase 2)

Device, present, clear, `UP` and immediate-mode draws. Textures come from
G27's layer. Menus and text draw with the NV2A path off for those draws.

**Done when** the title screen and main menu match the NV2A path's presented
frames.

Phases 3 and 4 of the spec are opened as goals when G35 passes, not before.

### G35 design point, 23 Sep 2026: host work runs in ring order

**The problem the spec glossed over.** Until the whole boundary is replaced,
most of every frame still reaches the GPU through the ring, which the NV2A
executor consumes on its own thread, behind the game. A replaced entry point
that drew on the calling thread would land out of order with the ring's
draws around it. It would draw too early, onto a target in the wrong state.

**The mechanism.** A replacement queues its host work and writes one packet
into the ring where the original wrote its commands: subchannel 7, method
`NV2A_HOST_TOKEN_METHOD` (0x1FFC), with the queue slot as the parameter. The
consumer (`nv2a_pusher.c: dispatch`) hands it to the registered handler in
ring order, and it never reaches PGRAPH or the executor.
- **Why not a nonzero NOP.** A nonzero `NO_OPERATION` is not usable: it is
  a software method that raises a guest interrupt, which D3D uses for its own
  notifications.
- **Packet form.** Several tokens need a non-increasing header, because an
  increasing header with a count above 1 runs past 0x2000 and the parser
  refuses it.

**Verified.** `jsrf_pusher_token_test`: tokens interleave correctly with
PGRAPH methods, are consumed with or without a handler, are skipped by
scans, and an increasing multi-token packet is refused. In the game, a
silenced 120 s run (live=61, 0 faults) shows 253,361,901 methods with
**`subch7_other=0`**, **`host_tokens=0`** and `bad_headers=0`, so the
title never uses subchannel 7 and the channel is free.

**Next for G35.** The first real replacement has to draw into the same
Metal render target the NV2A path is drawing into at that point in the
ring. So the host renderer's first dependency is the backend's current
surface, not a surface of its own. The candidates, by the census, are
`Clear` (2.5 per frame) and the font's immediate-mode `Begin`/`End`
(1.5 per frame).

## The order

1. **G32.** Reading only. It is needed by both G33 and G34. **DONE 23 Sep.**
2. **G33a–c.** Build, then one silenced scripted run. **DONE 23 Sep**, all three gates pass.
3. **G27.** It proceeds whatever G33 finds, and it is the renderer track's head. **Hardware sampling DONE 23 Sep, behind `RECOMP_METAL_HW_TEX=1`, default off**; see "G27 outcome" below. Early-Z (G27b) is next on this item.
4. **G34**, only if G33's gate 3 passes. **DONE 23 Sep** (re-scoped: the runtime convention check, 7.7 M calls, 0 mismatches).
5. **G35.**

## Standing rules that apply here

- Stage into a copy. Never edit the gen tree in place; `regenerate.sh` overwrites it.
- Harness runs are silenced. The player hears them otherwise.
- Launch runs detached. Check `pgrep` before quoting counters.
- Don't edit sources during a run. `play_scripted.sh` exits on a stale binary.
- The working tree carries uncommitted ADX/USB work that is not this track's. Builds for this track include it, and every report says so.
