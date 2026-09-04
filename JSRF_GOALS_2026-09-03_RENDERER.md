# JSRF goals — renderer bring-up, 3 September 2026

Supersedes the goal list in `JSRF_GOALS_2026-09-03.md`, whose loading and
startup goals are met. Evidence for the current state is in
`CLAUDE_HANDOVER_2026-09-03_HEAP.txt` and the two commits after it.

## Where we are

Startup and loading are no longer the blocker. A 60-second fresh-HDD run has
no guest fault, no allocation failure, no stack damage, a stable root pointer
and 290 distinct asset paths, and writes a 117 MB first-run cache. Every
vertex batch the title submits now completes the vertex stage: rejections went
70,228 to zero when `UB_D3D` vertex colours were decoded.

The genuine **Presented by SEGA** startup logo now renders. See
`CODEX_PROGRESS_2026-09-03_FIRST_GRAPHICS.md` for the fresh-HDD evidence and
remaining limitations. This is a startup-graphics milestone, not gameplay.

The sequence no longer stops there. The CRI ADX logo, the anti-graffiti legal
notice and the Created by Smilebit screen all render too, and the title runs on
to a heap exhaustion its own code reports as a disc error. See
`CLAUDE_PROGRESS_2026-09-03_FCMOV.md` and `CLAUDE_PROGRESS_2026-09-03_HEAP.md`.
The earlier memory repairs are in `CODEX_PROGRESS_2026-09-03_COMBINERS.md`.
The title did fill its vertex buffers:
CPU writes through `0x80000000 + offset` were landing in different storage
from the renderer's low-memory reads. The JSRF harness now explicitly shares
the physical heap view. Array-backed draws have finite, varying positions.

Aliasing exposed a second reader executing the same commands concurrently.
The validated harness pusher now owns execution; the legacy scanner is
survey-only for this harness. Two stable combiner configurations remain.

## Closed by measurement, 3 September

G1 and G4 below are answered; their text is kept so the reasoning survives.
The vertex programs arrive on subchannel 0, undropped, and the title selects
one twelve-instruction program — there was no program switch to miss. The
first-boot cache completes: nine `JSRF_CACHE_COMPLETE*.CMP` markers, no fatal
file, a file set stable at 259 files / 117 MB.

### G6 — CLOSED. The fragment stage accepts the measured loading draws

`913,580` of `1,370,379` draws are rejected with `combiner / texture program`.
`nv2a_texture_copy` implements one measured RGB565 blit configuration and
rejects every other by design. Group the distinct combiner setups the title
actually uses before writing any combiner code, and implement the one that
accounts for most of them first.

**Completed:** the four-stage texture-times-diffuse program, single-level
DXT1, alpha GREATER, source-alpha blending, culling and fixed Z24 LEQUAL are
implemented and tested. Both measured configurations now prepare without
rejections, and the newly accepted draws produce the real SEGA logo.
Ordered RGB565 dithering is explicitly approximate, not hardware-verified.

**Completion:** the rejection count falls substantially, and the draws that
newly pass write non-zero pixels.

### G7 — CLOSED. Title geometry writes non-zero pixels

The draws that already pass copy a black source surface. The previous
`x -0.5..-0.5` statistic measured only the first input vertex of each batch,
not the rasterized range. It now measures all input vertices and is labelled
accordingly. The CPU physical-heap alias repair fixed the separate, real
zero-input problem; the title's geometry now reaches the vertex stage intact.
A working combiner over an empty source still yields black.

**Completed:** fresh-HDD `codex-dxt-depth-08` draw 11 writes 62,008 nonzero
colour bytes. Its rendered framebuffer shows the recognizable SEGA logo;
the longer `codex-graphics-09` run confirms the result.

**Completion:** a surface with non-zero content that the title produced.

## Goals, in order

Each goal is finished when its completion test passes on a fresh-HDD run, not
when the code looks right.

### G8 — CLOSED. The startup sequence advances past the SEGA screen

1. Identify the repeated post-cache wait from actual guest calls and state.
2. Repair the measured runtime/translation boundary, with regression coverage.
3. Demonstrate the next genuine visible startup/menu state in a bounded
   fresh-HDD run, preserving the working logo and reporting new unsupported
   graphics state explicitly. Do not force a game-state transition.

The earlier evidence — counted-path and ten-argument directory ABI repairs —
was necessary and is unchanged. It was not sufficient: the title recognized its
cache and still sat on the logo.

