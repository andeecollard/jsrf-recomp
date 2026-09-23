# D3D's fixed-function combiner builder (G43, static part)

Guest function **0x00197F90** (D3D section, 0x521 bytes including its jump
table at 0x1984B4). XDK 4134. Host transcription:
`src/nv2a/d3d8_ff_combiner.{h,c}`; vectors:
`diagnostics/jsrf_first_fault/d3d8_ff_combiner_test.c` (ctest
`jsrf_d3d8_ff_combiner`). Read from the XBE with capstone under
`/usr/bin/python3`; nothing here was measured at runtime.

## Where it is called

Only from the lazy state flusher **0x001964A0**, when bit **0x800** of the
dirty-flags word 0x0019DED8 is set (`test bh, 8`). Its one stack argument is
the device (`[0x19DCE0]`); it returns with `ret 4`.

The device is the static object at **0x0019B200**: the code uses both
`[arg+8]` and the absolute `0x19B208` for the same flags word, and reads
`m_Textures[]` at the absolute **0x19BC78** (= device + 0xA78, matching
G40).

## What it reads

| guest | meaning |
|---|---|
| `device+0x370` | bound pixel shader. Nonzero: return at once, write nothing. |
| `device+8` bit 0x40 | "some combiner input reads D3DTA_SPECULAR". Saved, cleared on entry, re-set by the input helper. |
| `D3D_g_RenderState[108]` (0x19E290) | POINTSPRITEENABLE. Nonzero: start at stage 3 instead of 0. |
| `D3D_g_RenderState[93]` (0x19E254) | SPECULARENABLE. Gates the final SET_SPECULAR_ENABLE. |
| `0x19DEE0 + 0x80*s` words 12..20 | COLOROP, COLORARG0/1/2, ALPHAOP, ALPHAARG0/1/2, RESULTARG. |
| `m_Textures[s]` (0x19BC78 + 4s) | only whether it is NULL. |

Render-state indices are XDK **4134**: 4134 lacks the 4432+/4627+ states
(DEPTHCLIPCONTROL, STIPPLEENABLE, SIMPLE_UNUSED1..8, DEFERRED_UNUSED1..8), so
Cxbx-Reloaded's 5933-based numbers are 10 higher from FOGENABLE on, and 19
higher from PSTEXTUREMODES on. Two independent cross-checks: the fog updater
0x195610 reads index 82 (0x19E228) as its enable, and
SetRenderState_TextureFactor stores index 129 (0x19E2E4) = 5933's 148 - 19.

TSS words 12..20 match Cxbx's X_D3DTSS_* (COLOROP 12 ... RESULTARG 20;
TEXTURETRANSFORMFLAGS 21 is not read here). D3DTOP 1..26 index the jump table
directly (`jmp [op*4 + 0x1984B0]`).

**TEXTUREFACTOR is not read by this function.** It lives in the combiner
factor registers, written by `SetRenderState_TextureFactor` (0x18ECC0).

## What it writes

One contiguous run into the push buffer (`MakeSpace` 0x1916B0 if needed):

```
0x00041E60  count                 NV097_SET_COMBINER_CONTROL   (stage count only)
0x00200AC0  color_icw[0..7]       NV097_SET_COMBINER_COLOR_ICW
0x00201E40  color_ocw[0..7]       NV097_SET_COMBINER_COLOR_OCW
0x00200260  alpha_icw[0..7]       NV097_SET_COMBINER_ALPHA_ICW
0x00200AA0  alpha_ocw[0..7]       NV097_SET_COMBINER_ALPHA_OCW
[0x000403B8 (flags>>6)&1]         NV097_SET_SPECULAR_ENABLE, only if bit 0x40 of
                                  device+8 changed AND RenderState[93] == 0
```

The four arrays are built on the stack (color_icw at `esp+0x2C` after the
prologue, then +0x24, +0x48, +0x6C) and copied with one `rep movsd` of 0x24
dwords. Unused slots are zero. CONTROL is just the count, so FACTOR0/1 are in
"same for every stage" mode and the mux selects on LSB.

It does **not** write FACTOR0/1, SPECULAR_FOG_CW0/1, or any texture-shader
register. Those come from:
- **0x18ECC0** SetRenderState_TextureFactor: with no pixel shader, one packet
  `0x00400A60` + 16 copies of the factor (FACTOR0[0..7], FACTOR1[0..7]).
- **0x195610** the fog updater: CW0/CW1 at 0x195750 (fog on:
  `0x130C0300`, +0x20000 when SPECULARENABLE, i.e. lerp(fog.rgb, R0 or
  V1+R0, fog.a)) and 0x1957BC (fog off: `0xC`, or `0xE` with specular);
  CW1 is always `0x1C80`. Skipped when `device+0x370` and `device+0x374` are
  both nonzero. Only this tail of 0x195610 is transcribed
  (`d3d8_ff_final_combiner`); its fog-mode/density writes are not.

## Control flow

`ebx` carries the stage flags **F**: bits 0..1 the D3D stage, **0x10** the
first stage, and **0xE8** or'ed in for the alpha pass (0x20 = inputs read
`.a`, 0x48 = byte offset to the alpha arrays, 0x80 = no further alpha pass).

```
stage = PointSpriteEnable ? 3 : 0; F = stage | 0x10; op = COLOROP[stage]
loop:
  base = RESULTARG == TEMP(5) ? 0xD00 : 0xC00        (SUM_DST R1 / R0)
  colour pass: (op, ARG0..2) -> icw, ocw; write colour slot
  unless F & 0x80: F |= 0xE8; ALPHAOP/ALPHAARG0..2 -> alpha slot
  next stage; stop at 4 or when the next COLOROP == DISABLE; F = stage
zero-fill slots count..7
```

