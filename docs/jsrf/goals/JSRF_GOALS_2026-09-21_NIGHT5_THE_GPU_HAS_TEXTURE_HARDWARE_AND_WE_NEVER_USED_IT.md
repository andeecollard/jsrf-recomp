# JSRF goals — the GPU has texture hardware and we never used it, 21 September 2026 (night, fifth)

> Superseded for ORDERING ONLY on the translator track by
> `JSRF_GOALS_2026-09-22_THE_LIFTER_FOR_THIS_TITLE.md` (22 Sep 2026). G27 stays the head of the renderer track.

Supersedes `JSRF_GOALS_2026-09-21_NIGHT2_THE_PROBE_THAT_CANNOT_MISS.md` for
ORDERING ONLY. G1–G26 carry forward and nothing there is retracted. This file
opens **G27** and puts it first, because it is the only thing measured this
week with order-of-magnitude headroom rather than percentage points.

**The target is unchanged** and is still the player's own words:

> I want everything on the gpu
>
> We want to hit 60fps and for the player to move at the correct speed

and as of tonight, explicitly:

> I want this to work better than xemu

## G27 — SAMPLE TEXTURES IN HARDWARE

**The finding.** The renderer contains no hardware texture sampling at all:

    grep -c 'MTLSamplerState|newSamplerStateWithDescriptor|texture2d<'  ->  0

`MTLTextureDescriptor` appears only for the depth texture, the stencil texture
and the render surface. Guest textures live in `const device uchar*` buffers —
`[METAL] texture buffers: 13,100,361 requests` — and `sample_lod()` samples
them **in software inside the fragment shader**: perspective divide,
`dfdx`/`dfdy` and a `log2` for LOD, then two `sample_level` calls, each
walking a Morton address, decoding DXT1/DXT3 by hand and doing its own
bilinear. Up to **eight software texel fetches** where hardware does one.

**Why this is the frame.** `sync` is `waitUntilCompleted`, so its 6.94–9 ms
per flip IS GPU execution time, for a **640×480** scene on an M1 Max. That
should be well under a millisecond. Measured budget, tutorial scene,
scene-matched, 0 faults:

| | ms/frame | % |
|---|---|---|
| `sync` — GPU execution | 8.9 | 46% |
| our CPU draw code | 6.0 | 31% |
| Metal API calls | 0.13 | 0.7% |

**This supersedes two earlier framings**, both of which this file retracts as
*the primary target* (neither is wrong, both are smaller):

1. *Direct GPU presentation is the largest opportunity.* It removes readback,
   snap, convert and upload and **leaves the drain** — `d3d8_gl.c` says so
   itself. The drain is the GPU doing software texture filtering.
2. *Per-draw CPU marshalling is the largest opportunity.* It is 31% against
   the drain's 46%, and it has no hot spot to remove — `RECOMP_VSH_SPLIT`
   measured 13,615 vertices/frame at 212 ns each, only 39 ns of it fetch.

**Three optimisations were measured to ZERO before this was found**, and they
are recorded so nobody spends the night on a fourth: Metal batching already
coalesces, the vertex upload already narrows to `vsh_active->nattrs`, CPU
triangle culling is already skipped under `vsh_gpu_culling`, and the
`RECOMP_VSH_HOIST_INPUTS` A/B moved a frame by 0.4% — inside variance.

### What to build

- Guest textures as `MTLTexture`, not `MTLBuffer`. Native **BC1/BC2/BC3** for
  DXT1/DXT3 (Apple Silicon supports them); de-swizzle the Morton layout
  **once at upload**. The ratio is the argument: **50,341 uploads against
  13.1 M sample requests**, so the cost moves to the right side of it.
- A real `sampler` carrying the guest's filter, wrap and LOD-bias state, so
  `dfdx`/`dfdy`/`log2`/mip-blend stop being shader arithmetic.
- Then **early-Z**: `fs_hw_early` with `[[early_fragment_tests]]` already
  exists and is gated off, and the run reads `0 draws early, 7807313 late`.
  The comment above it states the cost — "every occluded fragment in the
  scene pays the whole combiner chain and up to four texture samples first".

Format frequency from the player's session, which sets the order of work:

| format | seen | as |
|---|---|---|
| 0x0C dxt1 | 5,136,742 | BC1 |
| 0x06 rgba8 | 5,010,711 | RGBA8Unorm, de-swizzled |
| 0x0E dxt3 | 2,864,629 | BC2 |
| 0x07, 0x03, 0x11, 0x04 | < 30,000 each | later |