**Completed:** the lifter emitted no code for `FCMOVcc`, so JSRF's `fminf`
(`sub_0014C870`) and `fmaxf` (`sub_0014C850`) returned their second argument.
The colour packer `sub_000A4CF0` clamps every channel with `fminf(c, 1.0f)` then
`fmaxf(c, 0.0f)` and writes the result back, so it stored 0.0f into all four
channels and packed a colour of 0. `sub_00024400` renders nothing unless
`+0xBC & 0xFF000000`, so the fade never drew and the startup script never
advanced. Fixed in `tools/recomp/lifter.py` with
`tools/recomp/test_lifter_fcmov.py`, backported to the generated tree by
`diagnostics/jsrf_first_fault/backport_fcmov.py` and gated by the
`jsrf_fcmov_backport` ctest. Full account in
`CLAUDE_PROGRESS_2026-09-03_FCMOV.md`.

Fresh-HDD `claude-fcmov-17` captures the CRI ADX logo and the anti-graffiti
legal notice, both new and both produced by game logic, and still captures the
SEGA logo byte-identical to `codex-graphics-09/frame003`. Newly reached
untextured draws are rejected and reported (`135030 texture 0 disabled`), not
silently accepted; vertex rejections stay at zero.

**Completion:** a different recognizable startup/menu frame produced by game
logic, plus passing file/graphics regression tests and updated evidence.

### G9 — ACTIVE. The guest heap is exhausted before the title screen

The disc-error dialog is **gone** as of Codex's cross-block flag-merge fix. The
`jle` at 0x0013D3F3 is reached from two predecessors that both `cmp` but name
different registers; the older translator discarded the flag state, the branch
could never be taken, and the loader recorded status -1 and opened
`JSRF_FATAL.ERR`. See `CLAUDE_PROGRESS_2026-09-03_FLAGS.md`.

That removed the consequence, not the cause. `claude-flagmerge-32` writes no
fatal marker and still fails 207 allocations, still stops on black after the
anti-graffiti notice, and still shows the leak unchanged:
`MmAllocateContiguousMemoryEx` 830 allocs / 32,483,736 bytes against
`MmFreeContiguousMemory` 227 frees / 2,203,648. The two were real and separate.

**This is now the blocker for G13, and the timeline proves it**: the stage
loader exhausts the arena and all file I/O stops at the same moment, 52,000 log
lines before the run ends. See G13 for the evidence and the specific fix.

**It is not a leak.** Codex's resource probes show all 235 resource allocations
succeeding, Release working (126,000 calls, all decrements), and the texture
cache holding 235 *distinct* resources in 233 *distinct* slots with zero
replacements -- the title's real startup working set. What consumes each arena
increase is `ordinal 184 ra=0x0014903F`, a doubling allocator that retains every
previous segment: 1+2+4 MB at a 52.9 MB arena, 1+2+4+8 MB at 61.8 MB. Giving it
4 MB more makes it take 8 MB more. Enlarging the arena is worse than neutral.

The question is now why that heap wants 15 MB of segments when the same code
fits on a 64 MB console. Check whether our `NtAllocateVirtualMemory` makes its
growth decision take a different branch before anything else: three of this
session's four bugs were exactly that.

A related audit: 110 conditional branches in this tree still reach the `_flags`
fallback, where the condition is a constant zero and the branch can never be
taken. Eleven are in the render/texture range and all but `sub_0014B536` have no
static caller or dispatch entry, so they are very likely cold — but the tree
cannot say which of the 110 matter. The lifter now marks them
`UNRESOLVED FLAGS`, `audit_unresolved_flags.py` counts them, and the
`jsrf_unresolved_flags_ratchet` ctest pins the number at 110.

**Repaired so far**, with regression coverage in `heap_alloc_test.c` and the
account in `CLAUDE_PROGRESS_2026-09-03_HEAP.md`:

- The reuse path rounded every split to a 4 KB page whatever the caller's
  alignment, so `ExAllocatePoolWithTag` held 3,126,516 bytes to satisfy 400,660
  bytes of 16-byte-aligned requests. Splits now happen at the caller's
  alignment, and a leading fragment is carved off so a misaligned free block can
  still serve a page-aligned request. Slack 4,581,336 → 1,936,552 bytes.
- The bump frontier and the free list could not combine. `unreached` at failure
  went from 905,028 to 0.
- `XBOX_STACK_SIZE` was 8 MB and had never been measured. Painting the region
  puts the main stack's high-water at 3 KB across a full startup, with the
  worker slices untouched; reduced to 6 MB, which keeps the slices' 4 MB and
  returns 2 MB to the arena.
- Reclaiming the bump path's alignment padding was tried and reverted: that
  padding is the tail of a page-granular allocation the title owns and writes
  into, and handing it out stopped the title dead at `IoCreateDevice`.
- The fixed low block (primary TLS, kernel data exports, stack) sat at
  `0x00700000` to clear any XBE. JSRF's image ends at `0x00288620`, so 4.5 MB of
  dead address space was charged to the arena. `g_xbox_low_base` is now derived
  from the loaded section extents, and the arena is 54 MB.

