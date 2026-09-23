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

## Next goals, 23 Sep 2026 (evening): G36–G38

The player said "set goals keep pushing" after the idle-host A/Bs.

### G36 — hardware texture sampling becomes the default

`RECOMP_METAL_HW_TEX=1` is in the player's `paths.conf` as of 23 Sep, and
JSRF.app is rebuilt from the build that carries it. Checked with `strings`:
the switch is present, the positive control is present, the nonsense
control is absent. The previous `paths.conf` and `last-run.log` were saved
as `*.bak-20260923-hwtex`.

**Done when:** one played session with the switch on reports nothing wrong
with textures. Then the default flips in `nv2a_metal.m` (`hw_tex_on`), with
empty treated as the default under the switch audit's rule 2, in a commit
that quotes the session.

### G37 — the first D3D entry point drawn by the host (G35, step 2)

**Target: `D3DDevice_Clear`** (2.5 calls per frame, 6 stack arguments, and
self-contained).
- **The replacement.** It queues its arguments and writes one host token
  into the ring in place of the original's commands. At token time, on the
  executor thread and in ring order, the handler sets the clear rectangle
  and the colour and depth/stencil values the original's methods would have
  set, then runs the executor's own `clear_surface()`.
- **What must be read first.** The exact commands and device-field writes
  of the original, from its code (`0x193830`, 24-byte pop). Anything it
  changes that later draws depend on must be reproduced.

**Done when:**
1. the command list and state effects are written down from the disassembly;
2. an in-process differential test shows that the executor state after
   running the original's commands equals the state after the token
   handler, over the clear-state fields, for a table of argument cases;
3. a silenced tutorial run with the replacement on shows 0 faults, the same
   scene, and `host_tokens` equal to the `Clear` call count.

### G37 outcome, 23 Sep 2026: D3DDevice_Clear is computed by host code

**Done; all three gates pass.**

**1. What the original does**, read from its code (`0x193830`, 924 bytes):
- **Swizzled target.** When the render target's format-table bit 0 is set,
  it emits `SET_SURFACE_FORMAT` from helper `0x192C70` with bit 9 cleared
  and bit 8 set, and restores the saved word at the end. When there is no
  depth surface and nothing is left to clear, it returns without restoring.
- **Colour.** Packed to 555 or 565 by a table at `0x193BD8`.
- **Depth.** One of four conversions by depth format (`0x2A`–`0x31`, table
  at `0x193BF4`): D24, float-24 through `z*1e30`, D16, and float-16 through
  `z*511.9375`. The stencil is ORed in unmasked.
- **Rectangles.** Each is clipped to the device clip at `+0x9D0`, scaled by
  the anti-aliasing factors at `+0x454`/`+0x458` plus 0.5, and truncated.
- **Commands.** Per rectangle: `CLEAR_RECT_H/V`, then one packet of three,
  `ZSTENCIL_CLEAR_VALUE`, `COLOR_CLEAR_VALUE` and `CLEAR_SURFACE`=flags.

**The code.**
- **The transcription.** `diagnostics/jsrf_first_fault/d3d8_lift_clear.h`.
  It reads the format tables from guest memory and calls the pure helper
  through the generated code.
- **The staging option.** `stage_d3d8_census.py --lift-clear`, with
  `RECOMP_D3D8_LIFT_CLEAR=shadow|1`.
- **Delivery.** `src/nv2a/d3d8_host.c` is the queue, and
  `nv2a_pusher_dispatch_host` replays each queued command through the ring's
  own dispatch. `jsrf_d3d8_host_test` covers ordering, single replay and bad
  tokens.

**2. Differential (shadow mode, 150 s tutorial):**
- **22,000 calls.** 21,997 match command for command, and 1 matches after a
  reservation prefix. That prefix is the semaphore at `0x1D70` that D3D's
  reservation routine writes when the ring is full. The replacement calls
  the same routine under the same condition before its token, so the same
  words precede it.
- **0 mismatch.** 1 call was unverifiable, a ring wrap inside the window.

**3. Replace mode, 150 s tutorial.**
- **Replacements.** 19,999 clears replaced by host tokens; 19,999 enqueued
  and 19,999 replayed, carrying 99,995 commands. 0 bad tokens, 0 queue-full
  fallbacks, 0 unsupported.