### The ranked backlog, 22 Sep 2026

Measured frame budget (tutorial, 19.46 ms, 66 draws, scene-matched, 0 faults):
`sync` 8.95 ms (46%, and it is GPU EXECUTION time), our own draw code 6.03 ms
(31%, 91 us/draw), Metal API calls 0.127 ms (0.7%).

| # | item | evidence | size |
|---|---|---|---|
| 1 | **Hardware texture sampling** | 0 `MTLSamplerState` in the renderer; up to 8 software texel fetches per sample | days |
| 2 | **Early-Z** -- SCORED 22 Sep, see below | unreachable as predicated; ceiling measured at **2.3 ms/frame** | days, not hours |
| 3 | **Stop copying into Metal** | `newBufferWithBytesNoCopy` uses: **0**; guest RAM is mmap'd and the M1 is unified, as the Xbox was | days |
| 4 | **Vertex fetch on the GPU** | `prepare_vertices` 47 us/draw = half the per-draw cost; 13,615 vertices/frame at 212 ns, fetch only 39 ns of it | days |
| 5 | **Texture cache memcmp** | 35,280,441 cache hits, each a FULL memcmp of the texture against guest RAM | hours |
| 6 | **Surface-cache thrash** | 196 evictions / 1,097 rebuilds, 194 in one mission; `clear refused: geometry` tracks 1:1. Also the flicker lead | days |
| 7 | **Shader blend mode 3** | every hw draw reads the destination under `raster_order_group(0)`. Lower modes are documented as rendering INCORRECTLY -- care, not a flag flip | days |
| 8 | **Present via Metal, not GL** | removes readback+snap+convert+upload; leaves the drain, so it follows #1 rather than leading | days |
| 9 | **Uncached `getenv` on hot paths** | one fixed on a ~1M iter/s loop, caught in both freeze samples under `_os_unfair_lock_lock_slow`; the audit counts 150 hand-rolled switch reads | hours |
| 10 | **Instrumentation cost in play sessions** | the player's `paths.conf` emits 594 log lines/s and a 108 MB log; `RECOMP_METAL_CB_STATS` alone cost ~0.7 ms/frame | hours |

### G27b -- EARLY-Z IS UNREACHABLE AS PREDICATED, AND WORTH 2.3 ms IF MADE REACHABLE

Scored 22 Sep 2026, scene-matched tutorial (`nodes=61`), 0 guest faults, three
arms, `refusals=0` in all of them:

| arm | draws early | `sync` | frame | fps |
|---|---|---|---|---|
| `EARLY_Z=0` | 0 | 9.32 ms | 18.67 ms | 59.3 |
| `EARLY_Z=1` | **0** | 9.47 ms | 19.13 ms | 58.9 |
| `EARLY_Z=1 EARLY_Z_REF0=1` | **837,579** | **7.02 ms** | **16.36 ms** | **66.2** |

**The switch alone does nothing.** Arm 2 is the failed positive control that
started this: `early_z on` and still `0 draws early`. A counter added to the
predicate said why, and the answer is one number:

    early-Z predicate: 0 eligible, refused 1,368,416 for alpha_test
    (of which 1,368,416 have alpha_ref == 0), 20,582 for z_cull

**Every single alpha-test refusal has `alpha_ref == 0`.** The title enables the
alpha test globally and never sets a cutout threshold, so the shader's discard
is `alpha <= 0` -- it rejects a fully transparent fragment and nothing else.
That one state bit makes early-Z unreachable on every draw in the renderer.

**The ceiling is real: -2.3 ms/frame, -25% of the GPU drain, +7 fps.** It also
corroborates G27 from the other side: early-Z's whole benefit is skipping
fragment work for occluded pixels, so 2.3 ms is a LOWER BOUND on what occluded
fragments currently cost in software texture sampling. The two compound.

**`RECOMP_METAL_EARLY_Z_REF0` IS NOT A CANDIDATE DEFAULT.** It is a ceiling
measurement and it is knowingly inexact -- a fully transparent fragment can
write depth and occlude what is behind it. Exit criterion 2 (image equality)
is NOT met and was not attempted: `measure.sh` captures no frames, and the
frame-buffer signatures cannot be compared across arms that ran at different
frame rates against a time-based pad schedule.

**The exact version, and it falls out of work already done.** Since the only
discarded fragment is `alpha == 0`, a draw whose bound textures contain no
fully transparent texel provably cannot discard. `nv2a_texture_decode.c`
already walks every texel at upload to produce RGBA8 -- recording "this
texture has an alpha-0 texel" costs one comparison per texel on a path that
runs **86,512 times against 35,280,441 sample requests**. A draw with no such
texture bound then takes `fs_hw_early` exactly, with no approximation.