**Enlarging the arena is not the answer, and that is now measured.** Across four
arenas — 50,855,936, the same with the allocator fixes, 52,953,088 and
57,606,144 — the title's live set at failure was 49,301,920, 49,407,608,
52,188,296 and 55,424,136: between 96% and 99% of whatever it was given, every
time. The largest arena reached no screen the previous one had not. A bounded
working set settles; this does not. No further effort should go into making the
arena bigger.

The title now reaches a fourth genuine screen, **Created by Smilebit**, and
still keeps the SEGA logo byte-identical to `codex-graphics-09/frame003`.

**What remains, measured.** Per-export tallies name it exactly:
`MmAllocateContiguousMemoryEx` 673 allocations / 31,409,696 bytes against
`MmFreeContiguousMemory` 183 frees / 1,765,376 bytes — roughly 29.6 MB taken and
never returned, 20.3 MB of it from one call site, `ra=0x0018E6E9`, across 231
live blocks. `NtAllocateVirtualMemory` by contrast churns 146 MB and returns
131 MB, so the free path itself works and no memory export is unbridged.

`0x0018E6E9` and every other `ordinal 166` return address is a call site of the
title's own D3D8 allocator vector at `MEM32(0x1C40F8)`; the sibling release slot
`MEM32(0x1C40FC)` has exactly one call site, `0x00191A8B`. Instrument that and
the refcount test above it, and establish whether the title declines to release
or whether the deciding branch is mistranslated -- the FCMOVcc bug was that
shape. The harness's `[D3D8-HLE]` layer is *not* involved: `main.c` brings it up
as a probe and nothing in JSRF routes through it.
Pool memory is never freed either (922 allocations, one `ExFreePool`) and two
1 MB pure `MEM_RESERVE` calls are charged real RAM, but together those are worth
about 2 MB against 30.

**Completion:** the contiguous allocations are released as the title expects, a
fresh-HDD run reaches the title screen without raising the disc error, and the
bytes recovered are accounted for by measurement rather than by enlarging the
arena.

### G1 — CLOSED. The title's vertex programs do take effect

`nv2a_pb_exec.c` consumes `SET_TRANSFORM_PROGRAM`, `_PROGRAM_LOAD`,
`_PROGRAM_START` and `_CONSTANT`, yet the decoded program never changes, while
the pusher's own histogram counts `0x0B00`–`0x0B4C` unhandled 1,231,748 times
each. Two layers disagree about who consumes those methods. Establish which
before writing any renderer code.

**Completion:** a late-batch sample reports a `slots` and `start` that change
with the title's uploads, and `oPos` varies between vertices of one batch.

### G2 — Shader inputs with no vertex array

The program at batch 20,000 reads inputs 0,1,3,4,7,8,9,10,11,12
(`reads=0x1F9B`) and only 0,3,4,9 have declared arrays. The rest silently take
current-vertex defaults. Establish whether the title sets them through
`SET_VERTEX_DATA4F`/`2F` and whether those registers are tracked.

**Completion:** every input a running program reads is either backed by an
array or by a value the title actually set; no input is served an unset
default without that being reported.

### G3 — CLOSED. Geometry lands somewhere real

Only after G1 and G2. 269 distinct methods reach the executor unhandled;
`SET_CLIP_MIN`/`MAX`, the window clip, `SET_TEXTURE_CONTROL0` and the zeta
surface offset are the most frequent. Implement what the measured draws need,
in the order their absence is shown to matter.

**Completion:** a dumped frame that is not byte-identical black, produced by
the title's own geometry rather than a synthetic pattern.

### G4 — CLOSED. The cache completes

Asset opens stop at 290 distinct paths and the run then cycles reads and
allocations. That is equally consistent with cache construction still in
progress and with a loop that never completes. This is a measurement, not a
theory, and it gates everything after loading.

**Completion:** either `JSRF_CACHE_COMPLETE01.CMP` is finished and the file
set stops growing, or the repeating work is identified by call site.

### G12 — CLOSED. The title renders its loading screen

This is the blocker for visible progress, and it is not the heap.

After the anti-graffiti notice the title is not stuck: `claude-worker-34` shows
DMA_PUT advancing, draw #978,000, resources still being created and released,
and no fatal marker. It has moved on and is rendering. Two things throw the
result away, and they are in this order.

**First, the positions collapse.** `RECOMP_COMBINER_TRACE=1` groups the whole
run into three configurations. Config 2 is 509,310 draws -- every rejected one
-- and its `collapsed-xy` count equals its draw count: every vertex in every
batch transforms to the same X and Y, so the geometry has no area. The shader
does run and its other outputs are real:

```
[VSH] start=0 slots=12 vertex=0 oPos=(0 0 0 31.3942) color=FF007272
[VSH] start=0 slots=12 vertex=1 oPos=(0 0 0 31.3942) color=FF007272
[VSH] start=0 slots=12 vertex=2 oPos=(0 0 0 31.3942) color=FF007272
```

