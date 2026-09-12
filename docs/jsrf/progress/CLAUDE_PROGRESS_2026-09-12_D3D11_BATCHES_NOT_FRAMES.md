# Windows takes every batch on the GPU, bit-exactly, and still shows a black
# frame. The reason is measured and it is not the renderer.

Date: 2026-09-12 (Europe/London)
Implements CLAUDE_PROGRESS_2026-09-12_D3D11_BRIDGE_DESIGN.md.

Read section 3 before believing section 1. The counter the design note chose as
its success criterion is fully satisfied by a renderer that produces no
picture, which is exactly what happened.

## 1. The counter

Two Windows runs anchored at the SAME guest clock, one JSRF at a time,
`oracle_anchor.sh win <clock>` with and without `ORACLE_D3D11=1`:

| clock 1000                   | CPU rasteriser | D3D11 |
|---|---|---|
| `[RASTER]` on the CPU        | 2977 batches, 4956 triangles | **0 batches, 0 triangles** |
| triangles total              | 4956 | 4956 |
| native batches               | -- | 2977, **0 software fallbacks** |
| wall clock to that anchor    | 32 s | 14 s |

Identical totals, nothing left on the CPU, nothing refused. At clock 300 the
same shape: 878/1457 -> 0/0, 877 native. Two fragment programs compiled for the
whole run; 2977 texture requests, 4 uploads.

## 2. The renderer is correct, per batch

`diagnostics/jsrf_first_fault/d3d11_copy_test.c` is the twin of
`metal_copy_test.c`: the same batch rasterised both ways from the same surface,
in the bottle, on a real device.

```
  untextured                   painted 256 cpu / 256 gpu;   0 differ, worst 0 steps
  untextured + specular        painted 256 cpu / 256 gpu;   0 differ, worst 0 steps
  texture copy                 painted 256 cpu / 256 gpu;   0 differ, worst 0 steps
  texture x diffuse            painted 256 cpu / 256 gpu; 124 differ, worst 1 step
  dithered                     painted 256 cpu / 256 gpu; 117 differ, worst 1 step
  bilinear                     painted 256 cpu / 256 gpu; 124 differ, worst 1 step
  src-alpha blend              painted 256 cpu / 256 gpu;  73 differ, worst 1 step
  alpha test                   painted 168 cpu / 168 gpu;  83 differ, worst 1 step
  depth test + write           painted 256 cpu / 256 gpu; 124 differ, worst 1 step
  stencil                      painted 256 cpu / 256 gpu; 124 differ, worst 1 step
  BC2                          painted 120 cpu / 120 gpu;  52 differ, worst 1 step
  BC1 mipmapped, 1 combiner    painted 120 cpu / 120 gpu;   0 differ, worst 0 steps
  2 textures, 1 combiner       painted 120 cpu / 120 gpu;   0 differ, worst 0 steps
  2 textures, 2 combiners      painted 120 cpu / 120 gpu;   0 differ, worst 0 steps
  unsupported target format    returned -1 (target-format), target preserved
```

Worst disagreement anywhere is ONE RGB565 channel step -- the rounding an 8-bit
render target owes the CPU path's float arithmetic. Six cases are bit
identical. The alpha-test row discards 88 of 256 fragments and both discard the
same 88.

The `painted` columns are the positive control and they earned their keep
immediately: the first version of this test reported `0 differ` on blend and
alpha test while BOTH renderers drew nothing, because the vertex diffuse colour
was zero, so source-alpha blending reduced to "leave the destination alone" and
every fragment failed the alpha test. Two rows of perfect agreement, measuring
nothing.

## 3. And the frame is black

Flip captures at guest clock 1000, `ORACLE_FB_DUMP=1`, non-black pixels per
presented frame:

```
  frame     mac/metal   win/cpu   win/d3d11
  flip007       31159         0           0
  flip009       31159     31159           0
  flip011       31159     31159           0
  ... and so on for every flip to flip023
```

macOS and the Windows CPU rasteriser agree exactly -- 31,159 pixels, the same
count. The D3D11 path draws 4,956 triangles and presents nothing.

