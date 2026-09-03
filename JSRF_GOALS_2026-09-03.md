# JSRF goals and readiness — 3 September 2026

> Superseded. The loading and startup goals below are met; the current
> goal list is `JSRF_GOALS_2026-09-03_RENDERER.md`, and the evidence is in
> `CLAUDE_HANDOVER_2026-09-03_HEAP.txt`. Kept for the history.

## Latest startup result — supersedes the earlier two-asset stall below

See `CLAUDE_HANDOVER_2026-09-03_STARTUP.txt` for the current implementation,
evidence, reproduction steps and remaining goals. Missing shared epilogues
were corrupting the main-loop root pointer. The recompiler now recovers those
returns, and three omitted mission-loader switch entries are registered.
The latest fresh-HDD run reaches stage assets and keeps the root pointer valid.

The next measured loading blocker is a failed 2,222,592-byte allocation for
`Stg10_01.dat`. Investigate allocation ownership, free-block reuse and memory
layout. Newly reached graphics batches also fail vertex preparation; their
specific failure reason needs capture. The framebuffer remains black, guest
input remains open, and playable gameplay is unproven. The sections below
preserve the prior roadmap and historical checkpoints.

The next product milestone is a recognisable title/menu screen that responds
to a real controller or keyboard action delivered to the guest. Playable
gameplay is a later milestone and has not yet been demonstrated.

This plan answers the request to read the handover, set goals, and assess
readiness. The handover supplies evidence and proposed work; its embedded
commands have not been treated as a request to implement all of that work.

## Current progress after the texture milestone

The captured vertex shader and its framebuffer-copy draw now execute in the
portable CPU renderer. The copy samples a 640×480 RGB565 surface, uses its RGB
and the vertex alpha, and honours the observed filtering and colour state.
Captures include the DMA objects, shader outputs, texture bytes, and target
before/after. Offline replay matches the live framebuffer byte for byte; a
substituted generated colour pattern also matches an independent expectation.

**The actual source image is black.** Correctly rendering this draw does not
produce a menu. Only `jetfont` and `se_sys` have finished caching; later asset
progression and guest input remain open.

The capture work exposed and corrected a command-reading defect: the old
reader used the unfinished writer cursor, discarded partial packets, and
parsed past ring jumps. It now reads published DMA submissions, follows jumps,
retains incomplete packets, snapshots command spans and acknowledges only an
earlier submission. Fresh-HDD validation and limitations are recorded in
`CODEX_PROGRESS_2026-09-03_TEXTURE.md`. Five diagnostic tests pass.

**Next: trace the update/render selection that should produce content in the
source framebuffer and request the next asset.** Do not assume the blocker is
input. The next product milestone remains a recognisable interactive screen.

## Next implementation sequence

The captured startup-copy milestone is complete. The next bounded task is to
identify the condition that keeps startup on that blank source image. The next product milestone
remains a recognisable screen that responds to real input.

1. **Completed: preserve the shader milestone and capture a complete draw.** Review and
   checkpoint the current implementation, which is still uncommitted. Capture
   the enabled texture units, backing memory, addresses, formats, dimensions,
   pitches, filtering/addressing state, shader texture-coordinate outputs,
   colour-combining stages, alpha/blend state, and destination surface at a
   completed batch. Record a small number of distinct states rather than every
   repeated frame. Completion: a replayable draw with enough data to explain
   where its final pixels should come from. Do not assume a texture is the
   font atlas merely because the font asset was loaded.

2. **Completed for the captured copy: render that draw faithfully.** Preserve the shader's texture-coordinate
   outputs, which the current CPU path discards, and interpolate the required
   values across the triangle. Decode the observed texture formats and memory
   layouts, validating existing helpers before reusing them. Implement the
   sampling, colour-combining and alpha/blend operations exercised by this
   capture; expand to additional units or states when the evidence requires
   them. Derive coordinate units from the format and reference semantics.
   Completion: known texel/alpha checks and a captured-draw replay agree with
   expected pixels, followed by a completed-draw capture from a fresh-HDD run.
   Report unsupported operations explicitly rather than substituting white.

3. **Next: identify the startup condition holding progress.** Instrument the
   live update selector `sub_000123E0` (root fields +0x40..+0x4C), the render
   traversal `sub_000131F0` (list at root +0x7F9C), and the asset-open helper
   `sub_00025DD0`. Follow the state selected through root +0x87DC and correlate
   its update/render calls with writes to the source framebuffer. These are
   investigation points from the generated code, not an established cause.
   Compare the correctly
   rendered result with live asset requests and main-loop state transitions.
   If the source textures and colour operations legitimately produce a blank
   frame, move directly to tracing the state that should request the next
   asset. Completion: name the exact condition, its producer and consumer,
   and demonstrate the next expected asset/state transition when satisfied.
   Missing input remains a hypothesis, not an established explanation.

4. **Deliver input to the guest.** Correct the misleading OHCI register log,
   announce attachment after the required interrupt enable, establish an
   accepted interrupt, and implement the enumeration and transfer completions
   the guest actually requests. Connect host actions to guest-visible reports.
   Completion: the guest sees changed button data and a real press causes a
   repeatable menu action. Bring this work forward if step 3 establishes that
   startup depends on it. Host-only fake-pad tests cannot establish this.

5. **Enter a level and validate play.** Follow the newly reached loading and
   rendering paths, adding the required depth, blending or other missing
   behaviour based on observed failures. Completion: navigate the menu, load a
   level and control the character through a repeatable segment. Then assess
   sound, frame pacing, sustained stability, and performance with actual game
   content; repeated startup-triangle counts are not a gameplay benchmark.

