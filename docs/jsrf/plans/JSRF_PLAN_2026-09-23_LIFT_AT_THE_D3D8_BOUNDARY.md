# JSRF plan — lift the graphics at the D3D8 boundary, 23 September 2026

**Status: feasibility spec; full replacement not built.** A separate,
switchable DrawVertices packet-emitter prototype now exists; see
`../progress/PROGRESS_2026-09-23_TARGETED_DRAW_LIFT_PROTOTYPE.md`. It retains
the existing renderer and does not complete a whole-boundary phase below.
Written at the player's
request after they judged the current renderer approach disappointing and
against the spirit of xboxrecomp. They asked for a spec of whether JSRF's
graphics can be lifted rather than emulated.

**Verdict: feasible, and the boundary is cleaner than anyone assumed.** JSRF
reaches its GPU through one narrow door. Every call into Direct3D comes from
the engine's own graphics layer or from a handful of init and vblank routines.
No direct game-code GPU-command writes were identified by the static census.
Pointer-derived access remains a runtime question. That makes a D3D8-level replacement
a bounded job: about 100 entry points plus D3D's global state arrays.

## 1. What was measured

All numbers come from the shipped gen's disassembly (`disasm/functions.json`,
`xrefs.json`), the XbSymbolDatabase dump for the retail XBE
(`~/jsrf-build/jsrf-xbsymbols.txt`), and the indirect-call feedback database.
Reproduce with:

    /usr/bin/python3 experiments/d3d8_boundary/census.py \
        --disasm ~/jsrf-build/jsrf-first-fault/disasm \
        --symbols ~/jsrf-build/jsrf-xbsymbols.txt \
        --decomp-symbols ../JSRF-Decompilation/ghidra/symboltable.tsv \
        --icall-sites tools/recomp/output/icall_sites.json

| measure | value |
|---|---:|
| XDK version (XBE library table, all seven libraries) | 4134 |
| D3D section: code range | `0x18CB40–0x19A037` |
| D3D section: functions | 265 |
| call sites from game code into D3D | 392 |
| distinct D3D functions game code calls | 103 |
| game functions that call D3D | 134 |
| of the 392, from the engine graphics layer (`0x14C000–0x156200`) | **339** |
| of the 392, from anywhere else | 53, in 7 functions (below) |
| game-side indirect call sites that land in D3D (icall DB, 1,379 sites) | **0** |
| game-code references to `D3D_g_pDevice` | **0** |
| game-code references to `MakeSpace` / `MakeRequestedSpace` | **0** |
| game-code mentions of the device object or pushbuffer pointers (raw listing search, `0x19A000–0x19DED3`) | **0** |
| game-code references to D3D global state | 195 from 41 functions |
| draws per flip, tutorial (G28 run) | ~68 |

**The 53 calls outside the graphics layer** are all in seven functions:

| function | what it does |
|---|---|
| `sub_001564B0` | sets the default render and texture-stage state (all 67 outside data references are here too) |
| `sub_001569A0`, `sub_00156A30`, `sub_00156AB0` | display modes, format checks, device creation and reset |
| `sub_0013B1C0`, `sub_0013B230` | `BlockUntilVerticalBlank` |
| `setVBlankCallback` (`0x15F9E0`) | registers the vblank callback |

**No precompiled pushbuffers, and no game code near the pushbuffer.**
XbSymbolDatabase finds no `RunPushBuffer`, `BeginPush` or `CreatePushBuffer`
in this XBE. In XDK 4134 the device is a global object at `0x19B200`
(`D3D_g_pDevice` holds that address). Its first two words are the pushbuffer
write and end pointers, which `SetRenderState_Simple` reads and advances
directly. A raw search of the whole `.text` listing finds **no address from
`0x19A000` to `0x19DED3`**, ruling out those literal references in the scanned
listing. This does not rule out pointers obtained indirectly or computed at
runtime. The 24 functions the NV2A
identifier flags as emitting GPU methods are all inside the D3D section.

Every D3D-region address game code does mention is state, not transport:
`0x19DED4`/`0x19DED8` (flags), `0x19DEE0–0x19DF34` (deferred texture
state), `0x19E090–0x19E0E0` (just below and at `D3D_g_RenderState`), and
`0x19E1C4–0x19E2A4` (render state, including the deferred block). One
cluster, `0x19E4BC–0x19E523`, is **DSOUND code**, not D3D state: the function
inventory assigns those addresses to DSOUND and the relevant cross-references
are calls. Its unaligned function starts are not unaligned state accesses.

