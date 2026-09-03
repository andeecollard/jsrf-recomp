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

## A validation vector for the microcode parser

The vertex-microcode parser has never been checked against microcode a game
actually uploaded, and it does not survive the check. Captured from Jet Set
Radio Future at runtime with `RECOMP_PB_EXEC_PROGRAM=1`, first five instruction
slots in upload order:

```
00000000 0020001B 0836106C 2F100FF8
00000000 0420061B 083613FC 5011F818
00000000 0400001B 083613FC 2070F82C
00000000 0240081B 1436186C 2F20F824
00000000 0060201B 2436106C 3070F800
```

with the only constants the title uploads:

```
c0  (1, 1, 16777215, 1)          c60 (0, 0.5, 1, 2)
c1  (0.53125, 0.53125, 0, 0)     c61 (-1, 0, 1, 2)
                                 c62 (0, 0, -1, 0)
```

Against `src/d3d/d3d8_vsh.c` as it stands, every slot decodes to `MAC=NOP
ILU=NOP` — the first uploaded dword is zero in all of them, and that is where
the opcode fields are read from. Reversing the word order instead gives an ILU
opcode of 8 on slot 2, which does not exist, and constant indices `c128` and
`c143`, which the title never sets.

Note that `c0` is `(1, 1, ...)` and `c1` is `(0.53125, 0.53125, ...)`: the
scale is one and the offset is the half-pixel bias, which is the NV2A viewport
applied inside the shader. There is no scale constant anywhere in the program,
so nothing outside it explains the factor of four between the title's vertex
values and its 640x480 surface. That factor is inside these instructions.

Fix the layout until it disassembles this, and the CPU executor in
`src/kernel/nv2a_pb_exec.c` can run the program instead of rejecting every
batch as not screen-space.
