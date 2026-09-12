# Plan: making the Windows D3D11 path present a frame

Date: 2026-09-12, written at the end of the session that built it.
Background: `../progress/CLAUDE_PROGRESS_2026-09-12_D3D11_BATCHES_NOT_FRAMES.md`,
which supersedes the design note `..._D3D11_BRIDGE_DESIGN.md` on two of its six
gates.

## Where this starts

`RECOMP_D3D11=1` takes every JSRF batch on the GPU with zero fallbacks, and its
per-batch output matches the CPU rasteriser to within one RGB565 channel step
across fourteen cases. It presents a black screen. macOS/Metal and the Windows
CPU rasteriser both present 31,159 non-black pixels per flip at guest clock
1000; this presents 0 while drawing the identical 4,956 triangles.

So the work left is not the renderer. It is everything around it: when the
surface lives on the GPU, when it comes back, and who else writes that memory.

## Step 0 -- FIRST, and it is a measurement, not a fix

**Do not start from the fix. One fact in the progress note is not established
and the whole of step 1 depends on it.**

What is established: the guest alternates render targets every batch; it clears
about 1.7 times per batch; every clear invalidates every retained surface; a
write-back lands (read the same pointer straight back and the 31,013 pixels are
there) and is black again by the next batch's upload of that same target.

What is NOT established: whether that second fact is a bug or correct
behaviour. If the order is draw -> clear -> draw, then the content SHOULD go,
and the CPU rasteriser loses it too. The black frame would then have a
different cause, and a GPU-resident clear would not fix it.

So the question to answer first is narrow: **what is the last write to the
buffer being displayed, before the flip?**

Instrument `FLIP_STALL` to print one line per flip:

  * `s_gpu.color_offset`, and whether any cache slot's `ram` matches it;
  * that slot's `ready` / `colour_pending` state;
  * non-black pixel count in the GPU surface, and in guest RAM at that instant.

Run it on both Windows configurations at clock 1000. The CPU run is the
control: the same line, minus the GPU columns, showing RAM non-black.

Three outcomes, three different step 1s:

| finding | means | next |
|---|---|---|
| displayed buffer has no cache slot | the guest draws into A and displays B | follow the flip's surface selection, not the clear |
| slot exists, GPU surface non-black, RAM black | the read-back is not reaching the displayed buffer | fix the sync's target, step 1b |
| slot exists, GPU surface black | nothing was ever drawn into the displayed buffer | the clear/draw interleave, step 1a |

Cost: one instrumented build, two runs, under ten minutes. Do not skip it.
Every wrong turn in this file's history came from acting on the third-most
likely explanation because it was the easiest to code.

## Step 1a -- GPU-resident clears (expected, but conditional on step 0)

`clear_surface()` memsets guest RAM and calls `nv2a_gpu_invalidate(NULL)`, so
every clear throws away every retained surface. At 1.7 clears per batch that is
the entire reason the surface never survives, and it is why the per-target
cache already in `nv2a_d3d11.c` shows 2,977 binds and 0 evictions while still
uploading on every one.

When the GPU path owns a surface for the target being cleared, clear that
surface instead and leave guest RAM stale until the next sync.

Two complications, both real:

  * **NV2A clears are scissored; `ClearRenderTargetView` is not.** Probe for
    `ID3D11DeviceContext1::ClearView` in the bottle first -- wined3d may not
    have it, and a missing method here is a crash, not an error. The portable
    fallback is a full-rect quad through the existing pipeline with colour and
    depth/stencil writes enabled, which is a dozen lines and cannot be absent.
  * **Stale guest RAM.** Everything that reads the framebuffer without going
    through a sync starts lying: `nv2a_pb_exec_surface`, the `[FB]` probe, the
    GDI presenter's 16 ms timer, `dump_surface_bmp`. Route them through
    `nv2a_gpu_sync()` or accept documented staleness -- explicitly, per site,
    not by omission.

Exit criterion: `uploads` per run drops from 2,977 to roughly the flip count,
and `[D3D11] surfaces:` shows binds far exceeding uploads. That number is the
proof, not the frame.

## Step 1b -- write back only what we drew