**The engine graphics layer is Smilebit's MUSASHI `CMGameGL`.** The
decompilation names more than 60 of its methods: render state, textures,
lights, transforms, viewports, render targets, fonts and a matrix stack. Every
draw lives in it:

| D3D function | call sites | callers |
|---|---:|---|
| `DrawVertices` | 12 | twelve thin wrappers at `0x155B80–0x155E00` |
| `DrawIndexedVertices` | 4 | four wrappers at `0x155EF0–0x155FE0` |
| `DrawVerticesUP` / `DrawIndexedVerticesUP` | 5 / 1 | wrappers at `0x14D200–0x14D310` |
| `Begin` / `End` / `SetVertexData2f/4f` | 2 / 2 / 8 / 12 | `CMGameGLFont::draw` and `sub_00151330` |
| `SetRenderTarget` | 2 | `CMGameGL::setRenderTargetFromArray` |
| `Clear` / `Swap` | 1 / 4 | `CMGameGL::clear`, `swapDefault`, `swapFinish` |
| `CopyRects` | 1 | `sub_00152780` |

**The graphics layer also writes D3D's state directly.** The XDK's inline
setters store into D3D's global arrays and set dirty bits instead of calling
a function. In JSRF, 26 layer functions read and write the word at
`0x19DED8`, including `or [0x19DED8], 0x900`. That word sits just below
`D3D_g_DeferredTextureState` (`0x19DEE0`). `CDevice_SetStateVB` at `0x00196520`
reads, tests and clears its bits before emitting draw state. Its deferred
state-validation role is confirmed; full bit meanings still need mapping. Others write into
`D3D_g_RenderState` (`0x19E0E0`) and the deferred render-state block
(`0x19E228`). The original D3D flushes these lazily at draw time
(`CDevice_SetStateVB` / `SetStateUP`). A replacement must do the same: read
the arrays at draw time rather than expect every change to arrive as a call.

**Five called D3D functions have no XbSymbolDatabase name.** All are small:

| address | what the code shows |
|---|---|
| `0x18D660` | reads the CRTC raster position register (`GetRasterStatus`-like) |
| `0x18E5D0` | tail-jumps to `Get2DSurfaceDesc` (`GetLevelDesc`-like) |
| `0x199B60` | tail-jumps through an import thunk (allocator) |
| `0x199AE0` | returns `D3DERR_*` codes from a format table (a `Check*` query) |
| `0x199820` | returns 1 (`GetAdapterCount`-like) |

## 2a. Reconciling with CODEX_HANDOVER (x) and (y)

`CODEX_HANDOVER.txt` sections (x) and (y) concluded that JSRF "has no
D3DDevice_SetRenderState to override" and that 58 game `.text` functions
write command words inline. Both were re-checked on 23 Sep against gen
`52b6b3f8`.

**Render state is an ordinary D3D call.** Section (x) is right that JSRF
never calls a `D3DRS_*`-style setter. The graphics layer instead calls
`SetRenderState_Simple` (`0x18E930`, `ecx` = NV2A method, `edx` = value) at
61 sites. That is a D3D entry point, and replacing it captures all 17
render-state methods (x) lists. The replacement decodes NV2A method numbers
rather than `D3DRS` enums, which is a small table, not a blocker.

**The 58 inline writers do not survive inspection.** Running the
identifier's own `extract_immediates` and `decode_pushbuffer_methods` over
every `.text` function flags 60 functions:

| decoded methods | functions | what they are |
|---|---:|---|
| `SET_OBJECT` only (method 0) | 35 | false positives: any constant with a zero method field decodes as it |
| `NO_OPERATION` only | 6 | false positives, same cause |
| `SET_TEXTURE_OFFSET` only (`sub_000696B0`), `SET_OBJECT`+`WAIT_FOR_IDLE` (`sub_00118D70`) | 2 | outside the graphics layer. No D3D call, no device reference, no begin/end header |
| alpha, blend, depth, stencil, dither, shade | 17 | graphics-layer state wrappers (`0x1504D0–0x1564B0`) whose constants are the `ecx` argument to `SetRenderState_Simple` |

Game code contains **none** of the command headers a draw needs. The
begin/end header `0x000417FC` appears 12 times in the D3D listing and 0 times
in `.text`, and the same holds for the flip and texture-offset headers. With
no reservation calls and no literal references to the ring pointers either,
static evidence says geometry, surfaces, textures, combiners, transforms and
the flip are all emitted inside the D3D section. Section (y)'s path B count
included the false positives above.