X, Y and Z are exactly zero while W is computed and the diffuse colour is
plausible. Compare the screens that do render, which use pre-transformed
screen-space positions: `oPos=(0 0 0 1)`, `(2560 0 0 1)`, `(0 1920 0 1)`.

This is not G2: the program reads inputs 0 and 2 (`reads=0005`) and both have
declared arrays -- `a0(t2 s3 st32 @01142000) a2(t2 s2 st32 @01142018)`, in a
live 128-byte block.

**W is provably correct, which localises the fault to the xyz writes.** The
constant file holds two matrices, and only 14 of 192 constants are non-zero:

```
c[103] = 1 0 0 0          c[107] = 1 0 0 -32
c[104] = 0 1.33333 0 0    c[108] = 0 1 0 24
c[105] = 0 0 0.113029 -135.529   c[109] = 0 0 1 1200
c[106] = 0 0 0.0260836 0  c[110] = 0 0 0 1
```

A projection matrix (4:3 aspect) and a view matrix (translation -32, 24, 1200).
The observed `W = 31.3942` is exactly `c[106].z * (z + c[109].w)` for an object
z of about 3.6: the view transform, then the projection W row. So the constants
arrive, the attributes are non-zero, the dp4 path works, and the program reads
the right registers -- for W. Only x, y and z come out zero, and `c[103] =
(1,0,0,0)` would need every vertex at x = 32 for that to be legitimate.

**FIXED.** Dumping the decoded program against the constant file named it. The
program's last two instructions are the viewport transform:

```
10  mul oPos.xyz (om=E) = R12.xyz * c58     | rcc R1.x = R12
11  mad oPos.xyz (om=E) = R12.xyz * R1 + c59
```

`om=E` is x, y and z only, which is exactly why w -- written at instruction 9
and never touched again -- stayed correct while xyz did not. And c58 and c59
were zero.

They are not ordinary constants. NV2A keeps the viewport scale and offset at
fixed slots in the vertex constant file: `NV_IGRAPH_XF_XFCTX_VPSCL` = 0x3a = 58
and `NV_IGRAPH_XF_XFCTX_VPOFF` = 0x3b = 59, written through
`NV097_SET_VIEWPORT_SCALE`/`_OFFSET` rather than `SET_TRANSFORM_CONSTANT`.
`nv2a_pb_exec.c` recorded both methods for the rasteriser and never mirrored
them into the constant file, so every program that reads them multiplied by
zero.

Mirroring them is the whole fix. `collapsed-xy` for config 2 went from 714,240
to **0**, and the SEGA logo stays byte-identical to
`codex-graphics-09/frame003`. Covered by `vsh_render_test.c`, which asserts both
slots land and that a neighbouring slot is not scribbled on.

Two candidates ruled out against xemu on the way, recorded so they are not
re-derived:

- Constant delivery matches `pgraph.c` exactly -- `slot = (method -
  NV097_SET_TRANSFORM_CONSTANT)/4`, write `[const_load][slot % 4]`, increment
  `CONST_LD_PTR` when `slot % 4 == 3`. Not the bug.
- The ±96 D3DSCM correction is a red herring. xemu's `convert_c_register` is
  `(((c>>5)&7)-3)*32 + (c&31) + 96`, which is the identity for every 8-bit
  input -- its own source says so (`FIXME: = c_reg?!`). Our plain index is
  equivalent. Not the bug.

**Second, the fragment stage — also fixed.** Those same draws are also
rejected by `nv2a_texture_copy_prepare`'s first line, because
`NV097_SET_TEXTURE_CONTROL0` bit 30 is clear:

```
[GPU] draws 967607, 13181073 indices; rasterised 72107 triangles
[TEXTURE] prepared=67277 rejected=900330 -- all "texture 0 disabled"
```

An untextured stage cannot sample T0, so it needs a diffuse-only program, not
the texture-times-diffuse one already implemented. Do what G6 did: group the
rejected configurations and implement the one that accounts for most first.
This was second because fixing it alone would have rendered nothing while the
geometry had no area.

**Completed.** The combiner input word packs each of A..D as [7:5] mapping, [4]
alpha, [3:0] register. The measured textured stage 0 is colour `0x08040000` --
A = register 8 (texture 0), B = register 4 (diffuse), so `T0 * V0`. With no
texture bound the title issues `0x04200000`: A = diffuse, B = register 0 under
mapping 1 (unsigned invert) = 1, so the stage is `V0 * 1`. Alpha likewise:
`0x18140000` becomes `0x14200000`. Stages 1..3 are byte-identical in both and
the shader stage program is 0 because nothing samples.

`nv2a_texture_copy` accepts that program with `untextured`, skips sampling and
reads the stage as opaque white so the existing modulate path multiplies the
diffuse in. `nv2a_pb_exec` no longer resolves a texture DMA object for it --
the stale handle from the last bound texture was rejecting every diffuse-only
draw with "texture DMA range" once the combiner check passed.

