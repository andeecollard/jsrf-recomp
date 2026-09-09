# JSRF vertex-program milestone — 3 September 2026

The captured startup vertex program now decodes, executes, and feeds the CPU
rasteriser. The completed-draw framebuffer captures show a solid white
640x480 surface. There is still no recognisable menu or playable scene.

This updates the shader-related findings in `CODEX_HANDOVER_2026-09-03.txt`.
The original handover is preserved. Its input and asset-progression blockers
remain open.

## What changed

- Shared portable shader representation, decoding and execution in
  `src/nv2a/nv2a_vsh.{c,h}`; HLSL emission moved to `nv2a_vsh_hlsl.c`.
  The existing D3D11 parser entry point delegates to the portable decoder.
- Correct upload-order fields: opcodes in word 1, source mux values 1/2/3,
  split source-C register bits, separate temporary/output masks, output-bank
  routing, paired-unit operand reads and temporary destinations, R12/oPos
  alias, and input usage restricted to operands actually read.
- CPU pushbuffer handling now retains program/constant banks, LOAD/START
  cursors, mode, and current 4F/4UB vertex attributes. Uploaded vertex programs
  produce the positions and diffuse colours used for rendering.
- Corrected primitive identifiers using `nv2a_regs.h`, including triangle,
  strip, fan and quad assembly. Screen clipping occurs before integer bounds
  conversion, so an oversized shader-produced triangle can draw safely.
- Added `RECOMP_HDD_ROOT` to the diagnostic executable, allowing isolated runs
  without replacing the shared emulated HDD.
- Added `RECOMP_FB_DUMP_DRAW=1` to capture the first three completed rasterised
  batches when `RECOMP_FB_DUMP` supplies a prefix. Existing periodic snapshots
  can catch a clear or draw in progress and look black or partially filled.

## A material correction

The first complete captured program contains 12 slots. The first five set up
position and colours; the remainder copy point size and other attributes.
Its three position outputs, confirmed independently, are:

```
(   0,    0, 0, 1)
(2560,    0, 0, 1)
(   0, 1920, 0, 1)
```

The handover's claim that a factor of four is hidden in the instructions is
incorrect. The program adds the half-pixel bias after its transform; it does
not divide these vertices by four. NV2A vertex programs already output screen
coordinates. The oversized triangle covers the target and should be clipped
there, with no second viewport transform.

Detailed decoding, references, and limitations are in
`docs/technical/nv2a-shaders.md`.

## Verification

- Current macOS diagnostic build succeeds.
- Default CTest suite: 3/3 pass. These comprise the existing AV encoder test,
  captured-shader/operand/mask/parallel-execution regressions, and an integration
  test that sends real commands to the CPU method sink and checks framebuffer
  pixels. The integration test covers a nonzero START, upload window wrap,
  constant updates, clipping, and rejection of an uninitialised START.
- Both new tests also pass with AddressSanitizer and UndefinedBehaviorSanitizer,
  compiled with warnings treated as errors.
- `vsh_reference_test.c` agrees with abaire/nv2a_vsh_cpu on 128 varied input and
  constant sets for the complete captured program. It is optional: configure
  `JSRF_VSH_REFERENCE_DIR` to that external project's `src` directory to add
  it to CTest. No reference implementation is linked into the runtime.
- `git diff --check` passes.

Two bounded runs used distinct copies of the pristine HDD:

| Run | Duration | Last shader report | Later triangle milestone |
| --- | --- | --- | --- |
| `codex-vsh-01` | 25 seconds | 29,836 executed batches, 0 rejected | 36,000 rasterised |
| `codex-vsh-02` | 25 seconds | 30,453 executed batches, 0 rejected | 36,000 rasterised |

These are the last emitted reports, not exact counts at termination. Both
runs were stopped by the diagnostic time limit. No fault log or fatal-error
file was found. Both still cached `jetfont` and `se_sys`; this work has not
unblocked later asset requests.

Logs and frames are under
`build-macos/jsrf-first-fault/render-investigation/<run>/`.
The isolated HDDs are adjacent directories named `<run>-hdd`.
For `codex-vsh-02`, `frame-000.bmp` through `frame-002.bmp` were captured after
completed draws: all 307,200 pixels have the same white RGB565-derived colour.
`frame-000.png` is a converted preview. Later periodic snapshots can be partial.

The first run's optional dialog scanner matched address `0x0006F244`; this
alone is not evidence of a live dialog. No claim that the scanner proves
absence or presence of a dialog is made here.

## Remaining work

Next is texture sampling and the render state used by this full-screen draw.
The renderer still omits textures, blending, depth and colour/perspective
interpolation. Input still does not reach the guest, and the condition that
prevents further asset requests remains unexplained.

Shader arithmetic uses host floats. Exceptional-value behaviour and all
possible instruction combinations have not been verified against hardware.
Constant-writing programs are decoded but rejected during execution and HLSL
generation. The Windows D3D11 path was not run or shader-compiled on this Mac;
its host clip-space conversion needs separate validation. The two 25-second
runs establish this startup milestone, not long-term gameplay stability.