**What stays open is pointer-derived access.** Game code could hold the
device address in an engine field and write through it. Nothing static seen
so far suggests this, but only a run can exclude it. Phase 0's store-PC
histogram is that test.

## 2. Which boundary

Two boundaries are possible. The D3D8 API is the recommended one.

**D3D8 API (recommended).** About 100 flat functions with XDK-documented
semantics, named by XbSymbolDatabase for XDK 4134 and for the second JSRF
build too, and the same boundary every other xboxrecomp port uses. The
replacement mechanism already exists: `jsrf_manual_overrides.c` is passed to
the recompiler as `--exclude-manual`, so a host function replaces a guest
function at every direct call. Game code never calls D3D indirectly, so
direct-call replacement covers every entry.

**`CMGameGL` engine layer (not recommended).** About 330 functions and 50 KB,
mostly unnamed, with engine-private structs. It would raise the level of
abstraction only slightly above an already clean API, at several times the
reverse-engineering cost. It stays useful as an instrument: counting calls at
the layer tells us which D3D paths each scene uses.

## 2b. Entry points and the flags word, described (G32, 23 Sep)

`experiments/d3d8_boundary/entry_points.json` describes all 103 entry points
from their code (`entry_points.py`), with names for the five the symbol dump
lacks (`inferred_names.json`). Findings:
- **Every return is consistent.** Each entry pops one fixed number of stack bytes on every path.
- **Exactly one entry point takes register arguments.** `SetRenderState_Simple` takes the method in `ecx` and the value in `edx`, and pops nothing. The other 102 take stack arguments only. Earlier flags on ten more were artifacts: `push ecx` used to reserve a stack slot, and byte writes to `cl`/`dl`.
- **Push counts agree at all but one site.** At `0x14DBCD` (`SetVertexShaderConstant`), the count argument `4` is pushed before an intervening call to `0x14DA30`, which returns with a plain `ret` and leaves it on the stack. So the site does pass three arguments.
- **The five inferred names.** `GetRasterStatus`, `D3DTexture_GetLevelDesc`, `D3D_FreeContiguousMemory` (a thunk to kernel ordinal 171, `MmFreeContiguousMemory`), `CheckDeviceMultiSampleType` and `GetAdapterCount`.

**The flags word `0x19DED8`** (`flags_map.py`), grouped by who sets each bit:

| bits | set by game code (`CMGameGL`) | set by D3D |
|---|---|---|
| `0x1 0x2 0x4 0x8` | `setTextureOps`, `sub_0014FF49` | `SetTextureState_TexCoordIndex` |
| `0x10 0x20 0x40` | none | `SetVertexShader`, `SelectVertexShader`, `SetStreamSource`, `SetStateUP` |
| `0x100` | `sub_00150FA0`–`sub_001510D0`, `sub_001564B0` | `SetRenderTarget`, `SetViewport` |
| `0x200` | `setLighting`, `sub_0014FD70`, `sub_001564B0` | `VertexBlend`, `NormalizeNormals`, projection/viewport update |
| `0x400` | `sub_00150130`, `sub_001564B0` | `SetShaderConstantMode` |
| `0x800` | `setTextureOps`, `sub_00150057`, `sub_00150FA0`–`sub_001510D0` | `SetPixelShader`, `SetTexture` |
| `0x1000` | `setAmbient`, `setLighting`, `setSpecularEnable`, others | `SetLight`, `LightEnable`, `SetMaterial`, `TwoSidedLighting` |
| `0x2000` | `setFogEnable`, `setSpecularEnable`, `sub_00150970` | none |
| `0x4000` | `setTextureOps`, `sub_0014FF49` | `SetPixelShader`, `SetTexture`, `PSTextureModes` |

`CDevice_SetStateVB` and `CDevice_SetStateUP` test every bit in
`0x1–0x4000` except `0x10–0x40`, plus higher bits nothing in the title sets.
The `0x10–0x40` group is set only inside D3D, and this scan does not find
where it is consumed. `sub_00150130` and `sub_00150280` clear large masks,
which look like resets. **Consequence for the replacement:** at every draw it
must re-read texture-stage state, render target and viewport, transforms,
shader constants, combiners, lights and fog from D3D's arrays whenever the
matching bit is set. Game code changes these without making any call.

## 3. What the replacement consists of

The recompiled game keeps running its own engine, including `CMGameGL`. Only
the D3D8 library is replaced by host code that drives Metal directly. The
NV2A model, the pushbuffer parser and the surface-inference machinery drop
out of the JSRF graphics path.

