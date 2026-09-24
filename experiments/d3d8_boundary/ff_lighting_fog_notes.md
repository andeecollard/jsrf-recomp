# D3D's fixed-function vertex state: lighting, material, texgen, texture transforms, fog (G42)

XDK 4134, read from the XBE with capstone under `/usr/bin/python3` (D3D
section VA 0x0018CB40, raw 0x0017D000). Host transcription:
`src/nv2a/d3d8_ff_vertex_state.{h,c}`; hand-derived vectors:
`diagnostics/jsrf_first_fault/d3d8_ff_vertex_state_test.c` (ctest
`jsrf_d3d8_ff_vertex_state`, 259 checks; 395 with G42b's inverse
model-view). The per-draw check is `d3d8_host_check_ff_vertex` (`src/nv2a/d3d8_host.c`), unit test
`d3d8_ffv_check_test.c` (ctest `jsrf_d3d8_ffv_check`). Companion to
`ff_combiner_notes.md` (G43).

## What JSRF uses

**All three are used; fog is not seen on in the two captured scenes.** The
evidence so far is xemu, running the real title, from the two full method
traces of 21 Sep (`~/jsrf-build/xemu-title-full`, 77 s, 89,884 draws;
`~/jsrf-build/xemu-loadmenu-full`, 196,324 draws), tallied by method number.
A census over gameplay comes from the mirror's own counters (the report
lines below) in the lead's run.

| state | title | load menu |
|---|---|---|
| fixed-function draws with LIGHTING_ENABLE = 1 | 3,075 | 56,235 |
| ... of which with one light (mask 1 = one INFINITE light) | 276 | 53,036 |
| ... of which with no light (ambient/emissive only) | 2,799 | 3,199 |
| light kinds written | directional only | directional only (0x1028-0x103C; 0x105C and 0x1040 never) |
| SPECULAR_PARAMS 0x09E0 | never written | never written |
| COLOR_MATERIAL | always 0 | always 0 |
| TWO_SIDE_LIGHT_EN | always 0 | always 0 |
| LIGHT_CONTROL when lit | 1 (no local viewer) | 1 |
| texgen | NORMAL_MAP (0x8511) on stages 0 and 1 | the same |
| texture matrix enabled | stage 0 at 1,232 draws, stage 1 at 2,419 | stage 0 2,587, stage 1 5,135 |
| texture matrices 2, 3 | never enabled | never enabled |
| FOG_ENABLE | 0 in all 3,268 writes | 0 in all 4,690 writes |
| FOG_COLOR | 0 and 0xFFFFFF | 0 and 0xFFFFFF |

The dominant stage-0 texture matrix is rows (0.5 0 0 0.5) (0 -0.5 0 0.5) 0
(0 0 0 1): layout E below (COUNT2 from CAMERASPACENORMAL), i.e. u = 0.5 nx +
0.5, v = -0.5 ny + 0.5 -- a normal-indexed ramp or sphere lookup, which is
what a cel-shaded title would use. Stage 1 uses layout A (2D coordinates
with a translation that scrolls).

So the transcription covers directional, point and spot lights, colour
material and two-sided lighting (cheap: the same code with other offsets),
but not the specular-parameter solver, which JSRF never reaches.

## The lazy flusher 0x1964A0

Called at the head of every draw; each bit of the dirty word 0x19DED8 runs one
updater, and all bits but 0xC0000070 are then cleared:

| bit | updater | writes |
|---|---|---|
| 0x100 | 0x195140 | point size / scale (0x0A30, 0x0318, 0x043C) |
| 0x4000 | 0x1952B0 | SHADER_STAGE_PROGRAM 0x1E70 |
| 0x800 | 0x197F90 | combiners (G43) |
| 0x0F | 0x195420 | texture stage address/filter/control (G39) |
| 0x2000 | **0x195610** | **fog**, then the final combiner (G43's tail) |
| 0x400 | **0x1957F0** | **texture matrices** |
| 0x1000 | **0x195F80** | **lighting and material** |
| 0x200 | **0x1962B0** | model-view 0x480, **inverse model-view 0x580** (G42b), composite 0x680 (G39 checks 0x480/0x680) |

Who sets them, for the state here:
- SetRenderState (0x18E960), deferred states through the table at 0x1C4070:
  FOGENABLE..RANGEFOGENABLE 0x2000; LIGHTING 0x1200; SPECULARENABLE 0x3000;
  LOCALVIEWER, COLORVERTEX, the eight material sources, BACKAMBIENT, AMBIENT
  0x1000.
- SetTextureStageState, table 0x1C4160: TEXTURETRANSFORMFLAGS (21) 0x400.
- SetTransform (0x18CC60), table 0x19A108 by state: VIEW 0x1200, PROJECTION
  0x200, TEXTURE0-3 0x400, WORLD0-3 0x200.
- SetMaterial (0x18CDA0): copies 17 words to device+0x9F0, 0x1000.
- SetLight (0x18DC00), LightEnable (0x18DDE0): 0x1000.
- SetTextureState_TexCoordIndex (0x18F060): 0x47F -- not 0x200, although it
  sets the device+0x450 bit that makes 0x1962B0 write the inverse model-view.
- NORMALIZENORMALS (RS 123, 0x18EC80: NORMALIZE_ENABLE 0x03A4 at once) and
  VERTEXBLEND (RS 118, 0x18F010: 0x0328 at once): 0x200. States from 117 on
  go through the per-state function table 0x199FDC, not 0x1C4070.

## Render-state indices (4134), each checked by its dirty bit and its reader

Cxbx-Reloaded's 5933 numbering in brackets: 10 higher up to SWAPFILTER, 19
higher from PSTEXTUREMODES on (4134 lacks PRESENTATIONINTERVAL and the eight
DEFERRED_UNUSED). All at 0x19E0E0 + 4*index.

| index | state | address | confirmed by |
|---|---|---|---|
| 82 | FOGENABLE [92] | 0x19E228 | fog updater's first test; dirty 0x2000 |
| 83 | FOGTABLEMODE [93] | 0x19E22C | fog updater's mode switch |
| 84-86 | FOGSTART, FOGEND, FOGDENSITY [94-96] | 0x19E230-38 | fog params |
| 87 | RANGEFOGENABLE [97] | 0x19E23C | FOG_GEN_MODE radial/planar |
| 92 | LIGHTING [102] | 0x19E250 | light updater's gate; dirty 0x1200 |
| 93 | SPECULARENABLE [103] | 0x19E254 | G43 |
| 94 | LOCALVIEWER [104] | 0x19E258 | LIGHT_CONTROL 0x10001 |
| 95 | COLORVERTEX [105] | 0x19E25C | COLOR_MATERIAL gate (0x1950A0) |
| 96-103 | BACKSPECULAR..EMISSIVE MATERIALSOURCE [106-113] | 0x19E260-7C | COLOR_MATERIAL fields |
| 104 | BACKAMBIENT [114] | 0x19E280 | back scene ambient |
| 105 | AMBIENT [115] | 0x19E284 | scene ambient |
| 106-113 | point states [116-123] | | dirty 0x100, point updater |
| 118 | VERTEXBLEND [137] | 0x19E2B8 | transform updater's skinning branch |
| 119 | FOGCOLOR [138] | 0x19E2BC | SetRenderState_FogColor stores it |
| 122 | TWOSIDEDLIGHTING [141] | 0x19E2C8 | TWO_SIDE_LIGHT_EN, back passes |
| 123 | NORMALIZENORMALS [142] | 0x19E2CC | inverse model-view scaling |

TSS words (0x19DEE0 + 0x80*stage): 21 TEXTURETRANSFORMFLAGS, 28 TEXCOORDINDEX.

## Texgen: SetTextureState_TexCoordIndex 0x18F060 (immediate)

Stores TSS[stage][28], then writes at once, not lazily:
- with a texgen mode (high word set): VERTEX_DATA4UB[9+stage] (0x1964+4s) =
  0xFF000000, the constant texcoord;
- TEXGEN_S/T/R[stage] (0x03C0+0x10s, one packet of three), all the same mode;
  TEXGEN_Q is not written;
- byte 0x22E554+9+stage (the slot -> attribute table G41 reads) = 9+stage for
  texgen, else 9 + the texcoord index;
- device+0x450 bit `stage` = "needs eye-space normals" (inverse model-view).

| TEXCOORDINDEX high word | NV2A mode | +0x450 bit |
|---|---|---|
| 0 | 0 (off) | 0 |
| 0x10000 CAMERASPACENORMAL | 0x8511 NORMAL_MAP | 1 |
| 0x20000 CAMERASPACEPOSITION | 0x2400 EYE_LINEAR | 0 |
| 0x30000 CAMERASPACEREFLECTIONVECTOR | 0x8512 REFLECTION_MAP | 1 |
| 0x40000 (OBJECT) | 0x2401 OBJECT_LINEAR | 0 |
| any other above 0x30000 (SPHERE) | 0x2402 SPHERE_MAP | 1 |
| any other below 0x30000 | 0x2400 | 0 |

## Texture transforms: 0x1957F0 (lazy, dirty 0x400)

Returns at once when the vertex shader object's flags (+4) have 0x12
(programmable or pass-through). For each stage: TEXTURETRANSFORMFLAGS 0 ->
TEXTURE_MATRIX_ENABLE[s] (0x0420+4s) = 0 and nothing else. Otherwise enable
= 1 and TEXTURE_MATRIX[s] (0x06C0+0x40s, 16 words) from the D3D matrix at
device+0x7D0+0x40s (= device+0x750 + 0x40*(2+s), where SetTransform stores
TEXTURE0+s). The packet header is computed as `(0x3FFEC4 - device) + esi`.

Key = in << 8 | (flags & 0xFF) << 4 | (flags >> 8 & 1), where `in` is 3 for
texgen, else byte `(tci & 0xFFFF) * 8 & 31` (x86 shift masking) of the
vertex shader object's +0x10 word, 2 when that byte is 0. Dispatch: above
0x320 two compares and a default; below, a byte table at 0x195B78 indexed by
key - 0x220 into the dword table at 0x195B64, whose fifth entry is 0.

Layouts (m = D3D row-major matrix, rows of the NV2A matrix):

| | key | case | rows |
|---|---|---|---|
| A | 0x220 | 0x1958E7 | (m0 m4 0 m8) (m1 m5 0 m9) 0 (0 0 0 1) |
| B | 0x230 | 0x195922 | (m0 m4 0 m8) (m1 m5 0 m9) (m2 m6 0 m10) (0 0 0 1) |
| C | 0x231 | 0x195963 | (m0 m4 0 m8) (m1 m5 0 m9) 0 (m2 m6 0 m10) |
| D | 0x241 | 0x1959B3 | (m0 m4 0 m8) (m1 m5 0 m9) (m2 m6 0 m10) (m3 m7 0 m11) |
| E | 0x320 | 0x195A0B | (m0 m4 m8 m12) (m1 m5 m9 m13) 0 (0 0 0 1) |
| F | 0x330 | 0x195AE2 | (m0 m4 m8 m12) (m1 m5 m9 m13) (m2 m6 m10 m14) (0 0 0 1) |
| G | 0x331 | 0x195ABC | (m0 m4 m8 m12) (m1 m5 m9 m13) 0 (m2 m6 m10 m14) |
| H | other > 0x320 | 0x195A8B | the transpose |

Every other key below 0x320 is **unresolved**: table byte 4 jumps to address
0 (for example COUNT4 with two inputs, 0x240), and keys below 0x220 or past
the table read code bytes. The transcription enables the stage, writes no
matrix and flags it. All layouts are word copies, so the matrix check is
exact.

## Fog: 0x195610 up to the final-combiner tail (lazy, dirty 0x2000)

First calls 0x1903A0, which loads a pass-through vertex program chosen by
FOGTABLEMODE and device+8 bit 2 when the vertex shader object has flag 2
(pre-transformed vertices) -- out of scope here, but G51.1's mode-6 draws
depend on it. Then:

- FOGENABLE 0: FOG_ENABLE (0x02A4) = 0, nothing else.
- Else FOG_GEN_MODE (0x02A0) and FOG_ENABLE = 1 in one packet, FOG_MODE
  (0x029C), FOG_PARAMS (0x09C0, three floats):

| FOGTABLEMODE | gen mode | FOG_MODE | params |
|---|---|---|---|
| 0 NONE | 0 (from specular alpha) | 0x2601 LINEAR | (1, 1, 0) |
| 3 LINEAR | RANGEFOG ? 1 : 2 | 0x2601 | s = 1/(end-start), or [0x19B0F4] = 8192 when end == start: (end*s + 1, -s, 0) |
| 1 EXP | RANGEFOG ? 1 : 2 | 0x800 | (1.5, density * [0x1E0DAC], 0) |
| 2 EXP2, and anything else | RANGEFOG ? 1 : 2 | 0x801 | (1.5, density * [0x1E0DA8], 0) |

FOG_COLOR (0x02A8) is written at once by SetRenderState_FogColor (0x18EB80)
with red and blue swapped, and RenderState[119] stored. FOG_PLANE (0x09D0) is
written only by SetShaderConstantMode (0x190240) as (0, 0, 1, 0).

## Lighting and material: 0x195F80 (lazy, dirty 0x1000)

`spec` = SPECULARENABLE or device+8 bit 0x40.

**Unlit** (object flags 0x12, or LIGHTING 0), four registers:
LIGHTING_ENABLE 0x0314 = 0, SPECULAR_ENABLE 0x03B8 = spec, LIGHT_CONTROL
0x0294 = 0x20001, TWO_SIDE_LIGHT_EN 0x17C4 = TWOSIDEDLIGHTING.

**Lit**, in order:
1. With `spec`: 0x195E40 writes SPECULAR_PARAMS 0x09E0 (six floats from the
   material power, via the solver 0x190850 with its tables at 0x19A4A0 and
   0x19A520) and, two-sided, BACK_SPECULAR_PARAMS 0x1E28. **Not transcribed**
   (JSRF never reaches it in the captures); flagged `D3D8FF_LUNRES_SPECULAR`
   and counted. LIGHT_CONTROL becomes 0x10001 with LOCALVIEWER and a light list.
2. LIGHT_CONTROL = 1 (or 0x10001), LIGHTING_ENABLE = 1, TWO_SIDE_LIGHT_EN,
   SPECULAR_ENABLE = 1.
3. COLOR_MATERIAL 0x0298 from 0x1950A0: 0 without COLORVERTEX, else two bits
   per source, BACKSPECULAR highest (EMISSIVE bits 0-1, AMBIENT 2-3, DIFFUSE
   4-5, SPECULAR 6-7, the back four 8-15); sources naming a colour the
   vertex format lacks are cleared (object flags 0x400 diffuse, 0x800
   specular, 0x1000 back diffuse, 0x2000 back specular).
4. 0x195CA0: SCENE_AMBIENT 0x0A10, MATERIAL_EMISSION 0x03A8, MATERIAL_ALPHA
   0x03B4 (= diffuse alpha); then, two-sided, 0x17A0 / 0x17B0 / 0x17AC with
   BACKAMBIENT and the back material (device+0xA34) and the source bits
   shifted by 8. With ambient colour a = AMBIENT/255 per channel:

   | sources | scene ambient | emission |
   |---|---|---|
   | ambient from the vertex | material emissive | a |
   | emissive from the vertex | a * material ambient | (1 1 1) |
   | both from the material | a * material ambient + emissive | (0 0 0) |

5. For each light on the enabled list (head device+0x398, next at record
   +0x8C; LightEnable inserts at the head, so NV2A light 0 is the most
   recently enabled), at most eight, NV2A light i at 0x1000 + 0x80i:
   - 0x195BA0: ambient, diffuse, specular colours (9 floats), each the
     light's colour times the material's unless that source is a vertex
     colour; two-sided, the same at 0x0C00 + 0x40i with the back material.
   - Directional (type 3): LOCAL_RANGE 0x1024 = 1e30; D = normalise(VIEW *
     (record+0x6C, w 0)), where +0x6C is -Direction normalised by SetLight;
     H = normalise(D + (0 0 -1)) (the vector at 0x19B0F8);
     INFINITE_HALF_VECTOR 0x1028 = H, INFINITE_DIRECTION 0x1034 = D.
     Mask bits 1 << 2i.
   - Point (1) and spot (other): LOCAL_RANGE = Range; LOCAL_POSITION 0x105C
     = VIEW * (Position, 1); LOCAL_ATTENUATION 0x1068 = Attenuation0..2.
     Point: mask 2 << 2i.
   - Spot adds SPOT_FALLOFF 0x1040 = record +0x78..0x80 and SPOT_DIRECTION
     0x104C = normalise(VIEW * (+0x6C, 0)) * record+0x84, w = record+0x88
     (all precomputed by SetLight). Mask 3 << 2i.
6. LIGHT_ENABLE_MASK 0x03BC.

The light records are an array of 0x90-byte entries whose pointer is at
device+0x390 (count +0x394), indexed by the D3D light index: D3DLIGHT8
(0x68 bytes), flags at +0x68, then SetLight's precomputations and the list
link.

Quirks kept: TWOSIDEDLIGHTING is a loop count (1 + value passes, the back
source bits shifted 8 more each pass, so 0 from the third on); a light type
other than 1, 2, 3 takes the spot path.

### Float helpers

- 0x1906F0 transform (SSE, float): x*row0 + y*row1 + z*row2 + w*row3.
- 0x1909E0 normalise (x87): |v|^2 rounded to float, then 0x190930.
- 0x190930 reciprocal square root: guess `(0xBE800000 - bits) >> 1`, one
  step with 0.47 and 1.47 (floats at 0x22E574/0x22E578, in D3D's writable
  data -- the mirror prints them once), rounded to float and made positive,
  then 0.5 * y * (3 - x y^2).
- 0x1906C0 add, 0x190690 scale (x87).

The transcription does x87 sequences in double, as the recompiler's x87
stack does, rounding to float at each `fstp dword`, and SSE sequences in
float one operation per statement, with FP contraction off. So it should
match the recompiled guest bit for bit; the check counts exact matches and
matches within 1e-5 relative apart, so any residual shows up as a number.

## The inverse model-view: 0x1962B0 and 0x190A30 (G42b, lazy, dirty 0x200)

**0x1962B0** (called last by the flusher, argument the device):

1. Returns at once when bit 31 of the dirty word 0x19DED8 is set (`js` at
   0x1962C6; the flusher has not yet rewritten the word) or the vertex shader
   object has flags 0x12. Who sets bit 31 is not identified; it survives the
   flusher's `and 0xC0000070`.
2. WV = 0x190750(WORLD at device+0x8D0, VIEW at device+0x750); MODELVIEW
   0x0480 (16 words) via 0x190FB0, which writes the transpose.
3. **Only if device+0x450 != 0 or LIGHTING (RS 92)**: 0x190A30(out, WV,
   NORMALIZENORMALS == 0) into a stack buffer, then INVERSE_MODELVIEW 0x0580
   = its first 12 words, header 0x300580, `rep movsd` -- the return value is
   ignored.
4. VERTEXBLEND 0: COMPOSITE 0x0680 = transpose(WV * device+0x470). Otherwise
   COMPOSITE = device+0x470 alone and, for WORLD1-3 (device+0x910..), the
   same pair into MODELVIEW1-3 (0x04C0 + 0x40i) and INVERSE_MODELVIEW1-3
   (0x05C0 + 0x40i, header 0x4004C0 + 0xFFF00100). 0x580 itself is the same
   either way; the blend matrices are not transcribed.

**0x190750** (SSE, float): row i of out = a[i][0] b.row0 + a[i][1] b.row1 +
a[i][2] b.row2 + a[i][3] b.row3, summed ((0 + 1) + 2) + 3, one `mulps` /
`addps` each.

**0x190A30** (x87 throughout; stdcall, `ret 0xC`): the inverse of M (row-major
m0..m15) by cofactors. Phase one (to 0x190BE2) forms the 2x2 minors of
columns 0-1 and from them output rows 2-3; phase two the minors of columns
2-3 and output rows 0-1; each 3x3 cofactor is three products summed left to
right. Some minors are rounded to float (`fstp dword` to a stack slot) and
some stay on the x87 stack in double -- the transcription keeps each exactly
where the guest does. The output is the inverse
**row-major, not transposed**: word 4r + c = cofactor C(c,r) / det, so the 12
words at 0x0580 are rows 0-2 of M^-1 (the normal transform needs (M^-1)^T,
and the NV2A takes its rows as dot-product vectors, so no transpose is
needed).

- det = C00 m0 + ... down column 0, in double: `s14 m12 + s10 m8 + s0c m4 +
  s1c m0`, the four cofactors already rounded to float.
- `fst` rounds a copy to float, `fcomp [0x1C43D0]` (+0.0) tests the DOUBLE:
  **det == 0 exactly returns -1 and writes nothing** (C3 set, `test ah,
  0x44; jp` not taken). The caller then sends 12 words of stale stack. A NaN
  det (C3 C2 C0) is computed through.
- **Scale (NORMALIZENORMALS off)**: s = |rsqrt(float(detf * detf))| with
  det's sign bit (`and 0x80000000; or`), rsqrt being D3D's 0x190930; each word
  = float(s * slot). The rsqrt's one Newton step from a bit-trick guess makes
  1/det approximate: diag(3,3,3,1) gives 0x3EAAA886 (0.3333170) for 1/3,
  5e-5 relative -- beyond the check's tolerance, so only exactness will do.
- **No scale (NORMALIZENORMALS on)**: each word = slot XOR det's sign bit:
  the adjugate up to sign; the NV2A renormalises the normals. (The note that
  stood here before G42b had the sense of this flag reversed.)

Transcribed as `d3d8_ff_matmul`, `d3d8_ff_inverse` and
`d3d8_ff_inverse_modelview`. Checked three ways: hand-derived vectors in
`d3d8_ff_vertex_state_test.c` (identity; a scale with translation, scaled and
unscaled; a negative determinant, with the sign of every zero traced; det 27,
where the approximate rsqrt shows; a singular matrix; the product's
summation order); and **the recompiled guest's own code run beside it**:
`imv_gen_crosscheck.c` extracts sub_00190A30 / 00190930 / 00190750 from the
gen tree and compares word for word -- 3,000,000 random matrices (dense,
sparse, wide exponents; both flag senses), 0 mismatches in the inverse or the
product, 512,362 singular on both sides, and the flipped-flag control
differing in 2,487,408 of 2,487,638.

## Not transcribed

- SPECULAR_PARAMS (0x195E40/0x190850): not reached by JSRF in the captures.
- The pass-through program selection in 0x1903A0 (fog table mode), for G51.1.

## The check in the D3D mirror

`stage_d3d8_census.py --mirror` hooks 0x1957F0 and 0x195F80 on entry, like
G43's two; the existing 0x195610 hook now also records the fog inputs on
every call (the fog registers are written even when the CW tail is skipped).
TexCoordIndex and FogColor are immediate, so the check reads their state at
the draw. Each draw carries every lazy group's inputs as last emitted and as
read at the draw.

SPECULAR_ENABLE 0x03B8 has two writers: the light updater and the combiner
builder (when device+8 bit 0x40 changes and SPECULARENABLE is 0). Both hooks
number their writes; the later one sets the expected value.

Groups and what is compared (against the executor's whole method shadow,
latched at the token):

| group | draws | registers |
|---|---|---|
| texgen | fixed-function 3D | TEXGEN_S/T/R[0..3] |
| texture transforms | fixed-function 3D | MATRIX_ENABLE[0..3]; MATRIX[s] where enabled and resolved |
| lighting | fixed-function 3D | everything the updater wrote (4 unlit, 29 with one directional light) |
| fog | every draw | FOG_ENABLE; GEN_MODE, MODE, PARAMS when on; FOG_COLOR |
| inverse model-view | fixed-function 3D draws with LIGHTING or device+0x450 set at the draw | INVERSE_MODELVIEW 0x0580..0x05AC, from the last emission that WROTE them |

Report lines (every 20,000 draws and at exit):

```
[D3D8-MIRROR] <why> ff texgen: fixed-function 3D draws N, all registers matching in N | registers compared=N MATCH=N | modes (off/EYE/OBJECT/SPHERE/NORMAL/REFLECTION) stage0 a/b/c/d/e/f ...
[D3D8-MIRROR] <why> ff texture transforms: ... registers compared=N EXACT=N within-tolerance=N | stages enabled a/b/c/d, layouts A-H ... unresolved N | before any emission N
[D3D8-MIRROR] <why> ff lighting: ... | lit N (by light count 0-8: ...; lights directional N point N spot N), colour material N, two-sided N, specular (SPECULAR_PARAMS not transcribed) N, SPECULAR_ENABLE last written by the combiner builder N | before any emission N
[D3D8-MIRROR] <why> ff fog: draws N, all registers matching in N | ... | fog on N (table NONE/EXP/EXP2/LINEAR a/b/c/d, range N), fog colour nonzero N | before any emission N
[D3D8-MIRROR] <why> ff inverse model-view: fixed-function 3D draws needing it N, compared N, all 12 registers matching in N | registers compared=N EXACT=N within-tolerance=N | NORMALIZENORMALS on N, vertex blend N, singular (stale stack sent, not compared) N, not written N | MODELVIEW from the same emission: draws N, all 16 words exact N | before any emission N
[D3D8-MIRROR] <why> ff {texture transform,lighting,fog,inverse model-view} laziness: emitted in this draw's flush N of N; state at the draw transcribes differently from the last emission in N (executor matches the draw-time state only N, the last emission only N)
[D3D8-MIRROR] <why> ff vertex-state reg XXXX: N mismatches          (only when nonzero)
[D3D8-MIRROR] draw N ff <group> MISMATCH (...) reg XXXX: d3d ... | exec ...   (first 8 per group)
[D3D8-MIRROR] G42 constants: rsqrt ... fog equal-range scale ..., eye ...
```

`RECOMP_D3D8_MIRROR_CONTROL=1` flips one expected word per group (texgen S0,
TEXTURE_MATRIX_ENABLE0, the first lighting register, FOG_ENABLE, and an
exponent bit of INVERSE_MODELVIEW[0] -- a low bit would pass the tolerance),
so every compared draw must fail in every group; the census counters still
count the real values.

The inverse model-view's hook is on 0x1962B0's entry (`d3d8m_xform_entry`):
it drops calls that return at once, numbers every other call (all write
MODELVIEW), and records the inputs of those that also write 0x580. Because
TexCoordIndex does not dirty 0x200, a NORMAL_MAP texgen switched on under
lighting-off can leave 0x580 older than the draw's WORLD/VIEW until
something else sets 0x200; comparing against the last WRITING emission is
what the executor holds, and the laziness line counts where the draw-time
state would have differed. "MODELVIEW from the same emission" compares the
16 words at 0x480 exactly with the transposed product whenever the last
emission of any kind was the writing one: a 0x580 mismatch with 0x480 exact
is in the inverse; with 0x480 wrong, in the product or its inputs.

Cost: each check item now carries the lighting inputs twice (1.4 KB each),
~11.8 KB per slot, so the 8,192-slot queue is ~96 MB once the ring has cycled
through it -- only in a mirror build with the mirror armed.