### The input helper 0x197EF0

`(edi = D3DTA arg, ecx = ctl, edx = F)` returns one input byte shifted into
its slot. `ctl` bits 16..19 pick the slot (3 = A, bits 24..31; 2 = B; 1 = C;
0 = D), 0x10 toggles the complement, 0x20 forces `.a`, 0x40 sets
EXPAND_NORMAL.

| `arg & 0xF` | register |
|---|---|
| 0 DIFFUSE | 0x4 V0 |
| 1 CURRENT | 0xC R0, but **0x4 V0 on the first stage** |
| 2 TEXTURE | 8 + (F & 3), or **0xFFFFFFFF if m_Textures[stage] is NULL** |
| 3 TFACTOR | 0x1 C0 |
| 4 SPECULAR | 0x5 V1, and sets device+8 bit 0x40 |
| 5 TEMP | 0xD R1 |

byte = reg | `((ctl|F|arg) >> 1) & 0x10` (ALPHAREPLICATE or alpha pass ->
`.a`) | `((ctl ^ arg) & 0x10) << 1` (COMPLEMENT -> UNSIGNED_INVERT) |
`ctl & 0x40`.

### Per-op input/output words (0x1984B4 table)

`X(slot)` = helper output; `1` = byte 0x20 (zero, inverted); `-1` = byte 0x40
(zero, expanded). OCW is `base` unless noted; OP bits 15..17.

| op | ICW | OCW |
|---|---|---|
| 1 DISABLE, first stage, colour | A=V0 B=1, **alpha slot written too (V0.a*1)**, then stop: count = 1 | base |
| 1 DISABLE, first stage, alpha | A=V0.a B=1 | base |
| 1 DISABLE, later stage (ALPHAOP only) | 0 | 0 |
| 2 SELECTARG1 | A=ARG1 B=1 | |
| 3 SELECTARG2 | C=1 D=ARG2 | |
| 4/5/6 MODULATE/2X/4X | A=ARG1 B=ARG2 | +0x10000 / +0x20000 |
| 7/8/9 ADD/ADDSIGNED/ADDSIGNED2X | A=ARG1 B=1 C=1 D=ARG2 | +0x8000 / +0x18000 |
| 10 SUBTRACT | A=ARG1 B=1 C=-1 D=ARG2 | |
| 11 ADDSMOOTH | A=ARG1 B=1 C=1-ARG1 D=ARG2 | |
| 12..15 BLEND{DIFFUSE,CURRENT,TEXTURE,FACTOR}ALPHA | A=ARG1 B=k.a C=1-k.a D=ARG2, k = op-12 as a D3DTA | |
| 16 BLENDTEXTUREALPHAPM | A=ARG1 B=1 C=1-TEXTURE.a D=ARG2 | |
| 17 PREMODULATE | first stage: A=ARG1 B=TEXTURE (this stage's); else A=ARG1 B=1 | |
| 18 MODULATEALPHA_ADDCOLOR | A=ARG1 B=1 C=ARG1.a D=ARG2 | |
| 19 MODULATECOLOR_ADDALPHA | A=ARG1 B=ARG2 C=ARG1.a D=1 | |
| 20 MODULATEINVALPHA_ADDCOLOR | A=ARG1 B=1 C=1-ARG1.a D=ARG2 | |
| 21 MODULATEINVCOLOR_ADDALPHA | A=1-ARG1 B=ARG2 C=ARG1.a D=1 | |
| 22 DOTPRODUCT3 | A=expand(ARG1) B=expand(ARG2); alpha slot zeroed and alpha pass skipped | `(base|0x820000)>>4`: AB_DST = R0/R1, AB_DOT, AB_BLUE_TO_ALPHA |
| 23 MULTIPLYADD | A=ARG0 B=1 C=ARG1 D=ARG2 | |
| 24 LERP | A=ARG0 B=ARG1 C=1-ARG0 D=ARG2 | |
| 25/26 BUMPENVMAP(LUMINANCE) | A=CURRENT B=1 (no bump; the texture shader does it) | |

### Quirks the transcription keeps

- **Missing texture.** When an ICW's top byte comes out 0xFF (a NULL
  `m_Textures[stage]` read anywhere in the word), the whole ICW becomes
  `current * 1` (`V0` on the first stage, `R0` after; `.a` in the alpha
  pass). The OCW, with its scale and bias, is kept.
- A **colour DISABLE on the first stage** emits a single diffuse stage and
  ignores ALPHAOP.
- A **COLOROP of DISABLE on a later stage** ends the chain even if its
  ALPHAOP is not DISABLE.
- **RESULTARG** only distinguishes TEMP from everything else, and is read
  once per stage for both passes.
- **Point sprites** start at stage 3 and so build at most one combiner stage,
  from T3.

## Not resolved statically

- **COLOROP/ALPHAOP 0 or > 26.** The guest has no range check: op 0 jumps
  through 0x1984B0 (0x00498D00), op 27 through the padding after the table
  (0x90909090). The transcription flags `D3D8FF_UNRES_BAD_OP` and returns -1.
- **An argument with `(arg & 0xF) > 5`.** The helper table has six entries;
  index 6 reads 0x90909090. Flagged `D3D8FF_UNRES_BAD_ARG`.
- Neither can happen with valid D3D input, but whether JSRF ever sets one is a
  runtime question.
- The value of `device+0x374` (the second gate in the fog updater) was not
  identified.
- The transcription has not yet been compared against the executor's
  combiner registers on real draws; that is G43's runtime half.
