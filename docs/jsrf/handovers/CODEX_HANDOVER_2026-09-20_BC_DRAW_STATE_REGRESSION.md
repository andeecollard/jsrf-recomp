# BC support: draw-state regression and the next useful boundary

20 September 2026, base `b408c18`. User requested applying BC research to support
Claude's other work. Added only `diagnostics/bc_draw_state/` and this handover;
no renderer edits, regeneration, app rebuild, player settings, or save writes.
No active Claude task was available through Codex task listing, so this file is
the handoff, not a claim that a message was delivered to Claude.

## Result

The existing Metal texture snapshot/cache path passes a new independent pixel
oracle for alternating font-sized DXT3 pages, same-address content replacement,
cache eviction, and overlapping draws. Four combinations of batching and
hardware fragment state all pass. Both intentionally broken cache controls fail
all four configurations, in the expected phase. Reproduction and exact counts:
`diagnostics/bc_draw_state/README.md`.

This narrows one candidate. It does NOT exonerate the renderer or prove the
guest is wrong. GPU fixed-function vertex processing, UVs, alpha, render-target
feedback, real command ordering and guest memory races are outside this test.
The current glyph substitution mechanism remains a hypothesis to locate at a
specific boundary; two correctly bound pages do not by themselves prove a
renderer ordering fault.

## BC recommendation checked against actual source

The BC NV2A notes describe texture methods as shadow-state writes, with
draw-time resolution inferred from the dirty-state machinery. We already have
the main shape:

- `src/kernel/nv2a_pb_exec.c`: `pb_exec_method_body` stores aligned methods in
  `s_methods[method/4]`; `draw_primitive` calls `prepare_texture_copy`, then
  passes the prepared state and resolved texture pointers to the GPU.
- Our draw submission happens on `SET_BEGIN_END(0)`; the BC analysis describes
  resolution at BEGIN. This difference is worth observing in an actual bad
  command sequence, not changing speculatively.
- `src/nv2a/nv2a_metal.m`: `texture_buffer` keys on source pointer and byte
  count, compares actual contents, and allocates a new buffer on change.
  Existing cached Metal buffers are not overwritten in place.
- `nv2a_metal_draw` obtains each texture buffer per draw, binds it with
  `setFragmentBuffer`, and supplies a local `Params` through `setFragmentBytes`.

Therefore “defer texture uploads until draw time” is not an outstanding fix.
Fine-grained dirty-state optimization is a separate performance project and
needs a measured cost; it should not be sold as the font-page fix.

## The next capture must not hide the suspected ordering failure

`capture_draw` in `nv2a_pb_exec.c` calls `nv2a_gpu_sync()` BEFORE recording a
selected draw. This flushes queued work and changes the scheduling boundary.
An isolated captured draw can test decoding and rasterization, but a clean
captured image cannot disprove an ordering/lifetime problem in the original
sequence. The existing frame benchmark also has useful copying machinery,
but a timing benchmark is not automatically a faithful failure reproduction.

For Claude's recorded bad glyph frame, collect a bounded sequence without an
extra GPU sync and correlate, by draw ordinal:

1. Raw texture offset/format methods and BEGIN/END positions.
2. Resolved guest texture address and content identity at draw submission.
3. Vertex/UV inputs, chosen CPU/GPU vertex path, and the bound Metal buffer's
   content identity. Keep content changes at the same address distinct.

Then replay the sequence, including preceding offscreen draws if relevant.
If the prepared input is already wrong, work upstream of Metal. If correct
input produces wrong pixels in the sequence, reduce that sequence into this
standalone regression. This is preferable to adding another whole-run counter
that only says both pages appeared sometime during a session.

## BC fence finding: a concrete separate seam, not a claimed glyph cause

The BC research measured a queued semaphore-release record; its consumer and
full completion semantics were inferred, not established in that note.
Our code has a different observable design:

- `nv2a_pusher.c::dispatch` invokes `nv2a_pb_exec_method`, then
  `pgraph_d3d11_method`.
- The latter's `NV097_BACK_END_WRITE_SEMAPHORE_RELEASE` case writes the guest
  word immediately. It does not wait for or register a Metal completion.
  The handler first requires `g_pg.initialized`; whether this path is reached
  in the current failing session must be measured.
- The Metal executor has no corresponding named semaphore-release case.

This is a candidate early-completion boundary, **not evidence that JSRF reaches
it at the failure**. First establish a release hit and preceding outstanding
work. Do not change it to an unconditional full readback or blindly copy the
BC ring: correct integration must define submission, GPU completion, visibility
of guest-memory results, DMA address resolution, and ordered publication.

Also, the existing `nv2a_metal_sync` already skips the wait when neither colour
nor depth will be read back (`2fb4f6d`). Recommending that same optimization
again would duplicate shipped work.

## Validation and integration

- Fresh isolated build of current renderer; 4/4 GPU tests passed on M1 Max.
- Two scratch renderer mutations; 4/4 failures each, with exact expected phase.
- Existing compiler warning at `nv2a_metal.m:1032` about `nv2a_vsh_parse` pointer
  type remains unrelated and unchanged.
- Tests initially failed because sandboxed Metal initialization was unavailable;
  rerun with GPU access passed. No test converts that failure into a skip.
- No gameplay run was made; no claim of improved frame rate or repaired text.
- Files are left uncommitted for review alongside Claude's work. This isolated
  CMake project avoids edits to the busy main harness and can be integrated
  into its GPU test workflow when desired.