Result, `claude-untex-42`, fresh HDD, 90 s:

```
[TEXTURE] prepared=139463 rejected=0
[GPU] rasterised 1557266 triangles     (was 40950)
```

**Zero rejections**, and the title renders its **"Now Loading"** screen with a
live animated progress bar -- 52 distinct frames in one run, where every
previous run had at most six. The four startup screens are unchanged and the
SEGA logo is still byte-identical to `codex-graphics-09/frame003`.

**Completion:** met. `collapsed-xy` 714,240 -> 0, "texture 0 disabled"
432,960 -> 0, the SEGA logo unchanged, and "Now Loading" is a recognizable frame
none of the four startup screens contain.

### G13 — ACTIVE. Get from the loading screen into a rendered stage

G12 left the title on its own animated "Now Loading" screen, and it is not
idling there. In `claude-untex-42` it is streaming real level content:

```
Z:\Media\Stage\Stg12_08.dat   Z:\Media\Stage\Stg12_t.dat
Z:\Media\StgObj\StgObj00.dat  StgObj11  StgObj31  CarObj01
D:\Media\Z_ADX\BGM\
```

Stage 12 geometry, stage objects, a car object and the background music
streams. The ADX tick advances (6059 -> 6158 over the sample), so the audio
decode thread is running rather than stuck, and no fatal marker is written.

**Measured: this and G9 are the same problem.** A 240-second run
(`claude-stage-43`) puts the timeline beyond doubt. In a 64,251-line log:

```
first file read                     line     364
out-of-memory failures            lines  11,607 - 11,918
last file open of any kind          line  11,927
last file read                      line  11,933
                                    ... 52,000 further lines, no I/O at all
```

The loader streams stage assets, exhausts the heap, and stops. Every subsequent
line is the loading screen animating over a load that can never finish. The
title spins on 3.9M `RtlEnterCriticalSection`/`RtlLeaveCriticalSection` pairs
waiting for workers that have nothing left to do. It is not waiting on audio --
the ADX tick advances to 16,536 -- nor on input, and no fatal marker is written.

The failing requests are `MmAllocateContiguousMemoryEx` from `ra=0x00199789`,
128 to 2,880 bytes each, every one page-aligned as hardware would. They fail
because the arena is full, and the largest single holder of it is the one thing
that should cost nothing:

```
ordinal 184 ra=0x0014903F   4 blocks  15,728,640 bytes   1 + 2 + 4 + 8 MB
```

That is the guest heap's segment reserve, issued with `AllocationType = 0x2000`
-- `MEM_RESERVE` with no `MEM_COMMIT`, confirmed in the guest code at
`0x0014900B` where `ebx = 0x2000` is pushed. On hardware a reserve takes address
space, not pages. We charge it 15.7 MB of a 58 MB arena.

**Step 0 answered this, and the answer was no.** Before separating reserve from
commit, measure whether the title commits what it reserves. It does: of the
first four reservations (1+1+2+4 MB) it commits 8,331,376 of 8,388,608 bytes --
**99%**. The pages are owed either way, so the separation buys almost nothing
and the work below was not done. Recording it because the reasoning is what
saves the next person the same days.

**What did work: honour the requested thread stack size.**
`PsCreateSystemThreadEx` takes `KernelStackSize` as argument 2 and the bridge
ignored it, handing every thread the fixed `XBOX_THREAD_STACK_SIZE` of 512 KB.
JSRF asks for 65,536. Four workers, 458,752 wasted each: 1.83 MB. Honouring it
(clamped to a 64 KB floor and the old value as the cap) took the thread-stack
total from 2,097,600 bytes to 262,592, and **out-of-memory failures went from
205 a run to 0**.

Also fixed on the way: `bridge_NtResumeThread` passed
`XBOX_TO_NATIVE(STACK_ARG(1))` unguarded, and `XBOX_TO_NATIVE(0)` is the base
of the guest mapping rather than NULL -- a caller passing the optional
`PreviousSuspendCount` as NULL had four bytes written to guest address 0, up to
39,130 times a run. `NtSuspendThread` beside it already guarded.

**The load still does not complete, and it is no longer memory.** With OOM at
zero the title still stops all file I/O at line 11,452 of 45,364 and animates
the loading screen for the remaining three quarters of the run. What it does
instead is run its own cooperative scheduler:

```
ord 224 NtResumeThread             39,130     ord 246 ObReferenceObjectByHandle  64,152
ord 231 NtSuspendThread            37,883     ord 250 ObfDereferenceObject       64,152
ord 143 KeSetBasePriorityThread    42,762     ord 159 KeWaitForSingleObject      17,760
ord 277 RtlEnterCriticalSection  3,936,779    ord 294 RtlLeaveCriticalSection 3,931,674
```

