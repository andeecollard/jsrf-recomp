# JSRF: first genuine graphics, 3 September 2026

## Outcome

The title now renders its real **Presented by SEGA** startup logo at 640×480.
This closes the visible-pixel completion tests for G3, G6 and G7. It is not
evidence of menu navigation or gameplay. The title remains on this logo in
the bounded run despite successful cache completion.

This follows the physical-heap alias and single-executor repairs recorded in
`CODEX_PROGRESS_2026-09-03_COMBINERS.md`. The earlier all-black measurements
remain valid historical checkpoints. No generated sources were changed,
and no changes have been committed or pushed.

First evidence: ignored run `codex-dxt-depth-08`, 20 seconds, fresh HDD.
`draw-000011.after` contains 62,008 nonzero bytes in a 614,400-byte RGB565
surface. `frame003.png` is only a format conversion of `frame003.bmp`, not
an edited or synthetic image. Its displayed content is the genuine SEGA logo.
No raster-test, forced colour or disabled-alpha/depth switch was used.

## Implemented scope

- Single-mip, 2D DXT1/BC1 sampling: row-major compressed blocks, RGB565
  endpoints, interpolated colours, and transparent black in three-colour mode.
  Power-of-two dimensions come from TEXTURE_FORMAT, not stale image-rectangle
  registers. Normalized projective coordinates, repeat/clamp and nearest/
  bilinear filtering are supported. Linear RGB565 copies remain supported.
- The already measured four-stage combiner now receives texture RGBA,
  multiplying it by interpolated diffuse RGBA. The original copy program
  still selects texture RGB and diffuse alpha.
- Alpha test GREATER, byte reference; source-alpha / one-minus-source-alpha
  additive blending. Alpha testing happens before colour or depth writes.
- Front/back culling in Y-down framebuffer coordinates. Triangle-strip
  assembly now alternates winding so both triangles retain the same facing.
- Fixed Z24S8 LEQUAL, affine post-viewport depth, optional depth writes,
  preserving the stencil byte. The depth surface is resolved through its DMA
  handle, range checked, and rejected if it overlaps texture or colour.
- Depth/stencil clears for linear Z24S8 with validated DMA bounds, clear
  rectangle and independent plane masks. The original colour-clear path was
  not otherwise redesigned.
- Deterministic ordered RGB565 dithering. **Its thresholds and precise
  quantisation have not been verified on NV2A hardware.** It is an explicit
  approximation, not grounds to claim pixel-perfect emulation.

The measured state is DXT1 `09910C29`, repeat `00010101`, alpha GREATER 0,
SRC_ALPHA / ONE_MINUS_SRC_ALPHA ADD, clockwise fronts/back-face culling,
fixed Z24 LEQUAL with writes, and linear RGB565 colour. Draw 11's depth
capture starts at `FFFFFF00` everywhere, allowing its depth ≈8,390,705 to
pass; no invented depth clear was required to reveal the logo.

Unsupported combiners, other blend/depth functions, float/W depth, mip chains,
multiple textures and other unsupported state still reject explicitly. This
is a bounded software path, not a complete NV2A renderer. Existing generic
GPU acknowledgements and timing bring-up behaviour are unchanged.

## Does it render alpha?

Yes. Texture sampling and the combiner operate on RGBA. BC1 has one-bit
texel alpha; bilinear filtering and diffuse alpha can make the fragment alpha
fractional. Alpha controls discard and blending. The title's destination is
RGB565, so that framebuffer stores no alpha channel of its own. The tested
ARGB8888 destination path also stores the blended alpha when selected.

## Verification

- Eight CTests pass, including the new synthetic `jsrf_dxt_fragment` test.
  Tests cover opaque and transparent BC1 modes, repeat/clamp seams, filtered
  alpha, exact expected source-alpha blends, GREATER vs GEQUAL, LEQUAL,
  depth-write masks, preservation of stencil, culling and truncated buffers.
- The public method-sink test covers alternating strip winding and depth-only
  / stencil-only clear rectangles with nonzero DMA base and invalid ranges.
- DXT fragment and method-to-framebuffer tests pass ASan/UBSan with
  `-Wall -Wextra -Werror`. Python combiner capture/grouping regression passes.
- Replay format v2 adds optional depth before/after buffers; v1 stays readable.
  Both colour and depth replay match the live captures for draws 11 and 300
  of the short run, and draw 11 of the longer run. The original synthetic
  RGB565 copy-pattern replay still matches all destination bytes.
- Logo colour-buffer SHA-256:
  `2678fc4da67204827a3a66805ce249a498aca7d0ceca931594b934d839fc3b52`.
  Logo BMP SHA-256:
  `c026dcaa0a6097983f32146203c27ae6d1c2337e02798427f654520175f99b49`.

Longer confirmation: `codex-graphics-09`, a separate fresh HDD and 300-second
bound. The cache reaches 259 files / 122,467,906 bytes and all nine completion
markers, with no JSRF_FATAL.ERR. The last report counts 129,811 prepared draws
and zero texture or vertex rejections: 43,277 copy draws and 86,534 modulated
draws. Both configurations have zero invalid positions/collapsed XY, with no
pusher bad headers. No guest-fault, unresolved-stub, missing-epilogue or
allocation-failure report was found. The runner stopped its child at 300
seconds; this was not normal title exit.

Of 62 BMP snapshots, 38 contain the logo and 24 are black. Report-time
snapshots can catch the surface
between clear and draw, so some BMPs remain black; completed draw captures
are the stronger rendering evidence. These runs are not FPS benchmarks.

## Next bounded target

Trace the transition after cache completion while the SEGA logo remains:
guest timing, logo-state progression and input readiness. Do not infer that
the title is in gameplay merely because pixels now render. Preserve the
working startup image and reject newly encountered unsupported graphics
state explicitly. Hardware-accurate dithering and broader renderer coverage
remain separate follow-ups.

## Semantic references

Register layouts are in `src/nv2a/nv2a_regs.h`. Additional primary references:
[xemu texture layout](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/texture.c),
[xemu fragment-state setup](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/gl/draw.c),
and [Microsoft BC1 documentation](https://github.com/MicrosoftDocs/windows-dev-docs/blob/docs/uwp/graphics-concepts/block-compression.md).
The CPU implementation and tests are local; no game texture is committed.
