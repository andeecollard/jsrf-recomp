# JSRF rendering progress and next steps — 5 September 2026

The city background now renders in the port. Completed-frame capture
`tmp/jsrf-ff-title.png` shows buildings, signs, roads and elevated rails.
There are still visible holes and missing effects; gameplay is not accepted.

## Implemented and checked

- Added the measured multitexture combiner subset: separate RGB/alpha stages,
  final specular addition, swizzled A8R8G8B8 and DXT1 mip sampling. Unsupported
  output modes, texture formats and LOD clamps remain explicit rejections.
- Added unlit, unskinned fixed-function composite projection and texture
  matrices, including all four texture attributes. Lighting, skinning and
  generated texture coordinates remain unsupported.
- Repaired APU main-register reads through the model, write-one-to-clear
  interrupt behaviour and preservation of the first idle-voice trap payload.
  This is separate from adding an audible macOS output sink.
- All 21 diagnostic tests pass. New coverage exercises actual ARM64 APU
  accesses, multitexture pixel output, mip selection, swizzle, bounds and
  fixed-function projection/secondary texture matrices.
- The 180-second `codex-ff-live` run detected the PS4 controller, reached the
  city sequence and continued presenting changing frames through 176 seconds.
  Last periodic pusher report had zero malformed headers. No controller-hang
  resolution or successful controllable gameplay is claimed.

Run logs and original completed frames are under
`build-macos/jsrf-first-fault/render-investigation/` (ignored build output).
Frame `codex-ff-framesnap010.bmp` is the source of the PNG above. The final
small combiner-initialisation and unsupported-LOD guard corrections were
rebuilt and regression-tested after this live capture.

## Next work, in order

1. Capture and classify the remaining triangle geometry rejections. Implement
   correct clipping for triangles crossing the camera/near plane rather than
   discarding whole triangles; verify against the remaining visible holes.
2. Add the measured fixed-function texgen paths. The final live report recorded
   10,438 texgen batch rejections and 276 lighting/skinning rejections.
3. Cover the remaining texture formats, blend and stencil/fog states from
   captured draws. Compare equivalent title-camera frames with installed xemu.
4. Profile the CPU fragment path before extending it further; current city
   rendering is visibly below a playable frame rate.
5. Verify controller-driven level entry and repeated transitions. Reproduce
   any audio wait with the completion trace before claiming it repaired.
6. Add and verify a macOS audio sink independently, then accept sustained
   controllable movement, complete frames and audible output together.

Reference behaviour was observed in installed xemu v0.8.136. Register semantics
were checked against xemu's [fixed-function shader generator](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/vsh-ff.c),
[combiner generator](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/psh.c)
and [texture layout](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/texture.c).

## September 7: playback performance, first measured optimisation

User prioritised playback speed while AFK; controller acceptance remains pending.
Five-second live CPU samples put the graphics worker predominantly in draw
execution and software rasterisation. Guest code forces -O0; the rasteriser is
already -O2, so changing the overall build type alone is not a renderer fix.

Removed duplicate texture sampling in sample_lod when lo == hi; retained the
same final floating-point interpolation expression. Added a white-box reference
test: 2,856 samples bit-identical across RGBA8/DXT1, mip levels, filter modes,
repeat/clamp and nearest/bilinear sampling. Build and all 26 CTests pass.

Same-environment bounded runs under build-macos/jsrf-first-fault/render-investigation:
- Baseline: codex-controller-response-01; frame 2400 at about 84s, 2700 at 173s.
- Changed: codex-speed-single-mip-01; frame 2400 at about 70s, 2700 at 151s.
- Slow 300-frame interval: 89s versus 81s, approximately 10% higher throughput.
  Single comparison with one-second log granularity, not a controlled benchmark.
- Changed run completed its 180-second observation and bounded cleanup; nonzero,
  changing presented-frame telemetry continued through shutdown grace.

This is only a small speed improvement, not real-time playback or visual/gameplay
acceptance. Remaining profile still points to software rasterisation. No clock
acceleration, frame skipping, Metal work, audio fix or controller fix in this step.

Follow-up: codex-speed-shared-weights-01 hoisted interpolation weights across
texture units. 516 differential renders were bit-identical and 26 tests passed,
but frames 2400–2700 still took approximately 81 seconds (68.04s to 149.06s).
Removed that experiment; cmp verified source exactly matches the retained
single-mip optimisation, and rebuilt successfully. Bounded run stopped normally.
Reference source and differential driver are retained beside this run's logs.

Audio source evidence for next work: POSIX waveOutOpen in
src/platform/win32_compat.h always returns MMSYSERR_INVALPARAM. SDL2 is already
linked to xbox_apu but no SDL output exists. apu_dsp.c fills monitor.frame_buf
with 256 stereo VP samples over eight subframes; existing XAudio2/waveOut monitor
paths then memset that buffer before mixing. A POSIX sink must preserve those
guest samples and submit 256 samples per 5333us, rather than copy the Windows
2048-sample-per-monitor-call path. No audio implementation yet.

## September 8: upstream integration without losing the JSRF port

Current upstream `sp00nznet/xboxrecomp` is `051a128`. It has 86 commits after
our shared base `342eb1f`; this JSRF branch has 102 commits after that base plus
uncommitted Metal, fixed-function, audio, input and recompiler work. Upstream
now owns substantial generic work that was previously local: stronger function
discovery, more correct x86 lifting, 170 routed kernel ordinals, physical DMA
resolution, a modular OHCI/controller model, queued DPCs/timers/vblank, broader
D3D8 resources, swizzled/DXT software sampling and the five flip methods.

It does not replace the current macOS path. Upstream's OHCI MMIO trap and worker
remain `_WIN32`-only; its POSIX D3D8 backend is the early OpenGL path; it has no
Metal backend or SDL APU sink; and its SDL controller backend does not hotplug.
The local CFG-aware flag merge/continuation work and JSRF's measured pushbuffer
ring/FLIP_STALL boundary are also absent upstream.

### Active integration gates

1. Leave this dirty checkout intact. Integrate in a separate worktree and keep
   the current branch as the behavioral reference.
2. Merge current upstream into the last committed JSRF state. Prefer upstream
   for generic discovery, kernel ordinals, format helpers and tests; preserve
   JSRF semantics where the two implementations answer different hardware
   questions.
3. Build and run upstream/toolchain tests before importing any uncommitted work.
4. Bring across, in dependency order, the local CFG flag-flow changes, JSRF
   pushbuffer boundary, Metal/VSH/fixed-function renderer, SDL audio sink and
   controller hotplug. Each layer must retain its focused tests.
5. Re-run the bounded JSRF reference route. Acceptance is unchanged: sustained
   changing frames, no malformed pushbuffer headers, native Metal draws without
   unintended software fallback, a DualShock press changing title state, and
   audible non-zero guest PCM at approximately real-time playback speed.

Do not delete manual recovered entries merely because the new discovery passes
exist. Regenerate first and retire an entry only when the generated function
map and runtime ICALL trace prove the replacement reaches the same address.