That is the real item, it is days rather than hours, and it should be built on
top of G27 rather than before it -- the decoder it needs is the same one.

**RULED OUT -- do not spend a night on these.** Each looked like the answer
and each is already done or measured to nothing: Metal batching is coalescing
(~183 draws/encoder); the vertex upload already narrows to
`vsh_active->nattrs`; CPU triangle culling is already skipped whenever
`vsh_gpu_culling` is on, which it is; and `RECOMP_VSH_HOIST_INPUTS` moved a
frame by **0.4%**, inside run-to-run variance.

**Order of work: 2, then 1.** Early-Z is a switch against an existing shader
variant, so it is an afternoon, and it tests whether the software-sampling
cost is where the measurement says before committing to the multi-day change.

### Exit criteria

1. A scene-matched `measure.sh` A/B on the switch, tutorial `nodes=61`,
   0 guest faults in both arms, showing `sync` ms/frame down. That is the
   number this goal is about; fps is the consequence, not the measurement.
2. Image equality against the software sampler on the same scene — this
   changes what every textured pixel is filtered by, so a frame-time win with
   an unverified picture is not a result. `metal_batch_check.sh` is the
   precedent for how that comparison is done here.
3. The switch is named in the report in **both** states, so `ab_score.py` can
   confirm the arms differed.

**OFF by default until both 1 and 2 pass.** The rule this tree learned from
`RECOMP_METAL_BATCH` — whose "revert to opt-in" was decided, written down in
three places, and never carried out in code — and re-learned tonight from an
ADX default that froze two player sessions.

## Ordering

> **UPDATED 22 Sep 2026: THE STABILITY GATE IS PASSED, SO G27 GOES FIRST.**
> A 2040 s player session held the ADX guard perfectly balanced --
> `locks=357285 unlocks=357285`, `held=0`, **271 contended, 0 stalls, 0
> poison** -- against sessions 11 and 12 which wedged by ~620 s. Three times
> the duration under three times the contention, no stall. The session ended
> in an UNRELATED guest fault at `sub_001A2E2E` (a corrupted pointer,
> `EAX=0xFFFFFFB4`), which is its own item and not the lock.
>
> The freeze hypothesis in the section below was refuted before it was
> tested; the guard now names its holder by thread id if it ever recurs. The
> ordering argument that follows is kept as the record of why G27 waited.

**G27 did not go first. Stability did.**

The ADX guard still deadlocks: sessions 11 and 12 both froze hard at ~620 s,
`held=1`, `waiter 1 / holder 2`, main thread parked in
`adx_guard_lock_enter` for good. `saved_priority` stayed healthy, so the
night-5 fix did hold — the defect underneath it, a holder that never
releases, did not.

**The host-thread-migration hypothesis is REFUTED**, before anyone spends a
session on it. If a guest thread took the lock on one host thread and
returned on another, the second thread's unlock would find `t_depth == 0`
and be counted UNMATCHED. Across **477,689 locks in three sessions** —
including both freezes — the count is `+0 UNMATCHED`. Lock and unlock always
pair on the same host thread. `RECOMP_ADX_LOCK_STEAL=1` is no longer the
experiment that splits this.

**What the evidence leaves.** `held=1` with `locks = unlocks + 1`: one thread
took the guard and never issued its unlock, and the session-12 sample shows
no thread inside the guest critical section. Of the four ADX threads, the
spinner is at 100%, both pumps sleep in `KeWaitForSingleObject`, and the
mwidle worker `sub_0013B2A0` is blocked **inside its own `SuspendThread`**.
A holder that took the lock and then suspended itself would look exactly
like this. That is the next hypothesis and it is not yet tested.

**The instrument that settles it** is small and is the first thing to build:
the guard prints `holder 2`, an internal counter, which names nothing. It
should print the holder's `pthread_threadid_np` — the same id `sample`
prints — so one freeze plus one sample identifies the holding thread by name
instead of by inference.

The audio glitching is probably the same fault one severity down. Output is
clean — 48000 Hz, `starved=0`, `ADPCM fail=0` — and the guest simply is not
refilling voices, gaps to 1.1 s. It is **not** frame-rate driven: correlated
inside one session, r = +0.183, effectively nil, and the weak sign is the
wrong way round. Renderer work will not fix it.

So: **ADX ownership, then G27.** Beating xemu on frame time while freezing at
620 s is not beating xemu.
