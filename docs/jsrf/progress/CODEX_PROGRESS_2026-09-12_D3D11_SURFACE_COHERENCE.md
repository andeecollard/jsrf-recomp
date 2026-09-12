# D3D11 surface coherence: the safe half works; retained clears need guest-read ownership

Date: 2026-09-12 (Europe/London)

This follows `plans/JSRF_PLAN_2026-09-12_D3D11_PRESENTS_A_FRAME.md` through
the proposed resident-surface path. The experiment produced one safe change,
one working but gated primitive, and two concrete blockers. It does not yet
make the clock-1000 frame present.

## What is now safe by default

The D3D11 cache now tracks colour and depth readiness independently and exposes
range-based synchronization and invalidation. A CPU write to surface A no
longer synchronizes and invalidates unrelated surfaces B and C. Flip snapshots
and framebuffer dumps request only the current colour range. When a retained
render target is subsequently used as a texture, the texture path first
materializes that range rather than reading stale guest RAM.

The production path still performs CPU clears. This is deliberate: direct
guest loads in the static recompile do not currently pass through a callback
that can demand a GPU-to-RAM synchronization.

At guest clock 300 under CrossOver/D3D11:

```
win clock=300 wall=8s dump=2
[RASTER] 0 batches + 0 triangles on the CPU; 1459 triangles total
[D3D11] 879 batches native, 0 software fallbacks
[D3D11] textures: 879 requests, 877 cache hits, 2 uploads
[D3D11] read-back: 2051 syncs, 877 wrote colour, 0 failed
[D3D11] surfaces: 879 binds, 0 evictions
[D3D11] coherence: 604 range syncs; 0 colour / 0 depth clears stayed on GPU
```

That reaches the anchor cleanly. The first title-stage captures are black, but
the ordered trace shows why: at these early flips the two display targets copy
the black `0x006F0000` source. This stage is not the clock-1000 presentation
criterion from the plan.

## The resident colour clear works in isolation

`RECOMP_D3D11_RESIDENT_CLEARS=1` enables an experimental full RGB565 clear on
a retained target. It uses a dedicated pixel shader and an oversized triangle.
Two plausible shortcuts were tested and rejected:

* `ClearRenderTargetView` silently left this offscreen target unchanged under
  CrossOver.
* The existing "untextured" fragment shader emits its initialized white value,
  not vertex diffuse, so it cannot be reused as a clear shader.

The D3D11 regression now keeps three guest targets dirty simultaneously. It
proves that invalidating A does not flush B or C, range-syncing B does not flush
C, and a green clear of A leaves guest RAM unchanged until A is explicitly
synchronized. The complete fourteen-case renderer comparison still passes:

```
resident surface coherence   ok
[D3D11] coherence: 4 range syncs; 1 colour / 0 depth clears stayed on GPU
all cases within tolerance
```

## Why resident clears are not enabled in the title

With the experimental colour clear enabled, JSRF renders its first batch and
then stops advancing the guest clock. Guest RAM remains stale by design, and
the static recompile has no general observer on guest CPU reads of framebuffer
memory. The guest consumes the stale bytes before any explicit presenter,
texture, or dump synchronization site can repair them.

The clear is therefore gated off by default. Enabling it before guest reads can
acquire an overlapping GPU-owned range is a correctness bug, not merely a
presentation bug.

## Depth/stencil stays on the CPU

A resident `ClearDepthStencilView` was added to the same regression and failed
with an exact byte-layout mismatch under CrossOver:

```
requested guest Z24S8: 0x12345678
read back guest Z24S8: 0x34567812
```

The API now refuses resident depth/stencil clears, causing the caller to retain
the existing byte-exact CPU loop. The regression requires that refusal and
that the retained depth range remains unchanged. A future implementation needs
a verified depth shader/resource representation on both native Windows and
CrossOver; silently accepting the rotated value would corrupt depth and
stencil state.

## The current clock-1000 blocker is outside D3D11

The safe configuration was run toward guest clock 1000. Before the anchor it
reproduced the already documented in-flight memory-watch fault:

```
HOST FAULT ADDRESS: 0x0000000101FFFFF8
HOST PC:            0x000000014327D8F9
```

The CrossOver debugger attachment has no useful first-exception backtrace; the
runtime's own first-fault record above is the actionable evidence. This fault
also occurs without D3D11 and is associated with the dirty
`xbox_memory_layout.c`, `kernel_bridge.c`, `recomp_mem_watch.c`, and
`fb_present.c` work described in the plan. Until that work is isolated, the
required D3D11/CPU/Metal frame comparison at clock 1000 cannot be rerun on this
tree.

## What the recompiler needs next

The resident-surface optimization is now specified by a test. To enable it in
the game, guest memory needs an ownership boundary:

1. A guest CPU read of an overlapping GPU-owned range must call
   `nv2a_d3d11_sync_range` before returning the value.
2. A guest CPU write must synchronize any pending GPU writes, invalidate only
   the overlapping colour/depth aspect, and then perform the write.
3. Render-target-as-texture should eventually bind an SRV or issue a GPU copy,
   avoiding the current GPU-to-RAM-to-decoded-texture round trip.
4. Presentation should consume the retained GPU target directly at
   `NV097_FLIP_STALL`; guest RAM read-back should be required only for an actual
   guest CPU consumer or a diagnostic dump.

That ownership protocol is the durable route for a static decomp/recomp. More
special-case sync calls in the presenter will not cover direct loads emitted by
the recompiler and will fail again at the next guest subsystem that reads a
surface.

## Upstream references worth using

* `xemu-project/xemu`, especially its NV2A surface cache, CPU read/write
  callbacks, surface download/upload, and render-to-texture paths, is the main
  behavioral reference for the ownership protocol.
* `abaire/nxdk_pgraph_tests` has focused NV2A tests and golden results captured
  from Xbox hardware. Add reduced clear, Z24S8, and render-target-as-texture
  cases there when a behavior is ambiguous.
* `XboxDev/nv2a-trace` can capture original-hardware GPU command streams, which
  is stronger evidence than inferring intent from JSRF's final frame.
* `XboxDev/ghidra-xbe` plus `Cxbx-Reloaded/XbSymbolDatabase` is the useful
  analysis pair for turning the mechanical lift into a progressively named and
  typed JSRF decompilation.
* `Cxbx-Reloaded/Cxbx-Reloaded` and its `xbox_kernel_test_suite` are useful
  kernel/HLE oracles; their graphics architecture is a reference, not a
  drop-in renderer for this static-recompile runtime.