all from a handful of sites around 0x00147C7A-0x00147E7F.

**Measured, and the scheduler is not the problem.** `RECOMP_SCHED_TRACE`
records the calling thread, target handle and previous suspend count for every
scheduling primitive, and the outcome of every wait:

```
suspend counts observed          0 and 1 only -- bounded, nothing accumulates
wait object=0x0019D630 type=0    entries=8000 woken=8006 timed-out=0
```

Four guest threads suspend and resume each other with correct counts, and every
wait is satisfied rather than timing out. The last file read is a clean
51,200-byte success with no error and no retry, and then the loader simply
stops asking for data.

**It stops because it has finished.** The last assets it opens are

```
Z:\Media\Mark\PRESS\JSRF_TEXS_0.JTX
Z:\Media\Mark\PRESS\JSRF_TEXSx0.JTX
```

the PRESS START textures. The title has loaded its startup content and is
waiting for a button. **G13 is therefore blocked on G5, not on memory, not on
the renderer and not on the scheduler** -- and G5 has not been started.

xemu was worth consulting twice on the NV2A side and is not needed here: this
is not a semantics question, it is a missing input path.

(The reserve/commit note that follows is kept for whoever revisits it.)

**The fix considered and deferred is the one `bridge_NtAllocateVirtualMemory` already names in its
own comment: a reserve that costs nothing and a commit that backs pages on
demand.** `xbox_ReserveAlloc` exists for this and is unreachable here: it
requires `g_memory_size > g_xbox_total_ram`, and raising the map size makes
`XBOX_HEAP_TOP` exceed `XBOX_CONTIG_SIZE` (64 MB), which
`xbox_EnablePhysicalHeapAlias` refuses -- and JSRF's renderer depends on that
alias. Separating "how much RAM" from "how much address space the heap may
serve" is the knot to untie; do that before writing commit-on-demand.

**Completion:** a fresh-HDD bounded run captures a frame of stage geometry --
not a logo, not the loading screen -- produced by game logic, with the startup
screens and the SEGA logo unchanged and any newly reached unsupported state
reported rather than silently accepted.

### G11 — 110 branches can still only go one way

`_flags` is the lifter's fallback when no flag state reaches a conditional
branch. The translator initialises it to zero and only the rep-string and xadd
paths ever write it, so for a jcc the condition is a constant zero and the
branch is never taken. That is arbitrary, not conservative, and it is what cost
the loader its status at 0x0013D3F3.

`audit_unresolved_flags.py` counts 110 survivors in 78 functions after Codex's
merge fix. Eleven are in the render/texture range and all but `sub_0014B536`
have no static caller and no dispatch-table entry, so they are very likely cold;
nothing shows JSRF executes any of them. The lifter now marks them
`UNRESOLVED FLAGS` and `jsrf_unresolved_flags_ratchet` pins the count.

Widen `_merge_predecessor_flag_states` only where the merge is provably sound —
a wrong condition is worse than a marked unknown one. Where it cannot be, the
honest options are recomputing the flags at the join or refusing to translate
the function, not a constant.

**Completion:** the count falls, with a lifter regression for each shape newly
resolved, and the ratchet lowered to match.

### G10 — The pushbuffer executor can write over the guest

`NV097_SET_SURFACE_COLOR_OFFSET` is an offset inside the colour DMA object, not
a guest VA, and `nv2a_pb_exec.c` treats it as one. Upstream `0655e8e` shows what
that costs when the two disagree: on the Dashboard a clear wrote 4.9 MB over the
loaded XBE, and the title spun in a pushbuffer retry loop with no fault and no
message. JSRF is unaffected today -- its surface is a real heap allocation at
`0x01160000` and the logos render correctly -- so this is latent, not urgent.

Take upstream's guard (refuse a surface write overlapping the loaded image,
once, with the reason) and its DMA_GET-beside-DMA_PUT reporting; GET stuck
behind PUT is the shape of a pushbuffer-full hang and we log only PUT. Getting
the address genuinely right needs `NV097_SET_CONTEXT_DMA_COLOR`, which is the
larger fix behind it.

**Completion:** a surface write that would land inside the loaded image is
refused and reported, with a regression test, and JSRF's rendering is unchanged.

### G5 — ACTIVE, and now the blocker. Real host input to the guest

Promoted from "not started": G13 ends here. The title loads
`Z:\Media\Mark\PRESS\JSRF_TEXS_0.JTX`, stops all file I/O with a clean final
read, and animates while four healthy threads wait for a button press. Every
other candidate has been measured and excluded -- the arena no longer runs out,
the scheduler's suspend counts are bounded and its waits are woken, and the
renderer accepts every draw the title issues.

The OHCI root hub has ports and completes reset, but nothing enumerates; that
needs descriptors and control transfers through the HCCA, which is device
emulation.

**Surveyed 4 September; here is where it actually stands.**

