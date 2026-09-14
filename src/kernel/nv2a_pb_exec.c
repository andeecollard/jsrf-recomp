#include "nv2a_ff.h"
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
#define nv2a_gpu_sync_range(target, bytes) nv2a_metal_sync()
#define nv2a_gpu_invalidate  nv2a_metal_invalidate
#define nv2a_gpu_invalidate_range(target, bytes) nv2a_metal_invalidate(target)
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
#include "nv2a_regs.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>     /* ptrdiff_t; MSVC gets it via another header */
#include <time.h>      /* clock_gettime, for the opt-in traces below */

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
typedef enum { PB_STAGE_VSH, PB_STAGE_SUBMIT, PB_STAGE_SYNC, PB_STAGE_N } PbStage;
static const char *const pb_stage_name[PB_STAGE_N] = { "vsh", "submit", "sync" };
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
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096
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
    if (on < 0) on = getenv("RECOMP_VSH_REUSE_STATS") ? 1 : 0;
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
static int vsh_reuse_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_VSH_REUSE") ? 1 : 0;
    return on;
}
static int vsh_reuse_verify(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_VSH_REUSE_VERIFY") ? 1 : 0;
    return on;
}

void nv2a_vsh_reuse_report(void)
{
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
 * costs a link error for a diagnostic none of them enable. The origin is the
 * first traced write, which the guest issues during D3D initialisation, so
 * this reads within a millisecond of the [FB] timeline it is compared against. */
static double trace_seconds(void)
{
    static struct timespec origin;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!origin.tv_sec && !origin.tv_nsec) origin = now;
    return (double)(now.tv_sec - origin.tv_sec)
         + (double)(now.tv_nsec - origin.tv_nsec) / 1e9;
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
#define ALPHA_TRACE_MAX 24
static struct {
    unsigned level;
    uint32_t first_draw, last_draw;
    double first_t, last_t;
    uint64_t hits;
} s_alpha_trace[ALPHA_TRACE_MAX];
static unsigned s_alpha_trace_count;
static uint64_t s_alpha_trace_overflow, s_alpha_trace_batches;

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
#define FACTOR_TRACE_MAX 24
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

static void note_unhandled(uint32_t method)
{
    int i;

    s_gpu.unhandled_total++;
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
static uint8_t *s_snap;             /* the last completed frame, packed */
static uint32_t s_snap_w, s_snap_h, s_snap_bpp;
static int s_snap_wanted;

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
static void snapshot_surface(void)
{
    uint32_t b = surface_bpp();
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t y;

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
    if (!s_snap_wanted || !s_gpu.color_offset || !s_gpu.clip_w || !s_gpu.clip_h)
        return;
    if (b != 2 && b != 4)
        return;
    if (!mem)
        return;

    if (s_snap_w != s_gpu.clip_w || s_snap_h != s_gpu.clip_h || s_snap_bpp != b) {
        uint8_t *n = (uint8_t *)realloc(s_snap,
                                        (size_t)s_gpu.clip_w * s_gpu.clip_h * b);
        if (!n)
            return;
        s_snap = n;
        s_snap_w = s_gpu.clip_w;
        s_snap_h = s_gpu.clip_h;
        s_snap_bpp = b;
    }
    for (y = 0; y < s_snap_h; y++)
        memcpy(s_snap + (size_t)y * s_snap_w * b,
               mem + s_gpu.color_offset
                   + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch
                   + (size_t)s_gpu.clip_x * b,
               (size_t)s_snap_w * b);
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
        if (live < 0) live = getenv("RECOMP_FB_LIVE") ? 1 : 0;
        if (!live && s_snap && s_snap_w && s_snap_h) {
            if (w) *w = s_snap_w;
            if (h) *h = s_snap_h;
            if (pitch) *pitch = s_snap_w * s_snap_bpp;   /* the copy is packed */
            if (bpp) *bpp = s_snap_bpp;
            return s_snap;
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
static void write_bmp(const char *tag, unsigned seq,
                      const uint8_t *base, uint32_t pitch,
                      uint32_t x0, uint32_t y0,
                      uint32_t w, uint32_t h, uint32_t bpp)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    char path[512];
    uint32_t y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;

    if (!prefix || !base || !w || !h || (bpp != 2 && bpp != 4))
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%s%03u.bmp", prefix, tag, seq);
    f = fopen(path, "wb");
    if (!f)
        return;

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
static void dump_snapshot_bmp(const char *tag, unsigned seq)
{
    if (!s_snap || !s_snap_w || !s_snap_h)
        return;
    write_bmp(tag, seq, s_snap, s_snap_w * s_snap_bpp, 0, 0,
              s_snap_w, s_snap_h, s_snap_bpp);
}

/* Non-black pixels in the presented copy. "Blank" is the whole question at the
 * flip, and a number answers it in the log without opening a file. */
static uint32_t snapshot_nonzero(void)
{
    uint32_t n = 0, i, count;

    if (!s_snap || !s_snap_w || !s_snap_h)
        return 0;
    count = s_snap_w * s_snap_h;
    if (s_snap_bpp == 2) {
        const uint16_t *p = (const uint16_t *)s_snap;
        for (i = 0; i < count; i++) if (p[i]) n++;
    } else if (s_snap_bpp == 4) {
        const uint32_t *p = (const uint32_t *)s_snap;
        for (i = 0; i < count; i++) if (p[i] & 0x00FFFFFFu) n++;
    }
    return n;
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
 * 500 us bins to 128 ms and one overflow bucket: 2 KB of statics, one
 * clock_gettime and one increment per frame, against a [FB] line that already
 * checksums the entire framebuffer once a second. Unconditional for that
 * reason -- an opt-in perf counter is one nobody has switched on when they
 * need the number.
 *
 * Percentiles report their bin's UPPER edge, so p50=16.5 means "half of all
 * frames finished in 16.5 ms or less", accurate to the 0.5 ms bin width.
 * max_us is exact and is not binned. */
#define FRAME_BIN_US   500u
#define FRAME_BINS     257u          /* 0..128 ms, plus one overflow bucket */
typedef struct {
    unsigned long long n, total_us, bin[FRAME_BINS];
    unsigned long long max_us;
} FrameHist;

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

static void frame_stats_flip(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_frame.started) {
        /* Signed, and in that order: tv_nsec wraps every second, so the naive
         * unsigned difference reads about 4e9 us once a second. */
        long long us = (long long)(now.tv_sec - s_frame.last.tv_sec) * 1000000LL
                     + ((long long)now.tv_nsec - (long long)s_frame.last.tv_nsec) / 1000LL;
        unsigned b;
        if (us < 0) us = 0;
        b = (unsigned)((unsigned long long)us / FRAME_BIN_US);
        if (b >= FRAME_BINS) b = FRAME_BINS - 1u;
        s_frame.run.bin[b]++;
        s_frame.win.bin[b]++;
        s_frame.run.n++;
        s_frame.win.n++;
        s_frame.run.total_us += (unsigned long long)us;
        s_frame.win.total_us += (unsigned long long)us;
        if ((unsigned long long)us > s_frame.run.max_us)
            s_frame.run.max_us = (unsigned long long)us;
        if ((unsigned long long)us > s_frame.win.max_us)
            s_frame.win.max_us = (unsigned long long)us;
    }
    s_frame.last = now;
    s_frame.started = 1;
}

/* Upper edge of the bin the p-th percentile falls in, in milliseconds. The
 * overflow bucket has no upper edge, so it reports the bottom of the bucket
 * and the caller's max_us is what says how far past it the run actually got. */
static double frame_pct(const FrameHist *h, double p)
{
    unsigned long long want, seen = 0;
    unsigned i;
    if (!h->n) return 0.0;
    want = (unsigned long long)((double)h->n * p);
    if (want < 1) want = 1;
    for (i = 0; i < FRAME_BINS; ++i) {
        seen += h->bin[i];
        if (seen >= want)
            return (i + 1u == FRAME_BINS)
                 ? (double)(FRAME_BINS - 1u) * FRAME_BIN_US / 1000.0
                 : (double)(i + 1u) * FRAME_BIN_US / 1000.0;
    }
    return (double)(FRAME_BINS - 1u) * FRAME_BIN_US / 1000.0;
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
            " p50=%.1f p90=%.1f p99=%.1f max=%.1f ms  over-33ms=%llu\n",
            tag, (unsigned long long)h->n, mean_ms,
            mean_ms > 0.0 ? 1000.0 / mean_ms : 0.0,
            frame_pct(h, 0.50), frame_pct(h, 0.90), frame_pct(h, 0.99),
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
        sum += per[i];
    }
    fprintf(stderr, "  [STAGE] per frame:");
    for (i = 0; i < PB_STAGE_N; ++i)
        fprintf(stderr, " %s=%.2f ms (%.0f calls)", pb_stage_name[i], per[i],
                (double)s_stage_win.n[i] / (double)frames);
    fprintf(stderr, " | rest=%.2f ms of %.2f\n",
            (double)frame_us / (double)frames / 1000.0 - sum,
            (double)frame_us / (double)frames / 1000.0);
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
        pb_stage_line(s_frame.win.n, s_frame.win.total_us);
    }
    memset(&s_frame.win, 0, sizeof s_frame.win);
    memset(&s_stage_win, 0, sizeof s_stage_win);
}

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
    if (!getenv("RECOMP_FLIP_TRACE"))
        return;
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

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;
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
                if (!gpu_cleared) nv2a_gpu_invalidate_range(z, bytes);
            }
#endif
#ifdef nv2a_gpu_surface_report
            if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
                fprintf(stderr, "  [EV] ZCLEAR %s base=%08X off=%08X value=%08X%s\n",
                        z ? "ok" : "REFUSED", base, offset, s_methods[0x1d8c/4],
                        gpu_cleared ? " (resident)" : "");
#endif
            if (z && !gpu_cleared) for (y=y0;y<y1;++y) for (x=x0;x<x1;++x) {
                uint8_t *p=z+(size_t)y*pitch+x*4;
                if (param&2) p[0]=(uint8_t)value;
                if (param&1) for (unsigned k=1;k<4;++k) p[k]=(uint8_t)(value>>(8*k));
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
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
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
                        param, s_gpu.clear_color);
#endif
            if (!gpu_cleared)
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
                uint16_t v = (uint16_t)s_gpu.clear_color;
                uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
                for (x = 0; x < s_gpu.clip_w; x++)
                    p[x] = v;
            }
        }
    }
    s_gpu.clears++;
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
    if (getenv("RECOMP_RASTER_TEST")) {
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
    int reuse_active = (vsh_reuse_on() || vsh_reuse_verify()) && programmable;

    for (uint32_t i = 0; i < s_gpu.idx_count; ++i) {
        if (programmable) {
            float inputs[16][4];
            uint16_t reuse_key = s_gpu.idx[i];
            int reuse_from = -1;
            if (reuse_active &&
                (reuse_seen[reuse_key >> 3] & (1u << (reuse_key & 7))))
                reuse_from = (int)reuse_at[reuse_key];
            if (reuse_from >= 0 && vsh_reuse_on() && !vsh_reuse_verify()) {
                memcpy(s_outputs[i],   s_outputs[reuse_from],   sizeof(s_outputs[i]));
                memcpy(s_positions[i], s_positions[reuse_from], sizeof(s_positions[i]));
                s_colors[i] = s_colors[reuse_from];
                g_vsh_reuse_hits++;
                continue;
            }
            NV2AVshResult result;
            memcpy(inputs, s_vsh.current, sizeof(inputs));
            for (uint32_t a = 0; a < 16; ++a) {
                if (!(s_vsh.decoded.inputs_read & (1u << a))) continue;
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
            if (i == 0 && (s_vsh.batches < 4 || s_vsh.batches == vsh_sample_batch()))
                trace_vertex_inputs(inputs);
            if (!nv2a_vsh_execute(&s_vsh.decoded, inputs, s_vsh.constants, &result))
                VSH_REJECT("shader execution failed", s_vsh.decoded.length);
            if ((result.written[0] & 12) != 12)
                VSH_REJECT("program left oPos.zw unwritten", result.written[0]);
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
                        memcpy(reuse_inputs[i], inputs, sizeof(inputs));
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
            if(s_method_seen[0x680/4] && s_method_seen[0x6bc/4]) {
                float inputs[16][4];
                memcpy(inputs,s_vsh.current,sizeof(inputs));
                for(unsigned a=0;a<16;++a) if(s_gpu.attr[a].size)
                    if(!fetch_vertex(a,s_gpu.idx[i],inputs[a])) VSH_REJECT("fixed-function vertex fetch",a);
                const char *reason=nv2a_ff_vertex(s_methods,inputs,s_outputs[i]);
                if(reason) VSH_REJECT(reason,0);
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

static void raster_batch(void)
{
    uint32_t i;
    uint32_t drawn_before = s_gpu.tris_drawn;

    /* Stage-by-stage fate of a batch drawn under a multiply blend. Sampled
     * here rather than at the accept test, because everything below the
     * vertex stage is invisible from there. */
    const int fade_batch = blend_is_multiply();
    if (fade_batch) ++s_blend_fade_fate.batches;

    if (s_gpu.idx_count < 3) {
        if (fade_batch) ++s_blend_fade_fate.short_idx;
        return;
    }
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
                /* One level per batch: the fade quad is flat-shaded, so lo and
                 * hi agree on it, and a batch where they disagree is not the
                 * quad we are looking for. */
                note_batch_alpha((unsigned)(lo * 255.0f + 0.5f));
            }
        }
    }

    const char *copy_error = prepare_texture_copy();
    trace_combiner(copy_error);
    capture_draw(copy_error);
    if (copy_error) {
        if (fade_batch) ++s_blend_fade_fate.prepare_rejected;
        if (++s_copy.rejected <= 8) fprintf(stderr, "[TEXTURE] rejected draw %u: %s (t=%.2f)\n", s_gpu.draws, copy_error, trace_seconds());
        return;
    }
    if (fade_batch) ++s_blend_fade_fate.rasterised;
    if (s_vsh.mode == 0 && !(s_method_seen[0x680/4] && s_method_seen[0x6bc/4]) && !batch_is_screen_space()) {
        s_gpu.batches_untransformed++;
        /* RECOMP_PB_EXEC_NOCLIPTEST: rasterise anyway, to tell "the vertices
         * were decoded but sit outside the clip rect" apart from "the vertices
         * are not usable at all". Diagnostic only -- the test exists because
         * untransformed vertices would otherwise paint nonsense. */
        {
            static int allow = -1;
            if (allow < 0) allow = getenv("RECOMP_PB_EXEC_NOCLIPTEST") ? 1 : 0;
            if (!allow)
                return;
        }
    }

#if NV2A_GPU_PATH
    if (s_copy.active && nv2a_gpu_on()) {
        static unsigned fallback_reports, unique_reports;
        static const char *seen_reasons[16];
        static int trace_fallbacks = -1;
        if (trace_fallbacks < 0)
            trace_fallbacks = getenv("RECOMP_GPU_FALLBACK_TRACE")
                || getenv("RECOMP_METAL_FALLBACK_TRACE") ? 1 : 0;
        unsigned long long _t_sub = pb_now_us();
        int triangles=nv2a_gpu_draw(&s_copy.state,s_copy.texture,s_copy.texture_bytes,
            s_copy.target,s_copy.target_bytes,s_copy.depth,s_copy.depth_bytes,
            s_outputs,s_gpu.idx_count,s_gpu.prim);
        pb_stage_add(PB_STAGE_SUBMIT, _t_sub);
        if(triangles>=0) {
            static unsigned reported;
            s_gpu.tris_drawn+=(unsigned)triangles;
            ++s_gpu.gpu_batches;
            if(reported++<10) fprintf(stderr,"[" NV2A_GPU_TAG "] rendered batch: %d triangles\n",triangles);
            goto batch_complete;
        }
        ++s_gpu.gpu_fallbacks;
        const char *reason=nv2a_gpu_last_reject();
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
    default:
        break;                             /* points and lines: not yet */
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
        /* RECOMP_FB_DUMP_DRAW=<stride>: capture after every stride-th batch
         * that actually drew, so the picture is a composed frame rather than
         * whatever the surface held when a report happened to fire. The first
         * batches are always the same full-screen blit, so a stride is what
         * makes the title's own geometry visible; the default keeps the old
         * behaviour of the first few. */
        static unsigned batches, captured;
        static long stride = -1;
        static const char *dump_slot;
        static int dump_on;
        if (stride < 0) {
            const char *env = pb_env_str("RECOMP_FB_DUMP_DRAW", &dump_slot);
            dump_on = env != NULL;
            stride = env && *env ? strtol(env, NULL, 0) : 0;
            if (stride < 1) stride = 1;
        }
        if (stride > 0 && dump_on && captured < 24
                && (batches++ % (unsigned long)stride) == 0) {
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

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
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

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
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
        /* First, so the interval covers the whole frame -- see frame_stats_flip. */
        frame_stats_flip();
#if defined(__APPLE__)
        g_mtl_frames++;   /* command buffers per frame needs a frame */
#endif
#ifdef nv2a_gpu_surface_report
        /* BEFORE the snapshot, because the snapshot syncs: the question is
         * what the GPU still owes guest RAM at the moment the guest calls the
         * frame finished, and a sync answers it by destroying it. */
        if (getenv("RECOMP_FLIP_TRACE") && nv2a_gpu_on()) {
            fprintf(stderr, "  [FLIPTRACE] pre-sync:\n");
            nv2a_gpu_surface_report();
        }
        if (nv2a_gpu_on() && nv2a_d3d11_event_trace())
            fprintf(stderr, "  [EV] FLIP   bound=%08X\n", s_gpu.color_offset);
#endif
        snapshot_surface();
        flip_trace();
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
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
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE_WORDS)
            s_gpu.inline_words[s_gpu.inline_count++] = param;
        break;

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
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
    if (!s_snap || !s_snap_w || !s_snap_h)
        return -1;
    return (int)snapshot_nonzero();
}

uint32_t nv2a_pb_exec_surface_va(void)
{
    return s_gpu.color_offset;
}

void nv2a_pb_exec_report(void)
{
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
    frame_stats_report();
    fprintf(stderr, "[TEXTURE] prepared=%u rejected=%u\n", s_copy.batches, s_copy.rejected);
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
    fprintf(stderr, "[VSH] executed batches=%u rejected=%u mode=%u start=%u slots=%d\n",
            s_vsh.batches, s_vsh.rejected, s_vsh.mode, s_vsh.start, s_vsh.decoded.length);
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
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    {
        static unsigned seq;
        dump_surface_bmp("report", seq++);
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
#if NV2A_GPU_PATH
    if (nv2a_gpu_on())
    {
        fprintf(stderr,"[" NV2A_GPU_TAG "] %u batches native, %u software fallbacks\n",
            s_gpu.gpu_batches,s_gpu.gpu_fallbacks);
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
    for (i = 0; i < n_top && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        fprintf(stderr, "  [GPU]   0x%04X x%u\n",
                s_unhandled[i].method, s_unhandled[i].count);
    }
    if (getenv("RECOMP_PB_EXEC_PROGRAM"))
        nv2a_pb_exec_dump_program();
    fflush(stderr);
}
