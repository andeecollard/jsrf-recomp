#include "nv2a_ff.h"
#include "d3d8_ring.h"
#include "../recomp_switch.h"
#include "frame_pool.h"
/* The accelerated raster path, under one set of names.
 *
 * macOS reaches it through Metal and Windows through D3D11. The two backends
 * present the same five entry points on purpose -- see the header comment in
 * nv2a_d3d11.c -- so every site below is host-independent, and a measurement
 * taken on one host means the same thing on the other. */
#if defined(__APPLE__)
#include "nv2a_metal.h"
#define NV2A_GPU_PATH        1
#define NV2A_GPU_SWITCH      "RECOMP_METAL"
#define NV2A_GPU_TAG         "METAL"
#define nv2a_gpu_draw        nv2a_metal_draw
#define nv2a_gpu_sync        nv2a_metal_sync
/* The range is now HONOURED rather than discarded. It used to read
 * `nv2a_metal_sync()`, so snapshot_surface passed the real flipped range and
 * got back whichever surface was bound -- harmless only because the swap
 * writes back eagerly, and the exact thing that would stop being harmless the
 * moment it does not. See nv2a_metal_sync_range. */
#define nv2a_gpu_sync_range(target, bytes) nv2a_metal_sync_range(target, bytes)
#define nv2a_gpu_invalidate  nv2a_metal_invalidate
#define nv2a_gpu_invalidate_range(target, bytes) nv2a_metal_invalidate(target)
#define nv2a_gpu_discard(color, depth) nv2a_metal_discard(color, depth)
#define nv2a_gpu_clear_color nv2a_metal_clear_color
#define nv2a_gpu_clear_depth_stencil nv2a_metal_clear_depth_stencil
#define nv2a_gpu_last_reject nv2a_metal_last_reject
#define nv2a_gpu_report      nv2a_metal_report
#elif defined(_WIN32)
#include "nv2a_d3d11.h"
#define NV2A_GPU_PATH        1
#define NV2A_GPU_SWITCH      "RECOMP_D3D11"
#define NV2A_GPU_TAG         "D3D11"
#define nv2a_gpu_draw        nv2a_d3d11_draw
#define nv2a_gpu_sync        nv2a_d3d11_sync
#define nv2a_gpu_sync_range  nv2a_d3d11_sync_range
#define nv2a_gpu_invalidate  nv2a_d3d11_invalidate
#define nv2a_gpu_invalidate_range nv2a_d3d11_invalidate_range
#define nv2a_gpu_clear_color nv2a_d3d11_clear_color
#define nv2a_gpu_clear_depth_stencil nv2a_d3d11_clear_depth_stencil
#define nv2a_gpu_last_reject nv2a_d3d11_last_reject
#define nv2a_gpu_report      nv2a_d3d11_report
#define nv2a_gpu_surface_report nv2a_d3d11_surface_report
#else
#define NV2A_GPU_PATH        0
#endif
/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * Geometry uses either pre-transformed attributes or the title's uploaded
 * NV2A vertex program. Shader outputs feed the bounded software rasteriser,
 * or the optional native GPU raster path (Metal on macOS, D3D11 on Windows). The supported fragment
 * subset includes the title's measured register combiners, RGB565/RGBA8/DXT1
 * textures, mipmaps, depth, blending, culling and dithering; every state that
 * remains unsupported is rejected and counted explicitly.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes surfaces to
 * <prefix><instrument>NNN.bmp, so the result can be looked at without a
 * display; the instrument is reportNNN, drawNNN, flipNNN (the live surface at
 * the present) or snapNNN (the copy the window is actually fed).
 * RECOMP_FLIP_TRACE=<stride> reports, inside the FLIP_STALL dispatch, what the
 * guest emitted between its last draw and its flip.
 */
#include "nv2a_vsh.h"
#include "nv2a_texture_copy.h"
#include "nv2a_drop.h"
#include "nv2a_regs.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>     /* ptrdiff_t; MSVC gets it via another header */
#include <time.h>      /* clock_gettime, for the opt-in traces below */
#if !defined(_WIN32)
#include <sched.h>     /* sched_yield, for flip_pace */
#define FLIP_PACE_YIELD() sched_yield()
#else
#include <windows.h>
#define FLIP_PACE_YIELD() SwitchToThread()
#endif

/* Where a frame's time goes, by stage.
 *
 * [FRAME] says a frame took 50 ms. It cannot say whether that was the guest
 * thinking, our CPU vertex pipeline, handing work to Metal, or blocking on the
 * GPU to hand something back -- and those four have completely different fixes.
 * A profile says which FUNCTIONS are hot across the whole process, which is not
 * the same question: this program is roughly 93% blocked, so the interesting
 * quantity is which stage the frame is blocked IN, and for how long per frame.
 *
 * Three stages are timed because three are separable at a call boundary:
 *
 *   vsh     prepare_vertices -- the CPU vertex pipeline, nv2a_vsh_execute and
 *           the attribute fetch underneath it. Per batch.
 *   prepare prepare_texture_copy -- decoding the method state into a draw
 *           description and resolving its three or more DMA objects. Per batch
 *           that survives the vertex stage. See the block below it.
 *   submit  nv2a_gpu_draw -- building and committing the command buffer. Per
 *           batch that survives to a draw.
 *   sync    nv2a_gpu_sync_range -- waiting for the GPU and reading the surface
 *           back to guest memory. Per snapshot.
 *
 * Everything else in the frame -- the guest's own execution, the pushbuffer
 * walk, method dispatch -- is reported as `rest`, by subtraction from the
 * frame time. It is a residual, not a measurement, and is labelled that way.
 *
 * Cost is two clock reads per batch, not per triangle. mach_absolute_time
 * appeared in a profile at 1097 samples, so this is deliberately coarse:
 * instrumenting per draw call rather than per primitive keeps it at a few
 * thousand reads a second against tens of millions of triangles.
 *
 * Both a run total and a per-report window, for the same reason [FRAME-WIN]
 * exists: a JSRF run is several workloads in sequence and the cumulative
 * average describes none of them. */
/* CLEAR is a stage because it was not one, and that is where a measurable part
 * of `rest` was hiding. clear_surface calls nv2a_gpu_invalidate_range with no
 * timer around it; on Metal that is nv2a_metal_invalidate, which syncs. A
 * sampling profile put 12.8% and 14.5% of the rendering thread's stacks under
 * one clear_surface path in two captures of one run, most of it waiting for a
 * command buffer or in the readback -- none of it attributed anywhere, because
 * `rest` is wall time minus the timed regions rather than anything measured.
 *
 * The GPU synchronisation inside a clear is attributed HERE and not also to
 * sync: the sync timer covers snapshot synchronisation only, and adding a
 * nested cost to two stages would make the stages overlap and the residual
 * meaningless. That is the whole point of splitting it out. */
/* PREPARE is a stage for the same reason CLEAR became one: it ran on the draw
 * path, once per batch, entirely outside every timer, so whatever it cost was
 * being reported as `rest` and read as guest CPU. prepare_texture_copy()
 * memsets a 400-byte NV2ATextureCopy, walks the combiner and texture method
 * state validating it, and then resolves two to five DMA objects through
 * nv2a_dma_resolve -- a LINEAR SCAN of the RAMHT hash table, up to 4096
 * entries. None of that was attributed anywhere.
 *
 * MEASURED before adding the timer, on this host, at the build's own -O2
 * (diagnostics/jsrf_first_fault/dma_resolve_stats_test.c --bench, a standalone
 * harness so the numbers do not need the title running):
 *
 *   nv2a_texture_copy_prepare       20 ns per call (the memset and the
 *                                   validation loops together)
 *   xbox_GpuMemoryRange             ~2 ns per call (two range compares)
 *   nv2a_dma_resolve                0.54 ns PER HASH ENTRY SCANNED
 *
 * So everything in prepare except the resolves is ~30 ns a batch, which at the
 * 69-180 batches a frame the [STAGE] line reports in gameplay is 2-5 us --
 * 0.002 to 0.005 ms, three orders of magnitude under the 3.4 ms `rest` this
 * was meant to explain. The resolves are the only part that can matter, and
 * what they cost depends entirely on how far into RAMHT the title's DMA
 * handles sit, which cannot be known without a run. That is why [DMA] below
 * counts entries scanned: it converts the unknown into one printed number.
 *
 * READING A SMALL VALUE HERE. pb_now_us has microsecond resolution and a
 * prepare call is tens of nanoseconds, so almost every individual sample
 * truncates to 0 us. That is not a dead instrument. t0 and t1 are each floored
 * independently, so the difference counts the microsecond boundaries crossed
 * in the interval, which over many samples averages to the true duration --
 * unbiased, just very noisy per sample. The positive control for a
 * `prepare=0.00 ms` reading is the call count printed beside it: prepare is
 * called once per batch that passes the vertex stage, so its calls must equal
 * vsh's calls minus the vsh rejects in [VSH]. Calls tracking vsh with a zero
 * time means the stage really is that small; calls at zero means the timer is
 * not being reached at all.
 *
 * WHY THERE IS NO UNCONDITIONAL `walk` STAGE. The obvious next split is the
 * pushbuffer walk and method dispatch, which is the rest of what `rest`
 * contains. It cannot be timed the way these are. The only boundary this file
 * owns is nv2a_pb_exec_method, which runs once per METHOD, not once per batch
 * -- thousands of times a frame against ~100 batches -- and clock_gettime
 * costs 21 ns a call on this host (measured, same harness). Two reads per
 * method at 21 ns each would ADD roughly 0.04 ms per thousand methods a
 * frame to the frame it is supposed to be explaining, and attribute the
 * addition to the stage. That is an instrument that manufactures its own
 * reading. RECOMP_PB_STAGE_WALK=1 turns it on anyway for a dedicated run,
 * because a biased number with a known bias beats no number; see pb_walk_on()
 * for how to correct it. It is off by default and costs one predicted branch
 * per method when off. */
/* `snap` IS NOT PART OF `sync`, AND THE DIFFERENCE IS THE WHOLE QUESTION.
 *
 * A handover read "[FLIP-SYNC] flip read-backs: 26236 taken, 0 skipped" beside
 * p99=39 ms and concluded "Every flip waits for the GPU and round-trips the
 * whole colour surface through guest RAM. That is where the judder is." Three
 * different costs are named in that one sentence and only two of them were
 * ever timed separately:
 *
 *     drain     waiting for the GPU to finish     [METAL] g_sync_drain_ns
 *     readback  GPU memory -> guest RAM           [METAL] g_sync_read_ns
 *     snap      guest RAM -> the presented copy   NOTHING MEASURED IT
 *
 * The third is this file's own memcpy at FLIP_STALL, 1.2 MB a frame at
 * 640x480x4, and it was inside `rest`. Presenting a completed GPU texture
 * directly -- the fix that keeps getting proposed for this -- removes the
 * readback and the snap and keeps the drain. Which of the three dominates
 * decides whether that is worth a week, so measure it before claiming it. */
typedef enum { PB_STAGE_VSH, PB_STAGE_PREPARE, PB_STAGE_SUBMIT, PB_STAGE_SYNC,
               PB_STAGE_SNAP, PB_STAGE_CLEAR, PB_STAGE_WALK,
               PB_STAGE_N } PbStage;
static const char *const pb_stage_name[PB_STAGE_N] = { "vsh", "prepare",
                                                       "submit", "sync",
                                                       "snap", "clear",
                                                       "walk" };
static struct { unsigned long long us[PB_STAGE_N], n[PB_STAGE_N]; }
    s_stage_run, s_stage_win;

static unsigned long long pb_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull
         + (unsigned long long)(ts.tv_nsec / 1000);
}

static void pb_stage_add(PbStage s, unsigned long long t0)
{
    unsigned long long d = pb_now_us() - t0;
    s_stage_run.us[s] += d; s_stage_run.n[s]++;
    s_stage_win.us[s] += d; s_stage_win.n[s]++;
}

/* RECOMP_PB_STAGE_WALK=1: time nv2a_pb_exec_method itself, so `rest` stops
 * containing the pushbuffer walk and method dispatch.
 *
 * OFF BY DEFAULT AND IT MUST STAY THAT WAY. This is the one stage whose
 * boundary is per METHOD rather than per batch, and the clock is not free:
 * clock_gettime(CLOCK_MONOTONIC) measured 21 ns a call on this host
 * (diagnostics/jsrf_first_fault/dma_resolve_stats_test.c --bench). Two reads
 * per method is 42 ns of instrument per method dispatched, which for the
 * ~2,700 methods a captured JSRF segment carries is ~0.12 ms a frame of pure
 * overhead, added to the frame time AND charged to the stage.
 *
 * HOW TO READ THE RESULT. pb_stage_line prints the call count beside every
 * stage, so a walk run reports methods-per-frame directly. The true walk cost
 * is approximately
 *
 *     walk_true  ~=  walk_reported - calls * 42 ns
 *
 * and the frame time it is a fraction of is likewise inflated by the same
 * amount, so compare the CORRECTED walk against the frame time from a run with
 * this switch off, never against the inflated one printed on the same line.
 * 42 ns is this host; re-measure with the bench on another.
 *
 * It is a presence-and-value switch through recomp_switch_on rather than
 * getenv, so RECOMP_PB_STAGE_WALK=0 is a control arm and not a second way of
 * turning it on -- see recomp_switch.h for the three times that went wrong. */
static int pb_walk_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_PB_STAGE_WALK");
    return on;
}


#if NV2A_GPU_PATH
/* Read once: this is consulted per batch, and getenv on Windows walks the
 * environment block every call. */
static int nv2a_gpu_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv(NV2A_GPU_SWITCH) ? 1 : 0;
    return on;
}
#endif

/* strtok_s is MSVC's name for what POSIX calls strtok_r. Same signature and
 * same semantics, so one alias covers it rather than restructuring the two
 * call sites. */
#if !defined(_MSC_VER)
#define strtok_s(str, delim, ctx) strtok_r((str), (delim), (ctx))
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
extern const uint8_t *xbox_Nv2aRegisterMemory(void);
extern int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size);
/* Declared here rather than in nv2a_texture_copy.h for the same reason the
 * four above are: this is a diagnostic read-out with exactly one consumer, and
 * putting it in the header would put it in front of every translation unit
 * that includes it. */
extern void nv2a_dma_resolve_stats(unsigned long long *scans,
                                   unsigned long long *entries,
                                   unsigned long long *misses,
                                   unsigned long long *worst);

/* Forward-declared so the switch caches below can be used by the per-frame
 * code that sits ABOVE their definitions. The definitions are next to
 * capture_draw, where the comment explaining the 444-sample getenv profile
 * lives; moving them up here would separate the code from its evidence. */
static int pb_env_on(const char *name, int *slot);
/* Its sibling, for the same reason: surface_census() reads a value-carrying
 * switch (RECOMP_SURFACE_CENSUS=<stride>[:<after>]), which recomp_switch.h
 * explicitly excludes from recomp_switch_on, and it sits above the
 * definition next to capture_draw. */
static const char *pb_env_str(const char *name, const char **slot);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Upstream carries three functions here -- surface_hits_image,
 * surface_write_refused and dma_resolve -- which decide whether a surface
 * offset is a guest VA or a physical address, and refuse a write that would
 * land on the loaded image. They are not kept, because this tree answers both
 * questions properly rather than by inference.
 *
 * dma_resolve guesses from the contiguous arena's high-water mark: below it,
 * add XBOX_CONTIG_BASE; above it, hope. nv2a_dma_resolve does the real RAMHT
 * lookup for the DMA object the method actually names, which is the base
 * upstream's comment says it lacks ("getting the address right needs the DMA
 * object base this ignores").
 *
 * The image guard is nv2a_range_hits_image, fed by nv2a_set_image_bounds from
 * the XBE's own section extents (see xbox_MemoryLayoutInit). Same refusal,
 * live, and already wired into the depth path.
 *
 * Restoring either of upstream's would reintroduce the heuristic beside the
 * lookup that replaced it, so the two would disagree on the same surface. */

/* NV097 methods this executor acts on. */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */

/* One immediate vertex, as this file packs it for the shared draw path:
 * position float4, diffuse D3DCOLOR, texcoord0 float2. */
#define IMM_VERTEX_DWORDS 7

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
/* THE GUEST ASKS FOR MORE THAN 4096 AND THE REST WAS THROWN AWAY.
 *
 * ARRAY_ELEMENT16 simply stopped storing once idx_count reached the cap, so a
 * batch bigger than the array was TRUNCATED and drawn anyway -- a mesh with its
 * tail missing, reported by nothing. That is what a fence that is half there,
 * or not there, looks like. Measured once a counter was put on the drop, in one
 * 280 s gameplay run:
 *
 *     no-room=43257098 (e16=86463170 e32=25513 da=0);
 *     biggest batch asked for 9681 of 4096 slots
 *
 * 9,681 -- 2.4x the array. Essentially all of it on ARRAY_ELEMENT16, the oldest
 * path in this file.
 *
 * 16384 covers that with 69% headroom, at 8 MB of static arrays (s_outputs and
 * reuse_inputs, 4 MB each). RAISING THIS ALONE WOULD MAKE THINGS WORSE:
 * nv2a_metal_draw rejects count>4096 and sizes its assembly array 3x4096
 * exactly, so every newly-admitted batch would be refused by the GPU and handed
 * to the CPU rasteriser. Both backends were raised with it. */
#define NV_MAX_INDICES  16384
/* Inline vertex data arrives as one dword per push, so a batch needs room for
 * the whole primitive: 16384 dwords is 2048 vertices at a typical eight-dword
 * layout, and the index array caps the batch at 4096 either way. */
#define NV_MAX_INLINE_WORDS 16384

/* Vertex-reuse opportunity, opt-in via RECOMP_VSH_REUSE_STATS=1. Counts only;
 * changes nothing about what is transformed. */
unsigned long long g_vsh_idx_total, g_vsh_idx_unique;
unsigned long g_vsh_batches_counted, g_vsh_idx_max_batch;
static int vsh_reuse_stats(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VSH_REUSE_STATS"); on = e ? (atoi(e) != 0) : 0; }
    return on;
}

/* Batch-local vertex reuse.
 *
 * s_outputs[] is indexed by POSITION IN THE INDEX ARRAY, so a vertex
 * referenced N times in one batch runs the whole shader N times. Measured on
 * this title at gameplay: 423,405,051 indices against 205,792,923 unique --
 * 51.4% duplicates, stable to one decimal across every report of a 300 s run.
 *
 * Sound because nv2a_vsh_execute is a pure function of (program, inputs,
 * constants): grepping nv2a_vsh.c for mutable file-scope state finds none, and
 * the inputs depend only on the vertex index and the attribute arrays, which do
 * not change within a batch. The cached entry is copied AFTER subpixel
 * quantisation, so a hit reproduces the original bytes exactly rather than
 * re-deriving them.
 *
 * Both switches are opt-in. RECOMP_VSH_REUSE_VERIFY runs the shader anyway on
 * every hit and compares, which is the correctness gate this has to pass before
 * any performance claim -- and it is deliberately separate from the reuse
 * switch so the two can never be confused in a log. */
unsigned long long g_vsh_reuse_hits, g_vsh_reuse_mismatch;

/* VALUE, NOT PRESENCE, and the old form cost a whole measurement.
 *
 * These read `getenv(X) ? 1 : 0`, so RECOMP_VSH_REUSE=0 set the variable to the
 * string "0", which is not NULL, and turned the cache ON. ab_switch.sh runs its
 * control arm as VAR=0 -- so the vshreuse A/B compared reuse-ON against
 * reuse-ON. Its three "different" runs measured 56.4, 56.4 and 55.0 us per
 * batch, which is what one configuration sampled three times looks like, and a
 * 93% "cost of a cache hit" was solved out of that noise and then used to
 * conclude that the vertex interpreter was not where the time goes. It is.
 *
 * This is the SAME bug as RECOMP_APU_SELFLINK_END, found and fixed in
 * apu_vp.c earlier the same day -- and not swept for anywhere else, which is
 * why it was still here. The right shape for an A/B switch is atoi; the
 * presence test is right only for a trace nobody passes =0 to. */
/* FIXED-FUNCTION TRANSFORM AND LIGHTING ON THE GPU. Default OFF.
 *
 * The sibling of RECOMP_METAL_VSH, one layer down: that switch moved the
 * guest's own vertex PROGRAMS onto the GPU and left every fixed-function batch
 * on the CPU, which in a gameplay run is most of them --
 *
 *   [METAL] vsh draws: 508174 GPU, 707669 CPU
 *   render-investigation/idxfix3/stderr.log:36987, [APU-VOICE] on=192..239
 *
 * -- and 520 of that run's 829 million index slots. OFF by default because a
 * generated shader that is subtly wrong compiles cleanly and draws black; the
 * gate that says this one does not is
 * diagnostics/jsrf_first_fault/ff_msl_diff_test.m, and it must have passed on
 * this emitter before the switch is turned on. */
static int ff_gpu_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_FF");
    return on;
}
static int vsh_reuse_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VSH_REUSE"); on = e ? (atoi(e) != 0) : 0; }
    return on;
}

/* THE OUTPUT SLOTS ANYTHING DOWNSTREAM CAN ACTUALLY READ.
 *
 * s_outputs is float[4096][16][4] -- 1 MB -- and every vertex writes all 256
 * bytes of its row, whether the shader ran or the reuse cache served it. That
 * write is the per-vertex cost: with RECOMP_VSH_REUSE on, a cache hit -- which
 * skips the attribute fetch AND the shader entirely and does nothing but this
 * copy -- still costs 93% of a full transform. Measured, three runs, 50.4%
 * duplicate indices moving the vsh stage only 5.79 -> 5.52/5.68 ms. The
 * arithmetic was never the expense. The streaming write into a 1 MB array is.
 *
 * Of the sixteen slots, NINE are dead. 1, 2 and 13-15 have no output register
 * in NV2AVshOutputReg at all, so no program can write them. FOG(5), PTS(6),
 * B0(7) and B1(8) are real registers a program may write, and nothing reads
 * them: grepping every NV2A_VSH_OUT_* mention across nv2a_metal.m,
 * nv2a_texture_copy.c and this file yields only T0, D0 and D1, plus POS as a
 * bare [0] and T1-T3 computed as [9+u]. The Metal backend's own vertex
 * assembly takes exactly 0, 3, 4 and 9..12 and ignores the rest -- so this
 * model drops fog, point size and back-face colours today, and narrowing the
 * copy does not change that, it only stops paying to stage values no one
 * collects.
 *
 * The two diagnostics that DO read all sixteen -- capture_draw, and the
 * reuse verifier's full-row memcmp -- force the wide copy when they are on,
 * which is why this is a predicate rather than a constant.
 *
 * RECOMP_VSH_NARROW_OUTPUTS=1 enables it. Default OFF until an A/B says which
 * value is wrong: the argument above is a good one, and a good argument is
 * exactly what shipped a regression earlier today. */
static const uint8_t s_live_outputs[] = { 0, 3, 4, 5, 9, 10, 11, 12 };   /* 5: oFog, read since G53 */

/* A clear of BOTH halves overwrites every byte of the retained surface, so
 * reading that surface back first is pure waste -- see nv2a_metal_discard.
 * The readback and the command-buffer wait it skips are, by this file's own
 * profile, most of what the clear stage costs.
 *
 * Gated because it is a correctness claim about coverage, not a tuning knob:
 * if a guest ever cleared both halves over LESS than the full clip region,
 * this would discard pixels it should have kept. clear_surface's CPU loops run
 * over clip_w x clip_h and the Metal surface is clip_w x clip_h, so that
 * cannot currently happen -- but the switch is how that gets falsified rather
 * than assumed.
 *
 * RECOMP_CLEAR_DISCARD=1 enables it. */
static int clear_discard_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CLEAR_DISCARD");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* HOIST THE PER-VERTEX DEFAULTS OUT OF THE PER-VERTEX LOOP.
 *
 * prepare_vertices() opened every vertex with
 * memcpy(inputs, s_vsh.current, sizeof inputs) -- 256 bytes, 16 attributes by
 * four floats -- to seed the attributes the fetch loop then overwrites. That
 * copy was measured on 21 Sep 2026 as part of the 47 us/draw the `vsh` stage
 * costs, which is HALF the 91 us our own code spends per draw (Metal's own
 * create+encode+commit is 1.9 us of it).
 *
 * It is the same 256 bytes every vertex, because s_vsh.current does not
 * change inside a batch. Seeding it once per batch is not an approximation:
 * every vertex overwrites exactly the attributes the fetch loop writes, and
 * an attribute the loop skips (not in inputs_read, or size 0) is never
 * written by any vertex, so it holds the same s_vsh.current value under
 * either scheme. The array stays fully initialised, so the reuse-verify path
 * that copies all sixteen (memcpy(reuse_inputs[i], ...)) is unaffected.
 *
 * MEASURED, AND IT BUYS ESSENTIALLY NOTHING. Scene-matched A/B the same
 * night (tutorial, nodes=61, 0 faults, 150 s each): vsh 2.88 -> 2.80 ms/frame,
 * -0.08 ms, which is 0.4% of an 18.7 ms frame and inside run-to-run variance
 * -- the whole-frame numbers moved -2.0% while cumulative fps moved -1.6, so
 * the two disagree on sign. Treat it as zero.
 *
 * KEPT ANYWAY, as a control arm, because the null result is the useful part:
 * it proves the per-vertex 256-byte seed is NOT where the vsh stage's time
 * goes, so nobody needs to try this again. RECOMP_VSH_SPLIT then said where
 * it does go -- 13,615 vertices/frame at 212 ns each, of which attribute
 * fetch is only 39 ns (18%). The remaining 82% is loop overhead spread
 * across the per-vertex body, not any one copy. This stage does not have a
 * hot spot to remove; it has 13,615 iterations to stop doing, which means
 * uploading the guest vertex buffer and letting the GPU fetch.
 *
 * OFF by default. RECOMP_VSH_HOIST_INPUTS=1. */
static int vsh_hoist_inputs(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VSH_HOIST_INPUTS");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

static int vsh_narrow_outputs(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VSH_NARROW_OUTPUTS");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

static inline void copy_live_outputs(float dst[16][4], const float src[16][4])
{
    unsigned k;
    for (k = 0; k < sizeof s_live_outputs; ++k) {
        unsigned s = s_live_outputs[k];
        memcpy(dst[s], src[s], sizeof dst[s]);
    }
}
static int vsh_reuse_verify(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_VSH_REUSE_VERIFY"); on = e ? (atoi(e) != 0) : 0; }
    return on;
}

void nv2a_vsh_reuse_report(void)
{
    /* The STATE prints unconditionally; the statistics stay opt-in.
     *
     * Everything below used to sit behind RECOMP_VSH_REUSE_STATS, and the
     * reuse=on/off line behind reuse-or-verify being on -- so the control arm
     * of an A/B printed nothing at all and ab_score.py had nothing to verify
     * the arms against. It said "arms NOT verified distinct", which was
     * correct and was ignored, and the A/B underneath it turned out to have
     * both arms in the same configuration. One cheap line closes that. */
    fprintf(stderr, "  [VSH-REUSE] (vsh_reuse %s, verify %s)\n",
            vsh_reuse_on() ? "on" : "OFF",
            vsh_reuse_verify() ? "on" : "OFF");
    if (!vsh_reuse_stats()) return;
    if (!g_vsh_batches_counted) {
        fprintf(stderr, "  [VSH-REUSE] armed, no batches counted yet\n");
        fflush(stderr);
        return;
    }
    fprintf(stderr,
            "  [VSH-REUSE] batches=%lu indices=%llu unique=%llu"
            " duplicate=%.1f%% max_batch=%lu\n",
            g_vsh_batches_counted, g_vsh_idx_total, g_vsh_idx_unique,
            g_vsh_idx_total ? 100.0 * (double)(g_vsh_idx_total - g_vsh_idx_unique)
                              / (double)g_vsh_idx_total : 0.0,
            g_vsh_idx_max_batch);
    if (vsh_reuse_on() || vsh_reuse_verify())
        fprintf(stderr, "  [VSH-REUSE] reuse=%s verify=%s hits=%llu mismatches=%llu\n",
                vsh_reuse_on() ? "on" : "off",
                vsh_reuse_verify() ? "on" : "off",
                g_vsh_reuse_hits, g_vsh_reuse_mismatch);
    fflush(stderr);
}

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    /* WHAT THE BATCH ASKED FOR versus what fitted, split by the method that
     * asked. The first version of this counter was shared across the
     * submission methods, so 31.9M told us nothing about which one to fix --
     * split by source, and keep a high-water mark, or the run is wasted.
     * All of these count INDICES, not parameter words: ARRAY_ELEMENT16
     * carries two per word and counting words put the sources on different
     * scales and made the ratio meaningless. */
    uint32_t   idx_wanted, idx_wanted_max;
    unsigned long long idx_overflow, idx_overflow_e16, idx_overflow_e32;
    /* INLINE_ARRAY is a separate array with a separate cap and was in neither
     * counter. Words, not indices -- an inline vertex is several words -- so
     * these are deliberately NOT summed into idx_overflow, which counts
     * indices. Mixing the two scales is the mistake the e16 note below
     * records. */
    unsigned long long inline_overflow;
    uint32_t   inline_wanted, inline_wanted_max;
    uint32_t   batches_no_layout;
    unsigned long long elem32_seen, elem32_indices, elem32_wide;
    uint32_t   elem32_batches_dropped;
    int        batch_wide;              /* an index did not fit uint16_t */
    uint32_t   inline_words[NV_MAX_INLINE_WORDS];
    uint32_t   inline_count;            /* dwords pushed this batch, 0 = none */
    float      vp_offset[4], vp_scale[4];
    int        vp_seen;                 /* the title programmed a viewport */
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, pitch, format;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    uint32_t gpu_batches, gpu_fallbacks;
    /* Batches and triangles that the GPU never saw.
     *
     * gpu_batches counts accelerated successes and gpu_fallbacks counts
     * batches that TRIED the accelerated path and were refused -- so between them they say nothing
     * about a batch that skipped the accelerated path altogether, which is
     * what happens whenever s_copy.active is 0. "0 software fallbacks" was
     * therefore not the same claim as "nothing is rasterised on the CPU", and
     * the difference was invisible. */
    uint32_t cpu_batches, cpu_tris;
} s_gpu;

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Raw uploads remain available for diagnosis. Execution uses persistent
 * program/constant banks and the hardware LOAD/START cursors below. */
static struct {
    uint32_t words[NV2A_VS_MAX_INSTRUCTIONS][4];
    uint8_t loaded[NV2A_VS_MAX_INSTRUCTIONS];
    float constants[NV2A_VS_MAX_CONSTANTS][4];
    float current[16][4];
    uint32_t load, start, constant_load, mode;
    int dirty;
    NV2AVshProgram decoded;
    uint32_t batches, rejected;
} s_vsh;
/* RECOMP_VSH_TRACE measures delivery separately from the D3D11 sink's
 * unhandled histogram. Compare actual selected words, not just START/length. */
static struct {
    int enabled;
    uint64_t program_words[8], changed, dropped, constants, constant_dropped;
    uint64_t loads, starts, modes, current4f, current4ub, decodes;
    uint8_t current_written[16];
    uint32_t hashes[32], unique;
    uint32_t start_values, load_values;
    uint64_t load_high;
    struct { uint32_t method, param; } recent[2048];
    uint64_t recent_count;
} s_vsh_trace;
static float s_positions[NV_MAX_INDICES][4];
static uint32_t s_colors[NV_MAX_INDICES];
/* Retain all shader outputs for texture interpolation and draw captures. */
static float s_outputs[NV_MAX_INDICES][16][4];
/* Does s_outputs hold the vertex program's OUTPUTS, or its INPUTS for the GPU
 * to transform? Every reader of s_outputs assumes the former; the GPU vertex
 * path makes it the latter, and the two are indistinguishable by inspection --
 * both are sixteen float4 per vertex. */
static int s_vsh_gpu_batch;
static int s_vsh_force_cpu;
static uint32_t s_methods[0x2000 / 4];
static uint8_t s_method_seen[0x2000 / 4];
static int s_capture_selected;
static struct {
    NV2ATextureCopy state;
    NV2ATextureCopy extra_stages[3];
    const uint8_t *texture;
    uint8_t *target, *depth;
    size_t texture_bytes, target_bytes, depth_bytes;
    uint32_t texture_address, target_address, depth_address, batches, rejected;
    uint32_t extra_address[3];   /* G39: stage 1-3 guest addresses, like texture_address */
    int active;
} s_copy;

/* Group exact register sets, not just a hash or the first failing register.
 * Keep inactive stages too: the first snapshot contains all combiner state,
 * and the bounded table makes overflow explicit instead of hiding new cases. */
#define COMBINER_TRACE_MAX 64
#define COMBINER_TRACE_WORDS 56
static struct {
    uint32_t words[COMBINER_TRACE_WORDS], first_draw;
    uint64_t draws, rejected, inline_draws, invalid_positions, collapsed_xy;
} s_combiner_trace[COMBINER_TRACE_MAX];
static unsigned s_combiner_count;
static uint64_t s_combiner_overflow;
static int s_combiner_capture;
static struct { const char *reason; uint64_t count; } s_texture_reasons[32];

/* Blend state as the guest programs it, rather than as a draw samples it.
 *
 * The [BLEND] line in nv2a_texture_copy_prepare() reads the blend methods at
 * the moment a batch is prepared, and a batch only gets that far after
 * surviving the vertex stage and that function's own early returns. A
 * configuration the title programs, draws one quad with, and puts back before
 * the next surviving batch leaves no trace there at all. So "the guest never
 * asks for DST_COLOR/ZERO" was an absence measured downstream of two filters,
 * either of which can swallow the asking.
 *
 * This sits on the method write, which nothing filters: every distinct
 * (enable, sfactor, dfactor, equation) the title ever programs, counted, with
 * the draw span it was live across. The combination already known to be asked
 * for is the positive control -- if 0x302/0x303/0x8006 is missing from this
 * list then the instrument is wrong, not the title.
 *
 * Read-only, opt-in via RECOMP_BLEND_TRACE, bounded to sixteen entries. */
#define BLEND_TRACE_MAX 16
static struct {
    uint32_t en, src, dst, eq, first_draw, last_draw;
    uint32_t clear_at_first, format_at_first;
    double first_t, last_t;
    uint64_t hits;
} s_blend_trace[BLEND_TRACE_MAX];
static unsigned s_blend_trace_count;
static uint64_t s_blend_trace_writes, s_blend_trace_overflow;

/* Which combination is in force right now, as an index into the table above,
 * so the draw path can report the fate of the batches drawn under it. -1 until
 * the first write. */
static int s_blend_current = -1;

/* The fate of every batch drawn while a multiply blend is programmed.
 *
 * 270 programmings of DST_COLOR/ZERO produced 80 refusals, and those two
 * numbers cannot both describe the same event. The gap is either batches dying
 * before they reach the accept test, or programmings that are never drawn
 * under at all -- a different fault with a different fix. Counting the batch
 * at each stage of the draw path separates them.
 *
 * "multiply" here means sfactor DST_COLOR, which is the shape a fade-to-black
 * uses; the dfactor is recorded beside it rather than tested, because the
 * title programs the two methods one at a time and the intermediate tuple is
 * real state that a draw can land in. */
static struct {
    uint64_t batches, short_idx, vsh_rejected, prepare_rejected, rasterised;
} s_blend_fade_fate;

/* Elapsed seconds for the traces below.
 *
 * Deliberately not trace_seconds(): several unit tests link this
 * translation unit without the platform library, and referencing it there
 * costs a link error for a diagnostic none of them enable.
 *
 * THE ORIGIN IS THE FIRST CALL, WHICH IS NOT THE START OF THE RUN. The
 * comment here used to claim the origin was "the first traced write, which
 * the guest issues during D3D initialisation" -- true only when some trace
 * that fires during init is armed. Arm only a late-firing one and the origin
 * slides to wherever that first fires. On 19 Sep 2026 the only armed consumer
 * was the glyph trap, its first event was flip 2429, and t started at 0.00
 * there -- so RECOMP_FB_WATCH_AFTER=10, documented as a wall-clock second,
 * silently discarded the first 88 of 110 captures.
 *
 * fb_watch_init() now primes it on the first flip, which happens long before
 * any trap can fire, so `after` is measured from a point that does not depend
 * on which switches are set. Anything else that wants a run-start origin
 * should prime it the same way rather than assuming it is already set. */
static double trace_seconds(void)
{
    static struct timespec origin;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!origin.tv_sec && !origin.tv_nsec) origin = now;
    return (double)(now.tv_sec - origin.tv_sec)
         + (double)(now.tv_nsec - origin.tv_nsec) / 1e9;
}

/* RECOMP_FF_BATCH_DUMP=<max lines> -- one line per FIXED-FUNCTION batch, the
 * same record from either arm, so the GPU path (RECOMP_METAL_FF=1) and the CPU
 * path (=0) can be diffed batch by batch. Exists because on 16 Sep 2026 the
 * GPU path drew two corrupt glyphs in Gum's tutorial text and the CPU path
 * did not, and jsrf_ff_msl_diff_test could not see it: that test feeds both
 * emitters synthetic inputs, and whatever state those two glyph batches carry
 * that the others do not was never in it. This prints what a real batch
 * carries: the key, the texture-matrix enables, the stage-0 texture, the
 * attribute layout, the "current" registers an unsized attribute falls back
 * to, the first vertex's inputs -- and, in BOTH arms, what nv2a_ff_vertex
 * says the outputs should be, so the GPU arm's record can be checked against
 * the CPU transform it replaced without a second run.
 *
 *   RECOMP_FF_BATCH_DUMP_AFTER=<seconds>  start printing at this wall-clock
 *   RECOMP_FF_BATCH_DUMP_TEX=<hex>        only batches whose stage-0 texture
 *                                         offset (0x1B00) equals this
 *
 * Read-only. The CPU transform it runs in the GPU arm bumps nv2a_ff.c's
 * counters, which is why the [FF] counters are not comparable between a run
 * with this set and one without. */
/* RECOMP_FF_BATCH_WATCH_TEX=<hex> -- watch every fixed-function batch whose
 * stage-0 texture offset is this, in either arm, and say when the vertex data
 * a STATIC batch carries CHANGES between two draws of the same shape. Built
 * for the glyph defect: the GPU fixed-function arm drew Gum's/Corn's speech
 * bubble with two quote glyphs relocated onto the "C" of "Corn" in one frame
 * of twenty-four (frame-bisect/ffglyph2/ff-on/fb/flip013.bmp), and the CPU
 * arm never did. The text is static, so its position and texcoord rows must
 * be identical draw to draw; if this instrument sees them change in the GPU
 * arm, the data was wrong BEFORE the shader (a fetch from guest memory the
 * guest had already reused, i.e. a race with the guest), and if it never
 * sees them change while the picture does, the defect is after the fetch:
 * packing, the ring, or the vertex function. Keyed by vertex count so the
 * bubble and the name label are tracked separately. */
/* THE VERTEX COUNT IS NOT AN IDENTITY, AND KEYING ON IT ALONE VOIDED THIS
 * INSTRUMENT'S ONE CONCLUSION.
 *
 * The comment above says the watch is "keyed by vertex count so the bubble and
 * the name label are tracked separately". They have different vertex counts,
 * so that worked for those two -- and stops working the moment two batches
 * with the SAME count use the watched texture, which for a font atlas is the
 * normal case: every glyph quad is six vertices.
 *
 * Measured on the 19 Sep 2026 black-screen run, which had this armed at
 * 01737000 and produced 14,273 lines nobody read: the verts=15 slot alternates
 * between exactly two vertex sets, A->B->A->B, for all 7,140 of its draws, and
 * verts=6 cycles three. That is not one batch changing. That is two and three
 * DIFFERENT batches per frame being compared against each other, and every one
 * of those comparisons was reported as a CHANGE.
 *
 * It matters because of what the report is for. Its stated discriminator is:
 * change means the data was wrong BEFORE the shader (a race with the guest),
 * no change while the picture is wrong means the defect is after the fetch.
 * Read off 14,269 changes, that says "race with the guest" -- confidently, and
 * on no evidence at all.
 *
 * So the key carries the draw's ORDINAL WITHIN THE FRAME as well. A static UI
 * element is drawn at the same point in every frame's sequence, so (count,
 * ordinal) names one batch and compares it against itself. Two batches that
 * merely share a vertex count no longer collide. */
/* Four was enough when the key was the vertex count and the question was
 * "the bubble or the name label". Keyed per batch it is not: a font atlas is
 * drawn as one six-vertex quad per glyph, and a line of dialogue is dozens.
 * Measured on a 55 s boot watching one texture, 8 slots left 29,331 draws
 * unwatched. At 32 KB a slot (two 1024-vertex float4 arrays) 64 costs 2 MB of
 * BSS in a diagnostic that is off unless asked for, and the exhaustion
 * counter says when even that is not enough. */
#define FF_WATCH_KEYS 64
#define FF_WATCH_MAX_VERTS 1024
static struct {
    uint32_t verts; unsigned long ordinal, draws, changes;
    float pos[FF_WATCH_MAX_VERTS][4], t0[FF_WATCH_MAX_VERTS][4];
} g_ff_watch[FF_WATCH_KEYS];
/* Draws issued so far in this frame, reset at the flip. */
static unsigned long g_ff_watch_ordinal;
/* Batches the watch could not track because every slot was taken. Counted so
 * "no changes" cannot be read off a watch that was never looking. */
static unsigned long g_ff_watch_nokey;
static float g_ff_watch_pos[FF_WATCH_MAX_VERTS][4], g_ff_watch_t0[FF_WATCH_MAX_VERTS][4];
static unsigned long g_ff_watch_changes;
static uint32_t ff_watch_tex(void)
{
    static int init; static uint32_t tex;
    if (!init) { const char *e = getenv("RECOMP_FF_BATCH_WATCH_TEX");
                 tex = e ? (uint32_t)strtoul(e, NULL, 16) : 0; init = 1; }
    return tex;
}
static void ff_watch_vertex(const char *arm, uint32_t i, const float inputs[16][4])
{
    uint32_t tex = ff_watch_tex();
    if (!tex || s_methods[NV097_SET_TEXTURE_OFFSET / 4] != tex) return;
    uint32_t n = s_gpu.idx_count;
    if (n > FF_WATCH_MAX_VERTS || i >= n) return;
    memcpy(g_ff_watch_pos[i], inputs[0], 16);
    memcpy(g_ff_watch_t0[i], inputs[9], 16);
    if (i + 1 != n) return;
    /* last vertex: compare with the previous draw of THIS batch -- the same
     * vertex count AND the same place in the frame's draw sequence. Keying on
     * the count alone compared different batches with each other; see the
     * note on g_ff_watch. */
    unsigned long ord = g_ff_watch_ordinal++;
    int k, free = -1;
    for (k = 0; k < FF_WATCH_KEYS; ++k) {
        if (g_ff_watch[k].verts == n && g_ff_watch[k].draws
                && g_ff_watch[k].ordinal == ord) break;
        if (free < 0 && !g_ff_watch[k].draws) free = k;
    }
    if (k == FF_WATCH_KEYS) {
        if (free < 0) { ++g_ff_watch_nokey; return; }
        k = free; g_ff_watch[k].verts = n; g_ff_watch[k].ordinal = ord;
        g_ff_watch[k].draws = 0;
    }
    if (g_ff_watch[k].draws++) {
        uint32_t j, diffs = 0, first = n; const char *what = "";
        for (j = 0; j < n; ++j) {
            int dp = memcmp(g_ff_watch[k].pos[j], g_ff_watch_pos[j], 16);
            int dt = memcmp(g_ff_watch[k].t0[j], g_ff_watch_t0[j], 16);
            if (dp || dt) { if (first == n) { first = j; what = dp && dt ? "pos+t0" : dp ? "pos" : "t0"; } ++diffs; }
        }
        if (diffs) {
            ++g_ff_watch_changes;
            ++g_ff_watch[k].changes;
            fprintf(stderr, "[FF-WATCH] t=%.2f arm=%s verts=%u batch#%lu draw#%lu"
                    " CHANGED %u vertices,"
                    " first v%u (%s): pos (%g %g %g %g)->(%g %g %g %g) t0 (%g %g)->(%g %g)\n",
                    trace_seconds(), arm, n, g_ff_watch[k].ordinal,
                    g_ff_watch[k].draws, diffs, first, what,
                    g_ff_watch[k].pos[first][0], g_ff_watch[k].pos[first][1],
                    g_ff_watch[k].pos[first][2], g_ff_watch[k].pos[first][3],
                    g_ff_watch_pos[first][0], g_ff_watch_pos[first][1],
                    g_ff_watch_pos[first][2], g_ff_watch_pos[first][3],
                    g_ff_watch[k].t0[first][0], g_ff_watch[k].t0[first][1],
                    g_ff_watch_t0[first][0], g_ff_watch_t0[first][1]);
        }
    } else {
        fprintf(stderr, "[FF-WATCH] t=%.2f arm=%s verts=%u batch#%lu (draw %lu of"
                " its frame) first sighting, tracking\n",
                trace_seconds(), arm, n, g_ff_watch[k].ordinal,
                g_ff_watch[k].ordinal);
    }
    memcpy(g_ff_watch[k].pos, g_ff_watch_pos, (size_t)n * 16);
    memcpy(g_ff_watch[k].t0, g_ff_watch_t0, (size_t)n * 16);
}

static void ff_batch_dump(const char *arm, const float inputs[16][4],
                          const void *key, unsigned keysize)
{
    static int max = -1, after = 0; static uint32_t tex = 0; static int shown;
    if (max < 0) {
        const char *e = getenv("RECOMP_FF_BATCH_DUMP");
        max = e ? atoi(e) : 0;
        e = getenv("RECOMP_FF_BATCH_DUMP_AFTER"); after = e ? atoi(e) : 0;
        e = getenv("RECOMP_FF_BATCH_DUMP_TEX"); tex = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
    }
    static unsigned long ff_batches;   /* s_vsh.batches counts PROGRAMMABLE batches only */
    ++ff_batches;
    if (!max || shown >= max) return;
    if (trace_seconds() < (double)after) return;
    uint32_t tex0 = s_methods[NV097_SET_TEXTURE_OFFSET / 4];
    if (tex && tex0 != tex) return;
    ++shown;
    float expect[16][4]; memset(expect, 0, sizeof expect);
    const char *why = nv2a_ff_vertex(s_methods, inputs, expect);
    fprintf(stderr, "[FF-BATCH] t=%.2f arm=%s ffbatch=%lu verts=%u prim=%u"
            " tex0=%08X fmt0=%08X texmat_en=%u,%u,%u,%u",
            trace_seconds(), arm, ff_batches,
            (unsigned)s_gpu.idx_count, (unsigned)s_gpu.prim,
            tex0, s_methods[NV097_SET_TEXTURE_FORMAT / 4],
            s_methods[0x420/4], s_methods[0x424/4],
            s_methods[0x428/4], s_methods[0x42c/4]);
    if (key) {
        const uint8_t *kb = (const uint8_t *)key; unsigned k;
        fprintf(stderr, " key=");
        for (k = 0; k < keysize; ++k) fprintf(stderr, "%02x", kb[k]);
    }
    {
        static const unsigned want[] = { 0, 3, 4, 9, 10, 11, 12 };
        unsigned k;
        for (k = 0; k < sizeof want / sizeof want[0]; ++k) {
            unsigned a = want[k];
            fprintf(stderr, " a%u=%u/%u/%u", a, s_gpu.attr[a].type,
                    s_gpu.attr[a].size, s_gpu.attr[a].stride);
        }
    }
    fprintf(stderr, " cur9=(%g %g %g %g)", s_vsh.current[9][0],
            s_vsh.current[9][1], s_vsh.current[9][2], s_vsh.current[9][3]);
    fprintf(stderr, " | v0 pos=(%g %g %g %g) d0=(%g %g %g %g) t0=(%g %g %g %g)",
            inputs[0][0], inputs[0][1], inputs[0][2], inputs[0][3],
            inputs[3][0], inputs[3][1], inputs[3][2], inputs[3][3],
            inputs[9][0], inputs[9][1], inputs[9][2], inputs[9][3]);
    if (why) fprintf(stderr, " | cpu-transform REFUSED: %s\n", why);
    else fprintf(stderr, " | expect oPos=(%g %g %g %g) oT0=(%g %g %g %g)\n",
            expect[0][0], expect[0][1], expect[0][2], expect[0][3],
            expect[9][0], expect[9][1], expect[9][2], expect[9][3]);
}

/* Distinct clear values, with the span of time each was live across. */
#define CLEAR_TRACE_MAX 16
static struct {
    uint32_t value, format, first_draw, last_draw;
    double first_t, last_t;
    uint64_t hits;
} s_clear_trace[CLEAR_TRACE_MAX];
static unsigned s_clear_trace_count;
static uint64_t s_clear_trace_overflow;

static void note_clear_value(uint32_t param)
{
    static int on = -1;
    unsigned i;

    if (on < 0) on = getenv("RECOMP_BLEND_TRACE") ? 1 : 0;
    if (!on) return;
    for (i = 0; i < s_clear_trace_count; ++i)
        if (s_clear_trace[i].value == param) {
            ++s_clear_trace[i].hits;
            s_clear_trace[i].last_t = trace_seconds();
            s_clear_trace[i].last_draw = s_gpu.draws;
            return;
        }
    if (s_clear_trace_count >= CLEAR_TRACE_MAX) { ++s_clear_trace_overflow; return; }
    i = s_clear_trace_count++;
    s_clear_trace[i].value = param;
    s_clear_trace[i].format = s_gpu.format;
    s_clear_trace[i].hits = 1;
    s_clear_trace[i].first_t = s_clear_trace[i].last_t = trace_seconds();
    s_clear_trace[i].first_draw = s_clear_trace[i].last_draw = s_gpu.draws;
    fprintf(stderr, "  [CLEAR-SEQ] new value at t=%.2f draw %u: 0x%08X"
            " (format=0x%X)\n", s_clear_trace[i].first_t, s_gpu.draws,
            param, s_gpu.format);
    fflush(stderr);
}

/* The diffuse alpha the title actually supplies, per batch.
 *
 * xemu draws the opening cards with the same blend we do -- SRC_ALPHA /
 * ONE_MINUS_SRC_ALPHA -- and holds every combiner factor at 0xffffffff across
 * the whole card, so the ramp it displays cannot come from the blend mode or
 * from a combiner constant. That leaves the vertex diffuse alpha as the only
 * place the fade level can arrive, which makes "what alpha do our vertices
 * carry while the card is up" the question that separates a guest that never
 * computes the ramp from a renderer that computes it and throws it away.
 *
 * Quantised to 1/255 and tabulated, because the interesting shape is the set
 * of distinct levels over the card: a ramp is many, a cut is one or two. */
/* One slot per possible level, so the table can never be the answer.
 *
 * 24 was enough for the intro, where there is one level, and silently wrong
 * for gameplay, where a 300 s run reported distinct-levels=24 overflow=4841 --
 * a full table, not a finding. The FACTOR table had the identical defect and
 * reversed a conclusion twice before it was noticed. An alpha level is a byte;
 * 256 slots costs nothing and overflow can then only mean a bug. */
#define ALPHA_TRACE_MAX 256
static struct {
    unsigned level;
    uint32_t first_draw, last_draw;
    double first_t, last_t;
    uint64_t hits;
} s_alpha_trace[ALPHA_TRACE_MAX];
static unsigned s_alpha_trace_count;
static uint64_t s_alpha_trace_overflow, s_alpha_trace_batches;

/* Distinct INPUT diffuse-alpha levels, and whether the attribute came from a
 * bound array rather than an immediate. Same shape as the output table. */
static float s_fetched_alpha_lo = 2.0f, s_fetched_alpha_hi = -1.0f;
static int s_fetched_alpha_seen;
static struct { unsigned level; int from_array; uint64_t hits; } s_alpha_in[ALPHA_TRACE_MAX];
static unsigned s_alpha_in_count;
static uint64_t s_alpha_in_overflow;

static void note_batch_alpha_in(unsigned level, int from_array)
{
    unsigned i;
    for (i = 0; i < s_alpha_in_count; ++i)
        if (s_alpha_in[i].level == level && s_alpha_in[i].from_array == from_array) {
            ++s_alpha_in[i].hits; return;
        }
    if (s_alpha_in_count >= ALPHA_TRACE_MAX) { ++s_alpha_in_overflow; return; }
    i = s_alpha_in_count++;
    s_alpha_in[i].level = level; s_alpha_in[i].from_array = from_array;
    s_alpha_in[i].hits = 1;
}

static void note_batch_alpha(unsigned level)
{
    unsigned i;
    for (i = 0; i < s_alpha_trace_count; ++i)
        if (s_alpha_trace[i].level == level) {
            ++s_alpha_trace[i].hits;
            s_alpha_trace[i].last_t = trace_seconds();
            s_alpha_trace[i].last_draw = s_gpu.draws;
            return;
        }
    if (s_alpha_trace_count >= ALPHA_TRACE_MAX) { ++s_alpha_trace_overflow; return; }
    i = s_alpha_trace_count++;
    s_alpha_trace[i].level = level;
    s_alpha_trace[i].hits = 1;
    s_alpha_trace[i].first_t = s_alpha_trace[i].last_t = trace_seconds();
    s_alpha_trace[i].first_draw = s_alpha_trace[i].last_draw = s_gpu.draws;
}

/* Combiner constants, as the title writes them.
 *
 * xemu steps SET_COMBINER_FACTOR0[20] through 0x60a0ff60, 0x9ca0ff60,
 * 0xbaa0ff60, 0xc4a0ff60 over the frames right after the first white clear --
 * one byte ramping 96, 156, 186, 196 while the rest of the colour holds. That
 * is a fade, and it is the only thing in xemu's entire method stream that
 * moves during the opening: blend factors, vertex diffuse, clear values and
 * the gamma LUT are all constant across the cards in both emulators.
 *
 * nv2a_texture_copy_prepare() refuses any draw whose combiner program uses a
 * constant register while any factor is nonzero, so whether our guest writes
 * this same ramp decides between a guest that never computes the fade and a
 * renderer that is handed it and throws it away. */
/* 24 was far too few and made the instrument answer the opposite question.
 *
 * The table keys on (method, value) pairs, so sixteen factor registers taking
 * more than a couple of values each fill it in the first second; a 75 s intro
 * run reported overflow=76158. "Only three distinct factor values were seen"
 * was therefore a statement about the table being full, not about the title --
 * and a ramp, which is by definition many distinct values, is precisely what
 * this could not show. Big enough now that overflow means something. */
#define FACTOR_TRACE_MAX 512
static struct {
    uint32_t method, value, first_draw, last_draw;
    double first_t, last_t;
    uint64_t hits;
} s_factor_trace[FACTOR_TRACE_MAX];
static unsigned s_factor_trace_count;
static uint64_t s_factor_trace_writes, s_factor_trace_overflow;

static void note_factor_write(uint32_t method, uint32_t param)
{
    static int on = -1;
    unsigned i;

    if (on < 0) on = getenv("RECOMP_BLEND_TRACE") ? 1 : 0;
    if (!on) return;
    ++s_factor_trace_writes;
    /* Keyed on method and value together: which slot carries the ramp is part
     * of the answer, and a table of values alone would merge slots that move
     * for different reasons. */
    for (i = 0; i < s_factor_trace_count; ++i)
        if (s_factor_trace[i].method == method && s_factor_trace[i].value == param) {
            ++s_factor_trace[i].hits;
            s_factor_trace[i].last_t = trace_seconds();
            s_factor_trace[i].last_draw = s_gpu.draws;
            return;
        }
    if (s_factor_trace_count >= FACTOR_TRACE_MAX) { ++s_factor_trace_overflow; return; }
    i = s_factor_trace_count++;
    s_factor_trace[i].method = method;
    s_factor_trace[i].value = param;
    s_factor_trace[i].hits = 1;
    s_factor_trace[i].first_t = s_factor_trace[i].last_t = trace_seconds();
    s_factor_trace[i].first_draw = s_factor_trace[i].last_draw = s_gpu.draws;
    fprintf(stderr, "  [FACTOR] t=%.2f draw %u: method 0x%04X = 0x%08X\n",
            s_factor_trace[i].first_t, s_gpu.draws, method, param);
    fflush(stderr);
}

static int blend_is_multiply(void)
{
    return s_methods[0x304/4] && s_methods[0x344/4] == 0x306;
}

static void note_blend_write(void)
{
    static int on = -1;
    uint32_t en, src, dst, eq;
    unsigned i;

    if (on < 0) on = getenv("RECOMP_BLEND_TRACE") ? 1 : 0;
    if (!on) return;
    ++s_blend_trace_writes;

    /* The whole tuple on every write to any part of it: the title sets these
     * four methods one at a time, and it is the combination in force that
     * decides what a fade looks like. Recording them separately would show
     * four independent value sets and hide which ones were ever live at once. */
    en  = s_methods[0x304/4]; src = s_methods[0x344/4];
    dst = s_methods[0x348/4]; eq  = s_methods[0x350/4];

    for (i = 0; i < s_blend_trace_count; ++i)
        if (s_blend_trace[i].en == en && s_blend_trace[i].src == src
                && s_blend_trace[i].dst == dst && s_blend_trace[i].eq == eq) {
            ++s_blend_trace[i].hits;
            s_blend_trace[i].last_draw = s_gpu.draws;
            s_blend_trace[i].last_t = trace_seconds();
            s_blend_current = (int)i;
            return;
        }
    if (s_blend_trace_count >= BLEND_TRACE_MAX) { ++s_blend_trace_overflow; return; }

    i = s_blend_trace_count++;
    s_blend_trace[i].en = en;   s_blend_trace[i].src = src;
    s_blend_trace[i].dst = dst; s_blend_trace[i].eq  = eq;
    s_blend_trace[i].hits = 1;
    s_blend_trace[i].first_draw = s_blend_trace[i].last_draw = s_gpu.draws;
    s_blend_trace[i].first_t = s_blend_trace[i].last_t = trace_seconds();
    /* The clear colour in force pins the combination to a card. The title
     * issues exactly three clear values, so 0xFFFF beside a multiply blend is
     * the white logo card fading and 0x0000 is something else entirely --
     * which is the whole question a draw number cannot answer. */
    s_blend_trace[i].clear_at_first = s_gpu.clear_color;
    s_blend_trace[i].format_at_first = s_gpu.format;
    /* Printed as it appears as well as summarised at the end, because when it
     * appears is the question: a combination live only across the intro cards
     * is the fade, and one live throughout is ordinary alpha blending. */
    s_blend_current = (int)i;
    fprintf(stderr, "  [BLEND-WRITE] new combination at t=%.2f draw %u:"
            " enable=%u src=0x%X dst=0x%X eq=0x%X (clear=0x%08X format=0x%X)\n",
            s_blend_trace[i].first_t, s_gpu.draws, en, src, dst, eq,
            s_gpu.clear_color, s_gpu.format);
    fflush(stderr);
}

static void trace_combiner(const char *error)
{
    s_combiner_capture = 0;
    if (error) {
        for (unsigned i=0; i<32; ++i) {
            if (!s_texture_reasons[i].reason) s_texture_reasons[i].reason=error;
            if (!strcmp(s_texture_reasons[i].reason,error)) {
                ++s_texture_reasons[i].count;
                break;
            }
        }
    }
    static int enabled=-1;
    if (enabled<0) enabled=getenv("RECOMP_COMBINER_TRACE")!=NULL;
    if (!enabled) return;
    static const unsigned single[]={0x1e60,0x1e70,0x1e74,0x1e78,0x288,0x28c,0x1e20,0x1e24};
    static const unsigned arrays[]={0x260,0xa60,0xa80,0xaa0,0xac0,0x1e40};
    uint32_t words[COMBINER_TRACE_WORDS];
    unsigned n=0;
    for (unsigned i=0; i<8; ++i) words[n++]=s_methods[single[i]/4];
    for (unsigned i=0; i<6; ++i)
        for (unsigned j=0; j<8; ++j) words[n++]=s_methods[arrays[i]/4+j];
    unsigned id;
    for (id=0; id<s_combiner_count; ++id)
        if (!memcmp(words,s_combiner_trace[id].words,sizeof(words))) break;
    if (id==COMBINER_TRACE_MAX) { ++s_combiner_overflow; return; }
    if (id==s_combiner_count) {
        ++s_combiner_count;
        s_combiner_capture=1;
        memcpy(s_combiner_trace[id].words,words,sizeof(words));
        s_combiner_trace[id].first_draw=s_gpu.draws;
        fprintf(stderr,"[COMBINER] new config=%u draw=%u result=%s\n",id,s_gpu.draws,error?error:"prepared");
        for (unsigned i=0; i<8; ++i)
            fprintf(stderr,"[COMBINER] config=%u %04X=%08X\n",id,single[i],words[i]);
        for (unsigned i=0; i<6; ++i) {
            fprintf(stderr,"[COMBINER] config=%u %04X..%04X:",id,arrays[i],arrays[i]+28);
            for (unsigned j=0; j<8; ++j) fprintf(stderr," %08X",words[8+i*8+j]);
            fputc('\n',stderr);
        }
    }
    ++s_combiner_trace[id].draws;
    if (error) ++s_combiner_trace[id].rejected;
    if (s_gpu.inline_count) ++s_combiner_trace[id].inline_draws;
    int invalid=0, collapsed=1;
    for (unsigned i=0; i<s_gpu.idx_count; ++i) {
        const float *p=s_outputs[i][0];
        for (unsigned k=0; k<4; ++k) if (!isfinite(p[k])) invalid=1;
        if (!(p[3]>0)) invalid=1;
        if (p[0]!=s_outputs[0][0][0] || p[1]!=s_outputs[0][0][1]) collapsed=0;
    }
    s_combiner_trace[id].invalid_positions+=invalid;
    s_combiner_trace[id].collapsed_xy+=collapsed;
}

/* Are the bytes the GPU reads the bytes the guest wrote?
 *
 * A surface resolves as dma_base + offset and nothing here masks bit 31, so a
 * low physical offset reads RAM. On POSIX the contiguous window is an alias of
 * that same RAM (xbox_EnablePhysicalHeapAlias, enabled by the JSRF harness),
 * so the two views are the same bytes BY CONSTRUCTION -- which makes that host
 * the positive control for this probe: it must always say "match", and a
 * MISMATCH there means the probe is wrong, not the allocator.
 *
 * On Windows the window is separate VirtualAlloc storage, deliberately not a
 * view of RAM. A contiguous block the guest fills through 0x80XXXXXX is then
 * invisible to a GPU reading 0x00XXXXXX. That distinction is what separates
 * "contiguous memory now comes from the heap, and the aliasing is fixed" from
 * "the window has been made inert, and the corruption stopped because nothing
 * writes there any more" -- and no counter in this file can tell them apart,
 * because the draw and triangle totals rise either way.
 *
 * RECOMP_CONTIG_VERIFY=1. Read-only: two bounded reads through the resolver
 * this file already uses, so the window bounds check is not reimplemented. */
static void contig_verify(const char *what, uint32_t address, size_t bytes)
{
    /* Bit 31 is the contiguous window's base; kernel.h is not included here
     * and one constant is not worth pulling it in. */
    static const uint32_t contig_base = 0x80000000u;
    enum { SPAN_BYTES = 4096 };
    static int enabled = -1;
    static unsigned long calls;
    uint32_t low, high, mid, lsum = 0, hsum = 0;
    const uint32_t *lp, *hp;
    size_t span, i;
    char where[128];
    int same;

    if (enabled < 0) enabled = getenv("RECOMP_CONTIG_VERIFY") != NULL;
    if (!enabled || !address || bytes < sizeof(uint32_t)) return;

    /* Report the first few of each kind and then thin out. A mismatch that
     * only begins once the title starts streaming would be invisible if this
     * stopped entirely, and constant if it never stopped. */
    ++calls;
    if (calls > 16 && (calls % 2048) != 0) return;

    low  = address & ~contig_base;
    high = low | contig_base;

    /* Compare a span, not the first dword, and sample from the middle.
     *
     * The top-left corner of a surface is usually black, so four leading zero
     * dwords equal to four other leading zero dwords is a match that would
     * also be printed if the two views were separate storage that happened to
     * be untouched. That is the same trap as any absence measurement: the
     * comparison has to be able to come out different. Summing a span and
     * saying when BOTH sides are empty keeps a vacuous agreement legible. */
    span = bytes < SPAN_BYTES ? (bytes & ~(size_t)3) : SPAN_BYTES;
    if (span < sizeof(uint32_t)) return;
    mid = (uint32_t)((bytes / 2) & ~(size_t)3);
    if ((size_t)mid + span > bytes) mid = 0;

    lp = xbox_GpuMemoryRange(low  + mid, span);
    hp = xbox_GpuMemoryRange(high + mid, span);
    if (!lp || !hp) {
        fprintf(stderr, "  [CONTIG-VERIFY] %s resolved=%08X low=%s high=%s"
                        " (no comparison)\n",
                what, address, lp ? "mapped" : "unmapped",
                hp ? "mapped" : "unmapped");
        fflush(stderr);
        return;
    }

    for (i = 0; i < span / sizeof(uint32_t); ++i) {
        lsum = lsum * 31u + lp[i];
        hsum = hsum * 31u + hp[i];
    }
    same = memcmp(lp, hp, span) == 0;
    where[0] = '\0';
    if (!xbox_HeapDescribe(low, where, sizeof where)) where[0] = '\0';

    fprintf(stderr,
            "  [CONTIG-VERIFY] #%lu %s resolved=%08X %s"
            " +%X/%u low %08X sum=%08X (%08X %08X) |"
            " high %08X sum=%08X (%08X %08X)%s%s\n",
            calls, what, address,
            (!lsum && !hsum) ? "BOTH-EMPTY" : (same ? "match" : "MISMATCH"),
            mid, (unsigned)span,
            low  + mid, lsum, lp[0], lp[1],
            high + mid, hsum, hp[0], hp[1],
            where[0] ? " " : "", where);
    fflush(stderr);
}

/* WHAT EACH DRAW TARGETS AND SAMPLES, TALLIED PER REPORT WINDOW. RECOMP_DRAW_MIX.
 *
 * The xemu Load-screen trace, per flip: 19 draws that bind the render target
 * as TEXTURE0 while rendering into it, 8 with DXT3 art, 2 composites into a
 * back buffer. Our side counted ONE draw a flip sampling the surface it
 * renders into (21 Sep 2026, LOADMENU-FEEDBACK-ON). Either our guest does
 * not issue the other eighteen with that texture, or they resolve to another
 * address. This is the same tally taken on our stream: raw target and
 * texture payloads beside the resolved addresses, so a DMA-base difference
 * shows as a mismatch between the two columns. Rows reset every report. */
static int s_draw_dump_black_armed;   /* RECOMP_FB_DUMP_DRAW_ON_BLACK, see the report */
#define DRAW_MIX_ROWS 48
static struct { uint32_t tgt, tex, fmt, tex_addr, tgt_addr, untex, blend, mask, tex1, fmt1, icw0, icw1, icw2, ncomb, vmode, a3size, a3type, a0size, cull, front_cw, depth_test, alpha_test, prim, depth_func, depth_write, z_cull, zclear, vp_off_z, vp_scale_z; float cur3[4], v0[4], z_lo, z_hi; unsigned n, verts; long tris; size_t tex_nz, tex_n; }
    s_draw_mix[DRAW_MIX_ROWS];
static unsigned s_draw_mix_rows, s_draw_mix_overflow;
static int s_draw_mix_last = -1;   /* the row the draw in flight belongs to */
static void draw_mix_note(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_DRAW_MIX");
    if (!on) return;
    const NV2ATextureCopy *c = &s_copy.state;
    uint32_t tgt = c->target_offset, tex = c->untextured ? 0 : c->texture_offset;
    uint32_t fmt = c->untextured ? 0 : s_methods[NV097_SET_TEXTURE_FORMAT / 4];
    uint32_t blend = c->blend ? (c->blend_src << 16 | c->blend_dst) : 0;
    /* Stage 1 and the combiner, because the Load screen's config multiplies
     * by TEXTURE1 in its second stage and the shader reads an unbound stage
     * as zero. Which texture, in which format, under which program, is the
     * whole question for a draw that comes out black. */
    uint32_t mask = c->texture_mask;
    uint32_t tex1 = (mask & 2) ? s_methods[(0x1B40) / 4] : 0;
    uint32_t fmt1 = (mask & 2) ? s_methods[(0x1B44) / 4] : 0;
    uint32_t icw0 = c->combiner_count > 0 ? c->color_icw[0] : 0;
    uint32_t icw1 = c->combiner_count > 1 ? c->color_icw[1] : 0;
    uint32_t icw2 = c->combiner_count > 2 ? c->color_icw[2] : 0;
    /* Where PRIMARY_COLOR comes from: the vertex mode (0 = fixed function,
     * else the program start slot), whether attribute 3 (diffuse) is read
     * from a stream at all, and the 'current' register it falls back to when
     * it is not. A UI quad drawn with no diffuse stream takes its colour
     * from that register; if we hold zero there the quad is transparent. */
    uint32_t vmode = s_vsh.mode, a3size = s_gpu.attr[3].size, a3type = s_gpu.attr[3].type;
    uint32_t a0size = s_gpu.attr[0].size;
    uint32_t cull = c->cull_face, front_cw = c->front_cw, depth_test = c->depth_test, alpha_test = c->alpha_test;
    uint32_t depth_func = c->depth_func, depth_write = c->depth_write, z_cull = c->z_cull;
    uint32_t zclear = s_methods[0x1D8C / 4], vp_off_z = s_methods[0x0A28 / 4], vp_scale_z = s_methods[0x0AF8 / 4];
    unsigned i;
    s_draw_mix_last = -1;
    for (i = 0; i < s_draw_mix_rows; i++)
        if (s_draw_mix[i].tgt == tgt && s_draw_mix[i].tex == tex
                && s_draw_mix[i].vmode == vmode && s_draw_mix[i].a3size == a3size
                && s_draw_mix[i].cull == cull && s_draw_mix[i].front_cw == front_cw
                && s_draw_mix[i].depth_test == depth_test && s_draw_mix[i].alpha_test == alpha_test
                && s_draw_mix[i].prim == s_gpu.prim
                && s_draw_mix[i].depth_func == depth_func && s_draw_mix[i].depth_write == depth_write
                && s_draw_mix[i].z_cull == z_cull && s_draw_mix[i].zclear == zclear
                && s_draw_mix[i].vp_off_z == vp_off_z && s_draw_mix[i].vp_scale_z == vp_scale_z
                && s_draw_mix[i].z_lo == c->z_clip_min && s_draw_mix[i].z_hi == c->z_clip_max
                && s_draw_mix[i].fmt == fmt && s_draw_mix[i].untex == c->untextured
                && s_draw_mix[i].blend == blend && s_draw_mix[i].mask == mask
                && s_draw_mix[i].tex1 == tex1 && s_draw_mix[i].fmt1 == fmt1
                && s_draw_mix[i].icw0 == icw0 && s_draw_mix[i].icw1 == icw1
                && s_draw_mix[i].icw2 == icw2 && s_draw_mix[i].ncomb == c->combiner_count)
            { s_draw_mix[i].n++; s_draw_mix[i].verts += s_gpu.idx_count; s_draw_mix_last = (int)i; return; }
    if (s_draw_mix_rows >= DRAW_MIX_ROWS) { s_draw_mix_overflow++; return; }
    s_draw_mix[i].tgt = tgt; s_draw_mix[i].tex = tex; s_draw_mix[i].fmt = fmt;
    s_draw_mix[i].tex_addr = s_copy.texture_address; s_draw_mix[i].tgt_addr = s_copy.target_address;
    s_draw_mix[i].untex = c->untextured; s_draw_mix[i].blend = blend; s_draw_mix[i].n = 1;
    s_draw_mix[i].mask = mask; s_draw_mix[i].tex1 = tex1; s_draw_mix[i].fmt1 = fmt1;
    s_draw_mix[i].vmode = vmode; s_draw_mix[i].a3size = a3size; s_draw_mix[i].a3type = a3type; s_draw_mix[i].a0size = a0size;
    s_draw_mix[i].cull = cull; s_draw_mix[i].front_cw = front_cw; s_draw_mix[i].depth_test = depth_test; s_draw_mix[i].alpha_test = alpha_test;
    s_draw_mix[i].prim = s_gpu.prim; s_draw_mix[i].verts = s_gpu.idx_count; s_draw_mix[i].tris = 0;
    s_draw_mix[i].depth_func = depth_func; s_draw_mix[i].depth_write = depth_write; s_draw_mix[i].z_cull = z_cull;
    s_draw_mix[i].zclear = zclear; s_draw_mix[i].vp_off_z = vp_off_z; s_draw_mix[i].vp_scale_z = vp_scale_z;
    s_draw_mix[i].z_lo = c->z_clip_min; s_draw_mix[i].z_hi = c->z_clip_max;
    /* The first vertex as the backend receives it: screen x,y and w on the
     * CPU-transformed paths, object space when a GPU program will run. */
    memcpy(s_draw_mix[i].v0, s_outputs[0][0], sizeof s_draw_mix[i].v0);
    s_draw_mix_last = (int)i;
    memcpy(s_draw_mix[i].cur3, s_vsh.current[3], sizeof s_draw_mix[i].cur3);
    s_draw_mix[i].icw0 = icw0; s_draw_mix[i].icw1 = icw1; s_draw_mix[i].icw2 = icw2; s_draw_mix[i].ncomb = c->combiner_count;
    /* Are the bytes we hand the sampler anything at all? A texture that is
     * all zero in guest RAM decodes to transparent black in every format
     * this backend accepts, and no shader test can tell that from a decode
     * fault. Sampled once per row, capped, at the row's first draw. */
    s_draw_mix[i].tex_nz = s_draw_mix[i].tex_n = 0;
    if (!c->untextured && s_copy.texture) {
        size_t k, n = s_copy.texture_bytes < 262144 ? s_copy.texture_bytes : 262144, nz = 0;
        for (k = 0; k < n; k++) if (s_copy.texture[k]) nz++;
        s_draw_mix[i].tex_nz = nz; s_draw_mix[i].tex_n = n;
    }
    s_draw_mix_rows++;
}
static void draw_mix_result(int triangles)
{
    if (s_draw_mix_last >= 0 && s_draw_mix_last < (int)s_draw_mix_rows)
        s_draw_mix[s_draw_mix_last].tris += triangles < 0 ? 0 : triangles;
}
static void draw_mix_report(void)
{
    unsigned i;
    if (!s_draw_mix_rows) return;
    fprintf(stderr, "[DRAW-MIX] %u distinct (target, tex0, fmt, blend) rows this window%s\n",
            s_draw_mix_rows, s_draw_mix_overflow ? " (OVERFLOWED)" : "");
    for (i = 0; i < s_draw_mix_rows; i++)
        fprintf(stderr, "[DRAW-MIX]   %6u  target %08X (addr %08X)  tex0 %08X (addr %08X) fmt %08X%s tex-nonzero=%zu/%zu  mask %X tex1 %08X fmt1 %08X  comb %u icw %08X %08X %08X  vmode %u a0size %u a3 type %u size %u cur3=(%g %g %g %g)  prim %u verts %u TRIS-ACCEPTED %ld cull %X cw %u ztest %u zfunc %X zwrite %u zcull %u zrange %g..%g zclear %08X vpz off %08X scale %08X atest %u v0=(%g %g %g %g)  blend %08X%s\n",
                s_draw_mix[i].n, s_draw_mix[i].tgt, s_draw_mix[i].tgt_addr,
                s_draw_mix[i].tex, s_draw_mix[i].tex_addr, s_draw_mix[i].fmt,
                s_draw_mix[i].untex ? " UNTEXTURED" : "",
                s_draw_mix[i].tex_nz, s_draw_mix[i].tex_n,
                s_draw_mix[i].mask, s_draw_mix[i].tex1, s_draw_mix[i].fmt1,
                s_draw_mix[i].ncomb, s_draw_mix[i].icw0, s_draw_mix[i].icw1, s_draw_mix[i].icw2,
                s_draw_mix[i].vmode, s_draw_mix[i].a0size, s_draw_mix[i].a3type, s_draw_mix[i].a3size,
                s_draw_mix[i].cur3[0], s_draw_mix[i].cur3[1], s_draw_mix[i].cur3[2], s_draw_mix[i].cur3[3],
                s_draw_mix[i].prim, s_draw_mix[i].verts, s_draw_mix[i].tris, s_draw_mix[i].cull, s_draw_mix[i].front_cw, s_draw_mix[i].depth_test,
                s_draw_mix[i].depth_func, s_draw_mix[i].depth_write, s_draw_mix[i].z_cull, s_draw_mix[i].z_lo, s_draw_mix[i].z_hi, s_draw_mix[i].zclear, s_draw_mix[i].vp_off_z, s_draw_mix[i].vp_scale_z,
                s_draw_mix[i].alpha_test,
                s_draw_mix[i].v0[0], s_draw_mix[i].v0[1], s_draw_mix[i].v0[2], s_draw_mix[i].v0[3],
                s_draw_mix[i].blend,
                (!s_draw_mix[i].untex && s_draw_mix[i].tex_addr == s_draw_mix[i].tgt_addr)
                    ? "  <<< SAMPLES ITS OWN TARGET" : "");
    s_draw_mix_rows = 0; s_draw_mix_overflow = 0;
}

/* RECOMP_FOG_TRACE=1 -- G53's census, read-only, opt-in.
 *
 * Every distinct fog-and-final-combiner state a batch draws with: the final
 * combiner words (0x288/0x28C), FOG_ENABLE/MODE/GEN_MODE (0x2A4/0x29C/0x2A0),
 * FOG_COLOR (0x2A8), FOG_PARAMS (0x9C0..), FOG_PLANE (0x9D0..), the transform
 * MODE (0x1E94 bits 1:0), whether the bound program writes oFog, and
 * SPECULAR_FOG_FACTOR0/1 (0x1E20/4). One line when a state is first seen, the
 * table with counts every 30 s. */
#define FOG_TRACE_MAX 48
enum { FT_CW0, FT_CW1, FT_EN, FT_MODE, FT_GEN, FT_COLOR, FT_P0, FT_P1, FT_P2, FT_PL0, FT_PL1, FT_PL2, FT_PL3,
       FT_XF, FT_OFOG, FT_SF0, FT_SF1, FT_N };
static struct { uint32_t w[FT_N]; unsigned long n; } s_fog_trace[FOG_TRACE_MAX];
static unsigned s_fog_trace_n; static unsigned long s_fog_trace_over;
static void fog_trace_print(const char *why)
{
    fprintf(stderr, "[FOG-TRACE] %s: %u states%s\n", why, s_fog_trace_n, s_fog_trace_over ? " (table full; more not listed)" : "");
    for (unsigned i = 0; i < s_fog_trace_n; ++i) {
        const uint32_t *w = s_fog_trace[i].w; float p[3], pl[4];
        memcpy(p, &w[FT_P0], 12); memcpy(pl, &w[FT_PL0], 16);
        fprintf(stderr, "[FOG-TRACE]   %9lu batches: CW0 %08X CW1 %08X | FOG_ENABLE %u MODE %X GEN %u COLOR %08X |"
                        " PARAMS %g %g %g | PLANE %g %g %g %g | xf MODE %u, program writes oFog %u | SPECFOG %08X %08X\n",
                s_fog_trace[i].n, w[FT_CW0], w[FT_CW1], w[FT_EN], w[FT_MODE], w[FT_GEN], w[FT_COLOR], p[0], p[1], p[2],
                pl[0], pl[1], pl[2], pl[3], w[FT_XF], w[FT_OFOG], w[FT_SF0], w[FT_SF1]);
    }
    fflush(stderr);
}
static void fog_trace(void)
{
    static int on = -1; static time_t last;
    uint32_t w[FT_N]; unsigned i;
    if (on < 0) on = recomp_switch_on("RECOMP_FOG_TRACE");
    if (!on) return;
    memset(w, 0, sizeof w);
    w[FT_CW0] = s_methods[0x288/4]; w[FT_CW1] = s_methods[0x28c/4]; w[FT_EN] = s_methods[0x2a4/4];
    w[FT_MODE] = s_methods[0x29c/4]; w[FT_GEN] = s_methods[0x2a0/4]; w[FT_COLOR] = s_methods[0x2a8/4];
    for (i = 0; i < 3; ++i) w[FT_P0 + i] = s_methods[(0x9c0 + 4*i)/4];
    for (i = 0; i < 4; ++i) w[FT_PL0 + i] = s_methods[(0x9d0 + 4*i)/4];
    w[FT_XF] = s_methods[0x1e94/4] & 3u;
    if (s_vsh.mode == 2 && s_vsh.decoded.valid)
        for (int k = 0; k < s_vsh.decoded.length; ++k) {
            const NV2AVshInstruction *in = &s_vsh.decoded.insns[k];
            if ((in->mac_dst.output_mask && in->mac_dst.output_reg == NV2A_VSH_OUT_FOG)
             || (in->ilu_dst.output_mask && in->ilu_dst.output_reg == NV2A_VSH_OUT_FOG)) { w[FT_OFOG] = 1; break; }
        }
    w[FT_SF0] = s_methods[0x1e20/4]; w[FT_SF1] = s_methods[0x1e24/4];
    for (i = 0; i < s_fog_trace_n; ++i) if (!memcmp(s_fog_trace[i].w, w, sizeof w)) break;
    if (i == s_fog_trace_n) {
        if (s_fog_trace_n >= FOG_TRACE_MAX) { ++s_fog_trace_over; return; }
        memcpy(s_fog_trace[s_fog_trace_n].w, w, sizeof w); s_fog_trace[s_fog_trace_n].n = 0; ++s_fog_trace_n;
        fprintf(stderr, "[FOG-TRACE] new state %u at draw %u: CW0 %08X CW1 %08X FOG_ENABLE %u MODE %X GEN %u COLOR %08X"
                        " xf MODE %u oFog %u\n", i, s_gpu.draws, w[FT_CW0], w[FT_CW1], w[FT_EN], w[FT_MODE], w[FT_GEN],
                w[FT_COLOR], w[FT_XF], w[FT_OFOG]);
    }
    ++s_fog_trace[i].n;
    {   time_t now = time(NULL);
        if (!last) last = now;
        if (now - last >= 30) { last = now; fog_trace_print("periodic"); } }
}

static const char *prepare_texture_copy(void)
{
    s_copy.active = 0;
    s_copy.texture_address = s_copy.target_address = 0;
    s_copy.depth_address=0; s_copy.depth=NULL; s_copy.depth_bytes=0;
    /* Preserve the original diagnostic flat-colour path when no fragment
     * state has been supplied. Once configured, unsupported states reject. */
    if (!s_method_seen[0x1e60/4] && !s_method_seen[0x1b0c/4]) return NULL;
    const char *error = nv2a_texture_copy_prepare(s_methods, &s_copy.state);
    if (error) return error;
    const uint8_t *regs = xbox_Nv2aRegisterMemory();
    if (!regs) return "NV2A mapping unavailable";
    uint32_t ramht, base, limit;
    memcpy(&ramht, regs + 0x2210, 4);
    NV2ATextureCopy *c = &s_copy.state;
    s_copy.texture_bytes = nv2a_texture_copy_texture_bytes(c);
    s_copy.target_bytes = (size_t)c->target_pitch*(c->clip_y+c->clip_h);
    /* An untextured program has no texture object to resolve, and the handle
     * left in the method state belongs to whatever was bound last. Resolving
     * it rejected every diffuse-only draw with "texture DMA range". */
    if (c->untextured) {
        s_copy.texture_address = 0;
        s_copy.texture = NULL;
    } else {
        if (!nv2a_dma_resolve(regs+0x700000, 0x100000, ramht, c->texture_handle, &base, &limit)
                || (uint64_t)c->texture_offset+s_copy.texture_bytes > (uint64_t)limit+1
                || (uint64_t)base+c->texture_offset > UINT32_MAX) return "texture DMA range";
        s_copy.texture_address = base+c->texture_offset;
        s_copy.texture = xbox_GpuMemoryRange(s_copy.texture_address, s_copy.texture_bytes);
    }
    if (!nv2a_dma_resolve(regs+0x700000, 0x100000, ramht, c->target_handle, &base, &limit)
            || (uint64_t)c->target_offset+s_copy.target_bytes > (uint64_t)limit+1
            || (uint64_t)base+c->target_offset > UINT32_MAX) return "target DMA range";
    s_copy.target_address = base+c->target_offset;
    s_copy.target = xbox_GpuMemoryRange(s_copy.target_address, s_copy.target_bytes);
    if ((!s_copy.texture && !c->untextured) || !s_copy.target)
        return "surface outside mapped RAM";
    contig_verify("target ", s_copy.target_address, s_copy.target_bytes);
    if (!c->untextured)
        contig_verify("texture", s_copy.texture_address, s_copy.texture_bytes);
    if (!c->untextured
            && (uint64_t)s_copy.texture_address+s_copy.texture_bytes > s_copy.target_address
            && (uint64_t)s_copy.target_address+s_copy.target_bytes > s_copy.texture_address)
        return "overlapping texture and target";
    if (c->depth_test || c->stencil_test) {
        s_copy.depth_bytes=(size_t)c->depth_pitch*(c->clip_y+c->clip_h);
        if (!nv2a_dma_resolve(regs+0x700000,0x100000,ramht,c->depth_handle,&base,&limit)
                || (uint64_t)c->depth_offset+s_copy.depth_bytes>(uint64_t)limit+1
                || (uint64_t)base+c->depth_offset>UINT32_MAX) return "depth DMA range";
        s_copy.depth_address=base+c->depth_offset;
        s_copy.depth=xbox_GpuMemoryRange(s_copy.depth_address,s_copy.depth_bytes);
        if (!s_copy.depth) return "depth outside mapped RAM";
        if (((uint64_t)s_copy.depth_address+s_copy.depth_bytes>s_copy.target_address
                    && (uint64_t)s_copy.target_address+s_copy.target_bytes>s_copy.depth_address)
                || ((uint64_t)s_copy.depth_address+s_copy.depth_bytes>s_copy.texture_address
                    && (uint64_t)s_copy.texture_address+s_copy.texture_bytes>s_copy.depth_address))
            return "overlapping depth surface";
    }
    c->extra_stages=s_copy.extra_stages;
    for(unsigned unit=1;unit<4;++unit) if(c->texture_mask&(1u<<unit)) {
        NV2ATextureCopy *t=&s_copy.extra_stages[unit-1];
        memset(t,0,sizeof(*t));
        error=nv2a_texture_copy_prepare_image(s_methods,unit,t);
        if(error) return error;
        size_t bytes=nv2a_texture_copy_texture_bytes(t);
        if(!nv2a_dma_resolve(regs+0x700000,0x100000,ramht,t->texture_handle,&base,&limit)
                || (uint64_t)t->texture_offset+bytes>(uint64_t)limit+1
                || (uint64_t)base+t->texture_offset+bytes>UINT32_MAX) return "texture DMA range";
        uint32_t address=base+t->texture_offset;
        s_copy.extra_address[unit-1]=address;
        if((uint64_t)address+bytes>s_copy.target_address &&
                (uint64_t)s_copy.target_address+s_copy.target_bytes>address)
            return "overlapping texture and target";
        if((c->depth_test || c->stencil_test) && (uint64_t)address+bytes>s_copy.depth_address &&
                (uint64_t)s_copy.depth_address+s_copy.depth_bytes>address)
            return "overlapping depth surface";
        c->extra_texture[unit-1]=xbox_GpuMemoryRange(address,bytes);
        c->extra_size[unit-1]=bytes;
        if(!c->extra_texture[unit-1]) return "surface outside mapped RAM";
    }
    s_copy.active = 1;
    ++s_copy.batches;
    return NULL;
}



#define PB_PROG_SLOTS 1024
static struct { uint32_t method, param; } s_prog_log[PB_PROG_SLOTS];
static int s_prog_logged;

static void note_program_write(uint32_t method, uint32_t param)
{
    if (s_prog_logged < PB_PROG_SLOTS) {
        s_prog_log[s_prog_logged].method = method;
        s_prog_log[s_prog_logged].param  = param;
        s_prog_logged++;
    }
}

void nv2a_pb_exec_dump_program(void)
{
    int i;
    if (!s_prog_logged)
        return;
    fprintf(stderr, "[GPU] vertex program and constants, first %d writes:\n",
            s_prog_logged);
    for (i = 0; i < s_prog_logged; i++) {
        uint32_t m = s_prog_log[i].method, v = s_prog_log[i].param;
        float f;
        memcpy(&f, &v, sizeof(f));
        if (m >= NV097_SET_TRANSFORM_CONSTANT)
            fprintf(stderr, "  [GPU]   0x%04X = 0x%08X  (% .6f)\n", m, v, f);
        else
            fprintf(stderr, "  [GPU]   0x%04X = 0x%08X\n", m, v);
    }
    fflush(stderr);
}

/* Slot + 1 of each in-range method in s_unhandled, 0 for none. A 5 s sample
 * of the combo trick stage on 23 Sep 2026 put the linear scan this replaces
 * at 7% of the pushbuffer thread's busy time: 244 million unhandled methods
 * in one session, each walking the table. NV2A methods are dword offsets
 * below 0x2000, so a direct index covers every one the title emits; anything
 * else keeps the scan. */
static int16_t s_unhandled_slot[0x2000 / 4];

static void note_unhandled(uint32_t method)
{
    int i;
    s_gpu.unhandled_total++;
    if (method < 0x2000 && !(method & 3)) {
        int16_t k = s_unhandled_slot[method >> 2];
        if (k) {
            s_unhandled[k - 1].count++;
            return;
        }
        if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
            s_unhandled[s_unhandled_count].method = method;
            s_unhandled[s_unhandled_count].count = 1;
            s_unhandled_count++;
            s_unhandled_slot[method >> 2] = (int16_t)s_unhandled_count;
        }
        return;
    }
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled_count++;
    }
}

/* Where each attribute sits inside one inline vertex, in dwords.
 *
 * INLINE_ARRAY carries the vertices in the command stream instead of pointing
 * at a buffer, so there is no offset or stride to read: the layout is implied
 * by which attributes are enabled and what format each one declares, packed in
 * attribute order. Recomputed per batch, because a title changes the format
 * between batches and a stale layout silently misreads every vertex.
 *
 * s_inline_stride of 0 means the layout could not be derived -- an attribute
 * declared a format this decoder does not know how to size -- and the batch is
 * then left alone rather than guessed at. */
static uint32_t s_inline_off[NV_VERTEX_ATTRS];
static uint32_t s_inline_stride;

#define INLINE_ATTR_ABSENT 0xFFFFFFFFu

static void inline_layout(void)
{
    uint32_t a, n = 0;

    s_inline_stride = 0;
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        s_inline_off[a] = INLINE_ATTR_ABSENT;
        if (!at->size)
            continue;
        s_inline_off[a] = n;
        if (at->type == 2)                  /* float per component */
            n += at->size;
        else if (at->type == 4)             /* four normalised bytes, one dword */
            n += 1;
        else
            return;                         /* unknown packing: refuse the batch */
    }
    s_inline_stride = n;
}

/* Read attribute `a` of inline vertex `index`. Same contract as fetch_attr. */
static int fetch_inline(uint32_t a, uint32_t index, float out[4])
{
    const uint32_t *w;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!s_inline_stride || a >= NV_VERTEX_ATTRS
            || s_inline_off[a] == INLINE_ATTR_ABSENT
            || (index + 1) * s_inline_stride > s_gpu.inline_count)
        return 0;

    w = &s_gpu.inline_words[index * s_inline_stride + s_inline_off[a]];
    if (s_gpu.attr[a].type == 2) {
        for (i = 0; i < s_gpu.attr[a].size && i < 4; i++)
            memcpy(&out[i], &w[i], sizeof(float));
        return 1;
    }
    for (i = 0; i < 4; i++)                 /* type 4 */
        out[i] = (float)((w[0] >> (i * 8)) & 0xFFu) / 255.0f;
    return 1;
}

/* Read attribute `a` of vertex `index` as floats. Anything this does not
 * decode returns 0, so a caller sees a degenerate vertex rather than reading
 * past the array -- and the batch is refused, which is how UB_D3D was found:
 * every draw JSRF submits declares its diffuse colour as
 *
 *     attribute 3 type=0 size=4 stride=32 offset=0x01BA2010
 *
 * and type 0 is UB_D3D, which was not decoded. That one gap refused 70,228 of
 * 70,228 rejected batches. UB_D3D is a packed D3DCOLOR: the bytes are stored
 * B, G, R, A, which is why it cannot share the UB_OGL case below it. */
static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->offset || !a->size || !a->stride)
        return 0;
    p = mem + a->offset + (size_t)index * a->stride;

    switch (a->type) {
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
        /* Stored B, G, R, A. Only a 4-component D3DCOLOR is defined this way;
         * a shorter one would be a different declaration and is refused. */
        if (a->size != 4)
            return 0;
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    default:
        /* S1, CMP and the rest are undecoded. The rejection report names the
         * type, so the next one that matters identifies itself rather than
         * being guessed at. */
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    switch (s_gpu.format & 15) {
    case 1: case 2: case 3: return 2;
    case 4: case 5: case 6: case 7: case 8: return 4;
    default: return s_gpu.clip_w ? s_gpu.pitch / s_gpu.clip_w : 0;
    }
}


/* Hand the surface being drawn into to whoever can put it on screen.
 *
 * The executor rasterises into GUEST memory, which is the only place the
 * picture exists: the D3D8 GL backend owns the window and draws nothing,
 * because this title feeds the NV2A directly rather than calling D3D. So the
 * two halves have to be introduced, and this is the read-only half of that --
 * a host pointer to the pixels plus the geometry needed to interpret them.
 *
 * Returns NULL until a clear has established a real surface. The caller runs
 * on the thread owning the GL context and must not touch anything else here.
 */
/* THE FLIP READBACK, AND THE SWITCH THAT SIZES IT.
 *
 * At every FLIP_STALL snapshot_surface() calls nv2a_gpu_sync_range(), which
 * WAITS FOR THE GPU and reads the whole colour surface back into guest RAM.
 * The presenter then uploads that CPU buffer back onto the GPU
 * (d3d8_gl.c, glTexSubImage2D) and draws a quad. So the frame crosses
 * Metal -> CPU -> OpenGL to reach the screen, once per frame.
 *
 * MEASURED 18 Sep 2026, and this is why the switch exists. [STAGE] sync is
 * that one call and nothing else:
 *     player session 17:55, no instrumentation   5.19 ms of a 16.43 ms frame
 *     scripted, heavier scene, walk armed       11.89 ms of 32.82 ms
 * 32% of the player's frame, and 82% of all Metal sync time is the DRAIN
 * rather than the copy (69,144 ms against 14,861 ms).
 *
 * DIAGNOSTIC, in the same sense as RECOMP_METAL_NO_DEPTH_SYNC: a flip that
 * never reads the surface back is wrong by construction IF ANYTHING READS IT
 * -- the presenter does, and so do fb_watch() and the [FB] sampler, which run
 * immediately after and read guest RAM. Expect a stale or torn picture with
 * this on. The point is not to ship it; it is to find out whether removing
 * the wait removes the time, or merely moves the stall somewhere else. Only
 * a measurement can tell those apart, and the answer decides whether direct
 * Metal presentation is worth building.
 *
 * COUNTED IN BOTH ARMS, deliberately: with it OFF, `taken` is precisely what
 * the other arm would skip, so one control run sizes the A/B before it is
 * run. That discipline is what made the depth A/B readable. */
static int no_flip_sync_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_NO_FLIP_SYNC");
    return on;
}
static unsigned long long g_flip_syncs_taken;
static unsigned long long g_flip_syncs_skipped;

/* THE PRODUCER'S VIEW of the frame it published last, and the pool the frame
 * actually lives in. The bare pointers below are read by code that runs on the
 * PUSHER THREAD only -- fb_watch() and the flip trace, both called from the
 * FLIP_STALL handler a few lines after the copy -- and they stay valid because
 * the producer holds a reference to that frame until it publishes the next
 * one. Anything reading from ANOTHER thread goes through
 * frame_pool_acquire/release instead; see frame_pool.h for what the single
 * shared buffer they replace was doing wrong. */
static FramePool s_snap_pool = FRAME_POOL_INIT;
static const FramePoolSlot *s_snap_held;   /* the producer's own reference */

static uint8_t *s_snap;             /* the last completed frame, packed */
static uint32_t s_snap_w, s_snap_h, s_snap_bpp;
static int s_snap_wanted;

/* WHICH surface the copy above came from, and whether it is new.
 *
 * s_snap on its own cannot answer either question, and both are needed by
 * anything that wants to compare one frame against another: this title flips
 * between several colour surfaces, so two consecutive snapshots are normally
 * two different buffers, and snapshot_surface() returns without copying
 * whenever the geometry is not yet real, which leaves s_snap holding a frame
 * that has already been looked at. See fb_watch, which was dead for twelve
 * scripted runs for want of exactly this. */
static uint32_t s_snap_offset;      /* s_gpu.color_offset the copy was taken from */
static unsigned long s_snap_seq;    /* ++ on every copy actually performed */

/* Take the finished frame at the guest's own frame boundary.
 *
 * Presenting the surface live shows it part-drawn as often as finished, which
 * on screen is a flicker between the picture and the clear colour. The fix is
 * to copy it once per frame -- but only at a boundary the title actually
 * marks. NV097_CLEAR_SURFACE is not one: measured in claude-gfx-window-05,
 * snapshotting there made every sampled frame black, because this title clears
 * several times per frame and to more than one surface, so the copy kept
 * catching an offscreen target. NV097_FLIP_STALL is the real boundary -- it is
 * the title saying this frame is done -- and it fires once per frame on the
 * surface being displayed.
 *
 * Only once someone has asked for a surface, so a run with no window pays
 * nothing. The copy races the reader and can tear a frame; neither side may
 * block the other, and a torn frame is a far smaller artefact than the
 * flicker it replaces. */
/* FLIPS ARE PACED TO THE DISPLAY (24 Sep 2026).
 *
 * On the Xbox a flip completes at a vertical blank, so a title can never
 * present faster than 59.94 Hz. JSRF steps its simulation once per frame
 * (CActMan's anim counter runs at the flip rate, [ACTMAN-TIME]), so the flip
 * rate IS the game speed. Nothing here waited: while the GPU path was the
 * bottleneck the title ran at or below 60 and it did not show, but light
 * scenes already reached 110-147 fps (player session of 23 Sep), and with the
 * combiner specialisation the tutorial ran at 87 fps -- the game at 145% of
 * its real speed, anim ticks 87/s measured.
 *
 * A MINIMUM INTERVAL of one vblank period, not the vblank grid. Waiting for the
 * next grid point is what double-buffered v-sync does, and it would turn every
 * 17 ms frame into 33 ms; the minimum interval caps at 59.94 without punishing
 * a frame that is merely late. The deadline is carried (next += period), so the
 * average rate is exact rather than drifting with sleep overshoot; a frame
 * later than a whole period resets it. Sleeps to 1 ms short and yields the
 * rest, because nanosleep on macOS overshoots by that much under load.
 * RECOMP_FLIP_PACE=0 restores free-running flips. */
static unsigned long long g_flip_paced, g_flip_pace_wait_us;
static void flip_pace(void)
{
    static int on = -1;
    static double next;
    const double period = 1001.0 / 60000.0;
    struct timespec ts;
    double now;
    if (on < 0) on = recomp_switch_on_default("RECOMP_FLIP_PACE", 1);
    if (!on) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (next == 0.0 || now > next + period) {           /* first flip, or a long stall */
        next = now + period;
        return;
    }
    if (now < next) {
        double wait = next - now;
        g_flip_paced++;
        g_flip_pace_wait_us += (unsigned long long)(wait * 1e6);
        if (wait > 0.0015) {
            struct timespec sl;
            double s = wait - 0.001;
            sl.tv_sec = (time_t)s; sl.tv_nsec = (long)((s - (double)sl.tv_sec) * 1e9);
            nanosleep(&sl, NULL);
        }
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
            if (now >= next) break;
            FLIP_PACE_YIELD();
        }
    }
    next += period;
    if (next < now) next = now + period;
}

void nv2a_pb_exec_flip_pace_stats(unsigned long long *paced, unsigned long long *wait_us)
{
    if (paced) *paced = g_flip_paced;
    if (wait_us) *wait_us = g_flip_pace_wait_us;
}

static void snapshot_surface(void)
{
    uint32_t b = surface_bpp();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t y;

#if NV2A_GPU_PATH
    if (nv2a_gpu_on() && mem && s_gpu.color_offset && s_gpu.pitch
            && s_gpu.clip_h)
    {
        if (no_flip_sync_on()) {
            ++g_flip_syncs_skipped;
        } else {
            unsigned long long _t = pb_now_us();
            nv2a_gpu_sync_range((uint8_t *)mem + s_gpu.color_offset,
                    (size_t)s_gpu.pitch * (s_gpu.clip_y + s_gpu.clip_h));
            pb_stage_add(PB_STAGE_SYNC, _t);
            ++g_flip_syncs_taken;
        }
    }
#endif
    if (!s_snap_wanted || !s_gpu.color_offset || !s_gpu.clip_w || !s_gpu.clip_h)
        return;
    if (b != 2 && b != 4)
        return;
    if (!mem)
        return;

    {
        const uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h;
        /* Into a slot NOBODY IS READING. The buffer this replaces was
         * realloc'd here while the presenter was memcpy'ing out of it. */
        FramePoolSlot *f = frame_pool_begin(&s_snap_pool, w, h, b);
        if (!f)
            return;                 /* every slot spoken for; pool counts it */

        {
            unsigned long long _t_snap = pb_now_us();
            for (y = 0; y < h; y++)
                memcpy(f->px + (size_t)y * w * b,
                       mem + s_gpu.color_offset
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch
                           + (size_t)s_gpu.clip_x * b,
                       (size_t)w * b);
            pb_stage_add(PB_STAGE_SNAP, _t_snap);
        }

        /* Pixels, size, surface and sequence become visible TOGETHER, so a
         * reader can never pair one frame's width with another's height.
         * Every early return above leaves the published frame where it was,
         * which is how a reader still tells "a new frame arrived" from "the
         * old one is still sitting here". */
        frame_pool_publish(&s_snap_pool, f, w, h, b, s_gpu.color_offset);

        /* Hold it on the producer's behalf, and only then drop the frame
         * before it: fb_watch() runs next, on this thread, off the pointers
         * below. */
        {
            const FramePoolSlot *prev = s_snap_held;
            s_snap_held = frame_pool_acquire(&s_snap_pool);
            if (prev) frame_pool_release(&s_snap_pool, prev);
        }
        if (s_snap_held) {
            s_snap        = s_snap_held->px;
            s_snap_w      = s_snap_held->w;
            s_snap_h      = s_snap_held->h;
            s_snap_bpp    = s_snap_held->bpp;
            s_snap_offset = s_snap_held->offset;
            s_snap_seq    = s_snap_held->seq;
        }
    }
}

const void *nv2a_pb_exec_surface(uint32_t *w, uint32_t *h,
                                 uint32_t *pitch, uint32_t *bpp)
{
    uint32_t b;
    const uint8_t *mem;

    s_snap_wanted = 1;

    /* The frame AT the flip, which is a whole frame by construction.
     *
     * Reading the surface the guest is still drawing into, on the presenter's
     * own timer, catches it mid-draw: a fullscreen image is two triangles, the
     * software rasteriser manages a few frames a second, and the result tears
     * on a hard diagonal. That is what the flip copy exists to avoid.
     *
     * This was #if !defined(_WIN32) -- Windows took the live surface because
     * the flip copy was seen holding the anti-graffiti image while rendering
     * went on elsewhere. That trades a frozen picture for a torn one and hides
     * which of the two is actually happening. It is a runtime switch now, so
     * the two can be told apart in one run each: RECOMP_FB_LIVE=1 restores the
     * live surface, and the [FB] sampler line says whether the picture is
     * moving under either. */
    {
        static int live = -1;
        /* ONE HELD FRAME PER CALLING THREAD, swapped here.
         *
         * This entry point returns a pointer and has no release call, and its
         * callers -- the GL presenter's hook and the Win32 window thread --
         * use the pixels after it returns. Giving them the producer's live
         * buffer is what let a frame be realloc'd and rewritten underneath
         * them. Asking for the next frame is what gives the last one back, so
         * the contract is unchanged for the caller: the pointer stays valid
         * until that same thread calls again. */
        static FRAME_POOL_TLS const FramePoolSlot *t_held;
        const FramePoolSlot *f;

        if (live < 0) live = recomp_switch_on("RECOMP_FB_LIVE");
        if (!live) {
            f = frame_pool_reacquire(&s_snap_pool, &t_held);
            if (f && f->px && f->w && f->h) {
                if (w) *w = f->w;
                if (h) *h = f->h;
                if (pitch) *pitch = f->w * f->bpp;  /* the copy is packed */
                if (bpp) *bpp = f->bpp;
                return f->px;
            }
        }
    }

    /* Before the first flip there is no finished frame, so show the live
     * surface rather than nothing: the intro logos appear during this window.
     *
     * Windows deliberately stays on this path after flips too. JSRF continues
     * issuing FLIP_STALL while the saved frame remains the anti-graffiti image,
     * even though it renders later frames into rotating live surfaces. A
     * generation timeout therefore cannot distinguish the stale copy. Live
     * presentation may tear, but it keeps the displayed image in step with
     * the game and is the reliable path under Wine/CrossOver. */
    b = surface_bpp();
    if (!s_gpu.color_offset || !s_gpu.clip_w || !s_gpu.clip_h)
        return NULL;
    if (b != 2 && b != 4)
        return NULL;
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    if (!mem)
        return NULL;

    /* Bounds-check before handing a pointer to the presenter.
     *
     * Every rasteriser path in this file validates the surface against guest
     * RAM before touching it; this one handed out a raw pointer computed from
     * color_offset, clip and pitch and trusted them. Those come from the guest
     * and are mid-update while the parser is running, so a plausible-looking
     * set can address past the end of RAM -- measured on Windows, which reads
     * the live surface every frame: an access violation at guest 0x01FFFFF8,
     * eight bytes below the 32 MB boundary, five seconds into the run.
     *
     * xbox_GpuMemoryRange is the accessor the rest of the file uses and it
     * checks the whole span, not just the first byte. Returning NULL here just
     * means the presenter skips a frame, which is what it already does before
     * the first flip. */
    {
        uint32_t first = s_gpu.color_offset
                       + (uint32_t)s_gpu.clip_y * s_gpu.pitch
                       + (uint32_t)s_gpu.clip_x * b;
        uint64_t span = (uint64_t)s_gpu.clip_h * s_gpu.pitch;
        const uint8_t *p;

        if (!span || span > 0x08000000ull)
            return NULL;
        p = (const uint8_t *)xbox_GpuMemoryRange(first, (size_t)span);
        if (!p)
            return NULL;
        if (w) *w = s_gpu.clip_w;
        if (h) *h = s_gpu.clip_h;
        if (pitch) *pitch = s_gpu.pitch;
        if (bpp) *bpp = b;
        return p;
    }
}


/* Write a surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * One file series per instrument.
 *
 * These three call sites sample at completely different moments -- a report
 * fires mid-composition, a batch capture after one draw, the flip when a frame
 * is finished -- and they used to share a single sequence, so a directory of
 * NNN.bmp mixed them with nothing to tell them apart. Correlating "how many
 * are blank" against the flip log then counts report snapshots as presented
 * frames. The tag is what keeps the question answerable. */
/* RECOMP_FB_WATCH=x,y,w,h -- at EVERY flip, compare a region of the presented
 * copy (s_snap) against the same region the LAST TIME THIS SAME COLOUR SURFACE
 * was presented, and say when it changed. For a parked scene with static text
 * the region must never change, so every line this prints is a frame the
 * renderer got wrong -- and unlike RECOMP_FB_DUMP_FLIP's 24 captures it sees
 * all of them, which is what a one-frame-in-a-thousand defect needs.
 * Legitimate changes (the text advances, a cutscene cut) print too, with a
 * large pixel count; corruption is a small count that reverts on the next
 * frame from that surface.
 *
 * KEYED ON THE SURFACE, NOT ON THE FRAME COUNTER. This trap used to compare
 * flip N against flip N-1, and in that form it was DEAD -- it fired on every
 * single flip of a completely healthy run and could not have caught anything.
 *
 * The title double-buffers: it composes into one colour surface, flips it,
 * composes into another, flips that. Flip N and flip N-1 are therefore two
 * DIFFERENT buffers holding two different pictures, so "changed since the
 * previous flip" was always true. Measured 17 Sep 2026 across 39 watch*.bmp
 * dumps from a parked tutorial: the watched region alternates strictly ABABAB
 * by flip index, with exactly TWO distinct images, each byte-identical within
 * its parity -- the odd-flip image is a speaker-name label reading "Gum", the
 * even-flip image is solid black. Twelve scripted runs "never caught" the
 * glyph defect because a real change was indistinguishable from the rotation.
 * Any watch*.bmp produced before this change is evidence of nothing.
 *
 * Comparing flip N against flip N-2 would also un-stick it, but only while the
 * rotation is exactly two deep and never skips -- and that is an assumption
 * about the title rather than a fact about the frame in hand. It is wrong the
 * moment the rotation is three deep (surface_audit and the FLIPTRACE surface
 * list both already report three distinct colour surfaces here), a flip is
 * dropped, or a cut rebinds. Keying on the surface address makes no such
 * assumption and is free at this call site: snapshot_surface() runs
 * immediately before this, inside the same FLIP_STALL dispatch, and now
 * records the colour offset it copied from. The cost is one uint32_t compare
 * over at most FB_WATCH_SLOTS entries, once per flip.
 *
 * ARMED AND QUIET, OR NOT ARMED AT ALL? The old trap's silence, had it been
 * silent, would have been indistinguishable from a mistyped rectangle, an
 * unset variable, or a snapshot that never refreshed -- an absence measurement
 * with no positive control, which is the failure this tree keeps paying for.
 * `comparisons` is that control: it counts the times a region was actually
 * memcmp'd against the same surface's previous frame. comparisons=0 means the
 * instrument never ran, whatever `changes` says. fb_watch_report() prints the
 * whole set from nv2a_pb_exec_report(), and prints NOTHING when the watch is
 * not armed -- so in a report, no [FB-WATCH] line means the variable was not
 * set, a line with comparisons=0 means it was set but never got to look, and a
 * line with comparisons>0 and changes=0 is the real absence measurement. */
/* AND THE TRIGGER THAT ACTUALLY DESCRIBES THE DEFECT: RECOMP_FB_WATCH_STILL.
 *
 * "Small" is not a proxy for corruption. Measured 18 Sep 2026 over a whole
 * session: small changes fire about ONE HUNDRED PER MINUTE, uniformly from
 * t=0 to the end -- 2,000 of them between t=0 and t=1090 in one run -- and
 * every one looked at was ordinary animation. The dumps are worse than the
 * lines: dump_snapshot_bmp writes s_snap_w x s_snap_h, the WHOLE frame, which
 * is 921,654 bytes at 640x480, so any workable RECOMP_FB_WATCH_DUMP cap is
 * spent within about thirty seconds of RECOMP_FB_WATCH_AFTER -- all of it on
 * animation. The note further down already records the first attempt dying
 * exactly that way: 59 dumps, every one legitimate, before the window where
 * the defect was seen.
 *
 * The defect (G2) is one glyph's quad drawn with ANOTHER glyph's texture
 * coordinates, in a speech box or a trick name. Speech-box text is STATIC
 * between frames. So the discriminator is not how BIG the change was, it is
 * what the rest of the frame was doing when it happened: the region changed
 * WHILE THE SCENE WAS OTHERWISE STILL. This call site can measure that for
 * nothing, because s_snap is already a whole frame -- keep the whole frame
 * per surface beside the region, and the pixels OUTSIDE the rectangle answer
 * "was anything else moving?".
 *
 * ADDED, NOT SUBSTITUTED. comparisons/changes/small/large keep counting
 * exactly what they counted before and the [FB-WATCH] line is unchanged, so a
 * log taken before this and one taken after are still comparable. What the
 * switch changes is which event PRINTS and which event spends a BMP, because
 * that is the part that was unusable.
 *
 * THE THRESHOLD IS A TUNABLE, because "still" is not "byte-identical" in a
 * title that keeps animating behind its own dialogue boxes.
 * RECOMP_FB_WATCH_STILL_PPM is how many pixels outside the rectangle may
 * differ while the frame still counts as still, in parts per million of the
 * pixels outside it. The default 1000 ppm is 0.1%, which is 307 pixels of a
 * 640x480 frame; a camera move or a fade is tens of thousands. 0 demands a
 * byte-identical background, 1000000 accepts anything.
 *
 * AND THE REPORT CARRIES THE DISTRIBUTION, not just the verdict -- otherwise
 * the threshold is the next thing nobody can read past. If a run says
 * still=0, the reader has to be able to tell "the region never changed while
 * the scene was still" from "the scene is never still at this threshold". So
 * the still line prints min_outside (the smallest outside change seen on any
 * region change), a five-bucket histogram of those changes, and quiet= -- the
 * flips where the scene WAS still and the region did not change, which is the
 * positive control for the threshold itself. quiet=0 with comparisons in the
 * thousands means the threshold is too tight, and says so without a BMP.
 *
 * THE SAME FOUR-WAY READING, for the new counter: no [FB-WATCH-STILL] line at
 * all means RECOMP_FB_WATCH_STILL was not set; an "armed nothing" line means
 * it was set but RECOMP_FB_WATCH named no rectangle; comparisons=0 means it
 * was armed and never got to look; comparisons>0 with still=0 is the real
 * absence measurement. */
static void dump_snapshot_bmp(const char *tag, unsigned seq);
/* Declared here for the still dumps, which write the RECTANGLE rather than
 * the frame -- see the dump site in fb_watch(). */
static void write_bmp(const char *tag, unsigned seq,
                      const uint8_t *base, uint32_t pitch,
                      uint32_t x0, uint32_t y0,
                      uint32_t w, uint32_t h, uint32_t bpp);

/* One per colour surface in the flip rotation. This title uses three; eight
 * leaves room for a mode change without the slots thrashing, and a rotation
 * deeper than this is reported rather than silently mis-compared. */
#define FB_WATCH_SLOTS 8

/* Parts per million of the pixels OUTSIDE the rectangle that may differ
 * between two presents of the same surface and the frame still count as
 * still: 1000 ppm = 0.1% = 307 pixels of 640x480. Overridden by
 * RECOMP_FB_WATCH_STILL_PPM. */
#define FB_WATCH_STILL_PPM_DEFAULT 1000UL

/* zero, within budget, within 10x, within 100x, more. */
#define FB_WATCH_STILL_BUCKETS 5

static struct {
    int           init;          /* the environment has been parsed */
    int           on;            /* ...and it named a usable rectangle */
    uint32_t      x0, y0, ww, hh;
    size_t        len;           /* bytes in one captured region */
    uint8_t      *cur;           /* this flip's region */
    struct {
        uint32_t  off;           /* the guest colour offset this slot holds */
        uint8_t  *px;            /* that surface's region as last presented */
        uint8_t  *full;          /* ...and its WHOLE frame, for the stillness test */
        int       valid;
    } slot[FB_WATCH_SLOTS];
    unsigned long evicted;       /* surfaces dropped for want of a slot */
    unsigned long oob;           /* flips where the rectangle fell outside the frame */
    unsigned long snap_seq;      /* the s_snap_seq already looked at */
    unsigned long flips;         /* flips carrying a fresh snapshot */
    unsigned long stale;         /* flips whose snapshot had not refreshed */
    unsigned long first;         /* first sight of a surface: nothing to compare */
    unsigned long comparisons;   /* THE POSITIVE CONTROL -- read this first */
    unsigned long changes, small, big;
    unsigned long printed, dumped;
    unsigned      cap;           /* RECOMP_FB_WATCH_DUMP; 1 = disabled sentinel */
    int           after;         /* RECOMP_FB_WATCH_AFTER, wall-clock seconds */

    /* The stillness trigger. Everything below is inert unless still_on. */
    int           still_on;      /* RECOMP_FB_WATCH_STILL */
    unsigned long still_ppm;     /* RECOMP_FB_WATCH_STILL_PPM */
    size_t        full_len;      /* bytes in one whole-frame copy */
    unsigned long still_outside; /* pixels outside the rectangle, this geometry */
    unsigned long still_budget;  /* ...and how many of them may differ */
    unsigned long still_cmp;     /* THE NEW POSITIVE CONTROL -- read this first */
    unsigned long still_changes; /* THE NEW COUNT: changed while the scene was still */
    unsigned long still_moving;  /* changed while the scene was moving */
    unsigned long still_quiet;   /* scene still AND the region unchanged */
    unsigned long still_min;     /* smallest outside diff seen on a region change */
    int           still_min_seen;
    unsigned long still_bucket[FB_WATCH_STILL_BUCKETS];
    unsigned long still_printed, still_dumped;
    /* Region changes that qualified but were thrown away because
     * trace_seconds() had not yet reached `after`. On 19 Sep 2026 this was 88
     * of 110 and the log said nothing at all: `still=110 ... dumps=22` reads
     * as a dump cap, and the cap was 149. Count them, or AFTER is a silent
     * filter on the evidence. */
    unsigned long still_too_early;
} s_fbw;

static void fb_watch_drop_slots(void)
{
    unsigned i;
    for (i = 0; i < FB_WATCH_SLOTS; ++i) {
        free(s_fbw.slot[i].px);
        s_fbw.slot[i].px = NULL;
        free(s_fbw.slot[i].full);
        s_fbw.slot[i].full = NULL;
        s_fbw.slot[i].off = 0;
        s_fbw.slot[i].valid = 0;
    }
}

/* The whole-frame copies only.
 *
 * They are sized by the FRAME, so the region slots' own size check cannot
 * stand in for this one: 640x480 and 320x240 hold the same 64x32 rectangle at
 * the same bpp, so `len` is unchanged while every stored frame is now the
 * wrong size. Reading one at the new size is a buffer overrun, not a wrong
 * number. */
static void fb_watch_drop_full(void)
{
    unsigned i;
    for (i = 0; i < FB_WATCH_SLOTS; ++i) {
        free(s_fbw.slot[i].full);
        s_fbw.slot[i].full = NULL;
    }
}

/* Resolve the environment ONCE, from either entry point.
 *
 * fb_watch_report() calls this too, so a run that armed the watch and never
 * reached a flip now prints a line with comparisons=0 instead of no line at
 * all. "Not armed" and "armed but never looked" were otherwise the same
 * silence, which is the distinction the reading guide above exists to keep. */
static void fb_watch_init(void)
{
    const char *e;

    if (s_fbw.init) return;
    s_fbw.init = 1;
    /* Pin the trace clock's origin here, on the first flip, BEFORE any trap
     * can fire. RECOMP_FB_WATCH_AFTER is measured against it, and a clock
     * whose zero is "whenever the first trap happened to fire" makes AFTER
     * mean something different in every run. See trace_seconds(). */
    (void)trace_seconds();
    s_fbw.still_ppm = FB_WATCH_STILL_PPM_DEFAULT;

    e = getenv("RECOMP_FB_WATCH");
    if (e && sscanf(e, "%u,%u,%u,%u", &s_fbw.x0, &s_fbw.y0,
                    &s_fbw.ww, &s_fbw.hh) == 4 && s_fbw.ww && s_fbw.hh) {
        s_fbw.on = 1; s_snap_wanted = 1;
    }

    /* Opt-in through the shared grammar, so "=0" is off -- the convention this
     * tree adopted after a comment reading "defaults on" sat thirty lines
     * above a gate that never did. The threshold beside it carries a VALUE and
     * so cannot go through that helper; it is named in switch_audit.py's
     * VALUE_CARRYING for exactly that reason. */
    s_fbw.still_on = recomp_switch_on("RECOMP_FB_WATCH_STILL");
    e = getenv("RECOMP_FB_WATCH_STILL_PPM");
    if (e && *e) {
        char *end = NULL;
        unsigned long v = strtoul(e, &end, 10);
        /* A misparse keeps the documented default rather than silently
         * becoming 0, which would be the strictest possible threshold and
         * would read in the report as a scene that never holds still. */
        if (end && end != e && v <= 1000000UL) s_fbw.still_ppm = v;
    }

    /* Eagerly, like every other gate here. These used to be resolved inside
     * the small-change branch, so the dump budget was decided by the first
     * event rather than by the run, and nothing could dump for the still
     * trigger before the old one had fired at least once. */
    e = getenv("RECOMP_FB_WATCH_DUMP");
    s_fbw.cap = e ? (unsigned)atoi(e) : 0;
    if (!s_fbw.cap) s_fbw.cap = 1;     /* 1 = disabled sentinel */
    e = getenv("RECOMP_FB_WATCH_AFTER");
    s_fbw.after = e ? atoi(e) : 0;
}

/* How many pixels OUTSIDE the watched rectangle differ between this frame and
 * the last one presented from the SAME surface.
 *
 * A row where nothing moved costs one memcmp, which is the case this hunts
 * for; only a row that differs is walked pixel by pixel. There is deliberately
 * NO early exit once the budget is blown: the histogram in the report is what
 * tells the next reader where the threshold belongs, and a truncated count
 * would make every moving frame read as "just over budget". The cost is one
 * whole-frame compare per flip on an opt-in switch. */
static unsigned long fb_watch_outside_diff(const uint8_t *prev)
{
    const uint32_t bpp = s_snap_bpp;
    const size_t stride = (size_t)s_snap_w * bpp;
    const uint32_t rx1 = s_fbw.x0 + s_fbw.ww, ry1 = s_fbw.y0 + s_fbw.hh;
    unsigned long diff = 0;
    uint32_t y, s;

    for (y = 0; y < s_snap_h; ++y) {
        const uint8_t *a = prev   + (size_t)y * stride;
        const uint8_t *b = s_snap + (size_t)y * stride;
        uint32_t span[2][2];        /* {first pixel, pixel count} */
        uint32_t n = 0;

        if (y < s_fbw.y0 || y >= ry1) {     /* the rectangle misses this row */
            span[0][0] = 0; span[0][1] = s_snap_w; n = 1;
        } else {                            /* left of it, then right of it */
            if (s_fbw.x0)       { span[n][0] = 0;   span[n][1] = s_fbw.x0; ++n; }
            if (rx1 < s_snap_w) { span[n][0] = rx1; span[n][1] = s_snap_w - rx1; ++n; }
        }

        for (s = 0; s < n; ++s) {
            size_t off = (size_t)span[s][0] * bpp;
            size_t bytes = (size_t)span[s][1] * bpp;
            uint32_t k;

            if (!memcmp(a + off, b + off, bytes)) continue;
            if (bpp == 4) {
                const uint32_t *pa = (const uint32_t *)(const void *)(a + off);
                const uint32_t *pb = (const uint32_t *)(const void *)(b + off);
                for (k = 0; k < span[s][1]; ++k) if (pa[k] != pb[k]) ++diff;
            } else if (bpp == 2) {
                const uint16_t *pa = (const uint16_t *)(const void *)(a + off);
                const uint16_t *pb = (const uint16_t *)(const void *)(b + off);
                for (k = 0; k < span[s][1]; ++k) if (pa[k] != pb[k]) ++diff;
            } else {
                for (k = 0; k < span[s][1]; ++k)
                    if (memcmp(a + off + (size_t)k * bpp,
                               b + off + (size_t)k * bpp, bpp)) ++diff;
            }
        }
    }
    return diff;
}

/* This frame becomes the surface's "last presented" whole frame. A failed
 * allocation leaves the slot without one, which costs the stillness verdict
 * for that surface and nothing else -- still_cmp counts what was really
 * measured, so it shows up as a comparison that did not happen rather than as
 * a frame that was still. */
static void fb_watch_remember_full(unsigned i)
{
    if (!s_fbw.full_len) return;
    if (!s_fbw.slot[i].full) {
        s_fbw.slot[i].full = (uint8_t *)malloc(s_fbw.full_len);
        if (!s_fbw.slot[i].full) return;
    }
    memcpy(s_fbw.slot[i].full, s_snap, s_fbw.full_len);
}

/* Where this change sat relative to the threshold, for the report's histogram.
 * With a budget of 0 the upper buckets still separate "a handful of pixels"
 * from "a tenth of the screen", which is what the +10/+100 are for. */
static void fb_watch_note_outside(unsigned long outside)
{
    unsigned long b = s_fbw.still_budget;

    if (!s_fbw.still_min_seen || outside < s_fbw.still_min) {
        s_fbw.still_min = outside;
        s_fbw.still_min_seen = 1;
    }
    if (!outside)                         ++s_fbw.still_bucket[0];
    else if (outside <= b)                ++s_fbw.still_bucket[1];
    else if (outside <= b * 10 + 10)      ++s_fbw.still_bucket[2];
    else if (outside <= b * 100 + 100)    ++s_fbw.still_bucket[3];
    else                                  ++s_fbw.still_bucket[4];
}

static void fb_watch(void)
{
    size_t row, len, p;
    unsigned i, victim, diff;
    uint32_t y;
    int still = 0, still_known = 0;
    unsigned long outside = 0;

    fb_watch_init();
    if (!s_fbw.on || !s_snap || !s_snap_w || !s_snap_h) return;
    /* A rectangle outside the frame silently disables the whole instrument,
     * and a mistyped one looks exactly like a scene that never changed. Count
     * it so the report can say which happened. */
    if (s_fbw.x0 + s_fbw.ww > s_snap_w || s_fbw.y0 + s_fbw.hh > s_snap_h) {
        ++s_fbw.oob; return;
    }

    /* Only a copy taken at THIS flip is a new observation. snapshot_surface()
     * returns without copying whenever the geometry is not yet real, and
     * re-reading the previous frame's bytes would spend a comparison on
     * something already compared -- inflating the positive control with work
     * that looked at nothing. Counted separately instead. */
    if (s_snap_seq == s_fbw.snap_seq) { ++s_fbw.stale; return; }
    s_fbw.snap_seq = s_snap_seq;

    row = (size_t)s_fbw.ww * s_snap_bpp;
    len = row * s_fbw.hh;
    if (s_fbw.len != len) {     /* a mode change: every slot is the wrong size */
        fb_watch_drop_slots();
        free(s_fbw.cur);
        s_fbw.cur = (uint8_t *)malloc(len);
        s_fbw.len = len;
        if (!s_fbw.cur) { s_fbw.len = 0; s_fbw.on = 0; return; }
    }
    for (y = 0; y < s_fbw.hh; ++y)
        memcpy(s_fbw.cur + (size_t)y * row,
               s_snap + ((size_t)(s_fbw.y0 + y) * s_snap_w + s_fbw.x0)
                        * s_snap_bpp, row);
    ++s_fbw.flips;

    /* The whole-frame copies carry their own geometry check -- see
     * fb_watch_drop_full -- and the budget is a fraction of a pixel count that
     * only this geometry knows. */
    if (s_fbw.still_on) {
        size_t flen = (size_t)s_snap_w * s_snap_h * s_snap_bpp;
        if (s_fbw.full_len != flen) {
            fb_watch_drop_full();
            s_fbw.full_len = flen;
            s_fbw.still_outside = (unsigned long)s_snap_w * s_snap_h
                                - (unsigned long)s_fbw.ww * s_fbw.hh;
            s_fbw.still_budget = (unsigned long)
                ((unsigned long long)s_fbw.still_outside
                 * (unsigned long long)s_fbw.still_ppm / 1000000ULL);
        }
    }

    for (i = 0; i < FB_WATCH_SLOTS; ++i)
        if (s_fbw.slot[i].valid && s_fbw.slot[i].off == s_snap_offset)
            break;
    if (i == FB_WATCH_SLOTS) {
        /* First time this surface has been presented. Remember it; there is
         * nothing to compare against yet, and inventing one is the whole
         * mistake being fixed. */
        for (victim = 0; victim < FB_WATCH_SLOTS; ++victim)
            if (!s_fbw.slot[victim].valid) break;
        if (victim == FB_WATCH_SLOTS) {
            /* More surfaces in the rotation than slots. NEVER reuse a slot as
             * though it held this surface -- that reintroduces the cross-buffer
             * compare. Reclaim slot 0 and treat this as a first sighting, and
             * say so in the report so the number is not mistaken for quiet. */
            victim = 0;
            ++s_fbw.evicted;
            free(s_fbw.slot[0].px);
            s_fbw.slot[0].px = NULL;
            free(s_fbw.slot[0].full);
            s_fbw.slot[0].full = NULL;
            s_fbw.slot[0].valid = 0;
        }
        if (!s_fbw.slot[victim].px) {
            s_fbw.slot[victim].px = (uint8_t *)malloc(len);
            if (!s_fbw.slot[victim].px) return;
        }
        memcpy(s_fbw.slot[victim].px, s_fbw.cur, len);
        s_fbw.slot[victim].off = s_snap_offset;
        s_fbw.slot[victim].valid = 1;
        if (s_fbw.still_on) fb_watch_remember_full(victim);
        ++s_fbw.first;
        return;
    }

    ++s_fbw.comparisons;

    /* WAS THE REST OF THE FRAME STILL? Measured before the region compare,
     * because that one returns the moment the region is unchanged -- and the
     * still flips where the region did NOT change are the positive control
     * for the threshold, so they have to be counted on that path too. */
    if (s_fbw.still_on) {
        if (s_fbw.slot[i].full) {
            outside = fb_watch_outside_diff(s_fbw.slot[i].full);
            still_known = 1;
            ++s_fbw.still_cmp;
            still = outside <= s_fbw.still_budget;
        }
        fb_watch_remember_full(i);
    }

    if (!memcmp(s_fbw.slot[i].px, s_fbw.cur, len)) {
        if (still_known && still) ++s_fbw.still_quiet;
        return;                 /* this surface is presenting the same picture */
    }

    diff = 0;
    for (p = 0; p < len; p += s_snap_bpp)
        if (memcmp(s_fbw.slot[i].px + p, s_fbw.cur + p, s_snap_bpp)) ++diff;
    ++s_fbw.changes;
    if (still_known) {
        fb_watch_note_outside(outside);
        if (still) ++s_fbw.still_changes; else ++s_fbw.still_moving;
    }
    /* A whole-region change is the scene moving (gameplay, a fade, a cut) and
     * would print sixty lines a second in a player's log; the defect this
     * hunts is a few glyphs, i.e. a SMALL change. Small changes print, capped;
     * the rest are counted.
     *
     * MEASURED, AND IT IS NOT THE DEFECT: ~100 small changes a minute for a
     * whole session, all animation. The counters stay so old logs still read
     * the same way, but with the stillness trigger armed the LINES and the
     * BMPs belong to it -- see the still block below. */
    if (diff * 4 < s_fbw.ww * s_fbw.hh) {
        ++s_fbw.small;
        if (!s_fbw.still_on && s_fbw.printed++ < 2000)
            fprintf(stderr, "[FB-WATCH] flip %lu t=%.2f region %u,%u %ux%u"
                    " surface 0x%08X CHANGED: %u of %u pixels differ from the"
                    " last frame presented FROM THIS SAME SURFACE"
                    " (small change #%lu, large so far %lu)\n",
                    s_fbw.flips, trace_seconds(), s_fbw.x0, s_fbw.y0,
                    s_fbw.ww, s_fbw.hh, s_snap_offset, diff,
                    s_fbw.ww * s_fbw.hh, s_fbw.printed, s_fbw.big);
        /* AND KEEP THE PICTURE. A line saying "643 pixels changed" cannot be
         * told apart from a line saying "the text is animating", and the
         * defect this hunts appeared ONCE in twenty-four captures -- a
         * periodic capture stride will almost always miss it. With
         * RECOMP_FB_DUMP set, every small change writes the presented copy as
         * watchNNN.bmp, so a long parked run either catches the corruption
         * with evidence or proves it did not recur. Bounded, because a scene
         * that animates text would otherwise fill the disk.
         *
         * AND ONLY ONCE THE SCENE HAS SETTLED. The first attempt spent its
         * whole budget on the dialogue typing itself out -- 59 dumps, all of
         * them legitimate animation, before the window where the defect was
         * seen. RECOMP_FB_WATCH_AFTER is the wall-clock second to start
         * dumping at. */
        if (!s_fbw.still_on && s_fbw.cap > 1 && s_fbw.dumped < s_fbw.cap - 1
            && trace_seconds() >= (double)s_fbw.after)
            dump_snapshot_bmp("watch", (unsigned)s_fbw.dumped++);
    } else ++s_fbw.big;

    /* THE EVENT THE DEFECT ACTUALLY IS: this region changed and nothing else
     * did. Independent of the small/large split on purpose -- a glyph drawn
     * with the wrong texture coordinates can rewrite most of a tight
     * rectangle, and would be classified "large" and never printed. */
    if (still_known && still) {
        if (s_fbw.still_printed++ < 2000)
            fprintf(stderr, "[FB-WATCH-STILL] flip %lu t=%.2f region %u,%u"
                    " %ux%u surface 0x%08X CHANGED WHILE THE SCENE WAS STILL:"
                    " %u of %u pixels in the region differ from the last frame"
                    " presented FROM THIS SAME SURFACE, while %lu of the %lu"
                    " pixels outside it did (budget %lu)"
                    " (still change #%lu, moving so far %lu)\n",
                    s_fbw.flips, trace_seconds(), s_fbw.x0, s_fbw.y0,
                    s_fbw.ww, s_fbw.hh, s_snap_offset, diff,
                    s_fbw.ww * s_fbw.hh, outside, s_fbw.still_outside,
                    s_fbw.still_budget, s_fbw.still_printed, s_fbw.still_moving);
        /* THE RECTANGLE, NOT THE WHOLE FRAME. dump_snapshot_bmp writes
         * s_snap_w x s_snap_h -- 921,654 bytes at 640x480 -- which is what
         * emptied the budget in thirty seconds the first time round. write_bmp
         * already takes a sub-rectangle, the watched region at 200x40 is 24 KB,
         * and it is the only part of the frame this instrument has a claim
         * about. RECOMP_FB_WATCH_DUMP still caps the series,
         * RECOMP_FB_WATCH_AFTER still delays it, and the files are stillNNN.bmp
         * so they cannot be confused with the old watchNNN.bmp series. */
        if (s_fbw.cap > 1 && s_fbw.still_dumped < s_fbw.cap - 1) {
            if (trace_seconds() >= (double)s_fbw.after)
                write_bmp("still", (unsigned)s_fbw.still_dumped++, s_snap,
                          s_snap_w * s_snap_bpp, s_fbw.x0, s_fbw.y0,
                          s_fbw.ww, s_fbw.hh, s_snap_bpp);
            else
                ++s_fbw.still_too_early;
        }
    }

    memcpy(s_fbw.slot[i].px, s_fbw.cur, len);
}

/* What the FF batch watch saw, whether or not it saw anything.
 *
 * g_ff_watch_changes was incremented from the day the watch was written and
 * printed nowhere, so the only way to read the instrument was to count its
 * per-event lines by hand -- and those are capped by nothing, so a busy run
 * buries them. "A counter that is never printed is not an instrument."
 *
 * Silent when the watch is not armed. The four-way reading: no line means
 * RECOMP_FF_BATCH_WATCH_TEX was not set; tracked=0 means it was set and no
 * batch ever used that texture (check the offset); changes=0 with tracked>0
 * is the real absence measurement; and dropped>0 means slots ran out, so a
 * low change count is not evidence of anything. */
static void ff_watch_report(void)
{
    unsigned k, tracked = 0;
    unsigned long draws = 0;
    if (!ff_watch_tex()) return;
    for (k = 0; k < FF_WATCH_KEYS; ++k)
        if (g_ff_watch[k].draws) { ++tracked; draws += g_ff_watch[k].draws; }
    fprintf(stderr, "  [FF-WATCH] texture 0x%08X: %u batch(es) tracked over"
            " %lu draws, %lu reported a change%s | slots %u%s\n",
            ff_watch_tex(), tracked, draws, g_ff_watch_changes,
            tracked ? "" : "   <- NO BATCH EVER USED THIS TEXTURE: the offset"
                           " is wrong, or the arm never drew it",
            FF_WATCH_KEYS,
            g_ff_watch_nokey
                ? "   <- SLOTS EXHAUSTED, batches went unwatched; a low change"
                  " count here means nothing"
                : "");
    /* Only the batches that actually changed, and at most a dozen: with 64
     * slots the full list is 64 lines a report, which buries the summary the
     * reader came for. A batch that never changed is the expected case and
     * `tracked` already counts it. */
    {
        unsigned listed = 0, changed = 0;
        for (k = 0; k < FF_WATCH_KEYS; ++k)
            if (g_ff_watch[k].changes) ++changed;
        for (k = 0; k < FF_WATCH_KEYS && listed < 12; ++k)
            if (g_ff_watch[k].changes) {
                ++listed;
                fprintf(stderr, "  [FF-WATCH]   batch#%lu: %u verts, %lu draws,"
                        " %lu changed\n", g_ff_watch[k].ordinal,
                        g_ff_watch[k].verts, g_ff_watch[k].draws,
                        g_ff_watch[k].changes);
            }
        if (changed > listed)
            fprintf(stderr, "  [FF-WATCH]   ...and %u more batch(es) that"
                    " changed\n", changed - listed);
    }
    if (g_ff_watch_nokey)
        fprintf(stderr, "  [FF-WATCH]   %lu draw(s) had no free slot\n",
                g_ff_watch_nokey);
}

/* What the trap did, whether or not it found anything.
 *
 * Silent when the watch is not armed, so this costs an unrelated run nothing;
 * see the comparisons/changes reading guide on fb_watch above. */
static void fb_watch_report(void)
{
    unsigned i, live = 0;

    fb_watch_init();
    if (!s_fbw.on) {
        /* Armed the trigger, named no rectangle. Saying nothing here would
         * read as "RECOMP_FB_WATCH_STILL was not set", which is the one
         * mistake this instrument's whole comment block is about. */
        if (s_fbw.still_on)
            fprintf(stderr, "[FB-WATCH-STILL] armed nothing:"
                    " RECOMP_FB_WATCH_STILL is set but RECOMP_FB_WATCH named no"
                    " rectangle -- set RECOMP_FB_WATCH=x,y,w,h\n");
        return;
    }
    for (i = 0; i < FB_WATCH_SLOTS; ++i)
        if (s_fbw.slot[i].valid) ++live;
    fprintf(stderr,
            "[FB-WATCH] region %u,%u %ux%u keyed on the colour surface:"
            " %lu comparisons against the same surface's previous frame,"
            " %lu changed (%lu small, %lu large) | %lu flips,"
            " %u surfaces in the rotation, %lu first sightings,"
            " %lu flips with no fresh snapshot,"
            " %lu flips with the rectangle outside the %ux%u frame,"
            " %lu slot evictions, %lu lines printed, %lu watch*.bmp written\n",
            s_fbw.x0, s_fbw.y0, s_fbw.ww, s_fbw.hh,
            s_fbw.comparisons, s_fbw.changes, s_fbw.small, s_fbw.big,
            s_fbw.flips, live, s_fbw.first, s_fbw.stale,
            s_fbw.oob, s_snap_w, s_snap_h,
            s_fbw.evicted, s_fbw.printed, s_fbw.dumped);
    if (!s_fbw.still_on) return;
    /* comparisons= is this line's positive control, exactly as on the line
     * above: 0 means the trigger was armed and never got to look. still= is
     * the new count, and everything after the second bar is what says whether
     * the threshold is worth trusting before anyone spends a BMP on it. */
    fprintf(stderr,
            "[FB-WATCH-STILL] region %u,%u %ux%u, the scene outside it held"
            " still: comparisons=%lu still=%lu moving=%lu quiet=%lu |"
            " ppm=%lu budget=%lu of %lu pixels outside the rectangle |"
            " min_outside=%ld | outside-diff over region changes:"
            " zero=%lu within_budget=%lu upto10x=%lu upto100x=%lu more=%lu |"
            " lines=%lu dumps=%lu still*.bmp\n",
            s_fbw.x0, s_fbw.y0, s_fbw.ww, s_fbw.hh,
            s_fbw.still_cmp, s_fbw.still_changes, s_fbw.still_moving,
            s_fbw.still_quiet, s_fbw.still_ppm, s_fbw.still_budget,
            s_fbw.still_outside,
            s_fbw.still_min_seen ? (long)s_fbw.still_min : -1L,
            s_fbw.still_bucket[0], s_fbw.still_bucket[1], s_fbw.still_bucket[2],
            s_fbw.still_bucket[3], s_fbw.still_bucket[4],
            s_fbw.still_printed, s_fbw.still_dumped);
    /* Only when it bit, so a run that never hit the gate stays as quiet as it
     * was before. dumps= alone cannot distinguish "the cap ran out" from "the
     * clock had not got there yet", and those want opposite fixes. */
    if (s_fbw.still_too_early)
        fprintf(stderr, "  [FB-WATCH-STILL] discarded_before_after=%lu of %lu"
                " still change(s) DISCARDED UNDUMPED: trace_seconds() had not"
                " reached RECOMP_FB_WATCH_AFTER=%ds. dumps= is not the whole"
                " story; lower AFTER to keep them.\n",
                s_fbw.still_too_early, s_fbw.still_changes, s_fbw.after);
}

/* Write one surface region to an explicit path. The prefix-and-sequence
 * naming the dumpers use is built by write_bmp below; a mark supplies its
 * own path (see nv2a_pb_exec_snapshot_to_file) so it can never land in, or
 * overwrite, the RECOMP_FB_DUMP directory. Returns 1 if a file was written. */
static int write_bmp_path(const char *path,
                          const uint8_t *base, uint32_t pitch,
                          uint32_t x0, uint32_t y0,
                          uint32_t w, uint32_t h, uint32_t bpp)
{
    uint32_t y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!path || !base || !w || !h || (bpp != 2 && bpp != 4))
        return 0;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    f = fopen(path, "wb");
    if (!f)
        return 0;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = base + (size_t)(y0 + y) * pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[x0 + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[x0 + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    {
        static int announced;
        if (!announced++)
            fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u, %ubpp)\n",
                    path, w, h, bpp);
    }
    return 1;
}

static void write_bmp(const char *tag, unsigned seq,
                      const uint8_t *base, uint32_t pitch,
                      uint32_t x0, uint32_t y0,
                      uint32_t w, uint32_t h, uint32_t bpp)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    char path[512];
    if (!prefix)
        return;
    snprintf(path, sizeof path, "%s%s%03u.bmp", prefix, tag, seq);
    write_bmp_path(path, base, pitch, x0, y0, w, h, bpp);
}

/* The presented frame, to a path the caller names. For a mark: the picture
 * the player was looking at when they pressed the key. Independent of
 * RECOMP_FB_DUMP by design. */
int nv2a_pb_exec_snapshot_to_file(const char *path)
{
    const FramePoolSlot *f = frame_pool_acquire(&s_snap_pool);
    int ok = 0;
    if (f && f->px && f->w && f->h)
        ok = write_bmp_path(path, f->px, f->w * f->bpp, 0, 0,
                            f->w, f->h, f->bpp);
    frame_pool_release(&s_snap_pool, f);
    return ok;
}

/* The surface as it stands right now, wherever the guest last pointed it. */
static void dump_surface_bmp(const char *tag, unsigned seq)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();

#if NV2A_GPU_PATH
    if (nv2a_gpu_on() && mem && s_gpu.color_offset && s_gpu.pitch
            && s_gpu.clip_h)
    {
        unsigned long long _t = pb_now_us();
        nv2a_gpu_sync_range((uint8_t *)mem + s_gpu.color_offset,
                (size_t)s_gpu.pitch * (s_gpu.clip_y + s_gpu.clip_h));
        pb_stage_add(PB_STAGE_SYNC, _t);
    }
#endif
    if (!mem || !s_gpu.color_offset)
        return;
    write_bmp(tag, seq, mem + s_gpu.color_offset, s_gpu.pitch,
              s_gpu.clip_x, s_gpu.clip_y, s_gpu.clip_w, s_gpu.clip_h, bpp);
}

/* The frame that was actually presented.
 *
 * nv2a_pb_exec_surface hands the window s_snap, the copy taken at FLIP_STALL,
 * not the live surface -- so a capture of the live surface at present time is
 * not a capture of what the viewer saw. Between the flip and the present the
 * parser finishes its bounded step, which can carry it into the next frame's
 * clear and can even re-point color_offset at another surface. This dumps the
 * copy, which is the only thing the display path reads. */
/* ACQUIRES, because this one has two callers on two threads: fb_watch() on the
 * pusher, and nv2a_pb_exec_dump_surface() on the report timer. The second was
 * reading the producer's buffer while the producer was filling it. */
/* Defined just below; the freshness report needs it. */
static uint32_t snapshot_nonzero_of(const FramePoolSlot *f);

/* A SNAPSHOT THAT IS NOT FRESH LIES IN THE OPPOSITE DIRECTION, AND SILENTLY.
 *
 * s_snap is republished at FLIP_STALL. If the guest stops flipping -- which is
 * one of the things a black screen can BE -- the pool keeps handing out the
 * last frame it was given, and a series of snapNNN files then shows the last
 * good picture over and over while the screen the player is looking at is
 * black. Read naively that says "the renderer is fine", which is exactly the
 * error the live-surface captures made in reverse.
 *
 * FramePoolSlot already carries what settles it: `seq`, which increments on
 * every publish and is 0 when nothing was ever published, and `offset`, the
 * colour surface the copy was taken from. So each dump records the identity of
 * the frame it wrote and says when that identity has NOT MOVED since the last
 * one. A repeat is still written to disk -- the file is evidence either way --
 * but it can no longer be mistaken for a fresh frame.
 *
 * NOT-PUBLISHED and REPEAT are distinguished, because they mean different
 * things: the first is a pool nothing ever reached, the second is a producer
 * that has stopped. */
static void dump_snapshot_bmp(const char *tag, unsigned seq)
{
    static unsigned long last_seq;
    static unsigned last_offset;
    static int have_last;
    const FramePoolSlot *f = frame_pool_acquire(&s_snap_pool);
    if (f && f->px && f->w && f->h) {
        int repeat = have_last && f->seq == last_seq;
        write_bmp(tag, seq, f->px, f->w * f->bpp, 0, 0, f->w, f->h, f->bpp);
        fprintf(stderr, "  [SNAP] %s%03u t=%.2f frame=%lu surface=%08X"
                " %ux%u %ubpp nonzero=%u/%u%s%s\n",
                tag, seq, trace_seconds(), f->seq, f->offset,
                f->w, f->h, f->bpp * 8,
                snapshot_nonzero_of(f), f->w * f->h,
                repeat ? "   <<< SAME FRAME AS THE LAST DUMP -- the guest has"
                         " not flipped since, so this file is a REPEAT" : "",
                (have_last && f->offset != last_offset)
                    ? "   (surface changed)" : "");
        last_seq = f->seq; last_offset = f->offset; have_last = 1;
    } else {
        fprintf(stderr, "  [SNAP] %s%03u t=%.2f NOTHING PUBLISHED -- no frame"
                " has ever reached the snapshot pool, so no file was written\n",
                tag, seq, trace_seconds());
    }
    frame_pool_release(&s_snap_pool, f);
}

/* Non-black pixels in the presented copy. "Blank" is the whole question at the
 * flip, and a number answers it in the log without opening a file. */
static uint32_t snapshot_nonzero_of(const FramePoolSlot *f)
{
    uint32_t n = 0, i, count;

    if (!f || !f->px || !f->w || !f->h)
        return 0;
    count = f->w * f->h;
    if (f->bpp == 2) {
        const uint16_t *p = (const uint16_t *)f->px;
        for (i = 0; i < count; i++) if (p[i]) n++;
    } else if (f->bpp == 4) {
        const uint32_t *p = (const uint32_t *)f->px;
        for (i = 0; i < count; i++) if (p[i] & 0x00FFFFFFu) n++;
    }
    return n;
}

/* The producer's own frame. Only correct on the pusher thread; every other
 * caller acquires. */
static uint32_t snapshot_nonzero(void)
{
    return snapshot_nonzero_of(s_snap_held);
}

/* Whoever owns the ring lends its recent-method dump.
 *
 * The executor is linked into tests that have no pusher at all, so calling
 * nv2a_pusher_dump_recent directly would make every one of them fail to link
 * for the sake of one diagnostic line. The same shape the pusher already uses
 * for its software-method handler: the owner installs it, and without one the
 * trace simply prints less. */
static void (*s_recent_dump)(int max_entries);

void nv2a_pb_exec_set_recent_dump(void (*fn)(int max_entries))
{
    s_recent_dump = fn;
}

/* Every distinct surface the guest has bound, in the order first seen.
 *
 * A title that composes into one surface and flips another leaves the flip
 * looking empty however well the drawing worked, and the only way to tell that
 * apart from "nothing was drawn" is to look at the others at the same moment.
 * Small and fixed: this title uses three. */
#define NV_MAX_TRACKED_SURFACES 8
static uint32_t s_surfaces[NV_MAX_TRACKED_SURFACES];
static unsigned s_surface_count;

static void note_surface(uint32_t offset)
{
    unsigned i;
    if (!offset)
        return;
    for (i = 0; i < s_surface_count; i++)
        if (s_surfaces[i] == offset)
            return;
    if (s_surface_count < NV_MAX_TRACKED_SURFACES)
        s_surfaces[s_surface_count++] = offset;
}

/* Non-black pixels in one surface, read with the geometry in force now. */
static uint32_t surface_nonzero(uint32_t offset, uint32_t bpp)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t n = 0, x, y;

    if (!mem || !offset || !s_gpu.pitch || !s_gpu.clip_w || !s_gpu.clip_h)
        return 0;
    for (y = 0; y < s_gpu.clip_h; y++) {
        const uint8_t *row = mem + offset
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++) if (p[x]) n++;
        } else if (bpp == 4) {
            const uint32_t *p = (const uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++) if (p[x] & 0x00FFFFFFu) n++;
        }
    }
    return n;
}

/* What the guest emitted between its last draw and its flip.
 *
 * Every other flip instrument samples after the poll returns, which is after
 * the parser has finished its bounded step -- so a blank capture there cannot
 * tell "the frame was never composed" from "the parser walked on into the next
 * clear before anyone looked". This runs inside the FLIP_STALL dispatch, the
 * one moment the guest itself calls a frame finished, and reports the copy
 * that will be presented plus the method order that led to it.
 *
 * RECOMP_FLIP_TRACE=<stride> traces every stride-th flip, capped, so a
 * 110-second run costs a few dozen lines. Silent unless asked for. */
/* Frame pacing, at the title's own frame boundary.
 *
 * There was no frame-time instrument here at all, which is why every
 * performance claim in this project so far has been made in one of three
 * currencies -- seconds of boot, total draws, or controller polls -- and none
 * of them is what a player feels. Boot time is dominated by file I/O. Total
 * draws divided by seconds is a mean, and a mean hides exactly the thing that
 * makes a game unplayable: a 90th percentile twice the median, or one 400 ms
 * stall a second. Two runs can post the same mean and be completely different
 * to play.
 *
 * NV097_FLIP_STALL is the right boundary and the only honest one: it is the
 * guest saying "this frame is finished", so the interval between two of them
 * is a frame as the title counts them. Deliberately measured FIRST in that
 * dispatch, before snapshot_surface(), so each interval covers everything
 * that happened in the frame including our own diagnostic sync -- the cost is
 * real, the player pays it, and leaving it out would flatter us.
 *
 * A histogram rather than a running mean, because percentiles are the point.
 * Unconditional: an opt-in perf counter is one nobody has switched on when
 * they need the number. The bins, the percentile and the per-frame split live
 * in frame_hist.h, where a test can reach them without a device. */
#include "frame_hist.h"

/* Two of them, and the second is the one you usually want.
 *
 * `run` is everything since process start. It is the honest summary of a whole
 * session, and it is nearly useless for comparing two builds, because a JSRF
 * run is three or four different workloads in sequence -- logos, title menu,
 * cutscene, gameplay -- and a cumulative percentile is a blend of however much
 * of each this particular run happened to reach. Two runs that got to
 * different places post different percentiles for that reason alone, which is
 * the same trap as comparing their draw totals.
 *
 * `win` covers only the interval since the previous report, so successive
 * lines describe successive segments and a scene can be compared against the
 * same scene in another run. Zeroed at the end of every report. */
static struct {
    FrameHist run, win;
    struct timespec last;
    int started;
} s_frame;

/* THE GO/NO-GO INSTRUMENT FOR G3, AND IT CAN SAY NO.
 *
 * The cumulative split -- 51 s draining and 12 s reading back across 7,936
 * flips -- gives 7.9 ms of sync per frame, "48% of the frame". That average is
 * true and it does not settle the question, because the frame time that has to
 * move is the MEDIAN: p50 = 18.0 ms against a 16.68 ms budget. Removing a
 * stall from frames that were already inside budget buys a better mean and
 * exactly nothing a player can feel.
 *
 * So bin the same three quantities per frame, from the same pair of samples,
 * and let the percentiles answer it:
 *
 *   [SYNC]  what sync cost on each frame.
 *   [NOSYNC] frame time MINUS that frame's sync -- what the frame would have
 *            cost had sync been free.
 *
 * NOSYNC's p50 is the decision. Above 16.68 ms and the surface-swap work in
 * G3 cannot reach 60 fps on its own, whatever the mean says, and the next
 * question is which OTHER stage owns the median frame. Below it, the work is
 * worth doing and this line is the number to beat afterwards.
 *
 * IT IS AN UPPER BOUND ON THE WIN, NOT A PREDICTION. Removing the stall
 * removes the serialisation, not the GPU work it was waiting on: some of the
 * drained time is work that would still have to happen, merely overlapped. A
 * bound that says "no" is still decisive; a bound that says "yes" is a
 * permission to try, which is exactly how the flip-sync plan should have been
 * treated before a week went into it.
 *
 * Derived from a cumulative nanosecond total, so it needs the backend to have
 * one; on a host without the Metal path the two histograms stay empty and
 * frame_stats_report prints neither, which is an absence that says "not
 * measured here" rather than a zero that reads like "costs nothing". */
static struct {
    FrameHist sync_run, sync_win, nosync_run, nosync_win;
    unsigned long long last_sync_ns;
    int have;
} s_sync;
/* Frames whose sync interval exceeded the frame interval. See the clamp. */
static unsigned long long s_sync_clamps;

/* RECOMP_SYNC_HIST -- OFF BY DEFAULT, AND THE REASON IS A GUEST HALT.
 *
 * A 240 s scripted run with this accumulating unconditionally stopped the
 * guest dead at 9,480 flips: flips, [APU-VOICE] guest_methods and [APU-BIN]
 * 2D heard all froze in the same window and never moved again, while the
 * emulator's own threads kept running at 389% CPU. The control at HEAD, same
 * schedule, same switches, ran to 13,398 flips with no repetition, and a
 * player session on HEAD played for ten minutes without it.
 *
 * That is ONE RUN PER ARM, which this project's own scorer voids -- and the
 * code here is a read of two counters and some arithmetic, with no lock, no
 * allocation and no Metal call, so a mechanism is not apparent. It may well be
 * a pre-existing intermittent hang that this run happened to hit, or timing
 * perturbation of one: play_scripted.sh's header already records that heavy
 * probes destabilise this title.
 *
 * Unproven either way, which is exactly why it ships off. [FRAME] is
 * deliberately unconditional on the argument that an opt-in perf counter is
 * one nobody has switched on when they need it; that argument does not
 * survive being implicated in a halt. Resolve it with two runs per arm before
 * making this the default. */
static int sync_hist_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_SYNC_HIST");
    return on;
}

static void frame_stats_flip(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_frame.started) {
        /* Signed, and in that order: tv_nsec wraps every second, so the naive
         * unsigned difference reads about 4e9 us once a second. */
        long long us = (long long)(now.tv_sec - s_frame.last.tv_sec) * 1000000LL
                     + ((long long)now.tv_nsec - (long long)s_frame.last.tv_nsec) / 1000LL;
        if (us < 0) us = 0;
        frame_hist_add(&s_frame.run, (unsigned long long)us);
        frame_hist_add(&s_frame.win, (unsigned long long)us);
#if defined(__APPLE__)
        if (sync_hist_on()) {
            /* Differenced against the PREVIOUS flip, so the interval is
             * exactly the one the frame time above covers. The first flip
             * establishes the baseline and is not binned -- its "frame" began
             * at process start and would bin every sync since boot into one
             * bucket, which is the shape of the counter that differenced
             * against the wrong timestamp and read ~0. */
            unsigned long long ns = nv2a_metal_sync_ns();
            if (s_sync.have) {
                unsigned long long sync_us, nosync_us;
                frame_hist_split((unsigned long long)us,
                                 (ns - s_sync.last_sync_ns) / 1000ull,
                                 &sync_us, &nosync_us, &s_sync_clamps);
                frame_hist_add(&s_sync.sync_run, sync_us);
                frame_hist_add(&s_sync.sync_win, sync_us);
                frame_hist_add(&s_sync.nosync_run, nosync_us);
                frame_hist_add(&s_sync.nosync_win, nosync_us);
            }
            s_sync.last_sync_ns = ns;
            s_sync.have = 1;
        }
#endif
    }
    s_frame.last = now;
    s_frame.started = 1;
}

static void frame_hist_line(const char *tag, const FrameHist *h)
{
    double mean_ms;
    unsigned long long over = 0;
    unsigned i;
    /* Frames that missed 30 Hz outright. A percentile says where the bulk
     * sits; this says how often the title visibly hitched. */
    for (i = 33000u / FRAME_BIN_US + 1u; i < FRAME_BINS; ++i)
        over += h->bin[i];
    mean_ms = (double)h->total_us / (double)h->n / 1000.0;
    fprintf(stderr, "  [%s] flips=%llu mean=%.2f ms (%.1f fps)"
            " p50=%.1f p90=%.1f p95=%.1f p99=%.1f max=%.1f ms  over-33ms=%llu\n",
            tag, (unsigned long long)h->n, mean_ms,
            mean_ms > 0.0 ? 1000.0 / mean_ms : 0.0,
            frame_pct(h, 0.50), frame_pct(h, 0.90), frame_pct(h, 0.95),
            frame_pct(h, 0.99),
            (double)h->max_us / 1000.0, over);
}

/* Per-frame stage cost for the window just ended. `rest` is the frame time
 * that none of the timed stages accounts for -- guest execution, the
 * pushbuffer walk, method dispatch -- and is a residual rather than a
 * measurement. A negative residual would mean a stage ran on a thread other
 * than the one flipping, so it is reported rather than clamped. */
static void pb_stage_line(unsigned long long frames, unsigned long long frame_us)
{
    double per[PB_STAGE_N];
    double sum = 0.0;
    unsigned i;
    if (!frames) return;
    for (i = 0; i < PB_STAGE_N; ++i) {
        per[i] = (double)s_stage_win.us[i] / (double)frames / 1000.0;
        /* WALK is deliberately excluded from the sum. Every other stage is
         * disjoint from the rest by construction -- that is the property that
         * makes `rest` a residual worth printing -- but WALK brackets the whole
         * method dispatch and therefore CONTAINS vsh, prepare and submit for
         * any draw-triggering method. Adding it would double-count them and
         * drive `rest` negative, which would look like the thread bug the
         * negative case is reserved for. */
        if (i != PB_STAGE_WALK) sum += per[i];
    }
    fprintf(stderr, "  [STAGE] per frame:");
    for (i = 0; i < PB_STAGE_N; ++i) {
        /* WALK prints only when it was switched on. A stage that is always
         * zero in a normal run trains the eye to skip the line. */
        if (i == PB_STAGE_WALK && !s_stage_win.n[i]) continue;
        fprintf(stderr, " %s=%.2f ms (%.0f calls)", pb_stage_name[i], per[i],
                (double)s_stage_win.n[i] / (double)frames);
    }
    fprintf(stderr, " | rest=%.2f ms of %.2f",
            (double)frame_us / (double)frames / 1000.0 - sum,
            (double)frame_us / (double)frames / 1000.0);
    if (s_stage_win.n[PB_STAGE_WALK])
        fprintf(stderr, "  [walk overlaps vsh/prepare/submit and inflates the"
                        " frame by ~42 ns x %.0f calls = %.2f ms; see"
                        " pb_walk_on]",
                (double)s_stage_win.n[PB_STAGE_WALK] / (double)frames,
                (double)s_stage_win.n[PB_STAGE_WALK] / (double)frames
                    * 42.0 / 1e6);
    fprintf(stderr, "\n");

    /* What the `prepare` figure above is actually made of.
     *
     * prepare is dominated by nv2a_dma_resolve's linear RAMHT scan, and the
     * scan's cost is entries-walked times a constant. Printing the entries
     * turns "prepare is 0.04 ms, is that the scan?" into arithmetic: at the
     * 0.54 ns per entry measured on this host, ns = entries * 0.54. It is also
     * the only figure that says whether memoising the lookup is worth writing.
     * A cache can remove at most the scan, so the saving is bounded above by
     * that product -- and at 27 ms a frame, 0.1 ms is about the smallest
     * saving worth the risk of caching a hardware table the guest can rewrite.
     * That threshold is ~185,000 entries a frame. Below it, do not cache.
     *
     * The two lines also CHECK EACH OTHER. The `prepare` timer and this entry
     * count are independent instruments -- a clock and a loop counter -- so
     * when the scans dominate prepare they must agree: [DMA] reporting 0.53 ms
     * of scanning beside prepare=0.53 ms means both work AND that the scan is
     * all of prepare. [DMA] at 0.53 beside prepare at 0.02 means one of them is
     * wrong, and no decision should be taken from either until that is settled.
     *
     * Differenced against the previous report so the window matches [STAGE]'s.
     * A cumulative average over a run that is several scenes long describes
     * none of them -- the same reason [FRAME-WIN] exists. */
    {
        static unsigned long long prev_scans, prev_entries, prev_misses;
        unsigned long long scans, entries, misses, worst;
        unsigned long long d_scans, d_entries, d_misses;
        nv2a_dma_resolve_stats(&scans, &entries, &misses, &worst);
        d_scans = scans - prev_scans;
        d_entries = entries - prev_entries;
        d_misses = misses - prev_misses;
        prev_scans = scans; prev_entries = entries; prev_misses = misses;
        fprintf(stderr, "  [DMA] RAMHT scans/frame=%.1f entries/frame=%.0f"
                " (%.1f per scan, %.4f ms at 0.54 ns/entry)"
                " full-table misses/frame=%.2f worst scan=%llu entries\n",
                (double)d_scans / (double)frames,
                (double)d_entries / (double)frames,
                d_scans ? (double)d_entries / (double)d_scans : 0.0,
                (double)d_entries / (double)frames * 0.54e-6,
                (double)d_misses / (double)frames, worst);
    }
}

/* Same percentiles as frame_hist_line, without the fps: the reciprocal of a
 * stage cost is not a frame rate and printing one invites the reader to
 * subtract two "fps" figures, which is not how time adds up. */
static void cost_hist_line(const char *tag, const FrameHist *h)
{
    fprintf(stderr, "  [%s] frames=%llu mean=%.2f ms"
            " p50=%.1f p90=%.1f p95=%.1f p99=%.1f max=%.1f ms\n",
            tag, (unsigned long long)h->n,
            (double)h->total_us / (double)h->n / 1000.0,
            frame_pct(h, 0.50), frame_pct(h, 0.90), frame_pct(h, 0.95),
            frame_pct(h, 0.99), (double)h->max_us / 1000.0);
}

/* The two lines G3 turns on. Printed only when there are samples, because a
 * histogram of nothing prints p50=0.0 and that reads like "sync is free". */
static void sync_stats_line(void)
{
    /* UNCONDITIONAL, AND THAT IS THE POINT. ab_score.py's identical-arms check
     * looks for this switch's token in the harvested state of BOTH arms; a
     * token that only appears when the switch is ON leaves the off arm with
     * nothing to match, switch_state_for returns None, and the VOID check is
     * silently SKIPPED. That is exactly how six switches were scored on
     * 16 Sep, and how the RECOMP_SYNC_HIST A/B on 17 Sep had to have its arms
     * verified by hand afterwards. */
    fprintf(stderr, "  [SYNC] sync_hist %s\n", sync_hist_on() ? "on" : "OFF");
    if (!s_sync.sync_run.n) return;
    cost_hist_line("SYNC", &s_sync.sync_run);
    cost_hist_line("NOSYNC", &s_sync.nosync_run);
    if (s_sync.sync_win.n) {
        cost_hist_line("SYNC-WIN", &s_sync.sync_win);
        cost_hist_line("NOSYNC-WIN", &s_sync.nosync_win);
    }
    /* The positive control for the subtraction. Non-zero means a sync was
     * timed outside the frame interval it was charged to -- another thread, or
     * a clock disagreement -- and every percentile above is then suspect. */
    fprintf(stderr, "  [SYNC] budget %.2f ms: %s (NOSYNC p50=%.1f ms),"
            " clamped frames=%llu\n", FRAME_BUDGET_MS,
            frame_hist_budget_ok(&s_sync.nosync_run)
                ? "reachable without the sync stall"
                : "NOT reachable by removing the sync stall alone",
            frame_pct(&s_sync.nosync_run, 0.50),
            (unsigned long long)s_sync_clamps);
    memset(&s_sync.sync_win, 0, sizeof s_sync.sync_win);
    memset(&s_sync.nosync_win, 0, sizeof s_sync.nosync_win);
}

static void frame_stats_report(void)
{
    if (!s_frame.run.n) {
        fprintf(stderr, "  [FRAME] no FLIP_STALL yet\n");
        return;
    }
    frame_hist_line("FRAME", &s_frame.run);
    if (s_frame.win.n) {
        frame_hist_line("FRAME-WIN", &s_frame.win);
        {
            extern void nv2a_pb_exec_flip_pace_stats(unsigned long long *, unsigned long long *);
            unsigned long long paced = 0, wait_us = 0;
            nv2a_pb_exec_flip_pace_stats(&paced, &wait_us);
            fprintf(stderr, "  [FLIP-PACE] flips held to one vblank period: %llu, waiting %.1f ms total"
                            " (RECOMP_FLIP_PACE=0 runs free)\n", paced, (double)wait_us / 1000.0);
        }
        pb_stage_line(s_frame.win.n, s_frame.win.total_us);
    }
    sync_stats_line();
    memset(&s_frame.win, 0, sizeof s_frame.win);
    memset(&s_stage_win, 0, sizeof s_stage_win);
}

/* G51.1 DRAW MODE: the host has already drawn this draw into the target, so
 * the batches that follow its token must not be drawn again. Set and cleared
 * by the host's tokens on this same thread, around exactly one D3D draw's
 * commands. Unset, it is one predictable branch per batch.
 *
 * WHAT A SKIPPED BATCH STILL DOES, and what it no longer does. Every method
 * is still latched (s_methods, s_vsh.current, the program and constant
 * uploads, the index list), because those arrive as methods, not in the
 * batch. What a skipped batch no longer runs is the per-batch derivation:
 * prepare_vertices (attribute fetch, the CPU program or the GPU program's
 * selection) and prepare_texture_copy (the draw's fragment and surface
 * state). Both are pure functions of the latched state, recomputed from
 * scratch by the next batch that draws, so no later draw can see a
 * difference: prepare_vertices only caches the program PARSE, lazily, behind
 * s_vsh.dirty, which a skip leaves set; nv2a_metal_vsh_ready/clear select the
 * program for the very next nv2a_metal_draw, which is re-selected before it.
 * s_copy is marked inactive, so the mirror's check at this draw's token
 * reads "the executor did not draw it" -- true -- instead of the previous
 * draw's texture and surface. G51.3 measured the cost this removes: [STAGE]
 * vsh 1.99 ms a frame unchanged by replacing 52 of 71 draws. */
static int s_host_skip, s_host_skip_late;
static unsigned long long s_host_skipped, s_host_seen;
/* RECOMP_D3D8_HOST_BISECT bit 128: skip where 645a7e6 did, after the batch's
 * vertex and fragment preparation, instead of before it. */
void nv2a_pb_exec_host_skip_late(int on) { s_host_skip_late = on; }
void nv2a_pb_exec_host_skip(int on) { s_host_skip = on; }
unsigned long long nv2a_pb_exec_host_skipped(void) { return s_host_skipped; }
/* G51.2 positive control: begin/end batches by transform mode, always counted
 * (one increment a batch), read by the host's census. */
static unsigned long long s_exec_mode_batches[3];
void nv2a_pb_exec_mode_counts(unsigned long long out[3]) { memcpy(out, s_exec_mode_batches, sizeof s_exec_mode_batches); }
/* Batches that ARRIVED while the skip was on, skipped or not: a batch the
 * executor stopped before its rasteriser (fewer than 3 indices, a refused
 * vertex or texture state) is seen and not skipped, and drew nothing. */
unsigned long long nv2a_pb_exec_host_seen(void) { return s_host_seen; }

/* G51.1: called at the end of every NV097_FLIP_STALL on the pusher thread,
 * after the snapshot. A pointer rather than a call so this file does not
 * depend on the host 2D module (some tests link the executor without it). */
static void (*s_flip_hook)(void);
void nv2a_pb_exec_set_flip_hook(void (*fn)(void)) { s_flip_hook = fn; }

static void flip_trace(void)
{
    static long stride = -1;
    static unsigned long flips, traced;
    static uint32_t last_tris, last_clears;
    uint32_t tris = s_gpu.tris_drawn;

    if (stride < 0) {
        const char *e = getenv("RECOMP_FLIP_TRACE");
        stride = e && *e ? strtol(e, NULL, 0) : 0;
        if (stride < 1) stride = 1;
    }
    /* Cached. This ran a getenv on EVERY flip -- 60 a second in a title that
     * is already missing its frame budget -- to re-answer a question whose
     * answer was fixed at startup and had in fact already been read three
     * lines above. Presence, not recomp_switch_on, because this is a trace and
     * `stride` carries the value: see the switch-semantics note in
     * recomp_switch.h for when presence is the right test.
     *
     * Honesty about size: this is hygiene, not a fix. One getenv is tens of
     * nanoseconds and there is one per flip, so it is worth single-digit
     * MICROseconds a second. It is here because the same mistake at draw rate
     * cost this file half its CPU once (see pb_env_on), not because it will
     * move the frame time. */
    {
        static int on = -1;
        if (!pb_env_on("RECOMP_FLIP_TRACE", &on))
            return;
    }
    if ((flips++ % (unsigned long)stride) == 0 && traced < 64) {
        traced++;
        fprintf(stderr, "  [FLIPTRACE] flip %lu: surface 0x%08X pitch %u"
                " clip %ux%u+%u+%u fmt 0x%08X | %u triangles, %u clears"
                " since the last flip | snapshot %ux%u nonzero %u\n",
                flips - 1, s_gpu.color_offset, s_gpu.pitch,
                s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                s_gpu.format, tris - last_tris,
                s_gpu.clears - last_clears,
                s_snap_w, s_snap_h, snapshot_nonzero());
        {
            char line[512];
            int off = snprintf(line, sizeof line, "  [FLIPTRACE]   surfaces:");
            uint32_t bpp = surface_bpp();
            unsigned i;
            /* Both aliases of each surface.
             *
             * AvSetDisplayMode states the scanout as a PHYSICAL address, and a
             * contiguous allocation's physical P is visible to the CPU at
             * 0x80000000 + P. Reading P directly lands in the loaded
             * image instead. Printing one number cannot tell "nothing was
             * drawn" from "it was drawn in the other window", and that is a
             * distinction this renderer has already been wrong about. */
            for (i = 0; i < s_surface_count && off > 0
                        && off < (int)sizeof(line) - 56; i++)
                off += snprintf(line + off, sizeof(line) - (size_t)off,
                                " 0x%08X=%u/hi=%u%s", s_surfaces[i],
                                surface_nonzero(s_surfaces[i], bpp),
                                surface_nonzero(s_surfaces[i] | 0x80000000u, bpp),
                                s_surfaces[i] == s_gpu.color_offset
                                    ? "(bound)" : "");
            fprintf(stderr, "%s\n", line);
        }
#ifdef nv2a_gpu_surface_report
        /* The same addresses as the line above, but what the GPU holds rather
         * than what guest RAM holds. Where they disagree is where the frame
         * goes missing. */
        if (nv2a_gpu_on())
            nv2a_gpu_surface_report();
#endif
        if (s_recent_dump)
            s_recent_dump(48);
    }
    last_tris = tris;
    last_clears = s_gpu.clears;
}


/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb);

/* Does a GPU write land on the loaded XBE image? Reported once per distinct
 * range so a per-frame clear cannot flood the log. */
/* Pushed in by the loader rather than read as an extern: the small diagnostic
 * binaries link this translation unit without the kernel layer, and they simply
 * never set it, which leaves the guard inert instead of unresolved. */
static uint32_t s_image_lo, s_image_hi;

void nv2a_set_image_bounds(uint32_t lo, uint32_t hi)
{
    s_image_lo = lo;
    s_image_hi = hi;
}

static int nv2a_range_hits_image(uint32_t guest_va, size_t bytes)
{
    static uint32_t last_va; static size_t last_bytes; static unsigned long n;

    if (!s_image_hi || !bytes) return 0;
    if ((uint64_t)guest_va >= (uint64_t)s_image_hi
        || (uint64_t)guest_va + bytes <= (uint64_t)s_image_lo)
        return 0;

    n++;
    if (guest_va != last_va || bytes != last_bytes) {
        last_va = guest_va; last_bytes = bytes;
        fprintf(stderr,
                "  [NV2A] REFUSED a surface write over the loaded image: "
                "guest 0x%08X..0x%08X overlaps image 0x%08X..0x%08X (%lu so far)."
                " The surface address is wrong; the clear is not the bug.\n",
                guest_va, (uint32_t)(guest_va + bytes),
                s_image_lo, s_image_hi, n);
        fflush(stderr);
    }
    return 1;
}

/* RECOMP_SURFACE_AUDIT -- does the surface the guest names match the one the
 * backend is holding, at the two moments where it matters?
 *
 * The backend retains exactly ONE surface. The guest binds whichever it likes
 * (s_gpu.color_offset moves on the register write) and the retained surface
 * only moves on the next DRAW, so between those two the guest can name one
 * surface while the backend holds another. Every claim about that window so
 * far -- including the one that motivated giving nv2a_metal_discard a target
 * -- has come from reading the code. This counts it in a real run.
 *
 * Sampled at the clear, because that is where the discard fast path decides
 * whether it may throw the retained surface away, and at the flip, because
 * that is where the presenter's snapshot is taken. `owed` says whether the
 * retained surface is holding rendering guest RAM has not seen: a mismatch
 * while owed is the only combination that can cost pixels.
 *
 * Read-only and opt-in. It answers a question, it does not change one. */
#if defined(__APPLE__) && NV2A_GPU_PATH
static int surface_audit_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_SURFACE_AUDIT");
    return on;
}
static uint64_t sa_clear, sa_clear_other, sa_clear_other_owed;
static uint64_t sa_flip, sa_flip_other, sa_flip_other_owed, sa_flip_nothing;
static const uint8_t *sa_seen[16];
static unsigned sa_seen_n;

static void surface_audit(int at_flip)
{
    const uint8_t *held = NULL, *heldz = NULL, *bound;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    int owed = -1, other;
    unsigned i;

    if (!surface_audit_on() || !mem || !s_gpu.color_offset) return;
    nv2a_metal_retained(&held, &heldz, &owed);
    bound = mem + s_gpu.color_offset;
    for (i = 0; i < sa_seen_n; ++i) if (sa_seen[i] == bound) break;
    if (i == sa_seen_n && sa_seen_n < 16) sa_seen[sa_seen_n++] = bound;
    other = (held != bound);
    if (at_flip) {
        ++sa_flip;
        if (owed < 0) { ++sa_flip_nothing; return; }
        if (other) { ++sa_flip_other; if (owed > 0) ++sa_flip_other_owed; }
    } else {
        ++sa_clear;
        if (owed < 0) return;
        if (other) { ++sa_clear_other; if (owed > 0) ++sa_clear_other_owed; }
    }
}

static void surface_audit_report(void)
{
    if (!surface_audit_on()) return;
    fprintf(stderr,
        "[SURFACE-AUDIT] %llu distinct colour surfaces bound\n"
        "[SURFACE-AUDIT] clears: %llu with a surface retained, %llu naming a "
        "DIFFERENT one than is held, %llu of those holding unsaved rendering\n"
        "[SURFACE-AUDIT] flips:  %llu total, %llu with nothing retained, %llu "
        "naming a DIFFERENT one than is held, %llu of those holding unsaved "
        "rendering\n",
        (unsigned long long)sa_seen_n,
        (unsigned long long)sa_clear, (unsigned long long)sa_clear_other,
        (unsigned long long)sa_clear_other_owed,
        (unsigned long long)sa_flip, (unsigned long long)sa_flip_nothing,
        (unsigned long long)sa_flip_other, (unsigned long long)sa_flip_other_owed);
}

/* RECOMP_SURFACE_CENSUS=<stride>[:<after-seconds>] -- every stride-th flip
 * from `after` onwards, print what each retained colour surface holds on the
 * GPU beside what guest RAM holds for it, and a verdict. Up to 200 reports.
 *
 * THE QUESTION IT SETTLES, and no instrument on the Metal path could settle it
 * before. When the screen is black, every number we have is read from GUEST
 * RAM: [FB] samples it, [FLIPTRACE]'s surface list samples it, the presenter
 * uploads it, and the player sees it. If the frame was rasterised but never
 * written back, all four say "black" and every one of them is telling the
 * truth about a picture that exists. The D3D11 branch answers this with
 * nv2a_gpu_surface_report() -- "what the GPU holds rather than what guest RAM
 * holds. Where they disagree is where the frame goes missing" -- and that
 * macro is defined on Windows ONLY, so on macOS the block compiles out.
 *
 * It is NOT free: each report drains the GPU and reads back every held
 * surface. That is why it takes a stride instead of being a plain trace, and
 * it is also why the stride must be chosen to sample frames the player can SEE
 * as well as black ones. The verdict's GPU_BLACK arm is an absence
 * measurement, and an absence measurement with no positive control beside it
 * proves nothing -- so a run that reports GPU-BLACK on a black frame is only
 * evidence if the same run reports PICTURE-HELD on a visible one.
 *
 * BEFORE snapshot_surface(), which syncs: a sync pays the write-back debt and
 * so destroys the very disagreement this is looking for. */
static void surface_census(uint32_t presented_offset)
{
    static const char *slot;
    static long stride = -1;
    static double after;
    static int on, reports;
    static unsigned long flips;
    const uint8_t *mem;

    if (stride < 0) {
        const char *env = pb_env_str("RECOMP_SURFACE_CENSUS", &slot);
        const char *colon = env ? strchr(env, ':') : NULL;
        on = env != NULL;
        stride = env && *env ? strtol(env, NULL, 0) : 0;
        if (stride < 1) stride = 1;
        after = colon ? strtod(colon + 1, NULL) : 0.0;
        if (!(after > 0.0)) after = 0.0;
    }
    if (!on || !nv2a_gpu_on() || reports >= 200) return;
    if (trace_seconds() < after) return;
    if ((flips++ % (unsigned long)stride) != 0) return;
    ++reports;
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    fprintf(stderr, "  [SURFACE-CENSUS] t=%.2f report %d, guest names"
                    " 0x%08X at this flip\n",
            trace_seconds(), reports, presented_offset);
    nv2a_metal_surface_census_report(mem, presented_offset);
}
#else
static void surface_audit(int at_flip) { (void)at_flip; }
static void surface_audit_report(void) { }
static void surface_census(uint32_t presented_offset) { (void)presented_offset; }
#endif

/* RECOMP_CLEAR_COLOR_FORCE=<hex> -- CLEAR TO THIS INSTEAD, AND SEE WHAT
 * SURVIVES IT. The last discriminator for a black screen, and the only one
 * the census cannot supply.
 *
 * gpu_nonzero counts NONZERO pixels, so "the draws wrote nothing" and "the
 * draws wrote black" are the same reading -- and they are different bugs:
 *
 *   geometry off-screen or zero-area   nothing is covered
 *   shaded/blended to nothing          the pixels are covered and come out 0
 *
 * Clear to magenta and the two separate instantly. If the screen comes back
 * MAGENTA the draws never covered it and the fault is in the transform. If it
 * comes back BLACK they covered it and wrote zero, and the fault is in the
 * shading. One run, one glance, no counter to misread.
 *
 * The VALUE carries meaning, so this is read directly rather than through
 * recomp_switch.h, which that header asks for explicitly -- 0x0000 is a
 * legitimate force-to-black and strcmp(v,"0") must not swallow it. Unset is
 * off; -1 is the sentinel for "no override".
 *
 * s_gpu.clear_color is left ALONE so [CLEAR-WIN] keeps reporting what the
 * GUEST asked for. An instrument that reported our own override back to us
 * would be measuring this switch. */
static uint32_t clear_colour_effective(void)
{
    static long forced = -2;
    if (forced == -2) {
        const char *e = getenv("RECOMP_CLEAR_COLOR_FORCE");
        forced = (e && *e) ? strtol(e, NULL, 0) : -1;
        if (forced >= 0)
            fprintf(stderr, "  [CLEAR-WIN] FORCING every colour clear to"
                            " 0x%08lX -- what stays this colour was never"
                            " covered by a draw\n", forced);
    }
    return forced >= 0 ? (uint32_t)forced : s_gpu.clear_color;
}

/* THE RASTER STATE THIS WINDOW ACTUALLY RAN WITH.
 *
 * The magenta probe on 21 Sep 2026 split the two black screens apart: the
 * intro's silhouettes write ZERO over a magenta clear (covered, shaded to
 * nothing) while the Now-Loading window stays FULLY MAGENTA (never covered at
 * all). The second one is a coverage fault, and coverage is decided by three
 * things nothing in this log has ever printed: the viewport scale, the
 * viewport offset and the surface clip.
 *
 * A zero or near-zero viewport SCALE collapses every triangle to a point and
 * is indistinguishable, in every counter we have, from geometry that was
 * never submitted -- 510,381 batches were ACCEPTED in the run that showed
 * this, with 0 backend refusals and 4 clip-w rejections, so the geometry
 * reaches the GPU and covers nothing.
 *
 * Printed per window rather than on change, because "unchanged" is the
 * interesting answer when the scene has changed. */
static void raster_state_report(void)
{
    fprintf(stderr, "  [RASTER-WIN] viewport scale=(%.1f %.1f %.1f %.1f)"
                    " offset=(%.1f %.1f %.1f %.1f) seen=%d |"
                    " clip %ux%u+%u+%u\n",
            s_gpu.vp_scale[0], s_gpu.vp_scale[1],
            s_gpu.vp_scale[2], s_gpu.vp_scale[3],
            s_gpu.vp_offset[0], s_gpu.vp_offset[1],
            s_gpu.vp_offset[2], s_gpu.vp_offset[3],
            s_gpu.vp_seen,
            s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y);
}

/* WHAT THE GUEST ASKED FOR IN THIS REPORTING WINDOW, not just the first time
 * it ever asked.
 *
 * The distinct-colour trace inside clear_surface prints one line per new
 * value and then goes quiet for the rest of the run -- eight lines, all of
 * them in the first minute. By the time a black window arrives it can no
 * longer say what colour was in force, and the two readings it cannot tell
 * apart are different bugs:
 *
 *   cleared to black, nothing drew over it   the draws are the fault
 *   cleared to a COLOUR, screen came out 0   the clear or the surface is
 *
 * The ROBOY run makes that concrete: its last distinct-colour line is
 * 0x000044B2 -- a non-black R5G6B5 -- at t~502, and the level-load blackouts
 * are at t=548 and beyond. Whether that colour was still in force there is
 * exactly the question, and the trace as written cannot answer it.
 *
 * Per window rather than per clear: a clear happens once or twice a frame, so
 * a line each would bury the log, and the question is about the window. */
#define CLEAR_WIN_SLOTS 8
static uint32_t s_clear_win_col[CLEAR_WIN_SLOTS];
static unsigned long s_clear_win_n[CLEAR_WIN_SLOTS];
static unsigned s_clear_win_used, s_clear_win_over;

static void clear_colour_note(uint32_t c)
{
    unsigned i;
    for (i = 0; i < s_clear_win_used; ++i)
        if (s_clear_win_col[i] == c) { s_clear_win_n[i]++; return; }
    if (s_clear_win_used < CLEAR_WIN_SLOTS) {
        s_clear_win_col[s_clear_win_used] = c;
        s_clear_win_n[s_clear_win_used++] = 1;
    } else {
        s_clear_win_over++;
    }
}

/* Printed UNCONDITIONALLY, including the no-clear case. "No colour clear in
 * this window" is a finding of its own -- a title that stops clearing is not
 * the same as one clearing to black -- and an absent line would read as the
 * instrument being off. */
static void clear_colour_report(void)
{
    unsigned i;
    if (!s_clear_win_used) {
        fprintf(stderr, "  [CLEAR-WIN] no colour clear since the last report\n");
        return;
    }
    fprintf(stderr, "  [CLEAR-WIN] colours asked for since the last report:");
    for (i = 0; i < s_clear_win_used; ++i)
        fprintf(stderr, " 0x%08X x%lu", s_clear_win_col[i], s_clear_win_n[i]);
    if (s_clear_win_over)
        fprintf(stderr, " (+%u more past the slots)", s_clear_win_over);
    fprintf(stderr, "\n");
    s_clear_win_used = s_clear_win_over = 0;
}

static void clear_surface(uint32_t param)
{
    unsigned long long _t_clear = pb_now_us();
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;
    /* Will this clear overwrite EVERY byte of the retained surface?
     *
     * Decided here, before either half runs, because the win is skipping both
     * syncs and the depth half comes first. Requires: both depth and stencil
     * (param&3 == 3), all three colour channels -- alpha is discarded on
     * download to RGB565 so it is not required, the same rule the D3D11
     * resident clear uses -- a 16-bit target, and every guard the colour half
     * below would otherwise bail on. If the DEPTH half turns out not to run,
     * `discarded` stays 0 and the colour half invalidates normally, so a
     * half-taken decision costs the win rather than the pixels. */
    surface_audit(0);
    int want_discard = 0, discarded = 0;
#ifdef nv2a_gpu_discard
    want_discard = clear_discard_on() && (param & 3u) == 3u
                && (param & (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G
                             | NV097_CLEAR_SURFACE_B))
                   == (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G
                       | NV097_CLEAR_SURFACE_B)
                && bpp == 2 && s_gpu.color_offset && s_gpu.pitch
                && s_gpu.clip_h && s_gpu.clip_w;
#endif
    (void)want_discard;
    /* The invalidate is per surface, and moved down to each half.
     *
     * It used to be one nv2a_gpu_invalidate(NULL) here, which flushes and
     * discards EVERY retained surface for a clear of one of them. JSRF holds
     * three colour surfaces and swaps between them batch by batch, so that
     * blast radius meant no surface ever survived a frame. */

    /* Fixed Z24S8, linear single-sample surface. Resolve the actual DMA
     * object and validate the entire mapped range before touching depth. */
    if ((param&3) && (s_methods[0x208/4]&0xfff0)==0x120
            && !(s_methods[0x290/4]&0x1000)) {
        const uint8_t *regs=xbox_Nv2aRegisterMemory();
        uint32_t ramht=0,base=0,limit=0,pitch=s_methods[0x20c/4]>>16;
        uint32_t offset=s_methods[0x214/4];
        uint32_t x0=s_methods[0x1d98/4]&0xffff, x1=(s_methods[0x1d98/4]>>16)+1;
        uint32_t y0=s_methods[0x1d9c/4]&0xffff, y1=(s_methods[0x1d9c/4]>>16)+1;
        if (x0<s_gpu.clip_x) x0=s_gpu.clip_x;
        if (y0<s_gpu.clip_y) y0=s_gpu.clip_y;
        if (x1>s_gpu.clip_x+s_gpu.clip_w) x1=s_gpu.clip_x+s_gpu.clip_w;
        if (y1>s_gpu.clip_y+s_gpu.clip_h) y1=s_gpu.clip_y+s_gpu.clip_h;
        size_t bytes=(size_t)pitch*y1;
        if (regs) memcpy(&ramht,regs+0x2210,4);
        if (regs && x0<x1 && y0<y1 && pitch>=(uint64_t)x1*4
                && nv2a_dma_resolve(regs+0x700000,0x100000,ramht,s_methods[0x198/4],&base,&limit)
                && (uint64_t)offset+bytes<=(uint64_t)limit+1
                && (uint64_t)base+offset<=UINT32_MAX) {
            uint8_t *z=xbox_GpuMemoryRange(base+offset,bytes);
            uint32_t value=s_methods[0x1d8c/4];
            int gpu_cleared = 0;
            /* Where the depth surface actually is, in its two parts.
             *
             * The address is the zeta DMA object's base plus
             * SET_SURFACE_ZETA_OFFSET, and on Windows it lands inside the
             * loaded image while macOS puts it above. Printing base and offset
             * separately says WHICH half is wrong -- a bad DMA object and a bad
             * offset are different bugs with different owners, and the refusal
             * line downstream only shows their sum. Once per distinct
             * combination; RECOMP_SURFACE_TRACE=1. */
            {
                static int on=-1; static uint32_t lb,lo,lp; static uint32_t lh;
                if (on<0) on=getenv("RECOMP_SURFACE_TRACE")?1:0;
                if (on && (base!=lb||offset!=lo||pitch!=lp||s_methods[0x198/4]!=lh)) {
                    lb=base; lo=offset; lp=pitch; lh=s_methods[0x198/4];
                    fprintf(stderr,
                        "  [SURFACE] zeta dma_handle=0x%08X base=0x%08X limit=0x%08X"
                        " offset=0x%08X pitch=%u -> 0x%08X..0x%08X\n",
                        lh, base, limit, offset, pitch,
                        base+offset, (uint32_t)(base+offset+bytes));
                    fflush(stderr);
                }
            }
            /* Never clear into the loaded image.
             *
             * A Z24S8 clear writes stencil 0x00 then depth 0xFF 0xFF 0xFF, so
             * every dword it touches becomes 0xFFFFFF00. Caught doing exactly
             * that over guest .data on the Windows host: 0x0025EFB8, a function
             * pointer the vsync pump calls when non-zero, and the XPP list head
             * at 0x002648D4. The guest then calls 0xFFFFFF00 and dies, frames
             * and seconds away from here, with nothing in the guest's own
             * stores to show for it.
             *
             * The DMA checks above validate the offset against the DMA object's
             * own limit, which says nothing about whether that object was set up
             * over the image. This is the missing invariant, not a workaround:
             * the GPU has no business writing the title's code or data, on any
             * host, and a surface that resolves there is misconfigured. Refuse
             * and say so, rather than corrupting the guest silently. */
            if (z && nv2a_range_hits_image(base+offset, bytes)) z = NULL;
#if NV2A_GPU_PATH
            if (z && nv2a_gpu_on()) {
#ifdef nv2a_gpu_clear_depth_stencil
                gpu_cleared = nv2a_gpu_clear_depth_stencil(z, bytes, pitch,
                        s_gpu.clip_w, s_gpu.clip_h, x0, y0, x1, y1,
                        param & 3u, value);
#endif
                if (!gpu_cleared) {
#ifdef nv2a_gpu_discard
                    /* Reaching here with z valid means the CPU loop below
                     * will write the whole depth range, and want_discard
                     * already established the colour half will too. */
                    if (want_discard
                            && nv2a_gpu_discard(mem + s_gpu.color_offset, z))
                        discarded = 1;
                    else
#endif
                    nv2a_gpu_invalidate_range(z, bytes);
                }
            }
#endif
#ifdef nv2a_gpu_surface_report
            if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
                fprintf(stderr, "  [EV] ZCLEAR %s base=%08X off=%08X value=%08X%s\n",
                        z ? "ok" : "REFUSED", base, offset, s_methods[0x1d8c/4],
                        gpu_cleared ? " (resident)" : "");
#endif
            /* The two `param` tests are loop-invariant and were being made
             * once per pixel, around single-byte stores -- roughly 307k
             * iterations and 1.2 MB written a byte at a time for a 640x480
             * Z24S8 surface. Hoisted, and the common case (both halves, so the
             * whole dword is `value`) is now a word store the compiler can
             * widen. Byte order is unchanged: p[k] = value >> 8k for k=0..3 is
             * `value` in little-endian, which is what the memcpy writes, and
             * memcpy rather than a cast keeps it alignment- and
             * aliasing-clean. The partial cases keep their original stores.
             *
             * This is arithmetic, not a policy change: the same bytes land in
             * the same places, and it touches nothing about who owns the
             * surface. */
            if (z && !gpu_cleared) {
                const unsigned half = param & 3u;
                for (y = y0; y < y1; ++y) {
                    uint8_t *row = z + (size_t)y * pitch;
                    if (half == 3u) {
                        for (x = x0; x < x1; ++x)
                            memcpy(row + (size_t)x * 4, &value, 4);
                    } else if (half == 2u) {
                        for (x = x0; x < x1; ++x)
                            row[(size_t)x * 4] = (uint8_t)value;
                    } else if (half == 1u) {
                        for (x = x0; x < x1; ++x) {
                            uint8_t *p = row + (size_t)x * 4;
                            p[1] = (uint8_t)(value >> 8);
                            p[2] = (uint8_t)(value >> 16);
                            p[3] = (uint8_t)(value >> 24);
                        }
                    }
                }
            }
        }
    }

#ifdef nv2a_gpu_surface_report
    /* Whether the depth half ran at all. A depth clear the executor declines
     * leaves a retained GPU depth buffer holding the previous frame, and every
     * later draw then fails its depth test -- which looks exactly like a
     * renderer that stopped drawing. */
    if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
        fprintf(stderr, "  [EV] CLEARP param=%02X zcond=%d\n", param,
                (int)((param&3) && (s_methods[0x208/4]&0xfff0)==0x120
                      && !(s_methods[0x290/4]&0x1000)));
#endif
    if (!(param & (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A)))
        { pb_stage_add(PB_STAGE_CLEAR, _t_clear); return; }   /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        { pb_stage_add(PB_STAGE_CLEAR, _t_clear); return; }
    {
        size_t bytes = (size_t)s_gpu.pitch * (s_gpu.clip_y + s_gpu.clip_h);
        int gpu_cleared = 0;
        (void)bytes; /* the Metal compatibility macro needs no range yet */
#if NV2A_GPU_PATH
        if (nv2a_gpu_on()) {
#ifdef nv2a_gpu_clear_color
            if (bpp == 2)
                gpu_cleared = nv2a_gpu_clear_color(mem + s_gpu.color_offset,
                        bytes, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
                        param, clear_colour_effective());
#endif
            if (!gpu_cleared && !discarded)
                nv2a_gpu_invalidate_range(mem + s_gpu.color_offset, bytes);
        }
#endif
#ifdef nv2a_gpu_surface_report
        if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
            fprintf(stderr, "  [EV] CLEAR  target=%08X colour=%08X%s\n",
                    s_gpu.color_offset, s_gpu.clear_color,
                    gpu_cleared ? " (resident)" : "");
#endif

        if (!gpu_cleared) for (y = 0; y < s_gpu.clip_h; y++) {
            uint8_t *row = mem + s_gpu.color_offset
                         + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
            if (bpp == 4) {
                uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
                for (x = 0; x < s_gpu.clip_w; x++)
                    p[x] = s_gpu.clear_color;
            } else if (bpp == 2) {
            /* NV097_SET_COLOR_CLEAR_VALUE arrives already in the surface's own
             * format, so a 16-bit surface takes the low half verbatim. It is
             * tempting to treat it as A8R8G8B8 and reduce it to 5:6:5 -- this
             * did -- but D3D converts the D3DCOLOR the caller passed to Clear
             * into surface format before it ever reaches the pushbuffer, so
             * converting again is a second reduction of an already-reduced
             * value.
             *
             * JSRF's opening cards are what exposed it. Every clear value the
             * title issues has a zero upper half, and reducing again drops the
             * red field on the floor:
             *
             *   0x0000FFFF  white  -> (0,255,255) cyan
             *   0x000020E4  grey   -> (0,32,224)  blue
             *   0x00000000  black  -> black, which is why this survived
             *
             * Read as the R5G6B5 they are, those are (255,255,255) and
             * (32,28,32), matching xemu's (227,226,229) and (30,27,30) on the
             * same cards. Only the black case agreed before, and black is the
             * one value both readings share. */
                uint16_t v = (uint16_t)clear_colour_effective();
                uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
                for (x = 0; x < s_gpu.clip_w; x++)
                    p[x] = v;
            }
        }
    }
    s_gpu.clears++;
    clear_colour_note(s_gpu.clear_color);
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    /* Cached, for the reason above: this sits on the clear path, which runs
     * two to four times a frame, and the comment right above already promises
     * "one branch per clear". It was one getenv per clear. */
    static int raster_test_on = -1;
    if (pb_env_on("RECOMP_RASTER_TEST", &raster_test_on)) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu);   /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    xbox_FramebufferWindowSet(s_gpu.color_offset, s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
    pb_stage_add(PB_STAGE_CLEAR, _t_clear);
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded, no depth buffer, no texturing, no perspective
 * correction, and only batches whose attribute 0 is already in screen space.
 * A title running a vertex program hands over object-space positions that mean
 * nothing without executing the program, so those batches are counted and
 * skipped rather than drawn somewhere wrong. Upgrade path is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c once vertex programs are
 * translated; this exists to get the first geometry on screen for every title,
 * which in practice is UI, HUD and 2D overlays -- all pre-transformed.
 */

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    row = mem + s_gpu.color_offset + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    if (!isfinite(area) || !isfinite(a[0]) || !isfinite(a[1])
            || !isfinite(b[0]) || !isfinite(b[1]) || !isfinite(c[0]) || !isfinite(c[1])) return;
    float left = (float)s_gpu.clip_x, right = (float)(s_gpu.clip_x + s_gpu.clip_w);
    float top = (float)s_gpu.clip_y, bottom = (float)(s_gpu.clip_y + s_gpu.clip_h);
    minx = (int)floorf(fmaxf(left, fminf(right, fminf(a[0], fminf(b[0], c[0])))));
    maxx = (int)ceilf(fmaxf(left, fminf(right, fmaxf(a[0], fmaxf(b[0], c[0])))));
    miny = (int)floorf(fmaxf(top, fminf(bottom, fminf(a[1], fminf(b[1], c[1])))));
    maxy = (int)ceilf(fmaxf(top, fminf(bottom, fmaxf(a[1], fmaxf(b[1], c[1])))));
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                put_pixel(mem, bpp, x, y, argb);
        }
    }
    s_gpu.tris_drawn++;
    s_gpu.cpu_tris++;
}

/* One vertex source for both paths: a batch that pushed inline data reads from
 * it, anything else reads the arrays the title pointed at. */
static int fetch_vertex(uint32_t a, uint32_t index, float out[4])
{
    if (s_gpu.inline_count)
        return fetch_inline(a, index, out);
    return fetch_attr(&s_gpu.attr[a], index, out);
}

static uint32_t pack_color(const float c[4])
{
    uint32_t n[4];
    for (int k = 0; k < 4; ++k)
        n[k] = (uint32_t)(fminf(fmaxf(c[k], 0.0f), 1.0f) * 255.0f);
    return (n[3] << 24) | (n[0] << 16) | (n[1] << 8) | n[2];
}

/* Why the last batch was refused.
 *
 * prepare_vertices has five distinct ways to fail and the counter only said
 * how often, so "38,472 rejected" named no cause and could not be acted on.
 * These are the causes, counted separately, with the first offending detail
 * kept for each -- which attribute could not be fetched, which output
 * components the program left unwritten. Reason strings are literals and are
 * compared by pointer, so the table stays a fixed size. */
static struct { const char *reason; uint32_t n; uint32_t detail; }
    s_vsh_reject[8];
static const char *s_vsh_reason;
static uint32_t s_vsh_reason_detail;

#define VSH_REJECT(text, value) \
    do { s_vsh_reason = (text); s_vsh_reason_detail = (uint32_t)(value); \
         return 0; } while (0)

static void note_vsh_reject(void)
{
    const char *reason = s_vsh_reason ? s_vsh_reason : "unrecorded";
    for (size_t i = 0; i < sizeof s_vsh_reject / sizeof s_vsh_reject[0]; ++i) {
        if (!s_vsh_reject[i].reason) {
            s_vsh_reject[i].reason = reason;
            s_vsh_reject[i].detail = s_vsh_reason_detail;
        }
        if (s_vsh_reject[i].reason == reason) { s_vsh_reject[i].n++; return; }
    }
}

static uint32_t vsh_sample_batch(void)
{
    static long batch = -1;

    if (batch < 0) {
        const char *env = getenv("RECOMP_VSH_SAMPLE");
        batch = env ? strtol(env, NULL, 0) : 20000;
        if (batch < 4) batch = 4;
    }
    return (uint32_t)batch;
}

static void trace_selected_program(void)
{
    if (!s_vsh_trace.enabled) return;
    ++s_vsh_trace.decodes;
    uint32_t hash = 2166136261u;
    for (int i = 0; i < s_vsh.decoded.length; ++i)
        for (int k = 0; k < 4; ++k)
            hash = (hash ^ s_vsh.words[s_vsh.start+i][k]) * 16777619u;
    for (uint32_t i = 0; i < s_vsh_trace.unique; ++i)
        if (s_vsh_trace.hashes[i] == hash) return;
    if (s_vsh_trace.unique == 32) return;
    s_vsh_trace.hashes[s_vsh_trace.unique++] = hash;
    fprintf(stderr, "[VSH-TRACE] selected hash=%08X draw=%u start=%u slots=%d reads=%04X valid=%d final=%d\n",
            hash, s_gpu.draws, s_vsh.start, s_vsh.decoded.length,
            s_vsh.decoded.inputs_read, s_vsh.decoded.valid, s_vsh.decoded.has_final);
    for (int i = 0; i < s_vsh.decoded.length; ++i) {
        const uint32_t *w = s_vsh.words[s_vsh.start+i];
        fprintf(stderr, "[VSH-TRACE] slot=%u %08X %08X %08X %08X\n",
                s_vsh.start+i, w[0], w[1], w[2], w[3]);
    }
}

static void trace_vertex_inputs(const float inputs[16][4])
{
    if (!s_vsh_trace.enabled) return;
    if (s_vsh.batches == vsh_sample_batch()) {
        uint64_t first = s_vsh_trace.recent_count > 2048 ? s_vsh_trace.recent_count-2048 : 0;
        for (uint64_t i = first; i < s_vsh_trace.recent_count; ++i)
            fprintf(stderr, "[VSH-METHOD] %04X %08X\n",
                    s_vsh_trace.recent[i%2048].method, s_vsh_trace.recent[i%2048].param);
    }
    for (unsigned a = 0; a < 16; ++a) {
        if (!(s_vsh.decoded.inputs_read & (1u << a))) continue;
        fprintf(stderr, "[VSH-TRACE] draw=%u input=%u array=%u current-written=%X value=(%.9g %.9g %.9g %.9g)\n",
                s_gpu.draws, a, s_gpu.attr[a].size, s_vsh_trace.current_written[a],
                inputs[a][0], inputs[a][1], inputs[a][2], inputs[a][3]);
    }
    uint8_t seen[NV2A_VS_MAX_CONSTANTS] = {0};
    for (int i = 0; i < s_vsh.decoded.length; ++i) {
        const NV2AVshInstruction *ins = &s_vsh.decoded.insns[i];
        unsigned used = nv2a_vsh_mac_sources(ins->mac_op) | (ins->ilu_op ? 4 : 0);
        for (int k = 0; k < 3; ++k) {
            const NV2AVshSrcOperand *src = &ins->mac_src[k];
            if (!(used & (1u << k)) || src->reg_type != NV2A_VSH_REG_CONST
                    || src->reg_index >= NV2A_VS_MAX_CONSTANTS || seen[src->reg_index]) continue;
            seen[src->reg_index] = 1;
            const float *c = s_vsh.constants[src->reg_index];
            fprintf(stderr, "[VSH-TRACE] draw=%u constant=%d relative=%d value=(%.9g %.9g %.9g %.9g)\n",
                    s_gpu.draws, src->reg_index, src->rel_addr, c[0], c[1], c[2], c[3]);
        }
    }
}

/* RECOMP_VSH_SPLIT=1 -- how much of the vsh stage is FETCHING attributes out of
 * guest RAM, and how much is RUNNING the guest's program.
 *
 * WHY IT HAS TO BE MEASURED BEFORE THE GPU PATH IS BUILT. Moving the vertex
 * program to the GPU by gathering attributes into the staging ring removes the
 * program execution and KEEPS the fetch. So if the fetch is most of the 9.65 ms
 * this stage costs at gameplay, that change buys a fraction of what it looks
 * like, and the honest answer is to attack the fetch instead -- by handing the
 * GPU the guest's arrays directly rather than gathering them.
 * docs/jsrf/plans/GPU_VERTEX_SHADING.md says plainly that nothing knows this
 * split, and 400 lines of backend is too much to write on a guess.
 *
 * Opt-in, because two clock reads per vertex in the hottest loop in the program
 * is exactly the instrumentation weight this title has been destabilised by
 * before. The counts are the positive control: nanoseconds with a zero vertex
 * count is a dead instrument, not a free fetch. */
static unsigned long long g_vsh_fetch_ns, g_vsh_exec_ns;
static unsigned long long g_vsh_fetch_n,  g_vsh_exec_n;
static int vsh_split_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_VSH_SPLIT"); return on; }
static unsigned long long vsh_now_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
  return (unsigned long long)t.tv_sec*1000000000ull + (unsigned long long)t.tv_nsec; }

/* RECOMP_FOG_DRAW_TRACE=<n> -- G53, read-only, opt-in: n fogged batches (40
 * if n is 1), then one in every 2,000, each with three of its vertices' fog
 * coordinate d and factor f, beside what they came from:
 *   fixed-function: the object position, the eye position through the
 *   model-view (0x480) and whether 0x480 and FOG_PLANE were ever written,
 *   and the composite's clip w -- which for a D3D perspective projection IS
 *   eye z, so d and w disagreeing says which matrix is not what it should be;
 *   programmable: the program's oFog, run on the CPU interpreter for the GPU
 *   path, and whether the program writes oFog at all.
 * The factor is nv2a_fog_factor on the batch's FOG_MODE and FOG_PARAMS. */
static void fog_draw_trace(int programmable)
{
    static int cap = -1; static unsigned long seen_batches, printed;
    float p0, p1;
    uint32_t mode = s_methods[0x29c/4], gen = s_methods[0x2a0/4];
    unsigned n;
    if (cap < 0) { const char *e = getenv("RECOMP_FOG_DRAW_TRACE"); cap = e && *e ? atoi(e) : 0; if (cap == 1) cap = 40; }
    if (cap <= 0 || !s_methods[0x2a4/4] || !s_gpu.idx_count) return;
    ++seen_batches;
    if (printed >= (unsigned long)cap && seen_batches % 2000u) return;
    ++printed;
    memcpy(&p0, &s_methods[0x9c0/4], 4); memcpy(&p1, &s_methods[0x9c4/4], 4);
    fprintf(stderr, "[FOG-DRAW] batch %lu draw %u: xf MODE %u (%s), GEN %u, FOG_MODE %X, params %g %g, colour %08X, %s"
                    " | 0x480 written %u, FOG_PLANE written %u (%g %g %g %g)\n",
            seen_batches, s_gpu.draws, s_methods[0x1e94/4] & 3u, s_vsh_gpu_batch ? "GPU" : "CPU", gen, mode, p0, p1,
            s_methods[0x2a8/4], programmable ? "programmable" : "fixed-function",
            s_method_seen[0x480/4] && s_method_seen[0x4bc/4], s_method_seen[0x9d0/4],
            *(const float *)&s_methods[0x9d0/4], *(const float *)&s_methods[0x9d4/4],
            *(const float *)&s_methods[0x9d8/4], *(const float *)&s_methods[0x9dc/4]);
    if (!programmable) {
        const float *mv = (const float *)&s_methods[0x480/4], *cp = (const float *)&s_methods[0x680/4];
        fprintf(stderr, "[FOG-DRAW]   model-view rows: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)"
                        " | composite w row (%g %g %g %g)\n",
                mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7], mv[8], mv[9], mv[10], mv[11],
                mv[12], mv[13], mv[14], mv[15], cp[12], cp[13], cp[14], cp[15]);
    }
    for (n = 0; n < 3 && n < s_gpu.idx_count; ++n) {
        float d = 0, eye[4] = { 0, 0, 0, 0 }, clipw = 0, pos[4] = { 0, 0, 0, 0 }, d1[4] = { 0, 0, 0, 0 };
        int has_eye = 0;
        if (!programmable) {
            int src = nv2a_ff_fog_source(s_methods);
            if (s_vsh_gpu_batch) {                          /* s_outputs holds the INPUTS */
                memcpy(pos, s_outputs[n][0], 16);
                d = nv2a_ff_fog_coord(s_methods, src, (const float (*)[4])s_outputs[n], eye); has_eye = 1;
                {   const float *cp = (const float *)&s_methods[0x680/4];
                    clipw = cp[12] * pos[0] + cp[13] * pos[1] + cp[14] * pos[2] + cp[15] * pos[3]; }
            } else {
                d = s_outputs[n][5][0]; clipw = s_outputs[n][0][3];
            }
        } else if (s_vsh_gpu_batch && s_vsh.decoded.valid) {
            NV2AVshResult r;
            memset(&r, 0, sizeof r);
            nv2a_vsh_execute(&s_vsh.decoded, (const float (*)[4])s_outputs[n], s_vsh.constants, &r);
            d = r.output[5][0]; clipw = r.output[0][3]; memcpy(d1, r.output[4], 16);
        } else {
            d = s_outputs[n][5][0]; clipw = s_outputs[n][0][3]; memcpy(d1, s_outputs[n][4], 16);
        }
        fprintf(stderr, "[FOG-DRAW]   v%u: d %g -> f %g | clip w %g", n, d, nv2a_fog_factor(mode, p0, p1, d), clipw);
        if (has_eye) fprintf(stderr, " | object (%g %g %g %g) eye (%g %g %g %g)", pos[0], pos[1], pos[2], pos[3],
                             eye[0], eye[1], eye[2], eye[3]);
        if (programmable) fprintf(stderr, " | oD1 (V1, summed by CW0 130E0300) %g %g %g %g", d1[0], d1[1], d1[2], d1[3]);
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static int prepare_vertices(void)
{
    int programmable = s_vsh.mode == 2;
    s_vsh_reason = NULL;
    s_vsh_reason_detail = 0;
    if (s_vsh.mode != 0 && !programmable)
        VSH_REJECT("fixed-function transform mode is not implemented", s_vsh.mode);
    if (programmable && s_vsh.dirty) {
        uint32_t n = 0;
        while (s_vsh.start < NV2A_VS_MAX_INSTRUCTIONS
                && n < NV2A_VS_MAX_INSTRUCTIONS - s_vsh.start
                && s_vsh.loaded[s_vsh.start + n] == 15) {
            if (s_vsh.words[s_vsh.start + n++][3] & 1) break;
        }
        nv2a_vsh_parse(n ? s_vsh.words[s_vsh.start] : NULL, (int)n, &s_vsh.decoded);
        s_vsh.dirty = 0;
        trace_selected_program();
    }
    /* How much of this batch is a vertex we have already transformed?
     *
     * MEASURE BEFORE OPTIMISING. s_outputs[] is indexed by POSITION IN THE
     * INDEX ARRAY, not by vertex index, so a vertex referenced N times runs
     * the whole shader N times. Whether that is worth caching depends entirely
     * on how indexed this title's geometry actually is, and nothing here has
     * ever counted it. An indexed triangle list sharing vertices between
     * adjacent faces would show a large gap; a batch of independent triangles
     * would show none, and the cache would be pure overhead.
     *
     * Opt-in: this tree has a documented history of instrumentation weight
     * destabilising the title, and this sits in the hottest loop there is.
     * O(n) with an 8 KB bitmap cleared by re-walking the same indices, so no
     * 64 K memset per batch. */
    /* CAN THE GPU RUN THIS PROGRAM? Asked once per batch, before the loop, and
     * the answer decides what the loop writes into s_outputs: the program's
     * OUTPUTS as always, or its INPUTS for the GPU to transform.
     *
     * The order matters. The backend is asked FIRST and the interpreter is
     * skipped only on a yes, so a program the backend cannot express is a
     * clean CPU batch rather than a half-transformed one. Everything that can
     * say no -- the switch, the hardware tail, an emitter refusal, a compile
     * failure, a full cache -- says it here, before any vertex is touched.
     *
     * nv2a_metal_vsh_clear() on the no path is not belt and braces: the
     * backend keeps the selected program in a static, and a stale one applied
     * to the next batch's vertices would transform them with the wrong
     * program and draw it without an error anywhere. */
    /* SET WHEN THIS BATCH'S s_outputs HOLDS PROGRAM INPUTS RATHER THAN
     * TRANSFORMED POSITIONS, so the one consumer that cannot tell the
     * difference is stopped from reading them. See the fallback in
     * raster_batch: when nv2a_gpu_draw rejects, the executor rasterises
     * s_outputs on the CPU, and object-space positions read as screen
     * coordinates draw nothing recognisable and report no error. Caught by
     * jsrf_vsh_render, which draws a real JSRF program into an 8x8 surface and
     * checks every pixel. */
    int gpu_vsh = 0, gpu_ff = 0;
    NV2AFFKey ff_key;
    memset(&ff_key, 0, sizeof ff_key);
#if defined(__APPLE__) && NV2A_GPU_PATH
    if (s_vsh_force_cpu) {
        /* The Metal draw rejected this batch and it is about to be handed to
         * the CPU rasteriser, which reads s_outputs as SCREEN positions. This
         * pass re-runs the interpreter so that is what it finds. */
        nv2a_metal_vsh_clear();
    } else
    if (programmable && s_vsh.decoded.valid && s_vsh.decoded.length > 0) {
        gpu_vsh = nv2a_metal_vsh_ready(
                      (const uint32_t (*)[4])s_vsh.words[s_vsh.start],
                      s_vsh.decoded.length, s_vsh.decoded.inputs_read);
        if (gpu_vsh) nv2a_metal_vsh_constants(s_vsh.constants);
        else nv2a_metal_vsh_clear();
        s_vsh_gpu_batch = gpu_vsh;
    } else if (!programmable && ff_gpu_on()) {
        /* THE SAME QUESTION, ASKED OF THE FIXED-FUNCTION UNIT, and asked in the
         * same place and the same order: the backend FIRST, and the transform
         * only if it says no. nv2a_ff_key accepts a strict subset of what
         * nv2a_ff_vertex supports, so a refusal here is the ordinary CPU batch
         * that already worked, never a half-transformed one.
         *
         * The seen-table assignment is the one the CPU branch below already
         * makes; it is hoisted because nv2a_ff_key reads that table to decide
         * whether a texture matrix is a transform or an accident, and it runs
         * before that branch does. */
        nv2a_ff_method_seen = s_method_seen;
        if (nv2a_ff_key(s_methods, &ff_key)) {
            nv2a_ff_params(s_methods, &ff_key);
            gpu_ff = nv2a_metal_ff_ready(&ff_key, (unsigned)sizeof ff_key,
                                         ff_key.inputs);
            /* nv2a_ff_key counted this batch as accepted before the backend
             * was asked; if the backend refuses (vsh path off, compile
             * failure, cache full) the batch runs on the CPU below and the
             * report must say so rather than "0 left on the CPU". */
            if (!gpu_ff) { --nv2a_ff_gpu_batches; ++nv2a_ff_gpu_cpu_batches;
                           ++nv2a_ff_gpu_backend_refused; }
            /* The same binding as a program's constant file, deliberately: the
             * params are 192 float4, so nv2a_metal.m's existing
             * setVertexBytes at index 1 needs no change at all. */
            if (gpu_ff) nv2a_metal_vsh_constants((const float (*)[4])nv2a_ff_constants);
            else nv2a_metal_vsh_clear();
        } else {
            nv2a_metal_vsh_clear();
        }
        s_vsh_gpu_batch = gpu_ff;
    } else {
        nv2a_metal_vsh_clear();
        s_vsh_gpu_batch = 0;
    }
#endif
    if (vsh_reuse_stats()) {
        static uint8_t seen[65536 / 8];
        uint32_t uniq = 0, i;
        for (i = 0; i < s_gpu.idx_count; ++i) {
            uint16_t v = s_gpu.idx[i];
            if (!(seen[v >> 3] & (1u << (v & 7)))) {
                seen[v >> 3] |= (uint8_t)(1u << (v & 7));
                uniq++;
            }
        }
        for (i = 0; i < s_gpu.idx_count; ++i) {
            uint16_t v = s_gpu.idx[i];
            seen[v >> 3] &= (uint8_t)~(1u << (v & 7));
        }
        g_vsh_idx_total  += s_gpu.idx_count;
        g_vsh_idx_unique += uniq;
        g_vsh_batches_counted++;
        if (s_gpu.idx_count > g_vsh_idx_max_batch) g_vsh_idx_max_batch = s_gpu.idx_count;
    }

    /* Batch-local reuse cache: vertex index -> the position that first
     * transformed it. Cleared by re-walking this batch's indices at the end,
     * so there is no 64 K memset per batch. */
    static uint16_t reuse_at[65536];
    static uint8_t  reuse_seen[65536 / 8];
    /* Inputs as actually consumed at the first occurrence. Only allocated in
     * verify mode's conscience: 4096 slots x 16 attrs x 4 floats = 1 MB, which
     * is fine for a diagnostic and is the only way to answer the question that
     * matters -- did the two occurrences see the same data? Without it a
     * mismatch cannot distinguish "the inputs changed under us" from "the cache
     * is wrong", and rarity does not distinguish them either. */
    static float reuse_inputs[NV_MAX_INDICES][16][4];
    /* Seeded once per batch when vsh_hoist_inputs() is on; the draw
     * thread check upstream is what makes one static safe here, the
     * same assumption reuse_inputs above already makes. */
    static float batch_inputs[16][4];
    int reuse_active = (vsh_reuse_on() || vsh_reuse_verify()) && programmable;
    /* Wide copy whenever something reads the slots the narrow one leaves
     * stale: the reuse verifier compares all sixteen, and capture_draw writes
     * all sixteen to its JSON. Both are opt-in, so at gameplay this is 1. */
    /* getenv read once: this runs per vertex batch, and getenv takes libc's
     * process-wide environment lock (24 Sep profile). */
    static int draw_capture = -1;
    if (draw_capture < 0) draw_capture = getenv("RECOMP_DRAW_CAPTURE") != NULL;
    const int narrow_outputs = vsh_narrow_outputs() && !vsh_reuse_verify()
                               && !draw_capture;

    const int hoist_inputs = vsh_hoist_inputs();
    if (hoist_inputs) memcpy(batch_inputs, s_vsh.current, sizeof batch_inputs);

    for (uint32_t i = 0; i < s_gpu.idx_count; ++i) {
        if (programmable) {
            float inputs_own[16][4];
            float (*inputs)[4] = hoist_inputs ? batch_inputs : inputs_own;
            uint16_t reuse_key = s_gpu.idx[i];
            int reuse_from = -1;
            if (reuse_active &&
                (reuse_seen[reuse_key >> 3] & (1u << (reuse_key & 7))))
                reuse_from = (int)reuse_at[reuse_key];
            if (reuse_from >= 0 && vsh_reuse_on() && !vsh_reuse_verify()) {
                if (narrow_outputs)
                    copy_live_outputs(s_outputs[i], s_outputs[reuse_from]);
                else
                    memcpy(s_outputs[i], s_outputs[reuse_from], sizeof(s_outputs[i]));
                memcpy(s_positions[i], s_positions[reuse_from], sizeof(s_positions[i]));
                s_colors[i] = s_colors[reuse_from];
                g_vsh_reuse_hits++;
                continue;
            }
            NV2AVshResult result;
            unsigned long long _t_fetch = vsh_split_on() ? vsh_now_ns() : 0;
            if (!hoist_inputs)
                memcpy(inputs_own, s_vsh.current, sizeof inputs_own);
            for (uint32_t a = 0; a < 16; ++a) {
                if (!(s_vsh.decoded.inputs_read & (1u << a))) continue;
                if (a == 3 && s_gpu.attr[3].size) s_fetched_alpha_seen = 1;
                if (s_gpu.attr[a].size && !fetch_vertex(a, s_gpu.idx[i], inputs[a])) {
                    /* Name the record, once. "attribute 3 failed" is one step
                     * from useless; the format byte is the answer, because
                     * fetch_attr only decodes float and UB_OGL and a D3D8
                     * title declares its diffuse colour as UB_D3D. */
                    static uint32_t told;
                    if (!(told & (1u << a))) {
                        told |= 1u << a;
                        fprintf(stderr, "  [VSH] attribute %u unfetchable: "
                                "type=%u size=%u stride=%u offset=0x%08X "
                                "inline=%u\n", a, s_gpu.attr[a].type,
                                s_gpu.attr[a].size, s_gpu.attr[a].stride,
                                s_gpu.attr[a].offset, s_gpu.inline_count);
                        fflush(stderr);
                    }
                    VSH_REJECT("vertex attribute could not be fetched", a);
                }
            }
            /* Diffuse alpha as the shader actually receives it, every vertex
             * and independent of any trace switch. Kept as its own braced
             * statement: the previous version was inserted in front of
             * trace_vertex_inputs, which is the unbraced body of the `if`
             * below, and so both stole that guard and ran the trace
             * unconditionally. */
            {
                float ia = inputs[3][3];
                if (ia < s_fetched_alpha_lo) s_fetched_alpha_lo = ia;
                if (ia > s_fetched_alpha_hi) s_fetched_alpha_hi = ia;
            }
            if (vsh_split_on()) {
                unsigned long long _n = vsh_now_ns();
                g_vsh_fetch_ns += _n - _t_fetch; ++g_vsh_fetch_n; _t_fetch = _n;
            }
            if (i == 0 && (s_vsh.batches < 4 || s_vsh.batches == vsh_sample_batch()))
                trace_vertex_inputs(inputs);
            /* THE GPU PATH STOPS HERE. s_outputs carries the program's inputs
             * instead of its outputs, and the backend knows which because it
             * was asked above. The screen-space snap and the clip transform
             * that follow on the CPU path are both emitted into the generated
             * vertex function, so nothing downstream of this is skipped -- it
             * moves. */
            if (gpu_vsh) {
                memcpy(s_outputs[i], inputs, sizeof(s_outputs[i]));
                continue;
            }
            if (!nv2a_vsh_execute(&s_vsh.decoded, inputs, s_vsh.constants, &result))
                VSH_REJECT("shader execution failed", s_vsh.decoded.length);
            if ((result.written[0] & 12) != 12)
                VSH_REJECT("program left oPos.zw unwritten", result.written[0]);
            if (vsh_split_on()) {
                g_vsh_exec_ns += vsh_now_ns() - _t_fetch; ++g_vsh_exec_n;
            }
            if (narrow_outputs)
                copy_live_outputs(s_outputs[i], result.output);
            else
                memcpy(s_outputs[i], result.output, sizeof(s_outputs[i]));
            memcpy(s_positions[i], result.output[0], sizeof(s_positions[i]));
            s_colors[i] = pack_color(result.output[NV2A_VSH_OUT_D0]);
            /* NV2A programs include the viewport transform and perspective
             * division. Only screen subpixel quantisation remains here. */
            for (int k = 0; k < 2; ++k) {
                if (!isfinite(s_positions[i][k]))
                    VSH_REJECT("oPos is not finite", k);
                if (fabsf(s_positions[i][k]) < 0x1p20f)
                    s_positions[i][k] = truncf(s_positions[i][k] * 16.0f) / 16.0f;
                s_outputs[i][0][k] = s_positions[i][k];
            }
            if (reuse_active) {
                if (reuse_from >= 0) {
                    /* Verification path: the shader ran anyway. Compare the
                     * COMPLETE transformed output, not just position -- a cache
                     * that got oPos right and a texcoord wrong would otherwise
                     * pass. */
                    g_vsh_reuse_hits++;
                    if (memcmp(s_outputs[i], s_outputs[reuse_from],
                               sizeof(s_outputs[i])) != 0 ||
                        memcmp(s_positions[i], s_positions[reuse_from],
                               sizeof(s_positions[i])) != 0 ||
                        s_colors[i] != s_colors[reuse_from]) {
                        if (g_vsh_reuse_mismatch < 4) {
                            int a, k, in_diff = 0;
                            fprintf(stderr,
                                    "  [VSH-REUSE] MISMATCH batch=%u i=%u idx=%u"
                                    " first_at=%d prog_len=%u\n",
                                    s_vsh.batches, i, reuse_key, reuse_from,
                                    s_vsh.decoded.length);
                            /* Did the INPUTS differ? This is the question --
                             * a pure shader cannot produce two answers from one
                             * input, so either they differed or the cache is
                             * wrong, and this says which. */
                            for (a = 0; a < 16; ++a)
                                for (k = 0; k < 4; ++k)
                                    if (memcmp(&reuse_inputs[reuse_from][a][k],
                                               &inputs[a][k], sizeof(float))) {
                                        in_diff++;
                                        if (in_diff <= 6)
                                            fprintf(stderr,
                                                "    input a%d.%d first=%.9g now=%.9g"
                                                " (bits %08X vs %08X)\n", a, k,
                                                (double)reuse_inputs[reuse_from][a][k],
                                                (double)inputs[a][k],
                                                *(const uint32_t *)&reuse_inputs[reuse_from][a][k],
                                                *(const uint32_t *)&inputs[a][k]);
                                    }
                            fprintf(stderr, "    inputs differing: %d of 64\n", in_diff);
                            /* And which OUTPUT components differ, named. */
                            for (a = 0; a < 16; ++a)
                                for (k = 0; k < 4; ++k)
                                    if (memcmp(&s_outputs[reuse_from][a][k],
                                               &s_outputs[i][a][k], sizeof(float)))
                                        fprintf(stderr,
                                            "    output o%d.%d cached=%.9g fresh=%.9g\n",
                                            a, k, (double)s_outputs[reuse_from][a][k],
                                            (double)s_outputs[i][a][k]);
                            fprintf(stderr, "    colors cached=%08X fresh=%08X\n",
                                    s_colors[reuse_from], s_colors[i]);
                            fflush(stderr);
                        }
                        g_vsh_reuse_mismatch++;
                    }
                } else {
                    reuse_seen[reuse_key >> 3] |= (uint8_t)(1u << (reuse_key & 7));
                    reuse_at[reuse_key] = (uint16_t)i;
                    if (vsh_reuse_verify() && i < NV_MAX_INDICES)
                        memcpy(reuse_inputs[i], inputs, sizeof reuse_inputs[i]);
                }
            }
            /* Sample a late batch as well as the first few. The early ones
             * are the measured full-screen blit and always look the same;
             * whether the title's own geometry transforms differently is a
             * question about batch 20,000, not batch 1. RECOMP_VSH_SAMPLE
             * moves the sample point. */
            if (s_vsh.batches == vsh_sample_batch() && i < 3) {
                /* The constants matter as much as the attributes. A program
                 * that produces an infinite w is dividing by something, and
                 * the transform rows are where that something comes from. */
                fprintf(stderr, "  [VSH] late batch c0..c7:");
                for (uint32_t k = 0; k < 8; ++k)
                    fprintf(stderr, " [%g %g %g %g]", s_vsh.constants[k][0],
                            s_vsh.constants[k][1], s_vsh.constants[k][2],
                            s_vsh.constants[k][3]);
                fprintf(stderr, "\n  [VSH] late batch attrs:");
                for (uint32_t k = 0; k < 16; ++k)
                    if (s_gpu.attr[k].size)
                        fprintf(stderr, " a%u(t%u s%u st%u @%08X)", k,
                                s_gpu.attr[k].type, s_gpu.attr[k].size,
                                s_gpu.attr[k].stride, s_gpu.attr[k].offset);
                fprintf(stderr, " reads=%04X inline=%u idx=%u\n",
                        s_vsh.decoded.inputs_read, s_gpu.inline_count,
                        s_gpu.idx[i]);
                /* The values actually handed to the program, and the raw
                 * bytes behind them. A constant oPos has exactly two causes
                 * -- the inputs are zero, or the transform is -- and these
                 * two lines tell them apart. */
                fprintf(stderr, "  [VSH] late batch in0=[%g %g %g %g]"
                                " in3=[%g %g %g %g] in9=[%g %g %g %g]\n",
                        inputs[0][0], inputs[0][1], inputs[0][2], inputs[0][3],
                        inputs[3][0], inputs[3][1], inputs[3][2], inputs[3][3],
                        inputs[9][0], inputs[9][1], inputs[9][2], inputs[9][3]);
                {
                    /* Vertex array offsets are relative to a DMA context, the
                     * way texture and surface offsets are -- and unlike those,
                     * fetch_attr treats them as absolute guest addresses. If
                     * either vertex DMA object has a non-zero base, that
                     * assumption is wrong and every array read lands in the
                     * wrong place. Resolve them the same way the copy path
                     * does and say what they are. */
                    const uint8_t *regs = xbox_Nv2aRegisterMemory();
                    uint32_t ramht = 0, base, limit;
                    if (regs) memcpy(&ramht, regs + 0x2210, 4);
                    for (int which = 0; which < 2; ++which) {
                        uint32_t m = which ? NV097_SET_CONTEXT_DMA_VERTEX_B
                                           : NV097_SET_CONTEXT_DMA_VERTEX_A;
                        uint32_t handle = s_methods[m / 4];
                        int seen = s_method_seen[m / 4];
                        int ok = regs && seen && nv2a_dma_resolve(regs + 0x700000,
                                0x100000, ramht, handle, &base, &limit);
                        fprintf(stderr, "  [VSH] vertex DMA %c: seen=%d "
                                        "handle=0x%08X resolved=%d base=0x%08X "
                                        "limit=0x%08X\n", which ? 'B' : 'A',
                                seen, handle, ok, ok ? base : 0,
                                ok ? limit : 0);
                    }
                }
                {
                    char owner[192];
                    xbox_HeapDescribe(s_gpu.attr[0].offset, owner, sizeof owner);
                    fprintf(stderr, "  [VSH] array owner: %s\n", owner);
                }
                {
                    const uint8_t *raw = (const uint8_t *)xbox_GetMemoryOffset()
                            + s_gpu.attr[0].offset
                            + (size_t)s_gpu.idx[i] * s_gpu.attr[0].stride;
                    fprintf(stderr, "  [VSH] late batch bytes:");
                    for (uint32_t k = 0; k < 32; ++k)
                        fprintf(stderr, " %02X", raw[k]);
                    fprintf(stderr, "\n");
                    /* POSIX currently gives the CPU's physical window separate
                     * storage. D3D resource locks can OR 0x80000000 into the
                     * same offset; inspect that view before calling an array
                     * unwritten. This is observation only, not a fetch override. */
                    uint64_t physical=(uint64_t)s_gpu.attr[0].offset
                            +(size_t)s_gpu.idx[i]*s_gpu.attr[0].stride+0x80000000u;
                    const uint8_t *alias=physical<=UINT32_MAX
                            ? xbox_GpuMemoryRange((uint32_t)physical,32) : NULL;
                    if (!s_gpu.inline_count && alias) {
                        fprintf(stderr,"  [VSH] physical-window bytes @%08X:",(uint32_t)physical);
                        for (uint32_t k=0;k<32;++k) fprintf(stderr," %02X",alias[k]);
                        fprintf(stderr,"\n");
                    }
                }
            }
            if ((s_vsh.batches < 4 || s_vsh.batches == vsh_sample_batch()) && i < 3)
                fprintf(stderr, "  [VSH] start=%u slots=%d vertex=%u oPos=(%.6g %.6g %.6g %.6g) color=%08X\n",
                        s_vsh.start, s_vsh.decoded.length, i,
                        result.output[0][0], result.output[0][1], result.output[0][2], result.output[0][3], s_colors[i]);
        } else if (gpu_ff) {
            /* THE GPU PATH STOPS HERE, exactly as it does for a programmable
             * program forty lines up: s_outputs carries the fixed-function
             * unit's INPUTS, and the generated vertex function does the
             * transform, the divide, the viewport offset and the texgen.
             *
             * Only the attributes the emitted function names are fetched. The
             * CPU branch below fetches position, diffuse and four texture
             * coordinates, and then fetches EVERY sized attribute again inside
             * the nv2a_ff_vertex block -- so this is six fewer fetches per
             * vertex as well as one fewer transform, and the fetch is the half
             * of this stage the change could not otherwise remove. */
            float inputs[16][4];
            memcpy(inputs, s_vsh.current, sizeof(inputs));
            for (uint32_t a = 0; a < 16; ++a)
                if ((ff_key.inputs & (1u << a)) && s_gpu.attr[a].size
                    && !fetch_vertex(a, s_gpu.idx[i], inputs[a]))
                    VSH_REJECT("fixed-function vertex fetch", a);
            /* The one refusal the batch KEY cannot carry, because it is a
             * property of this vertex and not of the batch's shape. Same
             * string as the CPU arm, so the reject table stays comparable. */
            if (!nv2a_ff_clip_w_ok(inputs[0]))
                VSH_REJECT("fixed-function clip W", 0);
            nv2a_ff_count_texq(&ff_key, (const float (*)[4])inputs,
                               s_copy.state.texture_mask);
            memcpy(s_outputs[i], inputs, sizeof(s_outputs[i]));
            if (i == 0) ff_batch_dump("gpu", (const float (*)[4])inputs,
                                      &ff_key, (unsigned)sizeof ff_key);
            ff_watch_vertex("gpu", i, (const float (*)[4])inputs);
        } else {
            float color[4];
            if (!fetch_vertex(0, s_gpu.idx[i], s_positions[i]))
                VSH_REJECT("position attribute could not be fetched", 0);
            int has_color = fetch_vertex(3, s_gpu.idx[i], color);
            s_colors[i] = has_color ? pack_color(color) : 0xFFFFFFFFu;
            memset(s_outputs[i], 0, sizeof(s_outputs[i]));
            memcpy(s_outputs[i][0], s_positions[i], sizeof(s_positions[i]));
            for (int k=0;k<4;++k) s_outputs[i][NV2A_VSH_OUT_D0][k] = has_color ? color[k] : 1;
            for(unsigned unit=0;unit<4;++unit) {
                memcpy(s_outputs[i][NV2A_VSH_OUT_T0+unit],s_vsh.current[9+unit],4*sizeof(float));
                if(s_gpu.attr[9+unit].size)
                    fetch_vertex(9+unit,s_gpu.idx[i],s_outputs[i][NV2A_VSH_OUT_T0+unit]);
            }
            /* Hand the executor's seen-table to the fixed-function unit.
             * nv2a_ff.c needs to tell "the guest uploaded these sixteen zero
             * words" from "nobody ever wrote this block and s_methods is
             * still zero-initialised" -- and for a TEXTURE matrix those two
             * differ by an entire batch of geometry: an unwritten matrix
             * transforms every coordinate to (0,0,0,0), q becomes 0, and the
             * sink drops every textured triangle with no counter naming the
             * cause. Measured: TMAT0 reads all sixteen words zero in a
             * gameplay run. The composite matrix two lines below has been
             * gated on exactly this table since long before today; the
             * texture matrices never were. */
            nv2a_ff_method_seen = s_method_seen;
            if(s_method_seen[0x680/4] && s_method_seen[0x6bc/4]) {
                float inputs[16][4];
                memcpy(inputs,s_vsh.current,sizeof(inputs));
                for(unsigned a=0;a<16;++a) if(s_gpu.attr[a].size)
                    if(!fetch_vertex(a,s_gpu.idx[i],inputs[a])) VSH_REJECT("fixed-function vertex fetch",a);
                const char *reason=nv2a_ff_vertex(s_methods,inputs,s_outputs[i]);
                if(i==0) ff_batch_dump("cpu",(const float (*)[4])inputs,NULL,0);
                ff_watch_vertex("cpu", i, (const float (*)[4])inputs);
                if(reason) VSH_REJECT(reason,0);
                /* RECOMP_FF_DUMP=1 -- READ-ONLY, answers two questions that
                 * cannot be settled by argument, and both have a fix attached
                 * that would WRECK THE PICTURE if the argument were taken on
                 * trust.
                 *
                 * 1. IS THE VIEWPORT SCALE BAKED INTO THE COMPOSITE MATRIX?
                 * nv2a_ff.c applies NV097_SET_VIEWPORT_OFFSET (0x0A20) and
                 * never NV097_SET_VIEWPORT_SCALE (0x0AF0), which appears
                 * nowhere in this tree. The guest programs VPSCL =
                 * (320,-240,16777215,0). If the scale is genuinely missing,
                 * 58.8% of gameplay draws collapse to a couple of pixels at
                 * the screen centre -- and the picture plainly does not do
                 * that, so D3D is probably folding it into the matrix it
                 * uploads at 0x680. Applying the scale on top of a matrix that
                 * already carries it multiplies the screen by 320 twice. Read
                 * clip.x/clip.w: in [-1,1] the scale is missing; around
                 * +/-320 it is already there. z and xy are independent
                 * scales, so read clip.z/clip.w separately.
                 *
                 * 2. IS matrix() TRANSPOSED FOR THE TEXTURE MATRIX? It reads
                 * the register block as M[row][col] and computes M*v, the
                 * column-vector convention; a D3DMATRIX is row-major with
                 * row-vector semantics, the transpose. On a texture matrix
                 * that makes q come out of the TRANSLATION ROW -- a
                 * data-dependent value that sometimes goes <= 0 -- instead of
                 * the constant 1 a last-column read gives. That fits the
                 * measured rejection rate, which is sparse (0.002%) rather
                 * than uniform. Read TMAT0: last ROW (0,0,0,1) means matrix()
                 * is right; last COLUMN (0,0,0,1) means it is transposed.
                 *
                 * First transformed vertex of the run only, so it costs one
                 * printf and cannot perturb what it measures. */
                {
                    static int ff_dump = -1, ff_shown;
                    if (ff_dump < 0) ff_dump = recomp_switch_on("RECOMP_FF_DUMP");
                    if (ff_dump && !ff_shown++) {
                        const float *cm = (const float *)&s_methods[0x680/4];
                        const float *tm = (const float *)&s_methods[0x6c0/4];
                        const float *vs = (const float *)&s_methods[0x0af0/4];
                        const float *vo = (const float *)&s_methods[0x0a20/4];
                        fprintf(stderr,
                            "  [FF-DUMP] VPSCL=(%.3f %.3f %.3f %.3f) seen=%d"
                            "  VPOFF=(%.3f %.3f %.3f %.3f) seen=%d\n",
                            vs[0],vs[1],vs[2],vs[3], s_method_seen[0x0af0/4],
                            vo[0],vo[1],vo[2],vo[3], s_method_seen[0x0a20/4]);
                        for (int r = 0; r < 4; ++r)
                            fprintf(stderr, "  [FF-DUMP] CMAT row%d = %12.4f %12.4f"
                                    " %12.4f %12.4f\n", r,
                                    cm[r*4+0],cm[r*4+1],cm[r*4+2],cm[r*4+3]);
                        for (int r = 0; r < 4; ++r)
                            fprintf(stderr, "  [FF-DUMP] TMAT0 row%d = %12.4f %12.4f"
                                    " %12.4f %12.4f\n", r,
                                    tm[r*4+0],tm[r*4+1],tm[r*4+2],tm[r*4+3]);
                        fprintf(stderr,
                            "  [FF-DUMP] in=(%.4f %.4f %.4f %.4f) -> out=(%.4f"
                            " %.4f %.4f %.4f)  surface %ux%u\n",
                            inputs[0][0],inputs[0][1],inputs[0][2],inputs[0][3],
                            s_outputs[i][0][0],s_outputs[i][0][1],
                            s_outputs[i][0][2],s_outputs[i][0][3],
                            s_gpu.clip_w, s_gpu.clip_h);
                        fflush(stderr);
                    }
                }
                memcpy(s_positions[i],s_outputs[i][0],sizeof(s_positions[i]));
                s_colors[i]=pack_color(s_outputs[i][3]);
            }
        }
    }
    /* Clear the reuse cache for the NEXT batch, by re-walking this batch's own
     * indices. Without this, entries leak across batches and a later batch
     * reuses a transform computed from a different vertex array, different
     * constants or a different program -- silently wrong geometry rather than a
     * crash. O(n), and it is why the cache is a bitmap plus a table rather than
     * a 64 K array that would need clearing wholesale. */
    if (reuse_active) {
        for (uint32_t i = 0; i < s_gpu.idx_count; ++i) {
            uint16_t v = s_gpu.idx[i];
            reuse_seen[v >> 3] &= (uint8_t)~(1u << (v & 7));
        }
    }
    if (programmable) s_vsh.batches++;
    fog_draw_trace(programmable);
    return 1;
}

/* Capture state at a draw boundary, not during a periodic interrupt report.
 * The first draw and two later samples distinguish initial setup from steady
 * state. Raw method values are included even when rendering does not support
 * them yet. The snapshot format is deliberately independent of C structs. */
/* One getenv per switch, cached for the life of the process.
 *
 * getenv is not free: on macOS it takes a lock and walks the environment
 * linearly (__findenv_locked), and on Windows it walks a block. Most switches
 * in this file already read through the `static int on = -1` idiom for that
 * reason -- see nv2a_gpu_on() and the comment above it. These four were
 * missed, and they sit in the three hottest functions in the file:
 * nv2a_pb_exec_method runs once per pushbuffer METHOD, draw_primitive and
 * capture_draw once per draw.
 *
 * MEASURED, 12 s sample of the title at the menu scene on the -O2 build:
 * getenv cost 444 samples, against 320 for the whole vertex shader
 * interpreter and 60 for nv2a_metal_draw -- roughly half of all the CPU this
 * file was using, spent asking the environment about switches that were off.
 *
 * Caching means the value is the environment as it stood at first use. That is
 * already the contract every other switch in this file has, and nothing sets
 * these after startup. */
static int pb_env_on(const char *name, int *slot)
{
    if (*slot < 0) *slot = getenv(name) != NULL;
    return *slot;
}

/* The string form, for switches that carry a value. Caches the miss too, so a
 * switch that is off costs one getenv for the whole run rather than one per
 * draw. Returns NULL when unset, exactly as getenv does.
 *
 * The sentinel is not decoration. Caching a miss as "" would make a variable
 * that is SET TO AN EMPTY STRING indistinguishable from one that is unset, and
 * they do not mean the same thing here: RECOMP_DRAW_CAPTURE="" is a valid
 * (if odd) prefix that captures into the working directory, and getenv reports
 * it as non-NULL. An address no string literal can share keeps the two
 * apart, so this is a cache and not a behaviour change. */
static const char pb_env_unset[1];

static const char *pb_env_str(const char *name, const char **slot)
{
    if (!*slot) {
        const char *v = getenv(name);
        *slot = v ? v : pb_env_unset;
    }
    return *slot == pb_env_unset ? NULL : *slot;
}

static int pb_verbose(void)
{
    static int on = -1;
    return pb_env_on("RECOMP_PB_EXEC_VERBOSE", &on);
}

static int pb_vertex_range(void)
{
    static int on = -1;
    return pb_env_on("RECOMP_VERTEX_RANGE", &on);
}

static const char *pb_draw_capture(void)
{
    static const char *slot;
    return pb_env_str("RECOMP_DRAW_CAPTURE", &slot);
}

static void capture_bytes(const char *extension, const void *data, size_t size)
{
    const char *prefix = pb_draw_capture();
    if (!prefix || !data || !s_capture_selected) return;
    char path[768];
    snprintf(path, sizeof(path), "%s%06u.%s", prefix, s_gpu.draws, extension);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    if (fwrite(data, 1, size, f) != size) perror(path);
    if (fclose(f)) perror(path);
}

static void capture_draw(const char *error)
{
    const char *prefix = pb_draw_capture();
    static const char *sample_slot;
    const char *sample = pb_env_str("RECOMP_DRAW_SAMPLE", &sample_slot);
    s_capture_selected = prefix && (s_gpu.draws == 1 || s_gpu.draws == 128 || s_gpu.draws == 2048
            || s_combiner_capture || (sample && s_gpu.draws==strtoul(sample,NULL,0))
            || (error && s_copy.rejected < 2));
    if (!s_capture_selected) return;
#if NV2A_GPU_PATH
    if (nv2a_gpu_on()) nv2a_gpu_sync();
#endif
    char path[768];
    snprintf(path, sizeof(path), "%s%06u.json", prefix, s_gpu.draws);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "{\n\"version\":1,\"draw\":%u,\"primitive\":%u,\"inline_words\":%u,\"reject_reason\":",
            s_gpu.draws, s_gpu.prim, s_gpu.inline_count);
    /* All rejection reasons are fixed renderer literals, not guest text. */
    if (error) fprintf(f,"\"%s\"",error); else fputs("null",f);
    fputs(",\"registers\":{",f);
    int comma = 0;
    for (unsigned i = 0; i < 0x2000/4; ++i) if (s_method_seen[i]) {
        fprintf(f, "%s\n\"%04x\":%u", comma ? "," : "", i*4, s_methods[i]); comma = 1;
    }
    fprintf(f, "},\n\"vertices\":[");
    for (unsigned i = 0; i < s_gpu.idx_count; ++i) {
        fprintf(f, "%s[", i ? "," : "");
        for (int o = 0; o < 16; ++o) {
            fprintf(f, "%s[", o ? "," : "");
            for (int k = 0; k < 4; ++k) {
                if (k) fputc(',', f);
                if (isfinite(s_outputs[i][o][k])) fprintf(f, "%.9g", s_outputs[i][o][k]);
                else fputs("null", f);
            }
            fputc(']', f);
        }
        fputc(']', f);
    }
    const uint8_t *regs = xbox_Nv2aRegisterMemory();
    uint32_t ramht = 0;
    if (regs) memcpy(&ramht, regs + 0x2210, 4);
    fprintf(f, "],\"ramht\":%u,\"copy_supported\":%s,\"texture_address\":%u,\"target_address\":%u,\"depth_address\":%u}\n",
            ramht, s_copy.active ? "true" : "false", s_copy.texture_address, s_copy.target_address,s_copy.depth_address);
    if (regs) capture_bytes("ramin", regs+0x700000, 0x100000);
    if (s_copy.active) {
        capture_bytes("texture", s_copy.texture, s_copy.texture_bytes);
        capture_bytes("before", s_copy.target, s_copy.target_bytes);
        if (s_copy.depth) capture_bytes("depth-before",s_copy.depth,s_copy.depth_bytes);
    }
    if (fclose(f)) perror(path);
    else fprintf(stderr, "[DRAW] captured %s\n", path);
}

/* Is attribute 0 already in screen space? Measured, not assumed: every vertex
 * of the batch has to land inside the surface. Object-space positions are
 * small numbers around the origin and fail this immediately, which is what
 * keeps an untransformed batch from being smeared across the top-left corner.
 */
static int batch_is_screen_space(void)
{
    float p[4];
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h)
        return 0;
    for (i = 0; i < s_gpu.idx_count; i++) {
        if (!fetch_vertex(0, s_gpu.idx[i], p))
            return 0;
        if (p[0] < (float)s_gpu.clip_x - 1.0f
         || p[0] > (float)(s_gpu.clip_x + s_gpu.clip_w) + 1.0f
         || p[1] < (float)s_gpu.clip_y - 1.0f
         || p[1] > (float)(s_gpu.clip_y + s_gpu.clip_h) + 1.0f)
            return 0;
    }
    return 1;
}

/* NV097 primitive types that are triangles under some winding. */
#define NV_PRIM_TRIANGLES      NV097_SET_BEGIN_END_OP_TRIANGLES
#define NV_PRIM_TRIANGLE_STRIP NV097_SET_BEGIN_END_OP_TRIANGLE_STRIP
#define NV_PRIM_TRIANGLE_FAN   NV097_SET_BEGIN_END_OP_TRIANGLE_FAN
#define NV_PRIM_QUADS          NV097_SET_BEGIN_END_OP_QUADS
#define NV_PRIM_QUAD_STRIP     NV097_SET_BEGIN_END_OP_QUAD_STRIP

static void raster_indices(uint32_t a, uint32_t b, uint32_t c)
{
    if (!s_copy.active) {
        raster_triangle(s_positions[a], s_positions[b], s_positions[c], s_colors[a]);
        return;
    }
    if (nv2a_texture_copy_triangle_depth(&s_copy.state, s_copy.texture, s_copy.texture_bytes,
            s_copy.target, s_copy.target_bytes, s_copy.depth,s_copy.depth_bytes,
            s_outputs[a], s_outputs[b], s_outputs[c])) {
        ++s_gpu.tris_drawn;
        ++s_gpu.cpu_tris;
    } else {
        if (++s_copy.rejected <= 4) fprintf(stderr, "[TEXTURE] rejected triangle geometry / bounds\n");
    }
}

/* G54: the state a [DROP] line names. */
static void drop_state(NV2ADropState *st)
{
    st->cw0 = s_methods[0x288/4]; st->texmodes = s_methods[0x1e70/4]; st->tex0_format = s_methods[0x1b04/4];
    st->mode_prim = (s_methods[0x1e94/4] & 3u) | ((s_gpu.prim & 0xFFu) << 8); st->draw = s_gpu.draws;
}
static void raster_batch(void)
{
    uint32_t i;
    uint32_t drawn_before = s_gpu.tris_drawn;
    {   static int registered;
        if (!registered) { registered = 1; nv2a_drop_set_state_source(drop_state); } }
    nv2a_drop_batch();

    /* Stage-by-stage fate of a batch drawn under a multiply blend. Sampled
     * here rather than at the accept test, because everything below the
     * vertex stage is invisible from there. */
    const int fade_batch = blend_is_multiply();
    if (fade_batch) ++s_blend_fade_fate.batches;

    if (s_gpu.idx_count < 3) {
        if (fade_batch) ++s_blend_fade_fate.short_idx;
        if (s_gpu.idx_count) nv2a_drop(NV2A_DROP_DROPPED, "raster", "fewer than three indices (a point or line batch)", s_gpu.idx_count);
        return;
    }
    if (s_host_skip && !s_host_skip_late) { s_copy.active = 0; ++s_host_skipped; return; }
    unsigned long long _t_vsh = pb_now_us();
    int _vsh_ok = prepare_vertices();
    pb_stage_add(PB_STAGE_VSH, _t_vsh);
    if (!_vsh_ok) {
        if (fade_batch) ++s_blend_fade_fate.vsh_rejected;
        static unsigned vsh_capture_count;
        if (!vsh_capture_count++ && getenv("RECOMP_DRAW_CAPTURE")) {
            int combiner_capture = s_combiner_capture;
            s_combiner_capture = 1;
            capture_draw(s_vsh_reason ? s_vsh_reason : "vertex preparation");
            s_combiner_capture = combiner_capture;
        }
        s_vsh.rejected++;
        note_vsh_reject();
        nv2a_drop(NV2A_DROP_DROPPED, "vertex", s_vsh_reason ? s_vsh_reason : "vertex preparation failed", s_vsh_reason_detail);
        if (s_vsh.rejected <= 4)
            fprintf(stderr, "  [VSH] rejected batch mode=%u start=%u valid=%d final=%d: "
                            "%s (%u), lighting=%u skin=%u\n",
                    s_vsh.mode, s_vsh.start, s_vsh.decoded.valid, s_vsh.decoded.has_final,
                    s_vsh_reason ? s_vsh_reason : "unrecorded", s_vsh_reason_detail,
                    s_methods[0x314/4], s_methods[0x328/4]);
        return;
    }
    /* Sampled after the vertex stage, so the value recorded is the one the
     * rasteriser would actually interpolate. Blend-enabled batches only: an
     * opaque batch carries alpha it never uses, and counting those would bury
     * the fade quad in a table of 255s. */
    {
        static int on = -1;
        if (on < 0) on = getenv("RECOMP_BLEND_TRACE") ? 1 : 0;
        if (on && s_methods[0x304/4]) {
            float lo = 2.0f, hi = -1.0f;
            for (uint32_t v = 0; v < s_gpu.idx_count; ++v) {
                float a = s_outputs[s_gpu.idx[v]][NV2A_VSH_OUT_D0][3];
                if (!isfinite(a)) continue;
                if (a < lo) lo = a;
                if (a > hi) hi = a;
            }
            if (hi >= 0.0f) {
                ++s_alpha_trace_batches;
                /* The INPUT alpha beside the output, because they answer
                 * different questions and only the pair localises the loss.
                 * note_batch_alpha samples s_outputs[..][OUT_D0][3], the vertex
                 * shader's RESULT. A ramp that reaches the input and not the
                 * output is the shader or our execution of it; one that never
                 * reaches the input was lost before the pushbuffer, in guest
                 * code. Both look identical from the output alone, which is all
                 * that was measured before. Attribute 3 is diffuse, and
                 * s_vsh.current[3] is what an immediate write leaves there. */
                /* The FETCHED value when the attribute is array-bound, which
                 * is the one the shader actually received. Sampling
                 * s_vsh.current[3][3] instead reported the untouched 1.0
                 * default and looked exactly like a real 255. */
                note_batch_alpha_in(
                    s_fetched_alpha_hi >= 0.0f
                        ? (unsigned)(s_fetched_alpha_lo * 255.0f + 0.5f)
                        : (unsigned)(s_vsh.current[3][3] * 255.0f + 0.5f),
                    s_gpu.attr[3].size != 0);
                /* The DECLARATION, because a constant 255 is also exactly what
                 * a 3-component diffuse produces: fetch_vertex fills what the
                 * declaration names and leaves w at the 1.0 default. That
                 * would be our mis-parse of the vertex format rather than the
                 * guest losing the value, and the two are indistinguishable
                 * from the fetched alpha alone. */
                {
                    static int shown;
                    if (!shown) {
                        shown = 1;
                        fprintf(stderr, "[ALPHA-IN] attr[3] decl: size=%u type=%u"
                                " stride=%u offset=0x%X\n",
                                s_gpu.attr[3].size, s_gpu.attr[3].type,
                                s_gpu.attr[3].stride, s_gpu.attr[3].offset);
                        fflush(stderr);
                    }
                }
                s_fetched_alpha_lo = 2.0f; s_fetched_alpha_hi = -1.0f;
                /* One level per batch: the fade quad is flat-shaded, so lo and
                 * hi agree on it, and a batch where they disagree is not the
                 * quad we are looking for. */
                note_batch_alpha((unsigned)(lo * 255.0f + 0.5f));
            }
        }
    }

    /* Timed on BOTH exits, the reject as well as the accept. A batch that
     * fails a DMA range check has already paid for the full RAMHT scan that
     * failed it -- a miss scans the whole table where a hit stops early -- so
     * timing only the success path would hide the expensive half. `copy_error`
     * is taken first and the stage closed before anything looks at it, so the
     * reject bookkeeping below is not charged here. */
    fog_trace();
    unsigned long long _t_prep = pb_now_us();
    const char *copy_error = prepare_texture_copy();
    pb_stage_add(PB_STAGE_PREPARE, _t_prep);
    trace_combiner(copy_error);
    capture_draw(copy_error);
    if (copy_error) {
        if (fade_batch) ++s_blend_fade_fate.prepare_rejected;
        if (++s_copy.rejected <= 8) fprintf(stderr, "[TEXTURE] rejected draw %u: %s (t=%.2f)\n", s_gpu.draws, copy_error, trace_seconds());
        nv2a_drop(NV2A_DROP_DROPPED, "fragment", copy_error, 0);
        return;
    }
    if (fade_batch) ++s_blend_fade_fate.rasterised;
    /* RECOMP_FONT_TRACE -- which sheet a text draw samples.
     *
     * jetfont.dat holds six textures: four 512x512 DXT3 CJK sheets and two
     * 256x256 DXT3 Latin pages. Page 0 is codes 0x21..0x75, `!` through `u`;
     * page 1 begins at 0x76, and the corrupted letters on record are exactly
     * v w x y z. The standing inference is that a page-1 character samples a
     * CJK sheet, and it has never been proven in code.
     *
     * Size settles it without decoding anything: a Latin page is 256x256 DXT3
     * = 65,536 bytes, a CJK sheet is 512x512 DXT3 = 262,144. A text draw
     * naming 262,144 bytes is on the wrong sheet by construction. Read-only,
     * opt-in, and it prints the address so two draws can be told apart. */
    {
        static int on = -1; static unsigned cap;
        if (on < 0) {
            on = recomp_switch_on("RECOMP_FONT_TRACE");
            /* RECOMP_FONT_TRACE=<n> raises the line cap. 4,000 lines is spent
             * in the first seconds of a boot, and a tutorial banner arrives
             * minutes into a replay, so the default cap traces the wrong
             * part of the run. */
            const char *want = getenv("RECOMP_FONT_TRACE");
            long n = (want && *want) ? strtol(want, NULL, 10) : 0;
            cap = (n > 1) ? (unsigned)n : 4000u;
        }
        if (on && !s_copy.state.untextured && s_copy.state.dxt3
                && s_copy.state.width == s_copy.state.height
                && (s_copy.state.width == 256 || s_copy.state.width == 512)) {
            /* Content hash of the first mip level. If the two Latin pages
             * hash the same, the loader put one page's pixels at both
             * addresses, and a page-1 glyph would draw page 0's cell with
             * perfectly correct UVs -- which is what `y` -> `$` looks like. */
            /* The hash walks 64 KB per draw and the page identity question it
             * was written for is closed, so it is now its own switch. A trace
             * that costs a frame changes the race it is watching. */
            static int want_hash = -1;
            if (want_hash < 0) want_hash = recomp_switch_on("RECOMP_FONT_TRACE_HASH");
            unsigned long long h = 1469598103934665603ULL;
            if (want_hash && s_copy.texture) {
                size_t level0 = (size_t)s_copy.state.width * s_copy.state.height;
                if (level0 > s_copy.texture_bytes) level0 = s_copy.texture_bytes;
                for (size_t i = 0; i < level0; i++) {
                    h ^= s_copy.texture[i];
                    h *= 1099511628211ULL;
                }
            }
            static unsigned shown;
            if (shown++ < cap)
                /* WHERE THE VERTICES CAME FROM, beside which sheet they
                 * sampled. The two passes of one line of text were shown on
                 * 20 Sep 2026 to be reading the SAME buffer, the second having
                 * overwritten the head of the first; printing attribute 0's
                 * base and stride is what turns that from an inference about
                 * quad contents into the address it happened at, and it is
                 * what a memory watch would need. */
                fprintf(stderr, "  [FONT-TRACE] draw=%u %ux%u dxt3 bytes=%zu addr=0x%08X"
                        " verts=%u fmt=%08X rect=%08X off=%08X lv=%u hash=%016llX"
                        " vtx=0x%08X stride=%u\n",
                        s_gpu.draws, s_copy.state.width,
                        s_copy.state.height, s_copy.texture_bytes, s_copy.texture_address,
                        (unsigned)s_gpu.idx_count,
                        s_methods[NV097_SET_TEXTURE_FORMAT / 4],
                        s_methods[NV097_SET_TEXTURE_IMAGE_RECT / 4],
                        s_methods[NV097_SET_TEXTURE_OFFSET / 4],
                        s_copy.state.levels, h,
                        s_gpu.attr[0].offset, s_gpu.attr[0].stride);
        }
    }
    if (s_vsh.mode == 0 && !(s_method_seen[0x680/4] && s_method_seen[0x6bc/4]) && !batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        nv2a_drop(NV2A_DROP_DROPPED, "vertex", "fixed-function batch before the composite matrix was ever written", 0);
        /* RECOMP_PB_EXEC_NOCLIPTEST: rasterise anyway, to tell "the vertices
         * were decoded but sit outside the clip rect" apart from "the vertices
         * are not usable at all". Diagnostic only -- the test exists because
         * untransformed vertices would otherwise paint nonsense. */
        {
            static int allow = -1;
            if (allow < 0) allow = recomp_switch_on("RECOMP_PB_EXEC_NOCLIPTEST");
            if (!allow)
                return;
        }
    }

    if (s_host_skip) { ++s_host_skipped; goto batch_complete; }   /* late skip (bisect) */
#if NV2A_GPU_PATH
    if (s_copy.active && nv2a_gpu_on()) {
        static unsigned fallback_reports, unique_reports;
        static const char *seen_reasons[16];
        static int trace_fallbacks = -1;
        if (trace_fallbacks < 0)
            trace_fallbacks = getenv("RECOMP_GPU_FALLBACK_TRACE")
                || getenv("RECOMP_METAL_FALLBACK_TRACE") ? 1 : 0;
        draw_mix_note();
        unsigned long long _t_sub = pb_now_us();
        int triangles=nv2a_gpu_draw(&s_copy.state,s_copy.texture,s_copy.texture_bytes,
            s_copy.target,s_copy.target_bytes,s_copy.depth,s_copy.depth_bytes,
            s_outputs,s_gpu.idx_count,s_gpu.prim);
        pb_stage_add(PB_STAGE_SUBMIT, _t_sub);
        draw_mix_result(triangles);
        if(triangles>=0) {
            static unsigned reported;
            s_gpu.tris_drawn+=(unsigned)triangles;
            ++s_gpu.gpu_batches;
            if(reported++<10) fprintf(stderr,"[" NV2A_GPU_TAG "] rendered batch: %d triangles\n",triangles);
            goto batch_complete;
        }
        ++s_gpu.gpu_fallbacks;
        const char *reason=nv2a_gpu_last_reject();
        nv2a_drop(NV2A_DROP_SIMPLIFIED, "Metal refused, CPU rasteriser drew it", reason, 0);
        int unique=1;
        for(unsigned i=0;i<unique_reports;i++)
            if(!strcmp(reason,seen_reasons[i]))unique=0;
        if(unique&&unique_reports<16)seen_reasons[unique_reports++]=reason;
        else if(unique)unique=0;
        if(fallback_reports++<3 || (trace_fallbacks && unique)) fprintf(stderr,
            "[" NV2A_GPU_TAG "] software fallback (%s): combiner=%u mask=0x%x untextured=%u format=%u/%u levels=%u size=%ux%u bpp=%u depth=%u/%u blend=%u dither=%u repeat=%u cull=%u clip=%u,%u\n",
            reason,
            s_copy.state.combiner_count,s_copy.state.texture_mask,s_copy.state.untextured,
            s_copy.state.rgba8,s_copy.state.dxt1,s_copy.state.levels,
            s_copy.state.width,s_copy.state.height,
            s_copy.state.target_bpp,s_copy.state.depth_test,s_copy.state.depth_write,s_copy.state.blend,
            s_copy.state.dither,s_copy.state.repeat,s_copy.state.cull_face,
            s_copy.state.clip_x,s_copy.state.clip_y);
    }
#endif
#if defined(__APPLE__) && NV2A_GPU_PATH
    /* RE-TRANSFORM BEFORE THE CPU RASTERISER TOUCHES THIS. s_outputs currently
     * holds the program's inputs; every branch below reads it as screen-space
     * positions. Re-running the interpreter is the whole cost of a rejected
     * GPU-vertex batch, and a rejected batch is rare by construction -- but it
     * is the difference between a correct frame and one whose geometry is
     * drawn from object space with nothing in the log to say so. */
    if (s_vsh_gpu_batch) {
        int ok;
        s_vsh_force_cpu = 1;
        ok = prepare_vertices();
        s_vsh_force_cpu = 0;
        s_vsh_gpu_batch = 0;
        if (!ok) { ++s_vsh.rejected; nv2a_drop(NV2A_DROP_DROPPED, "vertex", "CPU re-transform after a Metal refusal failed", 0); return; }
    }
#endif
    /* The flat CPU raster under a live Metal path draws into guest RAM that the
     * GPU's copy of the surface then covers: the draw is, in effect, gone. */
    if (!s_copy.active && nv2a_gpu_on())
        nv2a_drop(NV2A_DROP_DROPPED, "fragment", "no fragment state: flat CPU raster under Metal", 0);
    ++s_gpu.cpu_batches;
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            raster_indices(i, i+1, i+2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            raster_indices(i+(i&1), i+1-(i&1), i+2);
        break;
    case NV_PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            raster_indices(0, i, i+1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            raster_indices(i, i+1, i+2);
            raster_indices(i, i+2, i+3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            raster_indices(i, i+1, i+3);
            raster_indices(i, i+3, i+2);
        }
        break;
    default:                               /* points and lines: not yet */
        nv2a_drop(NV2A_DROP_DROPPED, "raster", "points and lines (the CPU rasteriser draws none)", s_gpu.prim);
        break;
    }
batch_complete:
#if NV2A_GPU_PATH
    if (s_capture_selected && nv2a_gpu_on()) nv2a_gpu_sync();
#endif
    if (s_copy.active) capture_bytes("after", s_copy.target, s_copy.target_bytes);
    if (s_copy.active && s_copy.depth) capture_bytes("depth-after",s_copy.depth,s_copy.depth_bytes);
    /* Report-time snapshots may interrupt the clear or raster loops. Capture
     * a few completed batches when inspecting the actual rendered result. */
    if (s_gpu.tris_drawn != drawn_before) {
        /* RECOMP_FB_DUMP_DRAW=<stride>[:<after-seconds>]: capture after every
         * stride-th batch that actually drew, so the picture is a composed
         * frame rather than whatever the surface held when a report happened
         * to fire. The first batches are always the same full-screen blit, so
         * a stride is what makes the title's own geometry visible; the default
         * keeps the old behaviour of the first few.
         *
         * THE SECOND FIELD IS WHAT MAKES THIS USABLE AT GAMEPLAY. Without it
         * the count starts at process start, and a gameplay run issues about
         * 720,000 batches -- so a stride large enough to still be capturing at
         * t=120 s puts tens of thousands of draws between captures, which is
         * whole frames apart and answers a different question. With it, the 24
         * slots land INSIDE one gameplay frame, consecutively, which is the
         * only way to see an artefact appear and name the draw that made it.
         *
         * WHAT IT COSTS, and it has to be said next to the number it produces:
         * dump_surface_bmp() calls nv2a_gpu_sync_range(), which on Metal is a
         * full drain and read-back. Capturing every few draws therefore
         * removes the GPU's backlog -- and this artefact is load dependent, so
         * a small stride can hide the very thing being looked for. Read a
         * clean capture set as "not reproduced at this stride", never as
         * "absent". */
        extern double xbox_TraceSeconds(void);
        static unsigned batches, captured;
        static long stride = -1;
        static double draw_after;
        static const char *dump_slot;
        static int dump_on;
        if (stride < 0) {
            const char *env = pb_env_str("RECOMP_FB_DUMP_DRAW", &dump_slot);
            const char *colon = env ? strchr(env, ':') : NULL;
            dump_on = env != NULL;
            stride = env && *env ? strtol(env, NULL, 0) : 0;
            if (stride < 1) stride = 1;
            draw_after = colon ? strtod(colon + 1, NULL) : 0.0;
            if (!(draw_after > 0.0)) draw_after = 0.0;
        }
        if (stride > 0 && dump_on && captured < 24
                && (xbox_TraceSeconds() >= draw_after || s_draw_dump_black_armed)
                && (batches++ % (unsigned long)stride) == 0) {
            fprintf(stderr, "  [DRAW-CAP] %u at batch %u, t=%.2f,"
                    " %u triangles\n",
                    captured, batches - 1, xbox_TraceSeconds(),
                    s_gpu.tris_drawn);
            dump_surface_bmp("draw", captured++);
        }
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    float v[4];
    uint32_t i;

    /* An inline batch whose layout could not be derived arrives here with
     * idx_count == 0 and leaves without drawing and without a trace. That is
     * the delivery mechanism for inline_layout()'s refusal of UB_D3D -- the
     * one format this title declares on every draw -- and it is exactly the
     * silence the `batch_wide` refusal below refuses to have. Count it. */
    if (!s_gpu.prim)
        return;
    if (s_host_skip) ++s_host_seen;
    if (!s_gpu.idx_count) {
        if (s_gpu.inline_count) {
            ++s_gpu.batches_no_layout;
            nv2a_drop_batch();
            nv2a_drop(NV2A_DROP_DROPPED, "vertex", "inline batch with an underivable layout", s_gpu.inline_count);
        }
        return;
    }
    /* An index that did not fit uint16_t means the indices we DO hold are a
     * subset with the gaps closed up, which is a different mesh. Refuse rather
     * than draw wrong topology; counted so the refusal is never silent. */
    if (s_gpu.batch_wide) {
        ++s_gpu.elem32_batches_dropped;
        nv2a_drop_batch();
        nv2a_drop(NV2A_DROP_DROPPED, "vertex", "32-bit index that does not fit 16 bits", s_gpu.idx_count);
        return;
    }
    s_gpu.draws++;
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured.
     * Include every input vertex, not only the first (often the same corner
     * of a full-screen triangle). This is explicitly a pre-shader range.
     *
     * OPT-IN since 13 Sep 2026, and it should have been from the start. This is
     * a SECOND full pass over every vertex of every draw -- its own
     * fetch_vertex per index, on top of the one the vertex pipeline already
     * does -- and it exists solely to print one diagnostic line per report. It
     * ran unconditionally on the hottest path in the renderer for the whole of
     * bring-up. That was the right trade while "does anything have coordinates
     * at all" was an open question; it is not now that the title renders.
     *
     * RECOMP_VERTEX_RANGE=1 brings it back. The report says so when it is off,
     * rather than printing a stale or zeroed range as though it were measured
     * -- an instrument that silently reports nothing is worse than one that is
     * plainly absent. */
    if (pb_vertex_range()) {
        float p[4];
        int nonzero=0;
        for (i=0; i<s_gpu.idx_count; ++i) if (fetch_vertex(0, s_gpu.idx[i], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                nonzero=1;
            }
            if (isfinite(p[0]) && isfinite(p[1])) {
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
        s_gpu.nonzero_draws+=nonzero;
    }

    raster_batch();

    if (pb_verbose()) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

/* The dispatch proper. Split out from nv2a_pb_exec_method so the optional walk
 * timer can bracket it in one place: the body has five early `return`s spread
 * through its switch, and a timer that has to be closed at every exit is a
 * timer that will eventually miss one -- silently, as an understated stage. */
static void pb_exec_method_body(uint32_t subch, uint32_t method, uint32_t param);

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    /* One predicted branch per method when the switch is off. The alternative
     * -- taking the timestamp unconditionally and discarding it -- costs the
     * 21 ns clock read on every method of every run, which is the whole
     * thing this gate exists to avoid. */
    if (!pb_walk_on()) {
        pb_exec_method_body(subch, method, param);
        return;
    }
    {
        unsigned long long _t_walk = pb_now_us();
        pb_exec_method_body(subch, method, param);
        pb_stage_add(PB_STAGE_WALK, _t_walk);
    }
}

/* NOTE ON WHAT `walk` DOES AND DOES NOT CONTAIN. It brackets the executor's
 * handling of one method, which is where the method dispatch lives. It does
 * NOT contain nv2a_pb_scan's own decode loop -- the header parse, the count
 * and subchannel extraction, the walk from word to word -- because that lives
 * in another file and this one owns no boundary around it. The stages it DOES
 * nest are already counted elsewhere: a draw-triggering method runs vsh,
 * prepare and submit inside this bracket, so `walk` overlaps them and the
 * arithmetic that makes `rest` a residual breaks while this switch is on.
 * pb_stage_line says so on the line itself rather than trusting anyone to
 * remember. */
static void pb_exec_method_body(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_vsh.dirty = 1;
        s_vsh_trace.enabled = getenv("RECOMP_VSH_TRACE") != NULL;
        for (int i = 0; i < 16; ++i) s_vsh.current[i][3] = 1.0f;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (pb_verbose()) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (s_vsh_trace.enabled && subch < 8
            && method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_CONSTANT)
        ++s_vsh_trace.program_words[subch];
    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        /* What else the title is driving, and on which channel.
         *
         * The executor renders into the surface the 3D class names and the
         * CRTC scans out a different address that nothing ever writes, so
         * something has to be moving pixels between them -- and the obvious
         * candidate is a second class, image blit or surface-to-memory, on
         * another subchannel. Everything here is dropped, so a whole engine
         * could be in use and look identical to silence. Count it. */
        {
            static unsigned long per_subch[8];
            static unsigned long total;
            if (subch < 8) ++per_subch[subch];
            if ((++total % 200000ul) == 1ul) {
                unsigned k;
                fprintf(stderr, "  [GPU] non-3D subchannel methods:");
                for (k = 1; k < 8; ++k)
                    if (per_subch[k])
                        fprintf(stderr, " subch%u=%lu", k, per_subch[k]);
                fprintf(stderr, " (latest method 0x%04X param 0x%08X on"
                        " subch %u)\n", method, param, subch);
                fflush(stderr);
            }
        }
        note_unhandled(method);
        return;
    }
    if (method < 0x2000 && !(method & 3)) {
        s_methods[method/4] = param;
        s_method_seen[method/4] = 1;
        if (method == NV097_SET_BLEND_ENABLE || method == NV097_SET_BLEND_FUNC_SFACTOR
                || method == NV097_SET_BLEND_FUNC_DFACTOR
                || method == NV097_SET_BLEND_EQUATION)
            note_blend_write();
        /* The clear value, timestamped, so a blend combination can be placed
         * against a card rather than against a draw number. The title issues
         * only a handful, and the sequence of them is a readable map of the
         * opening -- the only thing that says whether a fade belongs to the
         * cards or to what follows.
         *
         * Distinct values, not transitions: the title alternates two values
         * that differ only in bits a 16-bit surface discards, hundreds of
         * times a second, and logging every change buries the run in the
         * first half second. */
        if (method == NV097_SET_COLOR_CLEAR_VALUE)
            note_clear_value(param);
        if ((method >= 0x0a60 && method < 0x0a80)
                || (method >= 0x0a80 && method < 0x0aa0))
            note_factor_write(method, param);
    }
    if (s_vsh_trace.enabled) {
        unsigned i = (unsigned)(s_vsh_trace.recent_count++ % 2048);
        s_vsh_trace.recent[i].method = method;
        s_vsh_trace.recent[i].param = param;
    }
    if ((method >= NV097_SET_TRANSFORM_PROGRAM && method < 0x0C00)
            || (method >= NV097_SET_TRANSFORM_EXECUTION_MODE && method <= NV097_SET_TRANSFORM_CONSTANT_LOAD))
        note_program_write(method, param);
    if (method >= NV097_SET_TRANSFORM_PROGRAM && method < NV097_SET_TRANSFORM_CONSTANT) {
        unsigned component = (method & 15) / 4;
        if (s_vsh.load < NV2A_VS_MAX_INSTRUCTIONS) {
            if (s_vsh.words[s_vsh.load][component] != param || !(s_vsh.loaded[s_vsh.load] & (1u << component))) {
                s_vsh.dirty = 1;
                if (s_vsh_trace.enabled) ++s_vsh_trace.changed;
            }
            s_vsh.words[s_vsh.load][component] = param;
            s_vsh.loaded[s_vsh.load] |= (uint8_t)(1u << component);
        } else if (s_vsh_trace.enabled) ++s_vsh_trace.dropped;
        if (component == 3 && s_vsh.load < NV2A_VS_MAX_INSTRUCTIONS) s_vsh.load++;
        return;
    }
    if (method >= NV097_SET_TRANSFORM_CONSTANT && method < 0x0C00) {
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.constants;
            if (s_vsh.constant_load >= NV2A_VS_MAX_CONSTANTS) ++s_vsh_trace.constant_dropped;
        }
        unsigned component = (method & 15) / 4;
        if (s_vsh.constant_load < NV2A_VS_MAX_CONSTANTS)
            memcpy(&s_vsh.constants[s_vsh.constant_load][component], &param, sizeof(float));
        if (component == 3 && s_vsh.constant_load < NV2A_VS_MAX_CONSTANTS) s_vsh.constant_load++;
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4F_M && method < NV097_SET_VERTEX_DATA4F_M + 16*16) {
        unsigned word = (method - NV097_SET_VERTEX_DATA4F_M) / 4;
        memcpy(&s_vsh.current[word / 4][word % 4], &param, sizeof(float));
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.current4f;
            s_vsh_trace.current_written[word/4] |= (uint8_t)(1u << (word%4));
        }
        return;
    }
    if (method >= NV097_SET_VERTEX_DATA4UB && method < NV097_SET_VERTEX_DATA4UB + 16*4) {
        unsigned a = (method - NV097_SET_VERTEX_DATA4UB) / 4;
        for (int k = 0; k < 4; ++k) s_vsh.current[a][k] = ((param >> (8*k)) & 255) / 255.0f;
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.current4ub;
            s_vsh_trace.current_written[a] = 15;
        }
        return;
    }
    switch (method) {
    /* THE GPU'S FENCE WRITE-BACK -- the one method in this stream that tells
     * the guest work is FINISHED.
     *
     * D3D_SetFence (0x00191390) emits this carrying the pre-increment value of
     * [dev+0x30]; the guest's D3D_BlockOnTime spins on *(*(dev+0x34)) until it
     * passes. This executor did not decode 0x1D70 at all, so the word was left
     * to the pump, which published [dev+0x30] verbatim -- a value the hardware
     * can never deposit, and one that makes the wait's unsigned compare
     * succeed for every target. Every D3DVertexBuffer_Lock, every
     * D3D_BlockOnResource, returned instantly.
     *
     * Writing it HERE is what makes it honest: this runs on the pusher thread,
     * in stream order, after the preceding SET_BEGIN_END END dispatch has
     * already run raster_batch() and copied that draw's vertices out of guest
     * RAM. So the guest is released exactly when the data is safe, which is
     * what the hardware semaphore means.
     *
     * The other sink handles this too (nv2a_pgraph_d3d11.c), but through
     * SET_SEMAPHORE_OFFSET, which this title sets to 0 -- it puts the base in
     * the semaphore DMA object, which nothing here resolves. That path logs
     * "semaphore offset 0x00000000 not usable" and signals nothing. Going via
     * [dev+0x34] needs no DMA object: it is the address the guest itself
     * dereferences. */
    case NV097_BACK_END_WRITE_SEMAPHORE_RELEASE:
        d3d8_ring_fence_release(param);
        break;
    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        if (s_vsh_trace.enabled) ++s_vsh_trace.modes;
        s_vsh.mode = param & 3; break;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.loads;
            if (param < 32) s_vsh_trace.load_values |= 1u << param;
            else s_vsh_trace.load_high++;
        }
        s_vsh.load = param; break;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        if (s_vsh_trace.enabled) {
            ++s_vsh_trace.starts;
            /* A final value of 0 does not mean the title never selected
             * another program; it means the last selection was 0. Record
             * every distinct one. */
            if (param < 32) s_vsh_trace.start_values |= 1u << param;
        }
        if (s_vsh.start != param) s_vsh.dirty = 1;
        s_vsh.start = param; break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD: s_vsh.constant_load = param; break;
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        note_surface(param);
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    /* The title's own frame boundary: this frame is finished. */
    case NV097_FLIP_STALL:
        /* Paced BEFORE the frame interval is taken, so [FRAME-WIN] measures
         * what the title actually gets -- see flip_pace. */
        flip_pace();
        /* First, so the interval covers the whole frame -- see frame_stats_flip. */
        frame_stats_flip();
        /* The FF watch keys on the draw's place in the frame, so the count
         * restarts here. See the note on g_ff_watch. */
        g_ff_watch_ordinal = 0;
#if defined(__APPLE__)
        g_mtl_frames++;   /* command buffers per frame needs a frame */
        nv2a_metal_frame_bench_flip();
#endif
#ifdef nv2a_gpu_surface_report
        /* BEFORE the snapshot, because the snapshot syncs: the question is
         * what the GPU still owes guest RAM at the moment the guest calls the
         * frame finished, and a sync answers it by destroying it. */
        static int flip_trace_on = -1;   /* cached: this runs once per flip */
        if (pb_env_on("RECOMP_FLIP_TRACE", &flip_trace_on) && nv2a_gpu_on()) {
            fprintf(stderr, "  [FLIPTRACE] pre-sync:\n");
            nv2a_gpu_surface_report();
        }
        if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
            fprintf(stderr, "  [EV] FLIP   bound=%08X\n", s_gpu.color_offset);
#endif
        surface_census(s_gpu.color_offset);
        surface_audit(1);
        snapshot_surface();
        fb_watch();
        flip_trace();
        /* G51.1: the host's 2D shadow compares its frame's draws here.
         * NULL unless RECOMP_D3D8_HOST_2D armed it (main.c). */
        if (s_flip_hook) s_flip_hook();
        nv2a_drop_flip();
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            if (s_gpu.idx_wanted > s_gpu.idx_wanted_max)
                s_gpu.idx_wanted_max = s_gpu.idx_wanted;
            s_gpu.idx_wanted = 0;
            if (s_gpu.inline_wanted > s_gpu.inline_wanted_max)
                s_gpu.inline_wanted_max = s_gpu.inline_wanted;
            s_gpu.inline_wanted = 0;
            s_gpu.batch_wide = 0;
            s_gpu.prim = param;
            {   uint32_t xm = s_methods[0x1E94u / 4u];
                ++s_exec_mode_batches[xm == 4u ? 0 : xm == 6u ? 1 : 2]; }
            s_gpu.idx_count = 0;
            s_gpu.inline_count = 0;
        } else {
            /* An inline batch names its vertices 0..n-1, so the topology code
             * below is the same one the indexed path uses. */
            if (s_gpu.inline_count) {
                inline_layout();
                if (pb_verbose()) {
                    static int shown_inline;
                    if (shown_inline++ < 40) {
                        uint32_t a, k;
                        fprintf(stderr, "  [GPU] inline batch: prim %u, %u dwords,"
                                " stride %u dw\n", s_gpu.prim,
                                s_gpu.inline_count, s_inline_stride);
                        fprintf(stderr, "  [GPU]   viewport seen=%d"
                                " scale %.3f %.3f %.3f %.3f"
                                " offset %.3f %.3f %.3f %.3f\n",
                                s_gpu.vp_seen,
                                s_gpu.vp_scale[0], s_gpu.vp_scale[1],
                                s_gpu.vp_scale[2], s_gpu.vp_scale[3],
                                s_gpu.vp_offset[0], s_gpu.vp_offset[1],
                                s_gpu.vp_offset[2], s_gpu.vp_offset[3]);
                        for (a = 0; a < NV_VERTEX_ATTRS; a++)
                            if (s_gpu.attr[a].size)
                                fprintf(stderr, "  [GPU]   attr%-2u type %u size %u"
                                        " -> dw off %u\n", a, s_gpu.attr[a].type,
                                        s_gpu.attr[a].size, s_inline_off[a]);
                        fprintf(stderr, "  [GPU]   words:");
                        for (k = 0; k < s_gpu.inline_count && k < 24; k++)
                            fprintf(stderr, " %08X", s_gpu.inline_words[k]);
                        fprintf(stderr, "\n");
                    }
                }
                if (s_inline_stride) {
                    uint32_t nv = s_gpu.inline_count / s_inline_stride, k;
                    if (nv > NV_MAX_INDICES)
                        nv = NV_MAX_INDICES;
                    for (k = 0; k < nv; k++)
                        s_gpu.idx[k] = (uint16_t)k;
                    s_gpu.idx_count = nv;
                }
            }
            draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;    /* after the draw: fetch_vertex reads it */
        }
        break;

    case NV097_INLINE_ARRAY:
        /* THE SAME DEFECT THAT WAS THE FENCE, ON THE PATH THAT WAS NOT FIXED.
         * ARRAY_ELEMENT16 used to stop storing at the cap and draw the batch
         * anyway, tail missing, reported by nothing; that was measured and the
         * cap was raised. This line still does it. A truncated inline_count
         * divides by the stride into a SMALLER BUT STILL VALID vertex count,
         * so the batch draws a partial mesh -- no rejection, no fallback, and
         * no term in `no-room`, which splits e16/e32 only and is therefore
         * blind here. Count the dropped words before trusting no-room=0. */
        if (s_gpu.prim) {
            if (s_gpu.inline_count < NV_MAX_INLINE_WORDS)
                s_gpu.inline_words[s_gpu.inline_count++] = param;
            else
                ++s_gpu.inline_overflow;
            ++s_gpu.inline_wanted;
        }
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;
        /* This case arrived with the v0.11.0 merge (be74a7c, 13:53 on 20 Sep
         * 2026) and it changes what gets drawn: before it, a BEGIN_END whose
         * geometry came as an implicit run ended with idx_count == 0 and was
         * dropped in silence. Geometry that was invisible yesterday is drawn
         * today, which is a picture change nothing measured at the time.
         *
         * RECOMP_PB_DRAW_ARRAYS=0 restores the old behaviour, so the merge's
         * effect on a picture is an A/B inside ONE binary rather than two
         * builds that differ in every other way as well. The banner fires once
         * either way: if this title never submits the method, both arms are
         * the same run and the comparison must not be reported as a result. */
        static int suppressed = -1;
        static int announced;
        /* Default ON through the shared helper rather than a hand-rolled
         * read. Same meaning -- unset draws, "0" drops -- and it differs only
         * for a malformed value like "0abc", which the hand-rolled form read
         * as OFF and the stated grammar reads as ON. The grammar is the point
         * of the ratchet. */
        if (suppressed < 0)
            suppressed = !recomp_switch_on_default("RECOMP_PB_DRAW_ARRAYS", 1);
        if (!announced) {
            announced = 1;
            fprintf(stderr, "[PB-DRAW-ARRAYS] the guest submits DRAW_ARRAYS;"
                    " this build %s it (RECOMP_PB_DRAW_ARRAYS=%s)\n",
                    suppressed ? "DROPS" : "draws", suppressed ? "0" : "1");
        }
        if (suppressed || !s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim) {
            if (s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
            } else {
                s_gpu.idx_overflow += 2;
                s_gpu.idx_overflow_e16 += 2;
            }
            s_gpu.idx_wanted += 2;
        }
        break;

    /* ONE 32-BIT INDEX PER PARAMETER WORD, AND IT WAS NOT DECODED UNTIL NOW.
     *
     * 0x1808 fell through to default:, which recognises only the two
     * vertex-array register ranges, so it landed in the unhandled-method
     * histogram -- 164,101 times in one gameplay run of measure/methodaudit,
     * with 0x1800 and 0x1818 absent from the same table as the control.
     *
     * WHAT THAT COST. A batch whose indices arrive this way reaches
     * SET_BEGIN_END(0) with idx_count == 0, and draw_primitive returns before
     * it increments s_gpu.draws. So the batch is not drawn, not counted as a
     * draw, not counted as a vsh reject and not counted as a texture reject --
     * invisible to every instrument in this file. Worse than dropping: a batch
     * that mixes ELEMENT16 with ELEMENT32 kept only the 16-bit half, so the
     * TOPOLOGY was wrong and it was drawn anyway.
     *
     * A contributing cause is worth recording: nv2a_pb_scan.c's method-name
     * table labels 0x1808 "INLINE_ARRAY", which is 0x1818 -- so the one survey
     * that answers "which methods does this title use" named this method after
     * a method that was already handled.
     *
     * DRAW_ARRAYS (0x1810) is the other undecoded submission method and is
     * deliberately NOT added here. Decoding it black-screened the title, and
     * that is a separate mechanism with its own write-up; see
     * docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-16_THE_BLACK_SCREEN_IS_DRAW_ARRAYS.md.
     * This method is measured never to arrive during boot (elem32=0/0 in every
     * black run), which is why it can go in on its own. */
    case NV097_ARRAY_ELEMENT32:
        ++s_gpu.elem32_seen;
        if (s_gpu.prim) {
            if (param > 0xFFFFu) {
                /* idx[] is uint16_t because the hardware's own 16-bit path is
                 * the common one. An index that does not fit is NOT skipped:
                 * dropping one index of a triangle list shifts every vertex
                 * after it, so the batch would draw with silently wrong
                 * topology. Refuse the whole batch instead -- which is what
                 * happens today anyway, since today it is never assembled --
                 * and count it, so the next person knows whether widening
                 * idx[] to uint32_t is work worth doing. */
                ++s_gpu.elem32_wide;
                s_gpu.batch_wide = 1;
            } else if (s_gpu.idx_count < NV_MAX_INDICES) {
                s_gpu.idx[s_gpu.idx_count++] = (uint16_t)param;
                ++s_gpu.elem32_indices;
            } else {
                ++s_gpu.idx_overflow;
                ++s_gpu.idx_overflow_e32;
            }
            ++s_gpu.idx_wanted;
        }
        break;
    default:
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset = param;
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (method >= NV097_SET_VIEWPORT_OFFSET
                && method < NV097_SET_VIEWPORT_OFFSET + 16) {
            unsigned k = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
            memcpy(&s_gpu.vp_offset[k], &param, sizeof(float));
            memcpy(&s_vsh.constants[NV_IGRAPH_XF_XFCTX_VPOFF][k],
                   &param, sizeof(float));
            s_gpu.vp_seen = 1;
        } else if (method >= NV097_SET_VIEWPORT_SCALE
                && method < NV097_SET_VIEWPORT_SCALE + 16) {
            unsigned k = (method - NV097_SET_VIEWPORT_SCALE) / 4;
            memcpy(&s_gpu.vp_scale[k], &param, sizeof(float));
            memcpy(&s_vsh.constants[NV_IGRAPH_XF_XFCTX_VPSCL][k],
                   &param, sizeof(float));
            s_gpu.vp_seen = 1;
        } else {
            note_unhandled(method);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

/* Read one vertex-program constant.
 *
 * Exists for the regression that covers the viewport mirror below: the fault
 * was invisible from outside because the constant file is private, and the
 * only symptom was geometry that transformed to a point. */
int nv2a_pb_exec_vsh_constant(unsigned index, float out[4])
{
    if (index >= NV2A_VS_MAX_CONSTANTS || !out) return 0;
    memcpy(out, s_vsh.constants[index], sizeof(float) * 4);
    return 1;
}

/* The address the executor is actually rendering into, or 0 before the title
 * has named one. The framebuffer probe needs it because PCRTC_START is not a
 * substitute: a title that never programs a scanout leaves it holding whatever
 * was there, and this one leaves it pointing at a heap block. */
/* Capture the surface at a caller-chosen moment. The report-time and
 * per-batch captures both sample a frame part way through being composed;
 * only the flip is a moment when one is finished. */
void nv2a_pb_exec_dump_surface(void)
{
    static unsigned live_seq, snap_seq;
    dump_surface_bmp("flip", live_seq++);
    dump_snapshot_bmp("snap", snap_seq++);
}

/* Triangles rasterised so far. The delta between two flips is what says
 * whether a blank presented frame is one the title drew nothing into, or one
 * whose work was cleared away before it reached the screen. */
uint32_t nv2a_pb_exec_triangles(void)
{
    return s_gpu.tris_drawn;
}

/* Non-black pixels in the frame that was last presented, or -1 before the
 * first flip. The surface address alone cannot answer "is there a picture on
 * screen": the probe samples between flips, when color_offset names the buffer
 * being composed rather than the one being shown. */
int nv2a_pb_exec_snapshot_nonzero(void)
{
    const FramePoolSlot *f = frame_pool_acquire(&s_snap_pool);
    int n = (f && f->px && f->w && f->h) ? (int)snapshot_nonzero_of(f) : -1;
    frame_pool_release(&s_snap_pool, f);
    return n;
}

uint32_t nv2a_pb_exec_surface_va(void)
{
    return s_gpu.color_offset;
}

void nv2a_pb_exec_report(void)
{
    clear_colour_report();
    raster_state_report();
    /* THE FLIP READBACK. Printed in BOTH states, unconditionally, so a control
     * run sizes the A/B and ab_score.py's METAL_SWITCH_RE can see the token
     * and actually run its identical-arms VOID check. `taken` is one drain per
     * flip, and [STAGE] sync is the same call timed. */
    fprintf(stderr, "[FLIP-SYNC] flip read-backs: %llu taken, %llu skipped"
            " (no_flip_sync %s).  Each one waits for the GPU and copies the"
            " whole colour surface to guest RAM; the presenter then uploads it"
            " back. With it OFF, taken IS what the other arm would skip.\n",
            g_flip_syncs_taken, g_flip_syncs_skipped,
            no_flip_sync_on() ? "on" : "OFF");
    /* AND WHERE THE COST OF THAT SENTENCE IS ACTUALLY MEASURED, because the
     * count above is not a cost and has been read as one. Four numbers, four
     * different lines, and only the sum of them is what direct GPU
     * presentation would be traded against:
     *
     *   drain     [METAL] ... ms draining the GPU      stays either way
     *   readback  [METAL] ... ms reading back          removed
     *   snap      [STAGE] snap=                        removed
     *   convert   [PRESENT] convert/upload             removed
     *
     * no_flip_sync=1 is NOT that trade. It removes the drain and the readback
     * and leaves the presenter reading whatever guest RAM holds, which is the
     * previous frame or half of one -- its own comment at no_flip_sync_on()
     * says so. It is an upper bound on the saving, not a fix. */
    /* AND WHETHER THE PRESENTED FRAME IS THE ONE THAT WAS JUST DRAWN. A
     * publish with no free slot keeps the PREVIOUS frame on screen, which
     * looks exactly like a title that drew nothing -- the reading this file
     * has already had to retire once. Zero is the expected value and it is
     * also the positive control for `publishes`. */
    {
        unsigned long pub = s_snap_pool.publishes, drp = s_snap_pool.dropped;
        if (pub || drp)
            fprintf(stderr, "[FLIP-SNAP] %lu frames published, %lu DROPPED"
                    " for want of a free slot%s (%d slots)\n", pub, drp,
                    drp ? "   <-- the presenter showed the previous frame"
                        : "", FRAME_POOL_SLOTS);
    }
    fprintf(stderr, "[FLIP-SYNC]   cost of it: [METAL] drain + readback,"
                    " [STAGE] snap, [PRESENT] convert + upload.  Four lines,"
                    " and no_flip_sync=1 is an upper bound on the saving,"
                    " not a candidate fix -- it presents stale guest RAM.\n");
    if (s_vsh_trace.enabled) {
        fprintf(stderr, "[VSH-TRACE] upload words by subchannel:");
        for (int i = 0; i < 8; ++i)
            fprintf(stderr, " %d=%llu", i, (unsigned long long)s_vsh_trace.program_words[i]);
        fprintf(stderr, " changed=%llu dropped=%llu loads=%llu starts=%llu modes=%llu constants=%llu constant-dropped=%llu current4f=%llu current4ub=%llu decodes=%llu unique-hashes=%u load=%u constant-load=%u\n",
                (unsigned long long)s_vsh_trace.changed, (unsigned long long)s_vsh_trace.dropped,
                (unsigned long long)s_vsh_trace.loads, (unsigned long long)s_vsh_trace.starts,
                (unsigned long long)s_vsh_trace.modes, (unsigned long long)s_vsh_trace.constants,
                (unsigned long long)s_vsh_trace.constant_dropped, (unsigned long long)s_vsh_trace.current4f,
                (unsigned long long)s_vsh_trace.current4ub, (unsigned long long)s_vsh_trace.decodes,
                s_vsh_trace.unique, s_vsh.load, s_vsh.constant_load);
        /* Which program slots the title ever selected. A final start of 0 is
         * only the last selection; the bitmaps say whether there was ever
         * another one to miss. */
        /* The constant file itself.
         *
         * "constants=11,076,944 written, none dropped" says they arrive; it
         * does not say what is in them. JSRF's 3D geometry transforms to
         * oPos=(0 0 0 W) with a plausible W, which is what a zero position row
         * dotted against a good W row looks like -- so print the rows rather
         * than reasoning about the counter. Delivery matches xemu
         * (pgraph.c: slot%4 component, CONST_LD_PTR auto-increment), so if the
         * matrix is present and correct the fault is downstream of here. */
        {
            /* The decoded program, so the constants above can be read
             * against the instructions that consume them. oPos is output 0
             * and R12 aliases it, so a write to either is a position write. */
            static const char *macn[] = {"nop","mov","mul","add","mad","dp3",
                "dph","dp4","dst","min","max","slt","sge","arl"};
            static const char *ilun[] = {"nop","mov","rcp","rcc","rsq","exp",
                "log","lit"};
            static const char *rt[] = {"temp","input","const"};
            fprintf(stderr, "[VSH-PROG] length=%d valid=%d final=%d\n",
                    s_vsh.decoded.length, s_vsh.decoded.valid,
                    s_vsh.decoded.has_final);
            for (int i = 0; i < s_vsh.decoded.length; ++i) {
                const NV2AVshInstruction *q = &s_vsh.decoded.insns[i];
                fprintf(stderr, "[VSH-PROG] %2d %-3s", i,
                        q->mac_op < 14 ? macn[q->mac_op] : "?");
                fprintf(stderr, " dst(temp=%d out=%d wm=%X om=%X)",
                        q->mac_dst.temp_reg, (int)q->mac_dst.output_reg,
                        q->mac_dst.write_mask, q->mac_dst.output_mask);
                for (int j = 0; j < 3; ++j)
                    fprintf(stderr, " %s%d", rt[q->mac_src[j].reg_type & 3],
                            q->mac_src[j].reg_index);
                fprintf(stderr, " | %-3s dst(temp=%d out=%d wm=%X om=%X) %s%d\n",
                        q->ilu_op < 8 ? ilun[q->ilu_op] : "?",
                        q->ilu_dst.temp_reg, (int)q->ilu_dst.output_reg,
                        q->ilu_dst.write_mask, q->ilu_dst.output_mask,
                        rt[q->ilu_src.reg_type & 3], q->ilu_src.reg_index);
            }
            unsigned nonzero = 0;
            for (unsigned i = 0; i < NV2A_VS_MAX_CONSTANTS; ++i)
                for (unsigned k = 0; k < 4; ++k)
                    if (s_vsh.constants[i][k] != 0.0f) { ++nonzero; break; }
            fprintf(stderr, "[VSH-CONST] %u of %u constants non-zero\n",
                    nonzero, (unsigned)NV2A_VS_MAX_CONSTANTS);
            for (unsigned i = 0; i < NV2A_VS_MAX_CONSTANTS; ++i) {
                if (!s_vsh.constants[i][0] && !s_vsh.constants[i][1]
                        && !s_vsh.constants[i][2] && !s_vsh.constants[i][3])
                    continue;
                fprintf(stderr, "[VSH-CONST] c[%u] = %g %g %g %g\n", i,
                        s_vsh.constants[i][0], s_vsh.constants[i][1],
                        s_vsh.constants[i][2], s_vsh.constants[i][3]);
            }
        }
        fprintf(stderr, "[VSH-TRACE] distinct START slots 0x%08X, LOAD slots"
                        " 0x%08X, LOAD >= 32: %llu\n",
                s_vsh_trace.start_values, s_vsh_trace.load_values,
                (unsigned long long)s_vsh_trace.load_high);
    }
    if (s_blend_trace_writes) {
        fprintf(stderr, "[BLEND-WRITE] writes=%llu distinct=%u overflow=%llu\n",
                (unsigned long long)s_blend_trace_writes, s_blend_trace_count,
                (unsigned long long)s_blend_trace_overflow);
        for (unsigned i = 0; i < s_blend_trace_count; ++i)
            fprintf(stderr, "[BLEND-WRITE]   enable=%u src=0x%X dst=0x%X eq=0x%X"
                    "  hits=%llu draws %u..%u  t=%.2f..%.2f"
                    " (clear=0x%08X format=0x%X at first)\n",
                    s_blend_trace[i].en, s_blend_trace[i].src, s_blend_trace[i].dst,
                    s_blend_trace[i].eq, (unsigned long long)s_blend_trace[i].hits,
                    s_blend_trace[i].first_draw, s_blend_trace[i].last_draw,
                    s_blend_trace[i].first_t, s_blend_trace[i].last_t,
                    s_blend_trace[i].clear_at_first, s_blend_trace[i].format_at_first);
        fprintf(stderr, "[FACTOR] writes=%llu distinct=%u overflow=%llu\n",
                (unsigned long long)s_factor_trace_writes, s_factor_trace_count,
                (unsigned long long)s_factor_trace_overflow);
        for (unsigned i = 0; i < s_factor_trace_count; ++i)
            fprintf(stderr, "[FACTOR]   0x%04X = 0x%08X hits=%llu draws %u..%u"
                    " t=%.2f..%.2f\n", s_factor_trace[i].method,
                    s_factor_trace[i].value,
                    (unsigned long long)s_factor_trace[i].hits,
                    s_factor_trace[i].first_draw, s_factor_trace[i].last_draw,
                    s_factor_trace[i].first_t, s_factor_trace[i].last_t);
        fprintf(stderr, "[CLEAR-SEQ] distinct=%u overflow=%llu\n",
                s_clear_trace_count, (unsigned long long)s_clear_trace_overflow);
        for (unsigned i = 0; i < s_clear_trace_count; ++i)
            fprintf(stderr, "[CLEAR-SEQ]   0x%08X format=0x%X hits=%llu"
                    " draws %u..%u t=%.2f..%.2f\n",
                    s_clear_trace[i].value, s_clear_trace[i].format,
                    (unsigned long long)s_clear_trace[i].hits,
                    s_clear_trace[i].first_draw, s_clear_trace[i].last_draw,
                    s_clear_trace[i].first_t, s_clear_trace[i].last_t);
        fprintf(stderr, "[ALPHA] blend-enabled batches=%llu distinct-levels=%u"
                " overflow=%llu\n", (unsigned long long)s_alpha_trace_batches,
                s_alpha_trace_count, (unsigned long long)s_alpha_trace_overflow);
        fprintf(stderr, "[ALPHA-IN] distinct input levels=%u overflow=%llu\n",
                s_alpha_in_count, (unsigned long long)s_alpha_in_overflow);
        for (unsigned i = 0; i < s_alpha_in_count; ++i)
            fprintf(stderr, "[ALPHA-IN]   level=%3u source=%s hits=%llu\n",
                    s_alpha_in[i].level,
                    s_alpha_in[i].from_array ? "array " : "immed ",
                    (unsigned long long)s_alpha_in[i].hits);
        for (unsigned i = 0; i < s_alpha_trace_count; ++i)
            fprintf(stderr, "[ALPHA]   level=%3u hits=%llu draws %u..%u"
                    " t=%.2f..%.2f\n", s_alpha_trace[i].level,
                    (unsigned long long)s_alpha_trace[i].hits,
                    s_alpha_trace[i].first_draw, s_alpha_trace[i].last_draw,
                    s_alpha_trace[i].first_t, s_alpha_trace[i].last_t);
        fprintf(stderr, "[BLEND-FADE] batches under DST_COLOR=%llu short=%llu"
                " vsh-rejected=%llu prepare-rejected=%llu rasterised=%llu\n",
                (unsigned long long)s_blend_fade_fate.batches,
                (unsigned long long)s_blend_fade_fate.short_idx,
                (unsigned long long)s_blend_fade_fate.vsh_rejected,
                (unsigned long long)s_blend_fade_fate.prepare_rejected,
                (unsigned long long)s_blend_fade_fate.rasterised);
    }
    /* G26: DOES THE GUEST AGREE WITH US ABOUT WHERE IT IS DRAWING?
     *
     * During the Load-screen black, 28 batches a frame are accepted,
     * submitted and rasterised and put no non-zero pixel on the presented
     * surface. Either they are not landing on the surface we present
     * (COVERAGE) or they land on it and write zero (SHADING). This line
     * decides that, and it needs no forced fragment colour, no timing window
     * and no player on the right screen at the right second: it is passive,
     * it runs every report, and it CANNOT FAIL TO REACH ITS OWN CONDITION --
     * which is exactly what the white test could not promise, having twice
     * been driven by a menu that its own force had painted white.
     *
     * The comparison is BYTE FOR BYTE, and that is not luck. The guest emits
     * pSurface->Data verbatim as the payload of NV097_SET_SURFACE_COLOR_OFFSET
     * and we latch that payload just as verbatim into s_gpu.color_offset, so
     * no unit or mask stands between the two numbers. See d3d8_ring.h for the
     * disassembly that establishes it.
     *
     * The 0x80000000 alias gets its own verdict rather than being masked
     * away. A contiguous allocation's physical P is visible to the CPU at
     * 0x80000000 + P, "the same surface through the other window" is a
     * distinction this renderer has already been wrong about, and collapsing
     * it here would hide precisely the kind of near-miss that is worth
     * knowing about. */
    {
        D3D8RingTarget t;
        if (!d3d8_ring_read_target(&t)) {
            fprintf(stderr, "[RT] NO VERDICT -- D3D_g_pDevice is still empty;"
                    " the title has not created its device yet\n");
        } else if (!t.trusted) {
            /* The ring self-check failed, so the device pointer is not a
             * device. Reporting a divergence off this would be reporting the
             * probe's own bug as the renderer's. */
            fprintf(stderr, "[RT] NO VERDICT -- 0x%08X does not look like a"
                    " device: put=0x%08X ring=[0x%08X,0x%08X]."
                    " FIX THE PROBE, NOT THE RENDERER.\n",
                    t.device, t.put, t.ring_lo, t.ring_hi);
        } else {
            /* WHETHER WE HAVE EVER BOUND IT, not whether we are on it right
             * now. This distinction was measured before it was needed: a
             * 45 s attract-loop boot on 21 Sep read MATCH 43 times and
             * DIVERGE once, on a screen that was visibly rendering the whole
             * time. Nothing was wrong. s_gpu.color_offset legitimately moves
             * several times a frame -- this title uses three surfaces and
             * rotates them -- and the report timer samples it at an arbitrary
             * instant, so ONE LINE IS NOT A VERDICT.
             *
             * s_surfaces is every distinct colour offset the parser has
             * latched. If the guest's target is not in it, we have never
             * pointed at that surface at all, and no amount of sampling luck
             * explains that away. THAT is the coverage test. */
            const char *verdict;
            int bound_ever = 0;
            unsigned i;
            for (i = 0; i < s_surface_count; ++i)
                if (s_surfaces[i] == t.rt_data) { bound_ever = 1; break; }

            if (!bound_ever && (t.rt_data & 0x7FFFFFFFu)
                                  == (s_gpu.color_offset & 0x7FFFFFFFu))
                verdict = "ALIAS -- the same surface through the other window;"
                          " we differ from the guest only in bit 31";
            else if (!bound_ever)
                verdict = "COVERAGE -- the guest's target has NEVER been a"
                          " surface we parsed; the batches cannot be landing"
                          " where we present";
            else if (t.rt_data == s_gpu.color_offset
                        || t.rt_data == s_snap_offset)
                verdict = "MATCH -- we present what the guest targets, so a"
                          " black frame here is SHADING";
            else
                verdict = "sampled off-target -- bound at some point, on"
                          " another of this title's surfaces at this instant."
                          " NOT A VERDICT: read the tally";
            fprintf(stderr, "[RT] guest target=0x%08X data=0x%08X |"
                    " live=0x%08X presented=0x%08X | bound-ever=%s | %s\n",
                    t.rt_surface, t.rt_data, s_gpu.color_offset, s_snap_offset,
                    bound_ever ? "yes" : "NO", verdict);
            fprintf(stderr, "[RT]   depth=0x%08X data=0x%08X  backbuffer0="
                    "0x%08X data=0x%08X  device=0x%08X, ring checked\n",
                    t.depth_surface, t.depth_data,
                    t.back0_surface, t.back0_data, t.device);
        }
    }
    /* ARM THE FRAGMENT FORCE OFF THE BLACK ITSELF.
     *
     * The white test has twice been worth nothing because it depends on a
     * person reaching the screen under test before a second chosen in
     * advance -- 30 s in one run, 50 s in the next. The presented frame is
     * all-zero for 33-35 s on the Load screen and is never all-zero on a
     * menu, so the defect is a better trigger than any clock: it cannot fire
     * early and it cannot be missed.
     *
     * Counted on the report timer rather than per flip because
     * snapshot_nonzero() walks the whole 640x480 frame, and because [SNAP]
     * already prints the number this reads, so the log shows the arm's own
     * input beside its decision. */
    {
        /* BLACK ALONE IS NOT THE CONDITION, AND THAT WAS MEASURED THE HARD
         * WAY. The first version armed on N consecutive all-zero reports and
         * fired at t=18 in a player-driven run, long before the Load screen:
         * a person who has not pressed START yet sits through black
         * transitions, and this run had five reports of them. Across two
         * clean runs the boot's own black stretches are 1, 1, 1 and 2 reports
         * -- but that is how long they happen to be when somebody is driving
         * briskly, not a bound on anything.
         *
         * THE LOAD SCREEN IS NOT "BLACK". IT IS BLACK WHILE THE GUEST IS
         * STILL SUBMITTING A WHOLE SCENE -- 28 draw calls a frame, every
         * frame, which is the evening handover's own signature for it, and
         * the thing that tells it apart from the other black screen, where
         * the guest had stopped submitting and the rate was 1.0. A boot
         * transition that draws nothing can no longer arm this.
         *
         * Differenced per window and divided by the flips between, because a
         * cumulative total and a per-flip rate are different instruments and
         * this file has been wrong about that before. s_snap_seq counts
         * copies actually performed, so it is flips that carried a frame. */
        static long on_black = -1, dump_on_black;
        static unsigned black_reports;
        static uint32_t last_draws;
        static unsigned long last_seq;
        if (on_black < 0) {
            const char *e = getenv("RECOMP_FRAG_FORCE_ON_BLACK");
            on_black = (e && *e) ? strtol(e, NULL, 10) : 0;
            /* RECOMP_FB_DUMP_DRAW_ON_BLACK=<reports>: the same arm, but what
             * it starts is the per-draw capture (RECOMP_FB_DUMP_DRAW, whose
             * after-seconds field should then be set past the run's end).
             * The Load screen arrives at t=30 in one run and t=50 in the
             * next, so a wall-clock start lands its 24 slots on the wrong
             * screen; the defect itself is the only reliable trigger. */
            e = getenv("RECOMP_FB_DUMP_DRAW_ON_BLACK");
            dump_on_black = (e && *e) ? strtol(e, NULL, 10) : 0;
        }
        if (on_black > 0 || dump_on_black > 0) {
            unsigned long dseq = s_snap_seq - last_seq;
            uint32_t ddraws = s_gpu.draws - last_draws;
            /* 28 on the Load screen, 1.0 where the guest stopped submitting.
             * Ten is not near either of them. */
            double per_flip = dseq ? (double)ddraws / (double)dseq : 0.0;
            int submitting = per_flip >= 10.0;
            int black = snapshot_nonzero() == 0;
            last_seq = s_snap_seq;
            last_draws = s_gpu.draws;

            if (black && submitting) {
                ++black_reports;
#if defined(__APPLE__)
                /* Metal only: the Windows build has no nv2a_metal.h (mingw
                 * reported the implicit declaration, 24 Sep). */
                if (on_black > 0 && black_reports == (unsigned)on_black)
                    nv2a_metal_frag_force_arm();
#endif
                if (dump_on_black > 0 && black_reports == (unsigned)dump_on_black
                        && !s_draw_dump_black_armed) {
                    s_draw_dump_black_armed = 1;
                    fprintf(stderr, "  [DRAW-CAP] ARMED by the black condition"
                                    " at t=%.2f\n", trace_seconds());
                }
            } else {
                black_reports = 0;
            }
            /* The arm's own inputs, beside its decision. The run that armed
             * early printed nothing but the fact that it had. */
            fprintf(stderr, "  [FRAG-ARM] black=%s draws/flip=%.1f (%u over %lu"
                    " flips) -> %s, run=%u/%ld\n",
                    black ? "yes" : "no", per_flip, ddraws, dseq,
                    (black && submitting) ? "counting" : "reset",
                    black_reports, on_black);
        }
    }
    frame_stats_report();
    fprintf(stderr, "[TEXTURE] prepared=%u rejected=%u\n", s_copy.batches, s_copy.rejected);
    nv2a_drop_report("report");
    nv2a_texture_copy_census();
    draw_mix_report();
    for (unsigned i=0; i<32 && s_texture_reasons[i].reason; ++i)
        fprintf(stderr,"[TEXTURE]   %llu  %s\n",(unsigned long long)s_texture_reasons[i].count,s_texture_reasons[i].reason);
    if (s_combiner_count) {
        fprintf(stderr,"[COMBINER] distinct=%u overflow-draws=%llu\n",s_combiner_count,(unsigned long long)s_combiner_overflow);
        for (unsigned i=0; i<s_combiner_count; ++i)
            fprintf(stderr,"[COMBINER] config=%u first-draw=%u draws=%llu rejected=%llu inline=%llu invalid-position=%llu collapsed-xy=%llu\n",i,
                    s_combiner_trace[i].first_draw,(unsigned long long)s_combiner_trace[i].draws,
                    (unsigned long long)s_combiner_trace[i].rejected,
                    (unsigned long long)s_combiner_trace[i].inline_draws,
                    (unsigned long long)s_combiner_trace[i].invalid_positions,
                    (unsigned long long)s_combiner_trace[i].collapsed_xy);
    }
    if (vsh_split_on())
        fprintf(stderr, "[VSH] split: fetch %.1f ms over %llu vertices, "
                "execute %.1f ms over %llu vertices"
                " (a zero here with a zero count is a dead instrument,"
                " not a free stage)\n",
                g_vsh_fetch_ns / 1e6, (unsigned long long)g_vsh_fetch_n,
                g_vsh_exec_ns / 1e6,  (unsigned long long)g_vsh_exec_n);
    fprintf(stderr, "[VSH] executed batches=%u rejected=%u mode=%u start=%u slots=%d\n",
            s_vsh.batches, s_vsh.rejected, s_vsh.mode, s_vsh.start, s_vsh.decoded.length);
    /* A rejected batch is a batch the player does not see, so the split
     * between "we could not normalise a normal nothing reads" -- which now
     * draws -- and "we could not normalise one something reads" -- which still
     * does not -- is the difference between a fixed bug and a remaining one.
     * Printed unconditionally and beside the reject list, because the whole
     * failure here was that 37,575 dropped draws sat inside one number. */
    if (nv2a_ff_normal_unread || nv2a_ff_normal_read)
        fprintf(stderr, "[VSH] degenerate normals: %lu drawn (nothing read it)"
                ", %lu still rejected (lighting or NORMAL_MAP texgen reads it)\n",
                nv2a_ff_normal_unread, nv2a_ff_normal_read);
    /* WHERE THE FIXED-FUNCTION BATCHES WENT. Printed whenever the switch is on
     * or anything counted, because the whole claim of RECOMP_METAL_FF is that
     * the first number is large: a build where it is small has a refusal
     * reason beside it rather than a shrug. accepted + left-on-the-CPU is every
     * batch the key was asked about, so the columns add up and a missing batch
     * is visible.
     *
     * READ THIS BESIDE "[METAL] vsh draws", not instead of it. Once the switch
     * is on, a fixed-function batch also counts as a GPU draw over there, so
     * the identity that used to hold -- executed batches == vsh draws GPU --
     * no longer does, by design. This line is what restores the split. */
    if (ff_gpu_on() || nv2a_ff_gpu_batches || nv2a_ff_gpu_cpu_batches)
        fprintf(stderr, "[VSH] fixed-function on the GPU (metal_ff %s):"
                " %lu batches accepted, %lu left on the CPU"
                " (%lu skinning, %lu texgen, %lu local/spot light,"
                " %lu degenerate-normal shape, %lu backend refused,"
                " %lu clip-w vertices, %lu q<=0 vertices the sink would have"
                " dropped); texture matrix:"
                " %lu never uploaded, %lu all-zero\n",
                ff_gpu_on() ? "on" : "OFF",
                nv2a_ff_gpu_batches, nv2a_ff_gpu_cpu_batches,
                nv2a_ff_gpu_no_skin, nv2a_ff_gpu_no_texgen,
                nv2a_ff_gpu_no_light, nv2a_ff_gpu_no_normal,
                nv2a_ff_gpu_backend_refused, nv2a_ff_gpu_clip_w,
                nv2a_ff_gpu_texq_would_drop,
                nv2a_ff_gpu_texmat_unset, nv2a_ff_gpu_texmat_zero);
    if (nv2a_ff_fog_unknown || nv2a_ff_fog_vertices[1] || nv2a_ff_fog_vertices[2] || nv2a_ff_fog_vertices[3]
        || nv2a_ff_fog_vertices[4] || nv2a_ff_fog_vertices[5])
        fprintf(stderr, "[VSH] fixed-function fog coordinate on the CPU (vertices): spec alpha %lu, radial %lu,"
                " planar %lu, abs planar %lu, fog attribute %lu, UNMODELLED gen mode %lu (coordinate 0)\n",
                nv2a_ff_fog_vertices[1], nv2a_ff_fog_vertices[2], nv2a_ff_fog_vertices[3],
                nv2a_ff_fog_vertices[4], nv2a_ff_fog_vertices[5], nv2a_ff_fog_unknown);
    if (s_fog_trace_n) fog_trace_print("report");
    for (size_t i = 0; i < sizeof s_vsh_reject / sizeof s_vsh_reject[0]; ++i) {
        if (!s_vsh_reject[i].reason) break;
        fprintf(stderr, "[VSH]   %8u  %s (first detail %u)\n",
                s_vsh_reject[i].n, s_vsh_reject[i].reason, s_vsh_reject[i].detail);
    }
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    if (pb_vertex_range())
        fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                        " input x %.1f..%.1f  y %.1f..%.1f\n",
                s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
                s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    else
        fprintf(stderr, "[GPU] draws %u, %u indices; input range not measured"
                        " (RECOMP_VERTEX_RANGE=1)\n",
                s_gpu.draws, s_gpu.verts);
    /* Both halves of the submission accounting on one line, because the
     * question is always "which method, and did it fit". elem32 is the method
     * that was not decoded at all until 16 Sep 2026; no-room is the cap that
     * truncated batches silently before it was raised to NV_MAX_INDICES.
     * Everything here counts indices, so the sources are comparable. */
    fprintf(stderr, "[GPU] indices by source: elem32=%llu stored of %llu methods"
                    " (%llu too wide, %u batches refused for it); no-room=%llu"
                    " (e16=%llu e32=%llu); biggest batch asked for %u of %u\n",
            s_gpu.elem32_indices, s_gpu.elem32_seen, s_gpu.elem32_wide,
            s_gpu.elem32_batches_dropped, s_gpu.idx_overflow,
            s_gpu.idx_overflow_e16, s_gpu.idx_overflow_e32,
            s_gpu.idx_wanted_max, (unsigned)NV_MAX_INDICES);
    /* Inline submission on its own line and in WORDS, because inline_words[]
     * is a different array with a different cap and putting it beside an index
     * count would repeat the e16/e32 scale mistake. batches-no-layout is the
     * whole-batch drop, which no other counter sees. */
    fprintf(stderr, "[GPU] inline: no-room=%llu words of %u wanted (biggest"
                    " batch %u of %u words); %u batches dropped for an"
                    " underivable layout\n",
            s_gpu.inline_overflow, s_gpu.inline_wanted_max,
            s_gpu.inline_wanted_max, (unsigned)NV_MAX_INLINE_WORDS,
            s_gpu.batches_no_layout);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    /* TWO PICTURES, AND THE SECOND IS THE ONE PEOPLE ACTUALLY WANT.
     *
     * reportNNN is the LIVE SURFACE, sampled on the report timer, which is a
     * moment part way through composing a frame. Read as "what the picture
     * looks like" it is a trap, and on 21 Sep 2026 it sprang: a title-screen
     * report frame showed wide black bars across the scene, and the same run's
     * `presented nonzero` read 306,830 of 307,200 at that moment -- there were
     * no black bars in the frame anybody saw. Half-composed frames had already
     * been the evidence for two theories that died that day.
     *
     * snapNNN is s_snap, the copy taken at FLIP_STALL, which is the only thing
     * nv2a_pb_exec_surface ever hands the window. The long note at
     * dump_snapshot_bmp says exactly this and names the report timer as a
     * caller -- but nv2a_pb_exec_dump_surface, the function that would have
     * been that caller, HAD NONE. It was written, documented and never wired
     * up, so every framebuffer picture this project has looked at has been the
     * live surface.
     *
     * Both are kept and both are named. The live surface is still the right
     * thing to look at for "what did the parser leave behind"; it is simply
     * not the right thing to look at for "what does the game look like". */
    {
        static unsigned seq;
        dump_surface_bmp("report", seq);
        dump_snapshot_bmp("snap", seq);
        ++seq;
    }

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    int n_top;
    fprintf(stderr, "[GPU] rasterised %u triangles; %u batches skipped as not"
                    " screen-space, %u triangles fully off-surface\n",
            s_gpu.tris_drawn, s_gpu.batches_untransformed,
            s_gpu.tris_skipped_offscreen);
    /* How much of the drawing went through software, on EVERY host.
     *
     * This lived inside the Apple branch beside the Metal counters, so the one
     * host with no GPU path at all -- the one where the answer matters -- never
     * printed it. Same trap as the VEH fault counters: a counter that cannot
     * be read on the host under investigation is not a counter. */
    fprintf(stderr, "[RASTER] %u batches + %u triangles on the CPU;"
                    " %u triangles total\n",
            s_gpu.cpu_batches, s_gpu.cpu_tris, s_gpu.tris_drawn);
    /* Outside the NV2A_GPU_PATH block below on purpose: the framebuffer watch
     * runs on every host, and a counter that cannot be read on the host under
     * investigation is not a counter. */
    fb_watch_report();
    ff_watch_report();
#if NV2A_GPU_PATH
    if (nv2a_gpu_on())
    {
        fprintf(stderr,"[" NV2A_GPU_TAG "] %u batches native, %u software fallbacks\n",
            s_gpu.gpu_batches,s_gpu.gpu_fallbacks);
        surface_audit_report();
        nv2a_gpu_report();
#if defined(__APPLE__)
        nv2a_metal_cb_report();
#endif
    }
#endif

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm. RECOMP_PB_EXEC_TOP raises the
     * cut, because "which methods does this title use at all" is a different
     * question from "which dominate", and answering it by guessing at NV2A
     * register numbers is how you implement the wrong one. */
    {
        static int top = -1;
        if (top < 0) {
            const char *e = getenv("RECOMP_PB_EXEC_TOP");
            top = e ? atoi(e) : 10;
            if (top <= 0) top = 10;
        }
        n_top = top;
    }
    /* Sorted on a copy: the live table is indexed by s_unhandled_slot, and
     * swapping its entries in place would point every slot at the wrong
     * method from the first report onwards. */
    {
        static PbUnhandled sorted[PB_EXEC_MAX_UNHANDLED];
        memcpy(sorted, s_unhandled, (size_t)s_unhandled_count * sizeof sorted[0]);
        for (i = 0; i < n_top && i < s_unhandled_count; i++) {
            int best = i;
            for (j = i + 1; j < s_unhandled_count; j++)
                if (sorted[j].count > sorted[best].count)
                    best = j;
            if (best != i) {
                PbUnhandled t = sorted[i];
                sorted[i] = sorted[best];
                sorted[best] = t;
            }
            fprintf(stderr, "  [GPU]   0x%04X x%u\n",
                    sorted[i].method, sorted[i].count);
        }
    }
    if (getenv("RECOMP_PB_EXEC_PROGRAM"))
        nv2a_pb_exec_dump_program();
    fflush(stderr);
}

/* G39: what this executor made of the draw it ran last, for the host D3D
 * mirror's check (d3d8_host.c). Runs on the pusher thread, from a token that
 * sits behind the draw's commands, so s_copy is that draw. */
#include "d3d8_host.h"
static uint32_t exec_fmt_byte(const NV2ATextureCopy *t)
{
    if (t->dxt1) return 0x0Cu;
    if (t->dxt3) return 0x0Eu;
    if (t->rgba8) return t->xrgb8 ? 0x07u : 0x06u;
    if (t->sz16) return t->argb4 ? 0x04u : 0x03u;
    return 0x11u;
}
void nv2a_pb_exec_last_draw_textures(D3D8ExecDrawTextures *out)
{
    const NV2ATextureCopy *c = &s_copy.state;
    memset(out, 0, sizeof *out);
    /* G41: the vertex arrays and indices the last batch used, independent of
     * whether the texture/surface state below is meaningful. Raw registers:
     * the 0x1760 stride is bits 8-31 and VertexAttr keeps only 8 of them.
     * idx[] survives SET_BEGIN_END(0); the next BEGIN resets it. */
    out->va_valid = 1;
    for (unsigned i = 0; i < 16; ++i) {
        out->va_offset[i] = s_methods[(NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 4u * i) / 4u];
        out->va_format[i] = s_methods[(NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 4u * i) / 4u];
    }
    out->idx_count = s_gpu.idx_count;
    for (unsigned k = 0; k < 16 && k < s_gpu.idx_count; ++k) out->idx[k] = s_gpu.idx[k];
    /* G43: the combiner registers as latched from the method stream -- the
     * last value D3D (builder, TextureFactor, fog updater or SetPixelShader)
     * wrote to each, whichever it was. */
    out->ffc_valid = 1;
    for (unsigned k = 0; k < D3D8_HOST_FFC_N; ++k) out->ffc.w[k] = s_methods[d3d8_host_ffc_method(k) / 4u];
    /* G42: the whole shadow -- lighting, texgen, texture matrices and fog are
     * spread over most of it, and the check picks what it needs. */
    _Static_assert(sizeof out->regs == sizeof s_methods, "G42: the exec source copies the whole method shadow");
    out->regs_valid = 1;
    memcpy(out->regs, s_methods, sizeof out->regs);
    /* G51.3: what an attribute slot without an array reads. */
    out->cur_valid = 1;
    memcpy(out->cur, s_vsh.current, sizeof out->cur);
    out->active = s_copy.active;
    if (!s_copy.active) return;
    out->mask = c->untextured ? 0u : (c->texture_mask & 0xFu);
    out->target_addr = s_copy.target_address;
    out->target_pitch = c->target_pitch;
    out->target_bpp = c->target_bpp;
    out->depth_used = (c->depth_test || c->stencil_test) && s_copy.depth;
    out->depth_addr = s_copy.depth_address;
    out->depth_pitch = c->depth_pitch;
    nv2a_texture_copy_window(c, &out->win_x0, &out->win_y0, &out->win_x1, &out->win_y1);
    out->clip_x = c->clip_x; out->clip_y = c->clip_y; out->clip_w = c->clip_w; out->clip_h = c->clip_h;
    out->z_min = c->z_clip_min; out->z_max = c->z_clip_max;
    {   static const uint32_t m[11] = D3D8_HOST_STATE_METHODS;
        for (unsigned k = 0; k < 11; ++k) out->st_reg[k] = s_methods[m[k] / 4u]; }
    memcpy(out->vc, s_vsh.constants, sizeof out->vc);
    {   static const uint32_t pp[D3D8_HOST_PS_N][2] = D3D8_HOST_PS_PAIRS;
        for (unsigned k = 0; k < D3D8_HOST_PS_N; ++k) out->ps_reg[k] = s_methods[pp[k][0] / 4u]; }
    for (unsigned u = 0; u < 4; ++u) {
        out->tex_address[u]  = s_methods[(0x1B08u + 64u * u) / 4u];
        out->tex_control0[u] = s_methods[(0x1B0Cu + 64u * u) / 4u];
        out->tex_filter[u]   = s_methods[(0x1B14u + 64u * u) / 4u];
    }
    memcpy(out->ff_modelview, &s_methods[0x480u / 4u], sizeof out->ff_modelview);
    memcpy(out->ff_composite, &s_methods[0x680u / 4u], sizeof out->ff_composite);
    memcpy(out->ff_projection, &s_methods[0x440u / 4u], sizeof out->ff_projection);
    out->exec_mode = s_methods[0x1E94u / 4u];
    out->prog_start = s_methods[0x1EA0u / 4u];
    out->composite_ever_written = s_method_seen[0x680u / 4u];
    out->vsh_mode_internal = s_vsh.mode;
        memcpy(out->vs_words, s_vsh.words, sizeof out->vs_words < sizeof s_vsh.words ? sizeof out->vs_words : sizeof s_vsh.words);
    for (unsigned u = 0; u < 4; ++u) {
        const NV2ATextureCopy *t = u ? &s_copy.extra_stages[u - 1] : c;
        if (!(out->mask & (1u << u))) continue;
        out->addr[u] = u ? s_copy.extra_address[u - 1] : s_copy.texture_address;
        out->width[u] = t->width; out->height[u] = t->height; out->levels[u] = t->levels;
        out->fmt[u] = exec_fmt_byte(t);
    }
}
