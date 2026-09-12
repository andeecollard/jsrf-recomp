# The authentic Windows renderer is D3D8 interception, and JSRF's draw path is three functions

Date: 2026-09-12 (Europe/London)

## The question this answers

We have been parsing JSRF's push buffer and software-rasterising it, with a
Metal fast path on macOS and nothing on Windows. That is not what xboxrecomp
does, and upstream says so directly in `docs/technical/gap-analysis.md`:

| Feature | xemu | xboxrecomp | Status | Priority |
|---|---|---|---|---|
| Push buffer parsing (PFIFO DMA pusher) | Full | **Stub** | **N/A** | **Low (D3D8 API intercept instead)** |

Upstream intercepts the **D3D8 API** and routes it into `xbox_d3d8`, which is a
complete D3D11 renderer that already ships in this tree:

  * register combiners -> HLSL, 8 stages, 128-entry cache  -- DONE
  * vertex microcode -> HLSL, `d3d8_vsh.c`, 64-entry cache -- DONE
  * texture unswizzling (Morton), 33 swizzled + 20 linear formats -- DONE
  * mipmaps, cube and volume textures, render targets -- DONE

So the renderer is not missing. The *connection* to it is.

## Why JSRF was not connected

JSRF statically links Xbox D3D8 into the XBE (`D3D` section, 0x0018CB40, 267
functions, 52 KB), so there is no import to hook. Upstream hit the same shape
with Burnout 3 -- see `docs/technical/d3d8ltcg-device-context.md` -- and the
repo already carries the mechanism:

> `--exclude-manual` ... the recompiler does not generate a body for anything
> defined [in jsrf_manual_overrides.c] and the direct calls in the generated
> code link to these instead. That is the project's sanctioned way to replace a
> guest function reached by a DIRECT call.
>
> "It is a substitution of the same kind the project already makes for **D3D**
> and the kernel: the guest's hardware driver is not the thing we want to run."

## Confirmed: the section really is Xbox D3D8

Not a RenderWare wrapper. `../JSRF-Decompilation/ghidra/symboltable.tsv` names
five symbols inside the section, and one of them is decisive:

    0x0018CE30  IDirect3DDevice8::SetVerticalBlankCallback

## The draw path is THREE functions, not 102

102 of the 267 D3D-section functions are called from outside the section -- the
whole API surface. But the *draw* path is far narrower. Walking up from the
functions that emit the NV097 methods which dominate the push buffer:

| function | emits | Xbox D3D8 API | external callers |
|---|---|---|---|
| `sub_001993A0` | 0x1800 ARRAY_ELEMENT16 | **D3DDevice_DrawIndexedVertices** | 4 |
| `sub_00199300` | 0x1810 DRAW_ARRAYS | **D3DDevice_DrawVertices** | 12 |
| `sub_00199060` | 0x1818 INLINE_ARRAY | **D3DDevice_DrawVerticesUP** | 6 |

ARRAY_ELEMENT16 is 21.4M of the 15M-method unhandled count, so
`sub_001993A0` is the one that matters most.

`sub_00199300` is identified from its body, not guessed:

    mov dword ptr [eax],   0x417FC     ; NV097_SET_BEGIN_END, count 4
    mov dword ptr [eax+4], ecx         ; arg1 = PrimitiveType
    add esi, 0x40001810                ; NV097_DRAW_ARRAYS, count ((n-1)>>8)+1

Three __stdcall arguments at esp+0x10/0x14/0x18 after its three pushes:
(PrimitiveType, StartVertex, VertexCount).

Three more entry points feed the same path at depth 3-4 and are the next tier:
`sub_00198F10` (4 callers), `sub_001991C0` (3), `sub_001996A0` (2),
`sub_0018E460` (1).

## Why this matters more than a D3D11 batch renderer

The alternative under consideration was a `nv2a_d3d11_draw` mirroring our
`nv2a_metal_draw`, fed from the prepared batch. That would be a THIRD renderer,
divergent from upstream, duplicating one that already exists and is complete.
Interception also fixes both hosts, because `xbox_d3d8` has an OpenGL backend
on POSIX -- so it is a candidate answer to the macOS animation roadblock, not
only the Windows one.

## Measured, so the priority is honest

Before assuming rasterisation is the bottleneck (it is not, early on):

| guest loop | macOS total triangles | Windows |
|---|---|---|
| ~1660 | 8,255 | 8,063 (at 1620) |
| 2878 | 4,134,044 | -- |
| 3258 | 13,088,425 | -- |

Up to loop ~1660 both hosts draw the SAME geometry and the CPU rasteriser keeps
up easily -- Windows does 57-80 triangles/second there against ~9,200/s this
rasteriser was separately measured at, and the ack loop finds nothing to
consume on 251,454 of ~260,000 iterations. The early slowness is guest
execution (16 loops/s vs macOS 41), which is translation overhead and which
NO renderer change will fix.

From the intro animation onward it inverts: four and then thirteen million
triangles. At 9,200/s that is 450-1400 seconds. That is the point where the GPU
path is the only answer, and it is what "stuck on the anti-graffiti screen"
actually is.

## Next step

Override `sub_001993A0` first -- highest method volume -- in
jsrf_manual_overrides.c, reading the same device state the function would have
read and calling `xbox_d3d8`'s draw. Keep the push-buffer path as the fallback
for anything not yet intercepted, exactly as the Metal path falls back to
software today. Verify with [RASTER]: CPU triangles should fall while the
picture stays correct.

NOT YET VERIFIED, and it gates the work: that `xbox_d3d8` can be brought up
without the D3D8 *device* the guest never creates through it. The device
accessors exist (`d3d8_GetD3D11Device`, `d3d8_GetD3D11Context`,
`d3d8_GetDefaultRTV`, `d3d8_GetSwapChain` in `src/d3d/d3d8_internal.h`) and
main.c already creates a device under RECOMP_D3D8_PROBE on Windows, so the
pieces are there -- but nothing has yet driven a JSRF draw through them.
