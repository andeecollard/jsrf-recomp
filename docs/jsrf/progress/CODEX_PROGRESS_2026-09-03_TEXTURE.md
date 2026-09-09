# JSRF texture-copy milestone — 3 September 2026

The captured startup draw now samples its real texture and reproduces its
colour program. The result is black because the source framebuffer is black.
This completes the captured-copy goal, not a visible menu or playable game.

## What changed

- Added `src/nv2a/nv2a_texture_copy.c/.h`, a portable, deliberately bounded
  fragment path for the observed PROJECT2D program: one linear RGB565 texture,
  clamp-to-edge addressing, one mip, nearest/bilinear filtering, texture RGB
  and interpolated vertex alpha. Perspective interpolation uses reciprocal
  output W; PROJECT2D divides texture S/T by Q.
- Preserved all vertex-program outputs in the CPU renderer. Resolved texture
  and target DMA handles through captured RAMHT/PRAMIN objects and checked
  descriptor limits plus actual mapped memory before accessing surfaces.
  Source and target overlap is rejected.
- Added explicit rejection of other configured texture/combiner, blend,
  depth, stencil, colour-key, alpha-kill, polygon and clipping states. The old
  flat-colour diagnostic path is retained when no fragment state is supplied.
- Decoded target pixel size from its format, independently of row padding.
  A proven 1:1 RGB565 copy uses row copies after checking triangle coverage;
  other supported samples take the interpolating raster path. Dithering is
  accepted only for an exact RGB565-to-RGB565 texel copy.
- `RECOMP_DRAW_CAPTURE=<prefix>` saves draws 1, 128, 2048 and the first two
  rejected-state draws. Each supported capture has JSON methods and shader
  outputs, raw PRAMIN, texture bytes, and target bytes before/after rendering.
- Added `jsrf_texture_copy_replay` and `replay_texture_copy.py` for offline
  replay, including an optional generated-pattern check that changes no guest
  state. Captures and game-derived data remain in ignored build directories.

The previous shader milestone was checkpointed before these edits in
`build-macos/jsrf-first-fault/checkpoints/before-textures-2026-09-03.tar.gz`
and the adjacent patch. No commit was made. Original handover attachments were
preserved and excluded from that checkpoint.

## The actual draw

| State | Captured value / meaning |
| --- | --- |
| Texture unit | Only unit 0 enabled |
| Source | Physical address `0x01160000` |
| Format | `0x00011129`: 2D linear RGB565, one mip, texture DMA A |
| Dimensions / pitch | 640×480, 1280 bytes per row |
| Address / filter | Clamp to edge; linear filtering at this mip |
| Texture DMA A | Handle 3, object at PRAMIN `0x1120`, base 0 |
| Destination DMA | Handle 9, also base 0 |
| Destination | Initially `0x0128E000`, later `0x011F8000` |
| Vertex positions / UV | `(0,0)`, `(2560,0)`, `(0,1920)`, with W=Q=1 |
| Colour program | Texture RGB; vertex diffuse alpha; no blending/depth test |
| Source bytes | All 614,400 bytes zero at draws 1, 128 and 2048 |
| Result | Black; completed target matches source byte for byte |

The combiner stage multiplies texture RGB by one and writes R0.rgb; its alpha
stage multiplies diffuse alpha by one and writes R0.alpha. The final stage
selects those components. The generated pattern replay proves that colour
comes from texture memory; a successful all-black replay alone would be weak
validation.

Field and sampling semantics were checked against the local register header,
[xemu texture handling](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/texture.c),
[xemu colour-combiner generation](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/psh.c),
and [xemu vertex output conversion](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-prog.c).
This is a bounded implementation of the measured colour program, not a full
NV2A pixel-shader interpreter or hardware accuracy claim.

## Command-stream correction discovered during validation

The first faster run exposed periodic bad surface state. Rejected captures
included command-like values in surface dimensions and offsets. The old reader
skipped ring jump instructions, continued into unused tails, and advanced past
partial packets despite leaving them unconsumed.

`nv2a_pusher_run_segment` now reports consumed words and stops at partial
packets, jumps, or unsupported control flow. The JSRF harness follows validated
ring jumps, retains the unread cursor, snapshots submitted spans, and stops
rather than interpreting unknown control flow as data. The index acknowledgement
samples the submission before consumption, avoiding acknowledgement of newer
work produced while rendering or reporting.