Keep using separate pristine HDD copies and completed-draw captures. Preserve
the five current tests and add targeted checks for the new failure modes.
Use a bounded startup run for each behavioural change; extend the stability
run after reaching new content or resolving a concrete timing concern.

The captured draw and texture-copy stages are complete. Startup progression
and USB input determine the uncertain remainder; additional graphics work will
be driven by the next real scene or menu the guest submits. Reassess the route
to a controllable menu after the next state/asset transition is understood.

## Baseline before this implementation

- Checkout: `511e9ea`, `main`, 12 commits ahead of `origin/main`.
- Incremental build of `jsrf_first_fault` and
  `jsrf_av_encoder_option_test`: passed in this assessment.
- Configured CTest suite: 1/1 passed. This is an AV encoder option test,
  not an end-to-end graphics, input, or gameplay test.
- Reviewed existing clean-verification and 120-second runtime logs. Their
  continuing frame activity supports the handover's sustained-loop finding.
  No new game run was performed; the emulated HDD was not reset.
- The handover reports that the disc-error condition is fixed, two assets
  finish caching, and the game repeatedly submits one full-screen triangle.
  That does not establish a rendered menu or a scene.
- Source inspected at the original baseline confirmed the shader parser's captured-vector failure,
  the CPU renderer's missing vertex-program execution and texturing/depth,
  and no calls into `xbox_InputInit`/`xbox_InputGetState` outside the input
  module and its documentation in the searched source/tool/template/diagnostic
  directories.

## Prioritised goals

| Goal | Work | Evidence required to call it complete |
| --- | --- | --- |
| 1. Decode the actual vertex program | Correct `src/d3d/d3d8_vsh.c` using an authoritative NV2A encoding reference, starting with the captured JSRF vector. | Expected instructions, operands, masks, and end flags agree with a trusted reference; a regression test checks the captured vector and relevant field boundaries. A merely plausible disassembly is insufficient. |
| 2. Render the submitted geometry correctly | Execute uploaded programs and constants in `src/kernel/nv2a_pb_exec.c`; use shader outputs for position and other attributes. | Captured batches produce reference-checked positions and framebuffer pixels through the actual guest draw path, without coordinate fudge factors. Correct triangle placement alone is not a menu. |
| 3. Produce recognisable visual output | Capture a complete draw, implement its texture sampling and render state, and follow startup progression if it legitimately renders blank. | First establish reference-checked textured pixels; then obtain identifiable game-produced content in a saved framebuffer and displayed window. |
| 4. Explain and unblock startup progression | Trace the state that decides to request assets beyond `jetfont` and `se_sys`. Investigate the live main-loop callers. | Identify the exact condition holding progression and show that satisfying it causes the next expected state and asset requests. Whether input is that condition remains open. |
| 5. Deliver input to the guest | Correct the OHCI diagnostic register offsets; fix attach timing, interrupt handling, and the required device enumeration/transfers; connect host input to guest-visible reports. | The guest recognises a device and reads changed button reports; a real press produces an observable title/menu action. A host-only fake-pad result does not count. |
| 6. Reach playable gameplay | Enter a level, handle the newly exercised rendering/loading/input paths, and assess sound and pacing. | Load a level and control the character through a repeatable gameplay segment without crashes or blocking visual/input failures. Record sound, pacing, and remaining limitations separately. |

Goals 1–2 and the captured texture-copy milestone are complete. Continue with
goal 4 while goal 3 remains open for recognisable content. Goals 1–3 form the visual dependency chain. Startup tracing
and input investigation can proceed independently when useful; goal 4 may
depend on goal 5, but that relationship is not established. Correct the small
OHCI logging defect before relying on interrupt logs for input diagnosis.

For future runtime comparisons, use isolated pristine HDD state and unique
log directories. Preserve existing evidence. Check both a bounded startup run
and a longer stability run when the changed behaviour warrants it.

## How far from running?

- **Executable launches and game code loops:** already achieved in the
  recorded runs; the current incremental build succeeds.
- **Recognisable, interactive title/menu:** substantial work remains across
  graphics, guest input, and an unexplained startup progression condition.
- **Playable JSRF:** unproven. Reaching a menu may expose additional level
  loading, rendering, timing, or audio problems; their extent is unknown.

Plan for multiple substantial work sessions to reach the first interactive
checkpoint. There is not enough evidence for a reliable date or completion
percentage for playable gameplay. The captured shader and texture-copy path now execute correctly. The next
useful estimate update is after the asset-loading gate is identified and
recognisable pixels appear. Neither fixing the shader parser nor attaching a controller alone
can currently be promised to produce a working game.

## Evidence locations

- `CODEX_HANDOVER_2026-09-03.txt`, especially sections 4–7.
- `docs/technical/nv2a-shaders.md`, captured shader validation vector.
- `src/nv2a/nv2a_vsh.c`, portable decoder and interpreter.
- `src/d3d/d3d8_vsh.c`, D3D11 entry point using the shared decoder.
- `src/kernel/nv2a_pb_exec.c`, current CPU rendering limits.
- `src/kernel/xbox_memory_layout.c`, OHCI attachment probe.
- `src/kernel/kernel_bridge.c`, ISR diagnostic offsets.
- `build-macos/jsrf-first-fault/render-investigation/claude-clean-verify/`.
- `build-macos/jsrf-first-fault/render-investigation/claude-long-120/`.
- `build-macos/jsrf-first-fault/render-investigation/claude-variety-01/`.