Our sync writes the whole clip rect. If anything else wrote that memory since
the upload, we clobber it with stale pixels; and we resurrect pixels a clear
removed. Track the bounding box of the triangles actually rasterised into a
surface and write back only that.

Worth doing even if step 0 says the clear is not the cause: it makes the GPU
path safe to coexist with a CPU writer instead of merely lucky, and it makes
the read-back cheaper, which step 3 needs anyway.

## Step 2 -- change the success criterion, in the code

`[RASTER] 0 batches + 0 triangles on the CPU` is satisfied by a renderer that
drops every pixel. It cost this session most of a day of believing a working
result. Add the presented frame's non-black pixel count to
`nv2a_pb_exec_report()` so the report carries a number that a black screen
cannot satisfy, on every host, and say in the report line that the triangle
counters alone do not mean the picture is there.

`nv2a_pb_exec_snapshot_nonzero()` already computes this; it is only wired to
the snapshot, which an anchored run never populates.

## Step 3 -- then, and only then, the measurement this work was for

The design note's claim is that the payoff starts at the intro animation, where
macOS goes to 4.1M and then 13.1M triangles. Nothing in this session tested it:
clock 300 and 1000 are the title screen.

Anchor a pair at clock 2500-3000, past the title crossover at guest loop 2408.
The CPU rasteriser could not reach clock 3000 in 33 minutes -- its framebuffer
froze on the anti-graffiti screen at t=843 s and had not moved by t=2013 s,
which is exactly the symptom the design note predicted. "The CPU run did not
reach the anchor in N minutes" is a legitimate measured result; pair it with
the D3D11 run's time to the same anchor.

Blocked until step 4.

## Step 4 -- the uncommitted tree kills Windows

Between guest clock 300 and 500 this working tree dies inside `sub_00147EBB`:
sometimes a page fault reading `0x101FFFFF8`, sometimes a hang. The control run
without `RECOMP_D3D11` faults at the identical address and PC, so it is not the
renderer. With `xbox_memory_layout.c`, `kernel_bridge.c`, `recomp_mem_watch.c`
and `fb_present.c` set aside, the same binary reaches clock 1000 in 34 s
cleanly.

Those four carry in-flight `RECOMP_STORE_WATCH` work. The `main.c` hunk that
removes the VEH address filter belongs to the same change and was NOT set aside
for that test, so the fault is in one of the four, not in the filter removal.
Prime suspect on shape alone: the VEH handler now being consulted for faults it
was previously never shown, and claiming one it should not.

This belongs to whoever owns that work. If nobody claims it, bisect the four
files -- one build each, four builds.

## Step 5 -- performance, after correctness

Even with the surface resident, this path does a 640x480 read-back per flip and
a texture decode per unique texture. Neither has been measured against the
software rasteriser it replaces. Do not tune before step 3 produces a number;
the design note's own caveat is that early slowness is guest execution -- 16
loops/s against macOS's 41 -- and no renderer change touches that.

## Do not repeat these

  * **Do not trust `[RASTER]`.** See step 2.
  * **Do not run two JSRF instances at once.** One session did, and the
    contention put the guest into a loading stall that read as a renderer
    regression and cost a run.
  * **Do not compare the hosts at equal wall clock.** Section 1 of the
    WINDOWS_PLAYABLE handover spent three wrong findings establishing this.
  * **Do not reach for `d3d8_combiners_get_shader()`.** Its ICW byte order and
    OCW nibble order are reversed against the hardware words we hold, it has no
    final-combiner inputs to give, and it seeds R0 with all of T0 where the
    hardware seeds only R0.alpha. `prepare()` rejects dot, mux, bias and scale
    outright, so none of its extra coverage is even reachable.
  * **Do not put an array at module scope in HLSL.** Wine's `d3dcompiler_47`
    segfaults on it and takes the process with it. Eleven other constructs were
    probed and all compile, including dynamically indexed cbuffer arrays.
    Probe any new construct in the bottle before shipping it.
  * **Do not read a black Windows frame as a renderer fault without a CPU
    control taken the same way.** The CPU run produces no flip captures at all
    unless `RECOMP_D3D8_PROBE` is set, because the capture fires on
    `pgraph_d3d11_take_frame()`. `oracle_anchor.sh` now handles this; anything
    that bypasses it will not.