| group | entry points | notes |
|---|---:|---|
| Device and display | ~20 | create, reset, caps, modes, gamma, flicker filter, vblank wait and callback, raster status |
| Resources | ~20 | `Create*`, `D3DResource_Register`/`AddRef`/`Release`, `Lock*`, `GetDesc`, `GetSurfaceLevel`, `GetBackBuffer`, `GetDepthStencilSurface`, tiles |
| State | ~50 | ~30 `SetRenderState_*`, 5 `SetTextureState_*`, transforms, lights, material, viewport, shader constants, streams, indices, textures, shaders, render target |
| Draw | 7 | indexed and non-indexed, `UP` variants, immediate mode |
| Frame | 4 | `Clear`, `Swap`, `CopyRects`, `BlockUntilVerticalBlank` |
| Inline state | 3 arrays + flags | read at draw time, as D3D's own lazy flush does |

**Most of the hard translation already exists and is reused:**
- **Vertex shaders.** XDK vertex shaders are NV2A microcode. `nv2a_vsh_msl.c` already translates it to Metal.
- **Pixel shaders.** An XDK pixel shader definition holds register-combiner values directly. The combiner-to-Metal generator in `nv2a_metal.m` already consumes them.
- **Textures.** An XDK texture header carries the NV2A format word, size and data address. `nv2a_texture_decode.c` already decodes every format the game uses.

**What changes because the API names things:**
- **Textures upload once.** They are uploaded when a resource is created or registered, as native BC1/BC2/BC3 or de-swizzled RGBA8, and sampled by hardware. This is G27, built in the right place, with no per-draw memcmp (backlog item 5).
- **Render targets are objects.** `SetRenderTarget` names the colour and depth surfaces. The inference from addresses behind the sibling-slot depth clear, the empty composite texture and the surface-cache thrash has nothing left to infer (backlog item 6).
- **Presentation is direct.** `Swap` presents a Metal texture to the window, with no GL readback, snap or convert (backlog item 8).
- **Vertex data stays in place.** Stream sources point into guest RAM, which is mmap'd on unified memory, so Metal buffers can wrap it without copying (backlog item 3).
- **No command stream.** Nothing encodes or parses a pushbuffer. Per-draw CPU cost falls to state translation for about 68 draws per frame.

## 4. Risks, and how each is settled

| risk | how it is settled |
|---|---|
| **The CPU reads memory the GPU wrote,** for example a screenshot, save thumbnail or graffiti preview. The replacement never writes guest RAM. | The `Lock*` calls are the synchronisation points. Write the surface back on lock. Phase 0 counts locks on render targets. |
| **Game code writes vertex or texture memory without locking.** This is common on Xbox. | Vertex buffers wrap guest RAM, so no copy goes stale. Textures are loaded once from XPR bundles through `Register`. Any texture written later is found by a write-protect or hash check at `SetTexture` time, counted in Phase 0. |
| **Textures alias render targets,** which is how Xbox games render to texture. | `SetTexture` compares the texture's data address with the live surfaces. Aliasing becomes a lookup, not a guess. |
| **Inline state is missed.** A setter writes the arrays and no call is seen. | Read the arrays at every draw, as D3D does. Phase 3's differential mode catches any state that disagrees. |
| **Timing.** Game speed depends on vblank, `Swap` and the vblank callback. | `Swap` and `BlockUntilVerticalBlank` pace to the host display link. Scripted-replay frame counts must match the NV2A path. |
| **The xemu comparison is lost.** | Replaced by differential mode: the original D3D still exists, and the old path is kept as the reference (Phase 3). |
| **The second JSRF build (XDK 4134, 2002-01-15).** | Same XDK, so the same entry points. Rebind addresses from its own XbSymbolDatabase dump. |

## 5. Phases, gates and estimates

Estimates are working days. The player drives every run, and none of these
runs is scheduled yet.

**Phase 0 — census run (1 day and 1 run).** Replace every entry point with a
wrapper that counts calls and arguments, then calls the original. Behaviour is
unchanged. Record calls per frame, the locks on surfaces, the formats and
aliasing seen at `SetTexture`, and the dirty-flag bits observed at draws.
Also record the guest PC of every store into the pushbuffer ring. The
memory-watch PCs the generated code already carries make this a histogram,
not new instrumentation. This settles the one question the static census
cannot, pointer-derived writes (see section 2a).
*Gate:* tutorial draws per flip equal the G28 baseline within variance, a
census table exists, and **every pushbuffer store PC lies inside the D3D
section**. Any store from game code is listed by address, and the plan is
revised before Phase 1.