There is already a host-side backend, `src/input/xinput_device.c` -- SDL game
controllers, plus a local `RECOMP_FAKE_PAD=1` that pulses A and START. It is
byte-identical to upstream apart from that addition, and **nothing calls it**:
`xbox_InputGetState` has no caller in the harness or the bridges. JSRF does not
go through XAPI-on-the-host at all; it drives OHCI itself from `sub_001A1E74`
and `sub_001A52F7`, writing 0xFE801xxx and 0xFE802xxx directly.

Upstream is no help: `git grep -i "ohci\|HCCA" upstream/main -- src` is empty.
This tree is the furthest along.

With `RECOMP_OHCI_ATTACH=1` the state is better than "not started":

```
[OHCI] attach probe: port1=0x00010001   CCS set, connect-status-change set
HcControl       0x000000BE              UsbOperational, all four lists enabled
HcCommandStatus 0x00000000
HcInterruptStatus 0x00000040            RHSC standing
HcInterruptEnable 0x00000040            RHSC enabled
HcHCCA          0x009E2000              the title has published its HCCA
```

The title has a working controller, an HCCA, and RHSC enabled -- and it
registered the ISR for it: `KeConnectInterrupt routine=0x001C288F
context=0x009E4278 vector=1`, which is USB0.

**Step 1 turned out to be already done, and the gate is now known exactly.**
`bridge_device_irq_poll` already runs every non-NV2A ISR periodically, so the
USB handler at 0x001C288F is called and has been all along. It declines, and
its own code says why:

```
ecx = MEM32(esi)          ; register base from ServiceContext
eax = MEM32(ecx + 0x10)   ; HcInterruptEnable
edx = MEM32(ecx + 0x0C)   ; HcInterruptStatus
edx &= eax                ; gate 1: status & enable   -- we pass this
if (edx == 0) decline
test 0x80000000, eax      ; gate 2: MasterInterruptEnable
if (zero) decline                                     -- we fail here
```

The title writes `HcInterruptEnable = 0x40` -- RootHubStatusChange, no master
enable -- and never writes bit 31 in any run measured. So it is not yet
interrupt-driven for the root hub, and something else has to make it look at
the port. Forcing MIE as a probe (`RECOMP_OHCI_MIE=1`) does not help: it fires
before the title's own write, which then replaces it.

Chasing that did find a real modelling bug worth having. `HcInterruptEnable`
(0x10) is write-1-to-set and `HcInterruptDisable` (0x14) is write-1-to-clear,
and both read back the same mask; this aperture is plain memory, so every write
replaced the register instead of accumulating. `xbox_McpxHoldRegisters` now
shadows the pair. It is correct hardware behaviour and it is *not* the blocker
here, because the master bit was never set to be lost.

**Step 1 is answered.** The driver is the XBE's `XPP` section (0x001BC7C0,
30,616 bytes) -- not `sub_001A1E74`/`sub_001A52F7`, which are APU code writing
the 0xFE801xxx/0xFE802xxx aperture and were misidentified earlier.

`sub_001BD108` is the init. It opens with

    eax = MEM32(0x1C40BC); if (MEM8(eax + 5) == 0xA1) return;

and that gate passes -- measured `[0x1C40BC]=0x002D0000 byte[+5]=0xB1` -- so it
maps 0xFED00000 for 0x1000 and creates its device. The stack comes up.

`sub_001BD295` is the root-hub handler:

```
ebx = MEM32(esi)              ; OHCI register base
MEM8(esi + 0x460) = 4         ; four ports
eax = MEM32(ebx + 0x50)       ; HcRhStatus, acked by clearing the low half
eax = ebx + 0x54              ; &HcRhPortStatus[0]
  edi = MEM32(eax)            ; per port
  test MEM8(..), 1            ; CurrentConnectStatus
  ... set this port's bit in a bitmap ...
  MEM32(eax) = 0              ; ack the port
MEM32(ebx + 0x10) = 0x40      ; HcInterruptEnable = RootHubStatusChange
sub_001C2220(esi, &bitmap)    ; act on the changes
```

That `0x40` is the write we have been observing, so this routine has already
run -- once, during init, when no port was connected. It acks and then waits
for the next RootHubStatusChange interrupt. From that point the driver is
interrupt-driven, and the ISR needs `MasterInterruptEnable`.

**So MIE is set during controller start and we lose it.** Both writes happen
microseconds apart in init: the start routine sets MIE, `sub_001BD295` then
writes 0x40. The set/clear shadow added in `xbox_McpxHoldRegisters` cannot see
that -- it samples on a poll, and the second write replaces the first between
two polls. The fix is to put `HcInterruptEnable` (0xFED00010) and
`HcInterruptDisable` (0xFED00014) behind the **MCPX write trap**, which already
guards a page and already models write-clear registers, so set/clear applies at
write time instead of sample time.