A further correction was necessary: `0x0019B200` is the writer's allocation
cursor, which can expose incomplete commands. The generated title code at
`sub_001912EC` writes the completed pointer, masked by `0x03FFFFFF`, through
the DMA channel's +0x40 register. The harness now consumes the published
`0xFD800040` DMA_PUT. The first capture starts at the validated ring base so
initial setup is included. Low guest RAM remains the correct backing for this
title; adding `0x80000000` would select the wrong mapping.

This supersedes the earlier source comment claiming JSRF does not use the
PFIFO USER channel. Final runs have zero bad headers and zero rejected draws.
Generic kernel acknowledgements remain part of the existing bring-up runtime;
this does not establish hardware-accurate GPU scheduling or pacing.

## Verification

- Current macOS diagnostic build succeeds; CTest passes 5/5: AV encoder,
  vertex shader, method-to-framebuffer integration, texture/colour semantics,
  and streaming pushbuffer boundaries.
- Texture tests check nonzero DMA bases, descriptor and buffer bounds, padded
  rows, RGB565 decoding, nearest/bilinear filtering, projective Q, reciprocal-W
  interpolation, diffuse alpha, clamp-to-edge and unsupported-state rejection.
- The integration test drives real uploads and draw methods, then checks
  patterned RGB565 pixels in a destination behind a nonzero DMA base.
- Texture, renderer integration and pusher tests passed AddressSanitizer and
  UndefinedBehaviorSanitizer builds with warnings treated as errors.
- Offline replay of all three supported captures agrees with every live
  target byte. Each capture also passes the independent generated-pattern
  expectation. The all-black framebuffer SHA-256 is
  `34c69899504b36f13e8b22120cf0fd894e61fcd6b046fb8535b79cc491fa3b3f`.
- Completed framebuffer BMPs were checked and a PNG preview inspected.

Two final runs use separate empty HDD directories:

| Run | Duration | Last reported prepared copies | Rejected copies / shader batches | Bad headers | Last triangle milestone |
| --- | --- | --- | --- | --- | --- |
| `codex-textured-copy-06` | 25 s | 136,297 | 0 / 0 | 0 | 165,000 |
| `codex-textured-copy-07` | 25 s | 129,686 | 0 / 0 | 0 | 159,000 |

Counts are last emitted reports, not exact termination counts or display frame
rates. Both were terminated at the diagnostic time limit. Both cache
`jetfont.dat` (1,180,160 bytes) and `se_sys.dat` (230,912 bytes); a zero-byte
`se_sys.dat~` also remains. No later cached game asset, crash report or fatal
error file was found in these run directories. This does not prove long-term
stability or that every future draw will be supported.

Evidence is under
`build-macos/jsrf-first-fault/render-investigation/<run>/`; isolated HDDs are
adjacent `<run>-hdd` directories. Earlier exploratory runs are preserved and
are not final validation. In particular, run 04 used the prior executable
after a failed intermediate build; runs 06/07 were launched only after a
checked successful build.

Replay from the repository root:

```sh
python3 diagnostics/jsrf_first_fault/replay_texture_copy.py \
  build-macos/jsrf-first-fault/render-investigation/codex-textured-copy-07/draw-000001.json
python3 diagnostics/jsrf_first_fault/replay_texture_copy.py \
  build-macos/jsrf-first-fault/render-investigation/codex-textured-copy-07/draw-000001.json --pattern
```

## Next goal and readiness

Trace what should produce content in source framebuffer `0x01160000` and
request an asset beyond `jetfont` / `se_sys`. The generated code identifies
concrete investigation points:

1. `sub_000123E0` selects updates using root fields +0x40..+0x4C, then dispatches
   through the object at root +0x87DC. Determine which branch is live and which
   producer is meant to change it.
2. `sub_000131F0` traverses the render list beginning at root +0x7F9C. Correlate
   the live list, object flags and render calls with writes to the source
   framebuffer. The existence of the final copy does not prove scene rendering.
3. Instrument actual calls to asset-open helper `sub_00025DD0` and correlate
   the next request with those state transitions. No exact startup-blocking
   condition has yet been established; missing input remains a hypothesis.

The executable runs and the captured graphics path is now verified. A
recognisable interactive menu still requires startup progression, guest input,
and whatever additional graphics the newly reached screen exercises. Playable
JSRF remains unproven; there is no reliable date or completion percentage yet.