**Phase 1 — pass-through skeleton (2 days).** All ~100 wrappers are in place,
with calling conventions verified. Several XDK 4134 setters take register
arguments, so this is the first real risk. *Gate:* the tutorial and the
Garage 60-second reproducer run with 0 faults, and the pushbuffer is
identical word for word to a run without wrappers.

**Phase 2 — first pixels (3–4 days).** Device, present, clear, `UP` and
immediate-mode draws, with textures uploaded natively. Menus and text draw
through Metal with the NV2A path off. *Gate:* the title and main menu match
the NV2A path's presented frames.

**Phase 3 — 3D and differential mode (1–2 weeks).** Streams, indices, vertex
and pixel shaders, render targets, `CopyRects` and the composite. Differential
mode runs the original D3D into a shadow pushbuffer that the existing parser
decodes without drawing. Each draw's state from the replacement is compared
with the NV2A path's state, and the first disagreement is reported.
*Gate:* the tutorial and the Garage run with 0 disagreements on render
target, depth, blend, texture binding and shader.

**Phase 4 — parity and default (1 week).** Scene-matched A/B on an idle host,
presented-frame comparison, and one player session. *Gate:* frame time is
better than the NV2A path, the picture matches, and the game moves at the
correct speed. The switch then turns on by default for JSRF, and the NV2A path
stays buildable as the reference.

**Total: about 4–5 weeks to parity on the tutorial.** For comparison, the
ranked backlog items this absorbs (1, 3, 5, 6 and 8) are each sized "days"
on the NV2A path, a similar total, and they would leave the surface-inference
class of defect in place.

## 6. What this means for the goals list

If the player adopts this plan:
- **G27 continues; it is not superseded.** Both paths need native textures and hardware samplers, and both describe a texture by the same NV2A format word, size and address. Build G27's texture cache and sampler layer keyed by those three, not by pushbuffer state, so the replacement can call it unchanged. Retire the NV2A path's use of it only after the Phase 0 and Phase 1 gates pass (see section 7).
- **G27b carries over unchanged.** The alpha-test reference of 0 still blocks early-Z. The exact fix, skipping draws whose textures hold no zero-alpha texel, moves to texture upload.
- **The NV2A renderer is frozen, not deleted.** It is Phase 3's reference and the fallback for any title without a clean D3D boundary.
- **Upstream alignment.** Upstream's `src/d3d` layer is the same idea for Windows. It targets D3D11 through PC-style interfaces, so its state and format tables are reusable, but its entry-point glue is not. A Metal backend for it would be the contribution back.

Open before Phase 0, all answerable by reading code rather than running:
- Name the five unnamed entry points.
- Map the remaining bits of `0x19DED8`; its state-validation role is confirmed.
- Closed: `0x19E4BC–0x19E523` contains DSOUND functions, not graphics state.
- Record each entry point's calling convention. `SetRenderState_Simple` takes the method in `ecx` and the value in `edx`, so register arguments are confirmed for at least one.

## 7. Audit after the first interception experiment

The census reproduced 392 sites, 103 targets and 339 graphics-layer sites on
23 September. These are static coverage numbers. Since the audit, the script
also runs the raw `.text` listing search (0 mentions of `0x19A000–0x19DED3`,
0 begin/end headers) and, given `--icall-sites`, the indirect-call check (0 of
1,379 recorded sites), so those figures are reproducible. They remain static
evidence, not proof against pointer-derived access; Phase 0's store-PC
histogram is the runtime check.

The first native DrawVertices implementation passed 420 comparisons against
generated C and 30 against the original XBE routine under helper doubles.
Two GPU smoke arms used the old renderer and the opt-in native packet emitter;
the latter recorded at least 4096 calls. This supports entry-point replacement,
not tutorial image parity, a complete D3D lift or the 4–5 week estimate.

The replacement design must also settle these contracts:

- “Upload once” applies only to immutable resources. Detect writes to a
  texture that remains bound, not only writes followed by another SetTexture.
- Wrapping vertex memory avoids a copy but still needs a lifetime/ownership
  rule when the CPU changes bytes while submitted GPU work reads them.
- Object identities help surface tracking; overlapping allocations, reused
  addresses and texture/surface views still need explicit alias rules.
- A shadow path must not execute resource, timing or presentation side effects
  twice. Comparing draw state is useful but does not replace image comparison.
- Early-Z eligibility depends on final fragment alpha/discard behavior,
  including combiners and vertex/material inputs, not texture alpha alone.

G27 is not automatically superseded by this audit. Its hardware-sampling work
is shared work that the replacement still needs. Keep the old renderer usable
until the census and pass-through gates establish coverage and ordering.