Then the attach probe's connect will raise a real interrupt, the ISR will
claim it, and `sub_001C2220` will get a bitmap with a port in it.

**Step 1 is done.** `HcInterruptEnable`/`HcInterruptDisable` are now handled by
the MCPX write trap: a 1 written to Enable sets that bit, a 1 written to Disable
clears it from Enable, and Disable reads back the enable mask. The guest's own
writes confirm the diagnosis was right --
`MEM32(x + 0x10) = 0x80000000` appears in the controller-start path and again in
the ISR's re-arm at `loc_001C2911`, and `sub_001BD295` writes `0x40` shortly
after. As plain memory the second write threw the master bit away.

With the pair trapped:

```
HcInterruptEnable 0x80000073         MIE survives alongside RootHubStatusChange
device ISR vector 1 -> TRUE (handled)   was FALSE for every run before this
ord 119 sites: 0x001C290F ...        the ISR now queues its own DPC
```

The ISR claims the interrupt and queues its DPC, and `bridge_run_isr` runs
queued DPCs after the handler returns, so the driver is being driven properly
for the first time. Nothing regressed: 132,440 draws prepared with zero
rejections, no out-of-memory, no fatal marker, the startup screens unchanged.

It does not yet enumerate, which is expected -- that is step 2. The driver now
gets as far as asking questions this has no answers for.

Remaining, in order:
(2) **started.** Guarding the interrupt pair guards the whole OHCI page, so
    `RECOMP_OHCI_TRACE=1` now prints every register write the driver makes --
    the entire conversation, in order, instead of the specification's version
    of it. It showed the next fault immediately:

    ```
    #19 HcRhPortStatus0 <= 0x00010001   the probe: connect + change
    #21 HcInterruptEnable <= 0x80000033  driver sets MIE
    #23 HcRhPortStatus0 <= 0x00010000    driver acknowledges CSC
    ```

    `HcRhPortStatus` is not a value register either: the low half is commands
    (a 1 to bit 1 enables, bit 4 resets, bit 8 powers) and the high half is
    five write-1-to-clear change bits. As plain memory the acknowledge stored
    `0x00010000` and wiped CurrentConnectStatus with it, so the device vanished
    the moment it was noticed. `ohci_port_write` now models both halves, and a
    reset completes in place -- clearing PortResetStatus, setting
    PortEnableStatus and raising PortResetStatusChange with the root-hub status
    change, because there is no 10 ms to wait for here.

    `mcpx_hw_store` is the other half of that: the runtime announcing a connect
    means "this is now the value", which is the opposite of what a guest write
    means, so the attach probe drops the guard rather than going through the
    semantics and acknowledging the change it is announcing.

    Measured: `HcRhPortStatus[0] = 0x00000001` and it stays there across the
    acknowledge. The driver has a device that does not disappear.

    Still to do: it has not issued the port reset yet, and the control
    transfers on the default endpoint through the HCCA at 0x009E2000 have no
    service behind them. That is the remaining bulk of the device model.

(3) answer interrupt-IN with pad reports fed from `xbox_InputGetState`, which
    already exists and already has a synthetic pad for bring-up.

Only step 3 is title-facing; steps 1 and 2 are the device model, and every
title needs them.

**Completion:** a host key or pad action changes guest-visible state.

## Upstream, checked 3 September

`upstream` is `sp00nznet/xboxrecomp`, at v0.7.1. We are 41 commits ahead and 11
behind. Nothing upstream touches contiguous-memory lifetime -- `kernel_memory.c`
has not changed there since the initial import -- so G9 gets no help from it.
Two things are worth acting on:

- **`0655e8e` is a fix we do not have, in a file we have modified.**
  `NV097_SET_SURFACE_COLOR_OFFSET` is an offset inside the colour DMA object,
  not a guest VA, and our executor treats it as a VA. On the Dashboard that
  cleared 4.9 MB straight over the loaded XBE. It is benign for JSRF today --
  our surface is a real heap allocation at `0x01160000` and the logos render
  correctly -- but it is latent corruption and the guard is cheap. The same
  commit reports DMA_GET beside DMA_PUT, which is what distinguishes "finished
  submitting" from a pushbuffer-full hang; we currently log only PUT.
- **Upstream's lifter still drops `FCMOVcc`** (`git grep fcmov upstream/main --
  tools/recomp/lifter.py` is empty). The G8 fix and
  `tools/recomp/test_lifter_fcmov.py` are ours alone and affect every title with
  an x87 `fminf`/`fmaxf`, so they are worth a PR.

The other nine are D3D8 texture formats, a `ReleaseMutex` `ERROR_NOT_OWNER` fix,
and tooling.

## Non-goals for now

Sound stays bypassed. No Windows/D3D11 build. No full JSRF regeneration. No
upstream merge. Playable gameplay is a later milestone with no date.