- **Checks.** The G34 convention check ran around the replacement: 7,440,877
  calls, 0 mismatches. The run reached live=61 with 0 guest faults and
  `subch7_other=0`.

**What it is and is not.** This is the first D3D entry point whose NV2A
commands are computed by host C rather than guest D3D, delivered in ring
order. The replay still runs through the NV2A executor. The next step for
Clear is to call the Metal backend's clear directly from the token handler,
which removes the executor from this entry point.

### G38 — can the depth-writing alpha-tested draws ever discard?

The 2.3 ms early-Z ceiling is entirely in draws that are alpha-tested with
reference 0 AND write depth (G27b above). A fragment of such a draw is
discarded only if its final alpha rounds to 0.

**First, measure; do not build.** Classify those draws per frame by where
their final alpha comes from:
- no combiners: diffuse alpha, optionally times texture alpha;
- combiners: the alpha output chain.

For the diffuse alpha, which the guest vertex program writes, read the
program: is `oD0.w` a constant register, an input attribute, or computed?

**Done when** a table says how many of those draws have an alpha source
provably above 0. That decides whether an exact early-Z for them is days of
work or impossible.

### G38 first answer, 23 Sep 2026: the final alpha is always a product

**Every late, alpha-tested, depth-writing draw uses the combiners.** The
G27b counters show 0 without them in both idle-host early-Z arms. So a
fragment's fate is decided by the combiner alpha chain.

`RECOMP_METAL_ALPHA_CENSUS=1` (read-only) counts those draws per distinct
alpha configuration at the draw site. One silenced 150 s tutorial run with
`RECOMP_METAL_HW_TEX=1` (live=61, 0 faults): **651,625 draws, only 10
configurations.**

How to read the words: in each input byte, bits 0–3 are the register
(4 = diffuse, 8–11 = textures 0–3, 12 = spare0, 0 = zero), bit 4 selects
alpha, and bits 5–7 are the mapping (1 = invert, so `0x20` is the constant
1). In the output word, bits 8–11 are the sum destination and bits 4–7 the
AB destination.

| # | share | final alpha (spare0.a) |
|---|---:|---|
| 0 | 55.6% | tex0.a × diffuse.a × tex1.a |
| 1 | 13.3% | diffuse.a × tex0.a (stages 1/3 compute c0.a × tex2/3.a into spare1, which the final alpha never reads) |
| 2, 4, 5 | 18.3% | tex0.a × diffuse.a (4 and 5 add a `0 + 1 × spare0.a` pass-through) |
| 3, 6, 7, 9 | 12.4% | diffuse.a |
| 8 | 0.3% | tex1.a (stage 1 overwrites stage 0) |

**Consequence.** Every factor is at least 0 and at most 1. Filtering and
vertex interpolation only average, so a fragment's alpha is at least the
product of each factor's minimum over the draw. The shader discards at
reference 0 when alpha < 1/510. **A draw is exactly early-Z-safe when
∏ min(factor) ≥ 1/255**, with a margin for the float product.
- **Texture factors are cheap.** Record the minimum alpha of every level
  when `nv2a_texture_decode_rgba8` decodes a texture, on the path G27
  already runs.
- **Diffuse is the open factor.** The guest vertex program writes `oD0.w`
  on the GPU.

**Next measurement (G38b).** For the vertex programs these draws run, where
does `oD0.w` come from: a constant register, a vertex attribute (readable
on the CPU when the draw is prepared), or a computed value? If the first
two cover most of these draws, the exact early-Z is days of work. If
lighting computes it, it is not reachable this way.

### G38b answer, 23 Sep 2026: diffuse alpha is a constant in 122 of 126 programs

`experiments/d3d8_boundary/vsh_od0w.c` parses every program in the
title's corpus (`vsh_xbe_corpus.h`) with the production decoder. It finds
the last slot that writes `oD0` with the w bit, and traces the w lane back
through MOV/MUL/MAD/ADD/MIN/MAX and temporaries to what it bottoms out in:

| oD0.w source | programs |
|---|---:|
| constant registers only | 79 |
| never written (the output's default) | 43 |
| input register, possibly times constants | 3 |
| computed (a dot product, an ILU op, or a0-relative) | 1 |

**So for 122 of 126 programs, diffuse alpha is known on the CPU at draw
time**, from the constant file the draw already uploads. For three more it
is vertex data the CPU prepares.

**The exact early-Z this makes buildable (G38c):**
1. **Texture minimum.** The minimum alpha of every level, recorded when G27
   decodes a texture. Formats outside G27 count as [0, 1].
2. **Diffuse range.** Evaluate each program's traced `oD0.w` expression with
   the draw's constants. Programs that never write it take the output
   default. Input and computed ones count as [0, 1] for now.
3. **The combiner chain over ranges.** Run the draw's combiner alpha program
   (`shade()`'s input mappings, products, sums, mux, output mapping and
   clamps) over [lo, hi] ranges instead of values. The inputs are zero, the
   constant alphas, diffuse and specular, texture ranges, and the spare0 and
   spare1 initial values.
4. **The rule.** Early-Z (`fs_hw_early`: no discard can happen) when the
   final alpha's lower bound is at least 1/255. The discard threshold is
   1/510.

**Done when:** the early-Z image check extends to these draws and stays
byte-identical; the positive control still differs; and an idle-host A/B
scores `sync`.

### G38c outcome, 23 Sep 2026: exact, audited, and worth about 0.4 ms of GPU time

**Built** in `nv2a_metal.m`, behind `RECOMP_METAL_EARLY_Z_EXACT=1|audit|audit-control`
(value-carrying, so it is listed in `switch_audit.py`). It needs
`RECOMP_METAL_EARLY_Z=1` and `RECOMP_METAL_HW_TEX=1`.
- **Texture ranges.** Each hardware texture's alpha range is recorded over
  every uploaded level at decode.
- **Vertex programs.** Each program's `oD0.w`/`oD1.w` becomes a small
  expression tree over the constant file (`vsh_out_alpha`, saturated as the
  emitter does). The default output alpha is 1, and anything opaque is
  unknown.
- **The combiner chain.** `comb_alpha_floor` runs `shade()`'s alpha chain
  over ranges: the input mappings, products, sum and mux, output mapping,
  clamps, and the colour half's blue-to-alpha writes.
- **The rule.** A draw is proven when its floor is at least 1/255 and its z
  cull cannot fire. JSRF sets CULL with the full range [0, 16777215] on every
  draw, so the z cull is only a risk if a fragment's z leaves [0, 1].

**Audit, which proves the proof.** `audit` keeps proven draws late and has
the shader count, in a device atomic, any alpha-test or z-cull discard of a
proven draw.
- **Audit arm.** 150 s tutorial: 346,642 proven predicate calls, **0
  discards**.
- **Positive control.** `audit-control` marks every alpha-tested draw:
  **1,274,067,809 discards**. The counter sees real discards, so the 0 is
  evidence.

**Idle-host A/B** (`ab_switch.sh ezexact`, 2 x 150 s, early-Z and hardware
textures on in both arms, scene 30 held, 0 faults):

| arm | frame ms | sync ms | draws early / late |
|---|---|---|---|
| off | 17.37, 18.22 | 8.34, 8.75 | 18,846 / 804,870 and 17,090 / 837,506 |
| on  | 17.60, 17.61 | 8.12, 8.15 | 335,184 / 560,917 and 335,217 / 559,986 |

**The ranges overlap on frame time.** Sync is lower in both on runs, by
about 0.4 ms. About 37% of draws move early. The rest of the 2.3 ms ceiling
sits in draws that genuinely CAN discard, because their textures hold
alpha-0 texels. No exact rule moves those; only a depth pre-pass or an
approximation would. **`RECOMP_METAL_EARLY_Z_EXACT` stays off** until a
longer A/B separates it. At n=2 the saving is inside the spread.

### Order

1. **G36.** It waits on the player and costs nothing meanwhile.
2. **G37.** The lift track's next step.
3. **G38.** It measures before any build.

## G39 — the host knows each draw from D3D alone (first slice: textures), 23 Sep 2026

The replay path of G37 proves the mechanism. A renderer fed at the D3D boundary
needs more than that: the state of every draw, taken from D3D rather than from
the NV2A commands it emits. G39 builds that host mirror and checks it against
the executor draw by draw.

**The mechanism.**
- **The staging option.** `stage_d3d8_census.py --mirror` wraps `SetTexture`
  (0x18DF10) to record `(stage, texture)`, and `DrawIndexedVertices` and
  `DrawVertices` to snapshot the bound textures' Data/Format/Size after the
  original runs (`d3d8_mirror.c`, `RECOMP_D3D8_MIRROR=1`).
- **The check token.** Each snapshot rides a host token written behind the
  draw's commands, so it reaches the executor just after the draw it
  describes. There, `d3d8_host.c` compares it with the executor's
  reconstruction (`nv2a_pb_exec_last_draw_textures`: address, NV2A format
  byte, width, height and levels for every unit in use).

**Result.** 150 s silenced tutorial, census convention check on:
- **Draws.** 599,961 checked; the executor was active for all of them.
- **Texture units.** 976,530 compared, **976,526 match** (99.9996%). Missing
  in D3D 0, format or shape 0, **address 4**.
- **Checks and faults.** The convention check ran 7,721,721 calls with 0
  mismatches. live=61, 0 faults.

**The four mismatches** are one texture object, `0x0436C410`, on four draws
(138049, 138051, 138631, 138634). D3D's object reads 256x256 X1R5G5B5 at
`0xF93000`, but the executor drew a 64x64 DXT1 at `0xF92000`. Either that
stage was rebound by a path that is not `SetTexture`, or the object was
changed after the draw. Open; it names the next thing to hook.

**Next for G39:** the same check for the render target and depth surface,
the viewport, the blend, depth and alpha state from D3D's render-state array,
and the vertex and pixel shader handles. Each one checked is a piece of the
draw a host renderer can take from D3D.

### G39, second slice: render target and depth surface, 23 Sep 2026

At each draw the mirror now also reads D3D's current colour and depth surfaces
straight from the device (`+0x2070`, `+0x2074`), with their Data and the pitch
packed in Size (`(Size >> 24) + 1` units of 64 bytes). They are compared with
the executor's target and depth for the same draw: guest address and pitch.
Depth is compared where the executor used it, meaning a depth or stencil test.

**150 s silenced tutorial:**
- **Draws.** 579,985 checked.
- **Colour.** 579,985 compared, **579,985 match**: 0 address, 0 pitch.
- **Depth.** 579,985 compared, **579,985 match**: 0 missing, 0 address, 0 pitch.
- **Textures.** The same 4 address mismatches as before, still open.
- **Checks and faults.** The convention check ran 7,404,350 calls with 0
  mismatches. live=61, 0 faults.

**Positive control.** `RECOMP_D3D8_MIRROR_CONTROL=1` swaps the colour and
depth addresses in the snapshot. Over a 60 s run: 259,987 draws, **0 colour
matches and 0 depth matches**, all of them address mismatches. The check
can see a wrong surface, so the full match is evidence.

**What this means.** A host renderer can take every draw's render target
and depth buffer from D3D's device, with no reference to the NV2A surface
commands. Next in G39: viewport and blend/depth/alpha state, then the
shader handles.

### G39, third slice: viewport and blend/alpha/depth/stencil state, 23 Sep 2026

**Viewport.** D3D keeps it in the device at `+0x9D0..+0x9E4` (X, Y, W, H,
MinZ, MaxZ). `SetViewport` turns it into SET_VIEWPORT_OFFSET/SCALE, SET_CLIP_MIN/MAX
and, through `SetScissors`, the window clip. The mirror scales the rectangle
by the supersample factors (`+0x454/+0x458`), cuts it to the surface clip and
compares it with the executor's effective scissor. It also compares
MinZ/MaxZ x 16777215 with the executor's z range.

**State.** Eleven registers: alpha test enable/func/ref; blend
enable/sfactor/dfactor/equation; depth test enable/func/mask; stencil test
enable.
- **Nine** reach the GPU through `SetRenderState_Simple`, whose wrapper now
  records each method's last value (`--mirror` hooks 0x18E930).
- **Depth and stencil enable** go through their own setters instead,
  `SetRenderState_ZEnable` (0x18F6C0) and `_StencilEnable` (0x18F760). The
  first run showed them "never pushed by D3D", so those two are now hooked
  as well. The mirror holds "enabled or not".

**150 s silenced tutorial:** 579,984 draws.
- **Viewport:** **579,984 match**, 0 window and 0 z-range mismatches.
- **State:** all 11 registers **579,984/579,984**, with none left unpushed.
- **Surfaces:** still 579,984/579,984.
- **Textures:** the same 4 address mismatches as before.
- **Checks and faults.** The convention check ran 7,433,475 calls with 0
  mismatches. live=61, 0 faults.

**Positive control** (`RECOMP_D3D8_MIRROR_CONTROL=1` shifts viewport X by 1
and MinZ by 0.5, and flips every state value). 60 s, 259,960 draws: **0
matches** for the surfaces, the viewport and every state register. The
window test fires before the z-range test, so the z-range comparison's own
sensitivity is not separately shown.

**Where G39 stands.** From D3D alone, the host now knows each draw's
textures, render target, depth buffer, viewport and scissor, and its
blend/alpha/depth/stencil state, and all of it agrees with what the
executor drew. Still to check: the vertex and pixel shaders (programs,
constants and combiner setup), vertex streams and index data, and the
texture-stage states (filter and wrap). That is everything else a Metal
renderer fed at the D3D boundary needs.

### G39, fourth slice: vertex shader constants, 23 Sep 2026

`SetVertexShaderConstant(Register, pData, Count)` (0x1905F0, about 220 calls per
frame) is hooked. The mirror copies what D3D was handed into a host constant
file, at NV2A slot = Register + 96. At each draw, every slot D3D has written
is compared bit for bit with the executor's `s_vsh.constants`.

**150 s silenced tutorial:**
- **Slots.** 52,480,084 compared, **52,480,084 match**.
- **Draws.** All written slots matched in **559,967 of 559,967** draws.
- **Viewport.** Still 559,967/559,967.
- **Faults.** 0, live=61.

**Positive control** (+1.0 on every constant): 23,924,519 slots compared and
**0 match**. The only draws counted as "all matching" are the 5,431 before
any constant was written.

So the constant numbering (D3D register + 96) is confirmed, and every
constant a draw reads through D3D's API reaches the GPU unchanged. Next: the
pixel shader. In XDK 4134 its combiner setup lives in D3D's render-state
array, which the engine writes inline, so the array's layout comes first.

### G39, fifth slice: programmable pixel shaders, 23 Sep 2026

`SetPixelShader(handle)` (0x199BE0) takes the 57-word definition from
`handle+8` and emits it directly, and the code shows the register-to-word
mapping:
- words 0–7 go to COMBINER_ALPHA_ICW;
- words 10–41 go to 0x0A60–0x0ADC (FACTOR0, FACTOR1, ALPHA_OCW, COLOR_ICW);
- word 42 goes to 0x17F8, and words 43–44 to 0x1E20/24;
- words 45–53 go to COLOR_OCW and COMBINER_CONTROL, and words 55–56 to
  0x1E74/78;
- words 8–9 go to 0x288/0x28C when the final combiner is used.
It also copies the 57 words into `D3D_g_RenderState[0..56]`.

`SetPixelShaderConstant` (0x199DB0) packs each float4 to a colour. For every
stage whose constant-mapping nibble (definition `+0xE4/+0xE8/+0xEC`) names
that register, it calls `SetRenderStateNotInline(10+i / 18+i / 43+i)`, which
rewrites the stage's FACTOR register and the same render-state entry.

**The first check read the definition and failed on exactly one
register.** 0x0A6C (word 13) differed on all 65,339 programmable draws:
d3d 1, executor 0. The word is a placeholder that the constant mapping
overwrites. The mirror now reads `D3D_g_RenderState[0..56]`, which is D3D's
own current view of those registers, with the constant writes applied.

**150 s silenced tutorial:**
- **Programmable draws.** 559,967 draws in all, of which **66,054** used a
  programmable pixel shader. All 54 registers matched on every one of them:
  **3,566,916 of 3,566,916 words**.
- **Faults.** 0, live=61.

**Positive control** (every word flipped): 1,283,310 words and **0 match**.

**The finding that sets the next goal: 493,913 of 559,967 draws (88%) use
fixed-function combiners.** D3D builds their combiner registers at draw
time from the texture-stage states (0x197F90, 1,313 bytes). No host
mirror covers that yet, and it is the largest piece a D3D-fed renderer
still needs.

**G39 still open:**
- fixed-function combiners and texture-stage states (88% of draws);
- the vertex program's identity;
- vertex streams and index data;
- the 4 texture address mismatches (one object).

### G39, sixth slice: texture-stage states, 23 Sep 2026

D3D keeps deferred texture-stage state at `0x19DEE0`: 4 stages x 32 words.
A discovery pass paired each stage's first 16 words with the unit's NV2A
registers. Six distinct pairings in 120 s were enough to read the mapping
off:

| D3D stage word | NV2A |
|---|---|
| 0, 1, 2 ADDRESSU/V/W (1 wrap, 3 clamp) | TEXTURE_ADDRESS bytes 0/1/2, same values |
| 3 MAGFILTER (1 point, 2 linear) | TEXTURE_FILTER bits 24–27 |
| 4 MINFILTER, 5 MIPFILTER (0 none, 1 point, 2 linear) | TEXTURE_FILTER bits 16–23 = MIN + 2 x MIP |
| 6 MIPMAPLODBIAS (float) | TEXTURE_FILTER bits 0–12 = bias x 256, truncated (−0.8 -> 0x1F34) |

**150 s silenced tutorial:** **947,042 of 947,042** texture units match on
address, mag filter, min/mip filter and LOD bias. 0 faults, live=61.

**Positive control** (address word flipped): 414,160 units, **0 match**.
The control flips only the address, which is tested first, so the
sensitivity of the filter and bias tests is not separately shown.

**G39 still open:**
- fixed-function combiner generation from texture-stage states, words 12
  and up (COLOROP/ARG and so on), 88% of draws;
- the vertex program's identity;
- vertex streams and index data;
- the 4 texture address mismatches (one object).

### G39, seventh slice: vertex program identity, 23 Sep 2026

`SetVertexShader(handle)` (0x190490) keeps the object at device `+0x380` and
the handle at `+0x384`.
- **Handles.** An even handle is a fixed-function vertex format (the object
  is D3D's built-in, at 0x19DDA8). An odd one is object + 1.
- **Programmable objects.** Every programmable object this title binds
  carries flag `0x10`. That sends SetVertexShader to `LoadVertexShader(h, 0)`
  (0x190160), which copies a ready-made fragment from `object+0x114`
  (`object+0xC` dwords of SET_TRANSFORM_PROGRAM packets) into the ring, at
  slot 0. `SelectVertexShader` (0x1901C0) then selects program mode.

The mirror extracts the program words from that fragment at each draw and
compares them with the executor's program memory from slot 0. The first
attempt classified by flag `0x2` and counted every draw as fixed-function.
A handle census showed flag `0x10`.

**150 s silenced tutorial:**
- **Programmable draws.** **169,313**, all **identical** (20,729,976
  program words).
- **Fixed-function vertex draws.** **390,634** (70%).
- **Faults.** 0, live=61.

**Positive control** (first word flipped): 92,511 programmable draws, **0
identical**.

**The fixed-function share is now the whole remaining gap.** 70% of draws
use D3D's fixed-function vertex path: transforms, lights, material and
texture transforms, which D3D turns into NV2A FF registers. 88% use
fixed-function combiners built from the texture-stage states. Everything
programmable, and every other piece of per-draw state checked so far, a
host renderer can take from D3D exactly.

**G39 still open:**
- fixed-function vertex: SetTransform (about 90 per frame),
  SetLight/LightEnable/SetMaterial, texture transforms;
- fixed-function combiners: COLOROP/ARG and the rest of the texture-stage
  states;
- vertex streams and index data;
- the 4 texture address mismatches.
