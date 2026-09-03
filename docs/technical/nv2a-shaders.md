# NV2A Shader Translation

Xbox games do not ship HLSL. They configure the NV2A's fixed-function combiner
pipeline, or upload raw vertex-shader microcode. Both have to become something
D3D11 or OpenGL will accept, and both are translated at runtime and cached.

This document covers the two translators. For what is and is not implemented,
see [gap-analysis.md](gap-analysis.md); for the wider D3D8 layer, see
[d3d-translation.md](d3d-translation.md).

## Register combiners → HLSL pixel shaders

Xbox games do not use traditional pixel shaders. They configure the NV2A's
8-stage register combiner pipeline. Each stage performs independent RGB and
alpha math — multiply, dot product, MUX — over a register file of textures,
vertex colours and constants. A final combiner blends the result.

The game sets this up as a packed descriptor:

```c
SetPixelShader(0x00000103);   /* 3 stages, tex0 = 2D, tex1 = 2D */
```

At draw time that configuration is translated into an HLSL pixel shader:

```
Stage 0: r0.rgb = tex0 * diffuse
Stage 1: r0.rgb = r0 * tex1          (environment-map modulate)
Stage 2: r0.a   = tex0.a * diffuse.a
Final:   output = r0
```

A 128-entry shader cache keys on the combiner configuration, so each unique
setup is compiled once and reused. Multi-texturing is covered outright.

Bump and environment mapping are **partial**: the combiner side is there, but
texture-coordinate generation (`TEXCOORDINDEX` with the camera-space modes) is
not, so effects that depend on generated coordinates will not look right yet.

## Vertex-shader microcode → HLSL vertex shaders

When a game uses programmable vertex shaders — water displacement, skeletal
animation, custom lighting — it uploads NV2A microcode rather than any
high-level source. Each instruction is 128 bits and carries a paired MAC and
ILU operation.

The translator parses that microcode and emits HLSL:

- **14 MAC ops** — `MOV`, `MUL`, `ADD`, `MAD`, `DP3`, `DP4`, `DPH`, `DST`,
  `MIN`, `MAX`, `SLT`, `SGE`, `ARL`
- **8 ILU ops** — `MOV`, `RCP`, `RCC`, `RSQ`, `EXP`, `LOG`, `LIT`
- **192 constant registers**, 12 temporaries, 16 vertex inputs
- Relative addressing through the address register (`A0`)
- A 64-entry compiled-shader cache

Because both halves of an instruction issue together, the translator has to
emit them so that the MAC and ILU results are written from the *pre-instruction*
register values — reading a register the paired op just wrote is the classic
way to get this subtly wrong.

## Where the code lives

| Piece | File |
|---|---|
| Combiner → HLSL | `src/d3d/d3d8_combiners.c`, `src/d3d/d3d8_combiners.h` |
| Vertex microcode → HLSL | `src/nv2a/nv2a_pgraph_d3d11.c` |
| Texture unswizzling | `src/d3d/d3d8_swizzle.h` |

`d3d8_combiners.h` and `d3d8_swizzle.h` cite xemu as a reference for the
hardware's behaviour; both are our own implementations. See [NOTICE](../../NOTICE).

## Captured JSRF program: decoder and CPU execution verified

On 2026-09-03 the old parser was found to read its opcodes from word 0 rather
than word 1, and to misdecode source selectors and destination routing.
The portable decoder now lives in `src/nv2a/nv2a_vsh.c`; the D3D11 entry point
in `src/d3d/d3d8_vsh.c` delegates to it. CPU execution and HLSL generation share
its representation, including separate temporary and output masks and paired
MAC/ILU operand reads.

The first five slots captured from JSRF, in upload order, are:

```
00000000 0020001B 0836106C 2F100FF8
00000000 0420061B 083613FC 5011F818
00000000 0400001B 083613FC 2070F82C
00000000 0240081B 1436186C 2F20F824
00000000 0060201B 2436106C 3070F800
```

They decode as:

```
MOV R1, v0
MOV oD0, v3          + RCP R1.w, R1.w
                      RCP oFog, v0.w
MUL R2, R1, c0       + MOV oD1, v4
ADD oPos, R2, c1
```

The complete 12-slot capture is in
`diagnostics/jsrf_first_fault/vsh_capture.h`. The remaining slots copy point
size, back-face colours, and texture coordinates. Constant indices here are
raw hardware indices into the 192-entry bank, without an SDK register bias.

**Correction to the earlier analysis:** there is no divide-by-four instruction
in this program. With captured c0=(1,1,16777215,1), c1=(0.53125,0.53125,0,0)
and input w=1, the three vertices become (0,0), (2560,0), and (0,1920).
NV2A programs output screen-space coordinates. An oversized triangle may
cover a 640x480 surface; clipping it to that surface is valid. Applying the
viewport again or dividing its coordinates by four would be incorrect.

The CPU pushbuffer executor now retains program and constant upload banks,
honours LOAD/START cursors, executes the program in mode 2, and takes position
and diffuse colour from its outputs. The method window can wrap during a long
upload while the load cursor continues advancing. Invalid or incomplete
programs are rejected. Primitive types now use `nv2a_regs.h`: 0x05 is triangles,
0x06 is triangle strip. The old private values were off by one.

Validation:

- Captured instructions, upper constant indices, source-C split bits, masks,
  paired reads, and R12/oPos alias: `jsrf_vsh_test`.
- Real method-sink uploads at a nonzero START, window wrap, constant updates,
  oversized clipping, invalid START rejection, and framebuffer pixel checks:
  `jsrf_vsh_render_test`.
- Optional `jsrf_vsh_reference_test`: the complete JSRF program agrees with an
  independent CPU interpreter on 128 varied input/constant sets. Set the CMake
  cache variable `JSRF_VSH_REFERENCE_DIR` to that project's `src` directory
  to build this test. No reference code is linked into the game runtime.
- Two isolated 25-second JSRF runs execute and rasterise the captured triangle.
  Completed-draw captures are uniformly white. They do not show a menu.

References consulted on 2026-09-03:

- [xemu shader field mapping and execution semantics](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-prog.c).
- [xemu transform upload handlers](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/pgraph.c).
- [Independent NV2A CPU interpreter](https://github.com/abaire/nv2a_vsh_cpu).

The subsequent captured-copy milestone adds linear RGB565 sampling,
perspective/projective texture interpolation and the measured texture-RGB /
vertex-alpha colour program in `nv2a_texture_copy.c`. The original white output
above is superseded by black, matching the captured source image. See
`CODEX_PROGRESS_2026-09-03_TEXTURE.md` for replay evidence and command-reader
corrections. General combiner programs, blending, depth and full fixed-function
rendering remain unsupported. Constant-writing
programs are decoded but rejected by the interpreter and HLSL generator.
Arithmetic uses host floats and is not hardware-bit-exact across all exceptional
values. The Windows D3D11 runtime was not exercised in this macOS validation;
its existing conversion of NV2A screen-space outputs to host clip space still
needs separate validation.
