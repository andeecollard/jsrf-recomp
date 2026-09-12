# Six gates for putting JSRF's draws on the GPU under Windows — all verified

Date: 2026-09-12 (Europe/London)
Follows CLAUDE_PROGRESS_2026-09-12_D3D8_INTERCEPT_IS_THE_AUTHENTIC_PATH.md.

Nothing here is implemented yet. Everything here is **measured or read out of
the tree**, so the next session can start building instead of re-deriving.

## The conclusion first

Do NOT write a new renderer, and do NOT do full D3D8 API interception either.
Drive **upstream's existing D3D11 renderer components** from **the NV2A batch we
already parse**. That reuses the hardest part of upstream's work (the register
combiner -> HLSL compiler) without needing the guest's D3D8 device-state field
map, which full interception would require and which nobody has mapped for
JSRF.

## Gate 1 — the D3D11 device stands up under Wine, without the guest

    [D3D8-HLE] xbox_Direct3DCreate8 -> 000000014428A060
    [D3D8-HLE] CreateDevice -> hr=0x00000000 dev=000000014428A020

RECOMP_D3D8_PROBE=1, CrossOver bottle recomp-gate, no fault. Wine routes D3D11
through Vulkan/MoltenVK, which is present (111 VK extensions enumerated).
Accessors are already exported in `src/d3d/d3d8_internal.h`:
`d3d8_GetD3D11Device`, `d3d8_GetD3D11Context`, `d3d8_GetDefaultRTV`,
`d3d8_GetSwapChain`, `d3d8_GetBackbufferWidth/Height`.

## Gate 2 — the draw path is three guest functions

Walking up from the NV097 methods that dominate the push buffer:

| guest function | emits | Xbox D3D8 API | external callers |
|---|---|---|---|
| `sub_001993A0` | 0x1800 ARRAY_ELEMENT16 | D3DDevice_DrawIndexedVertices | 4 |
| `sub_00199300` | 0x1810 DRAW_ARRAYS | D3DDevice_DrawVertices | 12 |
| `sub_00199060` | 0x1818 INLINE_ARRAY | D3DDevice_DrawVerticesUP | 6 |

ARRAY_ELEMENT16 is 21.4M of the unhandled method count, so `sub_001993A0`
dominates. These are the interception points IF full API interception is ever
wanted; the design below does not need them.

## Gate 3 — the device context is already known

`sub_00199300` opens `mov ebx, dword ptr [0x19dce0]`, and main.c already calls
that global `JSRF_D3D_CHANNEL_PTR`. So 0x0019DCE0 is JSRF's D3D8 device pointer,
the analogue of Burnout 3's 0x35FB48 in
`docs/technical/d3d8ltcg-device-context.md`. Known fields so far: PUT at +0x30,
GET-pointer at +0x34.

## Gate 4 — the combiner compiler is reusable from state alone

    ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *);

It takes a state struct, not a device, and caches 128 compiled shaders. This is
the part that would otherwise have to be written from scratch, and it is done.

## Gate 5 — our parsed batch can feed it

`NV2ACombinerState` is decoded, not raw. The decoder that produces it is

    void d3d8_combiners_from_render_states(const DWORD *rs, NV2ACombinerState *);

and on Xbox the D3DRS_PS* render states ARE the raw combiner words. From
`src/d3d/d3d8_xbox.h`:

| render state | index |
|---|---|
| D3DRS_PSALPHAINPUTS0..7 | 200..207 |
| D3DRS_PSFINALCOMBINERINPUTSABCD | 208 |
| D3DRS_PSFINALCOMBINERINPUTSEFG | 209 |
| D3DRS_PSCOMBINERCOUNT | 234 |
| D3DRS_PSTEXTUREMODES | 251 |
| D3DRS_PSDOTMAPPING | 252 |
| D3DRS_PSINPUTTEXTURE | 253 |

`NV2ATextureCopy` — the batch state the CPU rasteriser and the Metal path both
already consume — carries `combiner_count`, `color_icw[8]`, `alpha_icw[8]`,
`color_ocw[8]`, `alpha_ocw[8]`, `texture_mask`. So the bridge is: fill a DWORD
`rs[256]`, place those words at the indices above, call
`d3d8_combiners_from_render_states`, then `d3d8_combiners_get_shader`.

The RGB input/output indices were not read out; find them beside the alpha ones
in d3d8_xbox.h before writing the bridge.

## Gate 6 — no vertex shader translation is needed

`docs/technical/nv2a-shaders.md`, on JSRF's own captured program:

> NV2A programs output screen-space coordinates ... the three vertices become
> (0,0), (2560,0), and (0,1920).

The CPU executor already runs the vertex program; `s_outputs[]` holds
transformed screen-space vertices with colours and texcoords. A passthrough
vertex shader mapping screen space to clip space is enough.

## The shape to build

Mirror `nv2a_metal_draw` exactly so the call site does not change:

    int nv2a_d3d11_draw(const NV2ATextureCopy *state,
                        const uint8_t *texture, size_t texture_size,
                        uint8_t *target, size_t target_size,
                        uint8_t *depth,  size_t depth_size,
                        const float (*vertices)[16][4],
                        unsigned count, unsigned primitive);
    int  nv2a_d3d11_sync(void);
    void nv2a_d3d11_invalidate(uint8_t *target);

Returning -1 falls back to the software rasteriser, which is how the Metal path
already behaves and is what keeps this safe to land incrementally. Call it from
the same site in `nv2a_pb_exec.c`, replacing the `#ifdef __APPLE__` with a
per-host choice.

Read back to guest RAM in `nv2a_d3d11_sync` at FLIP_STALL, not per batch — a
GPU->CPU copy per draw would be slower than the software rasteriser it replaces.

## Success criterion, already instrumented

`[RASTER] N batches + M triangles on the CPU; T triangles total` prints on both
hosts (commit cae4519). Working GPU submission shows CPU triangles falling while
the total holds. On macOS the same line reads `0 batches + 0 triangles on the
CPU` with 223,976 native Metal batches, which is the shape to aim for.

## And the honest caveat about what this fixes

Measured, not assumed: rasterisation is NOT the early bottleneck. Both hosts
draw the same ~8,000 triangles up to guest loop 1660 and Windows manages only
57-80 triangles/second there against ~9,200/s this rasteriser is capable of --
the ack loop finds nothing to consume on 251,454 of ~260,000 iterations. Early
slowness is guest execution: 16 loops/s against macOS's 41, which is translation
overhead and which no renderer change touches.

This work pays off from the intro animation onward, where macOS goes to
4,134,044 and then 13,088,425 triangles. At 9,200/s that is 450-1400 seconds,
and that is what "stuck on the anti-graffiti screen" actually is.