**So `0 triangles on the CPU` was never evidence of a working renderer.** It is
satisfied by a renderer that takes every batch and drops every pixel. The
design note picked that criterion and this session believed it for a while.

A note on the control: at first the Windows CPU run produced no flip captures
at all, which read as "Windows never presents". It is not about the renderer --
the capture fires on `pgraph_d3d11_take_frame()`, and that flag only exists
once `pgraph_d3d11_init()` has run, which on Windows happens only inside the
D3D8 bring-up block. macOS runs that block unconditionally. `oracle_anchor.sh`
now sets `RECOMP_D3D8_PROBE` for a Windows capture so the two hosts are
comparable.

## 4. Where the pixels go, measured to the instruction

Instrumented in `nv2a_d3d11.c` (`RECOMP_D3D11_TRACE=<n>`, plus the `read-back`
counters in the report), at clock 1000:

```
[D3D11] read-back: 7954 syncs, 2977 wrote colour, 0 failed; 350472316 non-black
        pixels; uploads 2977 cleared / 0 copied
```

One upload and one read-back **per batch** -- 2,977 of each, every one a full
640x480 round trip through guest RAM. 350M non-black pixels are written back
over the run, so the renderer is producing pixels and they are reaching memory.

Why every batch: the guest **alternates its render target on consecutive
batches**. The first twelve batches of a run:

```
batch 0 target=0x81E000  batch 1 target=0x788000  batch 2 target=0x81E000
batch 3 target=0x788000  ...  batch 8 target=0x6F0000 zeta=1
batch 9 target=0x6F0000  batch 10 target=0x81E000  batch 11 target=0x6F0000
```

Three surfaces, never the same one for long. A single retained GPU surface is
therefore thrown away on every batch. That is precisely the "GPU->CPU copy per
draw would be slower than the software rasteriser it replaces" the design note
warned about, arrived at from the other direction.

And then the decisive measurement. Batch 8 writes its result back and the same
pointer is read again immediately, on the same thread, before anything else
runs:

```
writeback target=0x26F0000 nonblack=31013; readback-now nonblack=31013 sum=780597485
batch 9   target=0x26F0000 ...
upload    target=0x26F0000 uniform=1 first=0000 ram-nonblack=0
```

The write lands -- 31,013 pixels, confirmed through the same pointer. By the
next batch's upload, microseconds later, that memory is black again. The guest
clears and writes the same framebuffer from its own thread, and a whole-surface
round trip per batch cannot coexist with that. The CPU rasteriser is immune
because it composites in place, incrementally: a guest clear costs it only what
was drawn before the clear.

## 5. The obvious fix, tried, and why it is not enough

One retained surface per guest target instead of one globally
(`SURFACE_CACHE_SIZE 4`, keyed on target, size, pitch, dimensions and depth
binding). It is in the file, and the equivalence test passes unchanged with it.
It changes nothing:

```
[D3D11] surfaces: 2977 binds, 0 evictions
[D3D11] read-back: 14954 syncs, 2977 wrote colour; uploads 2977 cleared / 0 copied
```

Zero evictions -- three targets, four slots -- and still an upload per batch,
because `clear_surface()` calls `nv2a_gpu_invalidate(NULL)`, which discards
every retained surface, and **the guest clears roughly 1.7 times per batch**
(the `[PGRAPH] clear N` log is rate-limited and hides this; the sync count does
not).

So the next step is not a bigger cache. It is to stop the clear from being a
CPU write to guest RAM at all: when the GPU path owns a surface,
`NV097_CLEAR_SURFACE` should clear the retained surface in place
(`ClearRenderTargetView` / `ClearDepthStencilView`, which `upload_surface`
already does for the uniform case) and skip the memset, so the surface survives
and the read-back happens where the design put it -- at the flip, once.
`clear_surface()` is shared with the Metal path, so this wants doing for both.

## 6. Two of the design note's six gates do not hold

Both were read out of the tree rather than run.

**Wine's HLSL compiler segfaults on a module-scope array.** The first
implementation was one uniform-driven interpreter -- the Metal fragment shader
transliterated, with a `static float4 r[14]` register file indexed by the
combiner words at runtime. It took the process down inside `d3dcompiler_47`,
which in this bottle is Wine's own over `libvkd3d-shader`. A probe of eleven
constructs says where the edge is:

| construct | result |
|---|---|
| trivial `ps_4_0` / `ps_5_0` | compiles |
| cbuffer, literal-indexed cbuffer array | compiles |
| **dynamically** indexed cbuffer array | compiles |
| texture sample, projected sample, four samplers | compiles |
| sample inside a branch, `discard` | compiles |
| literal-indexed local array, dynamically indexed local `const` array | compiles |
| `static float4 r[14]` at module scope | **crashes the compiler** |

So the register file cannot be an array. The fragment program is generated per
combiner configuration instead, every index resolved at emit time into fourteen
named locals -- the "locals only" shape the same probe compiled cleanly.

**`d3d8_combiners_from_render_states` cannot be fed our words.** It reads the
four packed inputs of an ICW from the LOW byte up and the AB destination from
the LOW nibble. NV2A hardware puts A in the high byte and the CD destination in
the low nibble -- nouveau's `NV20_3D_RC_IN_RGB_A__MASK` is `0xff000000` and
`NV20_3D_RC_OUT_RGB_CD_OUTPUT__MASK` is `0x0000000f` -- which is what
`nv2a_metal.m` decodes to produce correct JSRF frames. `NV2ATextureCopy` also
carries no final-combiner inputs, so that stage would have to be synthesised
rather than translated, and the generator seeds R0 with all of T0 where the
hardware seeds only R0.alpha. Three transforms this build cannot verify, to
reach a shader that then differs from the macOS one.

Nothing was lost by not using it. `prepare()` rejects dot products, mux, bias
and scale outright (`if(outputs[j]&~0xfffu) return "combiner output mode"`),
restricts destinations to R0 and R1, and refuses nonzero combiner constants.
Every extra mode that generator implements is unreachable from a state this
path accepts.

## 7. Two bugs found on the way

**The CPU rasteriser can `memcpy` from a NULL texture.**
`nv2a_texture_copy_triangle_depth`'s 1:1 copy fast path checks combiner count,
format, blend, depth and stencil, but not `untextured` -- and then reads
`texture`, which an untextured batch is entitled to pass as NULL. It has never
fired in the game only because an untextured state also leaves width and height
zero, so the bounds test happened to cover it. Fixed with the missing
condition; `metal_copy_test.c` dodges it by zeroing width and height.

**The uncommitted tree kills Windows between guest clock 300 and 500.** Inside
`sub_00147EBB`: sometimes a page fault reading `0x101FFFFF8`, sometimes a hang
with the log frozen mid-function. The control run without `RECOMP_D3D11` faults
at the identical address and PC, and with `xbox_memory_layout.c`,
`kernel_bridge.c`, `recomp_mem_watch.c` and `fb_present.c` set aside the same
binary reaches clock 1000 in 34 s with no fault. Those four files carry
in-flight `RECOMP_STORE_WATCH` work; the `main.c` hunk removing the VEH address
filter belongs to it too and was NOT set aside for that test, so the fault is in
one of the four. They have been restored; the measurements above were taken
with them out.

## 8. Running it

```sh
sh diagnostics/jsrf_first_fault/oracle_anchor.sh win 1000 out/cpu
ORACLE_D3D11=1 sh diagnostics/jsrf_first_fault/oracle_anchor.sh win 1000 out/gpu
ORACLE_FB_DUMP=1 ... # adds flipNNN.bmp, and RECOMP_D3D8_PROBE on Windows
RECOMP_D3D11_TRACE=20 # per-batch target, slot, and retained-vs-upload
```

`nv2a_pb_exec_report()` now runs at the anchor as well as on the periodic
report, because an anchored run switches the periodic one off and `[RASTER]`
was therefore unobtainable at a guest clock -- only at a wall clock, which
section 1 of the WINDOWS_PLAYABLE handover spent three wrong findings
establishing is meaningless.

`RECOMP_D3D11=1` brings up the D3D8 layer's device the way `RECOMP_D3D8_PROBE`
does but leaves its window hidden: nothing presents through it, the picture
belongs to `RECOMP_FB_WINDOW`, and a second black rectangle reads as a hang.
