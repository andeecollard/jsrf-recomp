#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nv2a_metal.h"
#include "../recomp_switch.h"
#include "nv2a_metal_state.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Native raster path for the fragment states already understood by the CPU
 * renderer. Surfaces stay on the GPU across compatible batches. Guest RAM is
 * synchronized only at a flip, clear, diagnostic capture, or software fallback. */
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipeline;
static id<MTLTexture> surface,stencil_surface;
#include <time.h>
static id<MTLCommandBuffer> last_command;

/* Command-buffer accounting, opt-in via RECOMP_METAL_CB_STATS=1.
 *
 * This path creates a command buffer AND a render encoder per draw and commits
 * immediately (see the draw function below), so command buffers per frame is
 * draws per frame -- around 700k over a 300 s run. Whether that costs anything
 * worth reclaiming is a measurement, not an assumption: Metal command buffers
 * are cheap by design, and the encoder and pipeline state may dominate. Count
 * and time the three phases separately before changing any of it. */
unsigned long long g_mtl_cbufs, g_mtl_frames;
unsigned long long g_mtl_ns_create, g_mtl_ns_encode, g_mtl_ns_commit;
static int mtl_cb_stats(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_METAL_CB_STATS") ? 1 : 0;
    return on;
}
static inline unsigned long long mtl_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
/* Batched submission: one command buffer and one render encoder across a run
 * of consecutive draws, instead of one of each per draw.  RECOMP_METAL_BATCH=1.
 *
 * WHY THIS IS CORRECT, which is the only interesting part.
 *
 * Draw order.  Metal executes the commands in one render encoder in the order
 * they were encoded, so encoding draws back to back into a single encoder
 * preserves the order the guest emitted them in, exactly as committing one
 * command buffer each did.
 *
 * Overlapping fragments.  This is where a per-draw command buffer was doing
 * real work: it put a full barrier between draws, and the fragment shader here
 * needs one, because it implements the guest's blending, depth test and
 * stencil itself by reading the attachments back.  Inside a single encoder
 * there is no such barrier -- but there does not need to be one, because the
 * shader already declares both attachments raster_order_group(0) (see fs(),
 * above), and that is precisely the guarantee that fragments covering the same
 * pixel are read-modify-written in primitive order.  It was there before this
 * change and is what makes this change legal; without it, batching would be
 * wrong and would look right most of the time, which is worse.
 *
 * Staging lifetime.  Unchanged in substance: a slab is still pinned to the
 * command buffer that reads it, before that buffer is committed, and released
 * from its completion handler.  What changes is that one command buffer now
 * covers several draws, so a slab is pinned once per batch rather than once
 * per draw -- tracked in batch_pins so the in-flight count still matches the
 * number of command buffers that can read the slab, not the number of draws.
 *
 * Flushing.  The batch must be committed before anything reads what the GPU
 * has produced or invalidates what it is drawing into.  Every such site in the
 * pushbuffer executor -- readback at a flip, a clear, a capture, a surface
 * change, a rejected batch -- reaches the GPU through nv2a_metal_sync(), so
 * one flush at the top of that function covers all of them.  The other flush
 * is the ring wrap: the pins for the open batch have not been installed yet,
 * so a wrap that waited on a slab this batch is about to read would wait for a
 * command buffer that has not been committed and never will be.  Flushing
 * first turns that deadlock into an ordinary wait. */
/* Full clears that dropped the surface instead of syncing it. Each one is
 * a GPU drain and a 4.9 MB readback that did not happen. */
static id<MTLFunction> hw_vs, hw_fs;
static unsigned long long g_mtl_discards;
static id<MTLCommandBuffer> batch_command;
static id<MTLRenderCommandEncoder> batch_encoder;
static unsigned batch_draws, batch_pins;
static uint64_t batch_flushes, batch_draws_total, batch_longest;
static void batch_flush(void);
/* -1 forces off, 1 forces on, 0 defers to the switch. The frame benchmark
 * drives both paths inside one process, so it cannot use the environment. */
static int batch_force;
/* OPT-IN again as of 14 Sep 2026, after being on by default for one commit.
 *
 * It was turned on by default on the evidence below, and then a person playing
 * the title interactively got stuck on the SEGA screen. That is a report from
 * the only test that actually matters, and it is not something the scripted
 * runs saw -- a scripted boot on the same binary reaches NtOpenFile 1408 and
 * 61 scene nodes. So the default goes back to off until that is either
 * reproduced and fixed or shown to be something else. Nothing below is
 * retracted; a measured rendering win does not outrank a title that will not
 * boot for the user, and the switch costs nothing to keep.
 *
 * RECOMP_METAL_BATCH=1 enables it. What the evidence below establishes:
 *
 *   image      identical colour and depth to the per-draw path across the five
 *              boundaries metal_batch_test exercises -- overlapping blended and
 *              depth-tested draws, a staging ring wrap, texture-cache eviction,
 *              a render-target change, and interleaved invalidate/readback.
 *              Correctness for those cases, not in general.
 *   lifetime   metal_ring_test: pinning each slab once per batch protects
 *              staging memory as well as pinning per command buffer did, with
 *              a control that corrupts 2392 of 4000 when pinning is removed.
 *   speed      replaying one captured 492-draw frame through both paths, 25
 *              alternating trials: 69.2 ms to GPU completion per-draw against
 *              46.5 ms batched, distributions not overlapping. A 16-draw frame
 *              gives 11.1 against 8.8 ms, medians apart but tails overlapping.
 *              That is replay rendering performance. It is NOT a measured
 *              gameplay frame-rate gain -- the scripted runs never produced
 *              matched enough workloads to claim one.
 *   integration a 300 s gameplay run each way: no crash, no software fallback,
 *              no rejected draw, the same resident-memory growth (196 -> 297
 *              MB either way, so that growth is not this), input alive to the
 *              end, and APU trap counts that track which scene the run reached
 *              rather than which path it used.
 *   gameplay   15 Sep 2026, and this is the first measurement of it AT a
 *              scene-verified mission: ten boots on pad/gameplay_nobarrage.pad,
 *              four usable runs per arm, scored only over report windows at
 *              least 20 s into sequence state 30 (ab_switch.sh).
 *
 *                            per frame, mean of 4 runs      range
 *                clear       10.75 -> 8.88 ms          9.49-11.47 v 8.70-9.24
 *                submit       8.09 -> 7.73 ms          7.63-8.29  v 7.60-7.83
 *                vsh         10.55 -> 11.25 ms         9.14-11.54 v 10.67-12.65
 *                whole frame 33.53 -> 32.27 ms         30.23-35.13 v 31.49-33.77
 *
 *              THE CLEAR COLUMN IS THE ONLY ONE WHOSE RANGES DO NOT OVERLAP,
 *              and it is the biggest mover at 1.87 ms/frame. That is not a
 *              surprise once you read why: a clear reaches the GPU through
 *              nv2a_metal_sync(), so its cost is a drain, and batching changes
 *              how much is outstanding when the drain happens. submit moves the
 *              way batching predicts but only by 0.36 ms and its ranges
 *              overlap.
 *
 *              WHOLE-FRAME TIME DOES NOT SEPARATE THE ARMS. The 1.26 ms of
 *              mean difference is inside the run-to-run spread, because vsh --
 *              the CPU vertex interpreter, which batching does not touch --
 *              varies by 3.5 ms between runs on its own and swamps it. So the
 *              honest claim is a localised 2.2 ms/frame off clear+submit, not
 *              a measured frame-rate gain.
 *
 * THE DEFAULT STAYS OFF ANYWAY, and the numbers above do not bear on why. The
 * gate is the SEGA-screen hang: one interactive session, never reproduced, and
 * a person has to run this switch interactively to close it. Scripted boots
 * cannot -- there are seventeen of them now (twelve before, five here) and the
 * hang was in none of them, which is the same evidence it already had.
 */
static int batch_on(void)
{static int on=-1;if(batch_force)return batch_force>0;
 if(on<0){const char*e=getenv("RECOMP_METAL_BATCH");on=e?(atoi(e)!=0):0;}
 return on;}
/* A cap exists so the effect of unbounded batching can be told apart from the
 * effect of batching at all, and so a pathological scene cannot defer the GPU
 * for an arbitrarily long time.  0 means no cap; the natural bound is the
 * frame, because the flip syncs. */
static unsigned batch_cap(void)
{static int cap=-1;if(cap<0){const char*e=getenv("RECOMP_METAL_BATCH_MAX");cap=e?atoi(e):0;if(cap<0)cap=0;}return(unsigned)cap;}

/* GPU-side cost of the same command buffers, behind its OWN switch.
 *
 * The CPU numbers above are not the whole question and, measured, are not even
 * the big half: one command buffer per draw means one render pass per draw,
 * and a render pass over an RGBA32Float attachment pays a full tile load and a
 * full tile store whatever it draws. That cost is invisible to a CPU timer.
 *
 * Reading it needs addCompletedHandler on every command buffer, which the ring
 * comment measured at 0.20 us of producer-side cost -- small, but not nothing,
 * and it perturbs exactly what the create/encode/commit timers measure. So it
 * is a separate switch: take the CPU timing run clean, then take this one. */
static int mtl_cb_gpu_force;
static int mtl_cb_gpu(void)
{
    static int on = -1;
    if (mtl_cb_gpu_force) return mtl_cb_gpu_force > 0;
    if (on < 0) on = getenv("RECOMP_METAL_CB_GPU") ? 1 : 0;
    return on;
}
static _Atomic unsigned long long g_mtl_sched_ns, g_mtl_gpu_n;
static _Atomic unsigned long long g_mtl_span_ns, g_mtl_submit_seq;
static _Atomic unsigned long long g_mtl_out_of_order, g_mtl_overlapped, g_mtl_bad_stamp;
static pthread_mutex_t gpu_acc_mutex = PTHREAD_MUTEX_INITIALIZER;
static double gpu_first_start, gpu_last_end, gpu_coverage;
static unsigned long long gpu_prev_seq;
static pthread_t gpu_handler_thread[4];
static unsigned gpu_handler_threads;
static unsigned gpu_raw_printed;

/* WHAT THIS DOES AND DOES NOT MEASURE.
 *
 * The first version added up GPUEndTime - GPUStartTime across every command
 * buffer and reported 3,086 seconds inside a 300 second run. I described that
 * as queue wait. That was a guess: an impossible total says the counter is not
 * measuring what was assumed, and nothing about which assumption broke. Apple
 * documents these as the times GPU execution started and finished. So this
 * version reports the things that can actually be checked, and labels them:
 *
 *   span      the sum of end-start, which is what was wrong before. Kept, and
 *             named a span rather than time, because comparing it against the
 *             coverage below is how overlap shows up.
 *   coverage  elapsed time covered by the union of the intervals. Bounded by
 *             wall clock, so it can be sanity-checked -- but it is COVERAGE,
 *             not hardware occupancy: a GPU idle inside an interval still
 *             counts, and this says nothing about how busy the device was.
 *   overlap   intervals that began before the previous one ended, counted
 *             directly instead of inferred.
 *   order     handlers arriving out of submission order, and how many distinct
 *             threads deliver them -- both of which would break any sequential
 *             accumulation, including the coverage above.
 *   raw       the first few stamp pairs, printed in full, so the unit is read
 *             off the log rather than assumed from the header file.
 *
 * If overlap is nonzero the span is meaningless as a duration and the coverage
 * is the only figure worth quoting. If the handlers are out of order or on
 * several threads, the coverage is wrong too and needs a sort. */
static void mtl_cb_gpu_watch(id<MTLCommandBuffer> command)
{
    unsigned long long seq;
    if (!mtl_cb_gpu()) return;
    seq = atomic_fetch_add(&g_mtl_submit_seq, 1);
    [command addCompletedHandler:^(id<MTLCommandBuffer> done){
        double start = done.GPUStartTime, end = done.GPUEndTime;
        double sched = done.kernelEndTime - done.kernelStartTime;
        unsigned i;
        pthread_t self = pthread_self();
        if (!(end > start)) { atomic_fetch_add(&g_mtl_bad_stamp, 1); }
        pthread_mutex_lock(&gpu_acc_mutex);
        for (i = 0; i < gpu_handler_threads; ++i)
            if (pthread_equal(gpu_handler_thread[i], self)) break;
        if (i == gpu_handler_threads && gpu_handler_threads < 4)
            gpu_handler_thread[gpu_handler_threads++] = self;
        if (seq < gpu_prev_seq) atomic_fetch_add(&g_mtl_out_of_order, 1);
        gpu_prev_seq = seq;
        if (end > start) {
            if (gpu_raw_printed < 6) {
                fprintf(stderr, "  [METAL-CB] raw stamp %u: start=%.9f end=%.9f "
                                "delta=%.9f s  kernel=%.9f s\n",
                        gpu_raw_printed, start, end, end - start, sched);
                ++gpu_raw_printed;
            }
            if (gpu_first_start == 0.0) gpu_first_start = start;
            if (start < gpu_last_end) atomic_fetch_add(&g_mtl_overlapped, 1);
            else                      gpu_coverage += start - gpu_last_end > 0 ? 0.0 : 0.0;
            {   double from = start < gpu_last_end ? gpu_last_end : start;
                if (end > from) gpu_coverage += end - from; }
            if (end > gpu_last_end) gpu_last_end = end;
            atomic_fetch_add(&g_mtl_span_ns, (unsigned long long)((end - start) * 1e9));
        }
        pthread_mutex_unlock(&gpu_acc_mutex);
        if (sched > 0) atomic_fetch_add(&g_mtl_sched_ns, (unsigned long long)(sched * 1e9));
        atomic_fetch_add(&g_mtl_gpu_n, 1);
    }];
}

void nv2a_metal_cb_report(void)
{
    if (!mtl_cb_stats()) return;
    fprintf(stderr,
            "  [METAL-CB] cbufs=%llu frames=%llu per_frame=%.1f"
            " create=%.2fms encode=%.2fms commit=%.2fms (totals)\n",
            g_mtl_cbufs, g_mtl_frames,
            g_mtl_frames ? (double)g_mtl_cbufs / (double)g_mtl_frames : 0.0,
            g_mtl_ns_create / 1e6, g_mtl_ns_encode / 1e6, g_mtl_ns_commit / 1e6);
    if (batch_on())
        fprintf(stderr,
                "  [METAL-CB] batched: %llu flushes, %llu draws, %.1f draws/flush,"
                " longest %llu\n",
                batch_flushes, batch_draws_total,
                batch_flushes ? (double)batch_draws_total / (double)batch_flushes : 0.0,
                batch_longest);
    if (mtl_cb_gpu()) {
        unsigned long long n = atomic_load(&g_mtl_gpu_n);
        double cov, elapsed;
        pthread_mutex_lock(&gpu_acc_mutex);
        cov = gpu_coverage; elapsed = gpu_last_end - gpu_first_start;
        fprintf(stderr,
                "  [METAL-CB] gpu: %llu completed, span-sum %.0fms,"
                " coverage %.0fms of %.0fms elapsed (coverage is NOT occupancy),"
                " scheduling %.0fms; overlapped %llu, out-of-order %llu,"
                " bad-stamp %llu, handler threads %u\n",
                n, atomic_load(&g_mtl_span_ns) / 1e6, cov * 1e3, elapsed * 1e3,
                atomic_load(&g_mtl_sched_ns) / 1e6,
                atomic_load(&g_mtl_overlapped), atomic_load(&g_mtl_out_of_order),
                atomic_load(&g_mtl_bad_stamp), gpu_handler_threads);
        pthread_mutex_unlock(&gpu_acc_mutex);
    }
    fflush(stderr);
}
static uint8_t *surface_target,*depth_target;
static size_t surface_target_size,depth_target_size;
static uint32_t surface_width,surface_height,surface_pitch,depth_pitch;
static int surface_valid,surface_dirty,depth_valid,depth_dirty,attempted;
static pthread_mutex_t initialization_mutex=PTHREAD_MUTEX_INITIALIZER;
static const char *reject_reason;

#define TEXTURE_CACHE_SIZE 128
typedef struct {
    const uint8_t *source;
    size_t size;
    id<MTLBuffer> buffer;
    uint64_t stamp;
} TextureBuffer;
static TextureBuffer texture_cache[TEXTURE_CACHE_SIZE];
static id<MTLBuffer> dummy_buffer;
static uint64_t texture_clock,texture_requests,texture_hits,texture_uploads;
static uint64_t inline_vertex_batches,allocated_vertex_batches;

/* Opt-in clip audit (RECOMP_METAL_CLIP_AUDIT), read-only.
 *
 * It exists because the triangle counter cannot answer the question people
 * keep asking it. nv2a_metal_draw returns n/3 -- triangles assembled and
 * culled, counted BEFORE the encoder is created -- so the depth clip mode is
 * invisible to it, and an A/B of RECOMP_METAL_DEPTH_CLAMP on that number
 * measures run-to-run variation and nothing else.
 *
 * This recomputes, on the CPU, exactly the clip position the vertex shader
 * emits, and applies exactly Metal's clip volume (-w<=x<=w, -w<=y<=w,
 * 0<=z<=w) to it. A primitive is discarded whole only when all three vertices
 * fall outside one shared plane, so that is what `outside_*` counts. */
static uint64_t audit_tris,audit_out_near,audit_out_far,audit_out_side;
static uint64_t audit_verts,audit_w_neg,audit_w_zero,audit_w_nan;
static uint64_t audit_z_below,audit_z_above;
/* The decisive pair. `onscreen_lost` is a triangle all three of whose guest-
 * computed screen positions land inside the viewport -- geometry the title
 * transformed to pixels a viewer should see -- that the clip volume throws
 * away anyway. `wneg_lost` is how many of those have a negative w, which is
 * the only thing that can invert the clip test once z is clamped. */
static uint64_t audit_onscreen,audit_onscreen_lost,audit_wneg_lost;
/* And the number that decides it: on screen, discarded, and every w
 * positive -- so not behind the camera, and nothing about it justifies
 * the discard. Broken out by which plane did it. */
static uint64_t audit_clean_lost,audit_clean_near,audit_clean_side;
/* Assembly-stage losses. Everything above measures triangles that reached
 * the encoder; these never get that far, and the triangle counter cannot
 * see them either because it counts what survives. */
static uint64_t audit_asm_total,audit_asm_nonfinite,audit_asm_texq,
    audit_asm_degenerate,audit_asm_culled;
/* Splitting the degenerate bucket. A zero-area triangle with two identical
 * vertices is a triangle-strip stitch and is SUPPOSED to vanish. One with
 * three distinct vertices that are nevertheless collinear is geometry that
 * something flattened -- the 1/16-pixel quantisation in nv2a_pb_exec.c is the
 * candidate -- and `deg_lost` counts the ones that were on screen with every
 * w positive, so nothing else justifies dropping them. */
static uint64_t audit_deg_nonfinite,audit_deg_dup,audit_deg_collinear,audit_deg_lost;
/* And the raster state the cull decision is made from, as actually seen. */
static uint64_t audit_cull_none,audit_cull_back,audit_cull_front,audit_cull_both,
    audit_cull_other,audit_front_cw,audit_front_ccw;
/* The w-sign census, and the reason for it.
 *
 * area() runs on positions the GUEST already perspective-divided. Dividing
 * by a negative w negates x and y, so a triangle with an ODD number of
 * w<0 vertices comes out with its screen winding reversed, and the backface
 * test then answers the opposite of the truth.
 *
 * The orientation that does not lie is the sign of the 3x3 determinant of
 * the homogeneous coordinates. Writing x_clip = ndc_x * w, that determinant
 * factors exactly into (screen area) * w0*w1*w2 -- so the correct test is
 * the current one times sign(w0*w1*w2), and the current code is missing
 * that factor. `flip` counts the triangles where it changes the answer. */
static uint64_t audit_w_allpos,audit_w_mixed,audit_w_allneg;
static uint64_t audit_mixed_culled,audit_signflip,audit_signflip_culled;
static float audit_z_min=INFINITY,audit_z_max=-INFINITY;
static float audit_w_min=INFINITY,audit_w_max=-INFINITY;

static int clip_audit_on(void)
{static int on=-1;if(on<0)on=getenv("RECOMP_METAL_CLIP_AUDIT")?1:0;return on;}

/* The vertex shader, in C. Keep the arithmetic character-for-character the
 * same as the MSL above, or the audit reports a volume nobody rasterises. */
static void clip_position(const float p[4],unsigned w,unsigned h,float out[4])
{float z=p[2]<0?0:p[2]>16777215.0f?16777215.0f:p[2];z/=16777215.0f;
 out[0]=(p[0]/(float)w*2-1)*p[3];out[1]=(1-p[1]/(float)h*2)*p[3];
 out[2]=z*p[3];out[3]=p[3];}

static void clip_audit(const float(*v)[16][4],const unsigned*idx,unsigned n,
    unsigned w,unsigned h)
{
    for(unsigned t=0;t+2<n;t+=3){
        float c[3][4];int near_out=0,far_out=0,lo=0,ro=0,bo=0,to=0;
        for(unsigned k=0;k<3;k++){
            const float*p=v[idx[t+k]][0];
            clip_position(p,w,h,c[k]);
            ++audit_verts;
            if(!isfinite(p[3]))++audit_w_nan;
            else if(p[3]<0)++audit_w_neg;
            else if(p[3]==0)++audit_w_zero;
            if(p[2]<0)++audit_z_below;
            if(p[2]>16777215.0f)++audit_z_above;
            if(isfinite(p[2])){if(p[2]<audit_z_min)audit_z_min=p[2];
                               if(p[2]>audit_z_max)audit_z_max=p[2];}
            if(isfinite(p[3])){if(p[3]<audit_w_min)audit_w_min=p[3];
                               if(p[3]>audit_w_max)audit_w_max=p[3];}
            near_out+=!(c[k][2]>=0);far_out+=!(c[k][2]<=c[k][3]);
            lo+=!(c[k][0]>=-c[k][3]);ro+=!(c[k][0]<=c[k][3]);
            bo+=!(c[k][1]>=-c[k][3]);to+=!(c[k][1]<=c[k][3]);
        }
        ++audit_tris;
        int lost=near_out==3||far_out==3||lo==3||ro==3||bo==3||to==3;
        if(near_out==3)++audit_out_near;
        if(far_out==3)++audit_out_far;
        if(lo==3||ro==3||bo==3||to==3)++audit_out_side;
        int onscreen=1,wneg=0;
        for(unsigned k=0;k<3;k++){const float*p=v[idx[t+k]][0];
            if(!(p[0]>=0&&p[0]<=(float)w&&p[1]>=0&&p[1]<=(float)h))onscreen=0;
            if(p[3]<0)wneg=1;}
        if(onscreen){++audit_onscreen;if(lost)++audit_onscreen_lost;}
        if(lost&&wneg)++audit_wneg_lost;
        if(lost&&onscreen&&!wneg){++audit_clean_lost;
            if(near_out==3)++audit_clean_near;
            if(lo==3||ro==3||bo==3||to==3)++audit_clean_side;}
    }
}

static NSString *const shader =
@"#include <metal_stdlib>\n"
 "using namespace metal;\n"
 "struct Vertex { float4 p,d0,d1,t0,t1,t2,t3; };\n"
 "struct Params { uint width,height,dither,untextured,combiner_count,texture_mask,add_specular,alpha_test,alpha_ref,modulate,blend,blend_src,blend_dst,depth_test,depth_write,depth_func;"
 "  uint z_cull; float z_lo,z_hi;"
 " uint stencil_test,stencil_write,stencil_mask,stencil_ref,stencil_func_mask,stencil_func,stencil_fail,stencil_zfail,stencil_zpass;"
 " uint tw[4],th[4],pitch[4],linear[4],rgba8[4],dxt1[4],dxt3[4],repeat[4],levels[4],min_filter[4]; float lod_bias[4];"
 " uint color_icw[8]; uint alpha_icw[8]; uint color_ocw[8]; uint alpha_ocw[8]; };\n"
 "struct Out { float4 p [[position]]; float4 d0,d1,t0,t1,t2,t3; };\n"
 "struct Frag { float4 color [[color(0)]]; uint stencil [[color(1)]]; };\n"
 "vertex Out vs(uint id [[vertex_id]], const device Vertex *v [[buffer(0)]], constant Params &s [[buffer(1)]],const device uint*indices [[buffer(2)]]) {\n"
 /* No clamp here. The guest's z range and its out-of-range policy belong to
  * the fragment stage (see below); clamping before the perspective
  * multiply also corrupts the endpoints the hardware clipper interpolates
  * from, which is what made straddling ground polygons wrong even once
  * their winding was fixed. xemu does the same: normalise, never clamp. */
 " Vertex x=v[indices[id]];Out o;float4 p=x.p;float z=p.z/16777215.0f;"
 " o.p=float4((p.x/s.width*2-1)*p.w,(1-p.y/s.height*2)*p.w,z*p.w,p.w);\n"
 " o.d0=x.d0;o.d1=x.d1;o.t0=x.t0;o.t1=x.t1;o.t2=x.t2;o.t3=x.t3;return o; }\n"
 "uint morton(uint x,uint y,uint w,uint h) { uint index=0,bit=0;"
 " for(uint b=1;b<w||b<h;b<<=1) { if(b<w){if(x&b)index|=1u<<bit;bit++;}"
 " if(b<h){if(y&b)index|=1u<<bit;bit++;}} return index; }\n"
 "float4 texel(const device uchar *t,int2 p,uint u,uint base,uint w,uint h,uint pitch,constant Params&s){\n"
 " if(s.repeat[u]){p.x=(p.x%int(w)+int(w))%int(w);p.y=(p.y%int(h)+int(h))%int(h);}"
 " else p=clamp(p,int2(0),int2(w-1,h-1));"
 " uint at=base+(s.rgba8[u]?4*morton(uint(p.x),uint(p.y),w,h):s.dxt1[u]?uint(p.y/4)*pitch+uint(p.x/4)*8:s.dxt3[u]?uint(p.y/4)*pitch+uint(p.x/4)*16:uint(p.y)*pitch+uint(p.x)*2);\n"
 " if(s.rgba8[u])return float4(float(t[at+2]),float(t[at+1]),float(t[at]),float(t[at+3]))/255;"
 " if(s.dxt1[u]){uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);"
 " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*((p.y&3)*4+(p.x&3)))&3;"
 " uint c=pick?c1:c0;float4 a=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);if(pick<2)return a;"
 " if(c0<=c1&&pick==3)return float4(0);c=c0;float4 x=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);"
 " c=c1;float4 y=float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);float w=c0<=c1?.5f:(pick==2?2.0f/3.0f:1.0f/3.0f);return float4(x.rgb*w+y.rgb*(1-w),1);}"
 " if(s.dxt3[u]){uint i=uint(p.y&3)*4+uint(p.x&3),a=(uint(t[at+i/2])>>(4*(i&1)))&15;at+=8;"
 " uint c0=uint(t[at])|(uint(t[at+1])<<8),c1=uint(t[at+2])|(uint(t[at+3])<<8);"
 " uint pick=(uint(t[at+4])|(uint(t[at+5])<<8)|(uint(t[at+6])<<16)|(uint(t[at+7])<<24))>>(2*i)&3;"
 " uint c=pick?c1:c0;float3 x=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);if(pick<2)return float4(x,float(a)/15);"
 " c=c0;float3 r=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);c=c1;float3 b=float3(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31);"
 " float w=pick==2?2.0f/3.0f:1.0f/3.0f;return float4(r*w+b*(1-w),float(a)/15);}"
 " uint c=uint(t[at])|(uint(t[at+1])<<8);return float4(float(c>>11)/31,float((c>>5)&63)/63,float(c&31)/31,1);}\n"
 "float4 sample_level(const device uchar*t,float2 uv,uint u,uint level,bool linear,constant Params&s){"
 " uint base=0,w=s.tw[u],h=s.th[u],pitch=s.pitch[u];for(uint l=0;l<level;l++){"
 " base+=s.dxt1[u]?((w+3)/4)*((h+3)/4)*8:s.dxt3[u]?((w+3)/4)*((h+3)/4)*16:s.rgba8[u]?w*h*4:pitch*h;w=max(1u,w/2);h=max(1u,h/2);pitch=s.dxt1[u]?((w+3)/4)*8:s.dxt3[u]?((w+3)/4)*16:s.rgba8[u]?w*4:pitch;}"
 " if(s.rgba8[u]||s.dxt1[u]||s.dxt3[u]){if(s.repeat[u])uv-=floor(uv);else uv=clamp(uv,float2(0),float2(1));uv*=float2(w,h);}"
 " uv=clamp(uv,float2(0),float2(w,h));if(!linear)return texel(t,int2(floor(uv)),u,base,w,h,pitch,s);"
 " float2 p=uv-.5f,f=floor(p),fxy=p-f;int2 q=int2(f);"
 " return(texel(t,q,u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,0),u,base,w,h,pitch,s)*fxy.x)*(1-fxy.y)"
 " +(texel(t,q+int2(0,1),u,base,w,h,pitch,s)*(1-fxy.x)+texel(t,q+int2(1,1),u,base,w,h,pitch,s)*fxy.x)*fxy.y;}\n"
 "float4 sample_lod(const device uchar*t,float4 tc,uint u,constant Params&s){float2 uv=tc.xy/tc.w;"
 " float2 scale=float2(s.tw[u],s.th[u]);float lod=log2(max(0.000001f,max(length(dfdx(uv)*scale),length(dfdy(uv)*scale))));"
 " float l=max(0.0f,lod+s.lod_bias[u]);if(s.min_filter[u]<3||s.levels[u]<2)l=0;"
 " l=min(l,float(s.levels[u]-1));uint lo=s.min_filter[u]>=5?uint(floor(l)):uint(floor(l+.5f));"
 " uint hi=s.min_filter[u]>=5&&lo+1<s.levels[u]?lo+1:lo;bool linear=s.linear[u]!=0;"
 " if(lod+s.lod_bias[u]>0&&s.min_filter[u])linear=(s.min_filter[u]&1)==0;"
 " float4 a=sample_level(t,uv,u,lo,linear,s),b=hi==lo?a:sample_level(t,uv,u,hi,linear,s);"
 " return mix(a,b,hi==lo?0.0f:l-float(lo));}\n"
 "float input(uint code,uint channel,thread float4 *r){uint source=code&15;float x=r[source][(code&16)?3:channel];"
 " switch(code>>5){case 0:return max(0.0f,x);case 1:return 1-min(1.0f,max(0.0f,x));"
 " case 2:return 2*max(0.0f,x)-1;case 3:return 1-2*max(0.0f,x);"
 " case 4:return max(0.0f,x)-.5f;case 5:return .5f-max(0.0f,x);case 6:return x;default:return-x;}}\n"
 "bool cmpf(uint f,float a,float b){if(!f)f=0x203;switch(f){case 0x200:return false;case 0x201:return a<b;case 0x202:return a==b;case 0x203:return a<=b;case 0x204:return a>b;case 0x205:return a!=b;case 0x206:return a>=b;default:return true;}}\n"
 "bool cmpu(uint f,uint a,uint b){switch(f){case 0x200:return false;case 0x201:return a<b;case 0x202:return a==b;case 0x203:return a<=b;case 0x204:return a>b;case 0x205:return a!=b;case 0x206:return a>=b;default:return true;}}\n"
 "uint stop(uint op,uint old,uint ref){switch(op){case 0:return 0;case 0x1e01:return ref;case 0x1e02:return min(255u,old+1);case 0x1e03:return old?old-1:0;case 0x150a:return old^255;case 0x8507:return(old+1)&255;case 0x8508:return(old-1)&255;default:return old;}}\n"
 "uint stupd(uint old,uint op,constant Params&s){if(!s.stencil_write)return old;uint mask=s.stencil_mask&255,n=stop(op,old,s.stencil_ref&255);return(old&~mask)|(n&mask);}\n"
 /* Per-channel, because DST_COLOR is not a scalar. Mirrors blend_factor in
  * nv2a_texture_copy.c; the accept test there gates the set, so `default`
  * is unreachable and contributes nothing rather than standing in for
  * ONE_MINUS_SRC_ALPHA. `dst` here is colour only: this surface keeps the
  * 24-bit depth in its alpha channel, so there is no destination alpha. */
 "float3 bfactor(uint f,float4 src,float3 dst){switch(f){"
 " case 0x000:return float3(0);case 0x001:return float3(1);"
 " case 0x300:return src.rgb;case 0x301:return 1-src.rgb;"
 " case 0x302:return float3(src.a);case 0x303:return float3(1-src.a);"
 " case 0x306:return dst;case 0x307:return 1-dst;"
 " default:return float3(0);}}\n"
 /* TEXTURING AND THE REGISTER COMBINERS, SHARED BY BOTH FRAGMENT ENTRY POINTS.
  * Extracted so the software-state `fs` below and the hardware-state `fs_hw`
  * cannot drift: combiners are the part that genuinely belongs in a shader --
  * upstream's D3D11 backend puts them in one too -- while blending, depth and
  * stencil only live here because depth was packed into alpha. One copy, two
  * tails. */
 "float4 shade(Out i, const device uchar*t0, constant Params&s, const device uchar*t1, const device uchar*t2, const device uchar*t3){\n"
 " float4 d0=i.d0,d1=i.d1,c=float4(1),tex=float4(0);"
 " if(s.texture_mask&1)tex=sample_lod(t0,i.t0,0,s);"
 " if(s.combiner_count){float4 r[14];for(uint n=0;n<14;n++)r[n]=float4(0);r[4]=d0;r[5]=d1;r[8]=tex;"
 " if(s.texture_mask&2)r[9]=sample_lod(t1,i.t1,1,s);if(s.texture_mask&4)r[10]=sample_lod(t2,i.t2,2,s);if(s.texture_mask&8)r[11]=sample_lod(t3,i.t3,3,s);"
 " r[12].a=(s.texture_mask&1)?r[8].a:1;for(uint stage=0;stage<s.combiner_count;stage++){float4 ab,cd;"
 " for(uint k=0;k<4;k++){uint word=k==3?s.alpha_icw[stage]:s.color_icw[stage];uint ch=k==3?2:k;"
 " float a=input(word>>24,ch,r),b=input((word>>16)&255,ch,r),cc=input((word>>8)&255,ch,r),d=input(word&255,ch,r);"
 " ab[k]=a*b;cd[k]=cc*d;}for(uint k=0;k<4;k++){uint word=k==3?s.alpha_ocw[stage]:s.color_ocw[stage];"
 " uint dd=word&15,da=(word>>4)&15,ds=(word>>8)&15;if(dd)r[dd][k]=clamp(cd[k],-1.0f,1.0f);"
 " if(da)r[da][k]=clamp(ab[k],-1.0f,1.0f);if(ds)r[ds][k]=clamp(ab[k]+cd[k],-1.0f,1.0f);}}"
 " c=clamp(r[12]+(s.add_specular?float4(r[5].rgb,0):float4(0)),0.0f,1.0f);}"
 " else if(!s.untextured){c=tex;c.a=clamp(d0.a,0.0f,1.0f)*(s.modulate?c.a:1);if(s.modulate)c.rgb*=max(float3(0),d0.rgb);}"
 " return c;}\n"
 /* THE HARDWARE-STATE ENTRY POINT.
  *
  * Same colour as fs(), then nothing. No destination read, so no
  * raster_order_group and no serialisation of overdrawn pixels; no depth test
  * and no stencil, because a real Depth32Float_Stencil8 attachment and an
  * MTLDepthStencilState do both; no blend, because the pipeline's blend
  * descriptor does it.
  *
  * WHAT STAYS, and why each one has to:
  *   z range   NV097_SET_ZMIN_MAX_CONTROL asks for CULL or CLAMP outside
  *             SET_CLIP_MIN/MAX. CULL is a per-fragment discard that no fixed
  *             function expresses, so it stays. It costs early-Z on the
  *             fragments that reach it, which is a real cost and is the first
  *             thing to measure if this path disappoints.
  *   alpha     Metal has no fixed-function alpha test at all.
  *   dither    the guest's own ordered dither, matching the 16-bit target.
  *
  * The depth VALUE needs no work here: vs() already emits o.p.z = z*p.w with
  * z = guest_z/16777215, so after the hardware divide the rasteriser has
  * exactly the guest's normalised depth, which is what the attachment stores
  * and what MTLCompareFunction compares. That is why this change does not
  * touch the vertex stage. */
 "fragment float4 fs_hw(Out i [[stage_in]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]]){\n"
 " float4 c=shade(i,t0,s,t1,t2,t3);"
 /* discard_fragment() does NOT return in MSL -- execution continues and the
  * write is dropped at the end -- so each discard returns explicitly. The
  * returned value is immaterial; the explicit return is what stops the rest
  * of the shader running for a fragment that is already gone. */
 " float zg=i.p.z*16777215.0f;"
 " if(s.z_cull&&(zg<s.z_lo||zg>s.z_hi)){discard_fragment();return c;}"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref){discard_fragment();return c;}"
 /* Byte-for-byte the dither fs() uses. It is per-channel because the guest
  * target is RGB565 and the quantisation step differs between green and the
  * other two; a uniform bias would dither green twice as hard. */
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}"
 /* THE SHADED ALPHA, not 1. The blend unit takes its SRC_ALPHA and
  * ONE_MINUS_SRC_ALPHA factors from this value, where the software path took
  * them from c.a inside bfactor(). Returning 1 here silently turned every
  * alpha-blended draw into an opaque one. The software path could get away
  * with writing something else into alpha only because it had already done the
  * blend itself by that point -- and it wrote depth there, which is the whole
  * reason this path exists. */
 /* NO CHANNEL SWAP, and that was worth measuring rather than reasoning about.
  *
  * Metal's only packed 16-bit target is named B5G6R5Unorm, which reads as
  * blue-in-the-high-bits where the guest's RGB565 puts red there. A swap was
  * written on that reading and scored 38060 of 65536 pixels wrong at up to 27
  * channel steps -- not a rounding difference, channels in the wrong place.
  * Removing it: 697 pixels, worst error ONE step. Metal's packed-format names
  * run from the least significant bits up, so B5G6R5 already IS the guest's
  * layout and the attachment can hold its words verbatim. */
 " return float4(c.rgb,clamp(c.a,0.0f,1.0f));}\n"
 /* The software-state path: everything the hardware is not being allowed to
  * do. Unchanged in behaviour; it just calls shade() for its colour now. */
 "fragment Frag fs(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],uint stencil [[color(1),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]]){\n"
 " Frag o;o.color=dst;o.stencil=stencil;float4 c=shade(i,t0,s,t1,t2,t3);"
 /* The guest's depth-range policy, per fragment, in guest z units.
  * NV097_SET_ZMIN_MAX_CONTROL selects discard (CULL) or saturate (CLAMP)
  * outside SET_CLIP_MIN/MAX. JSRF asks for CULL. */
 " float zg=i.p.z*16777215.0f;"
 " if(s.z_cull){ if(zg<s.z_lo||zg>s.z_hi) return o; }"
 " else zg=clamp(zg,s.z_lo,s.z_hi);"
 " float zn=clamp(zg/16777215.0f,0.0f,1.0f);"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref)return o;"
 " uint mask=s.stencil_func_mask&255;if(s.stencil_test&&!cmpu(s.stencil_func,s.stencil_ref&mask,stencil&mask)){o.stencil=stupd(stencil,s.stencil_fail,s);return o;}"
 " if(s.depth_test&&!cmpf(s.depth_func,zn,dst.a)){if(s.stencil_test)o.stencil=stupd(stencil,s.stencil_zfail,s);return o;}"
 " if(s.blend){float3 d=dst.rgb;c.rgb=c.rgb*bfactor(s.blend_src,c,d)+d*bfactor(s.blend_dst,c,d);}"
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}o.color=float4(c.rgb,s.depth_write?zn:dst.a);if(s.stencil_test)o.stencil=stupd(stencil,s.stencil_zpass,s);return o;}\n";

static int initialize(void)
{
    pthread_mutex_lock(&initialization_mutex);
    if(attempted){int ready=pipeline!=nil;pthread_mutex_unlock(&initialization_mutex);return ready;}
    device=MTLCreateSystemDefaultDevice();if(!device){attempted=1;pthread_mutex_unlock(&initialization_mutex);return 0;}
    NSError *error=nil;MTLCompileOptions *options=[MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if(@available(macOS 15.0,*)) options.mathMode=MTLMathModeSafe; else options.fastMathEnabled=NO;
#pragma clang diagnostic pop
    id<MTLLibrary> library=[device newLibraryWithSource:shader options:options error:&error];
    if(library){MTLRenderPipelineDescriptor *desc=[MTLRenderPipelineDescriptor new];
        desc.vertexFunction=[library newFunctionWithName:@"vs"];desc.fragmentFunction=[library newFunctionWithName:@"fs"];
        /* Retained for the hardware-state pipelines, which are built lazily
         * per distinct blend state rather than once here. */
        hw_vs=[library newFunctionWithName:@"vs"];hw_fs=[library newFunctionWithName:@"fs_hw"];
        desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA32Float;
        desc.colorAttachments[1].pixelFormat=MTLPixelFormatR8Uint;
        pipeline=[device newRenderPipelineStateWithDescriptor:desc error:&error];
        queue=[device newCommandQueue];}
    attempted=1;
    if(!pipeline||!queue){pipeline=nil;fprintf(stderr,"[METAL] initialization failed: %s\n",error.description.UTF8String);pthread_mutex_unlock(&initialization_mutex);return 0;}
    fprintf(stderr,"[METAL] native raster pipeline ready: %s\n",device.name.UTF8String);pthread_mutex_unlock(&initialization_mutex);return 1;
}

/* ============================================================
 * Hardware render state: pipelines and depth/stencil objects
 * ============================================================
 *
 * Metal bakes blending into the pipeline state object and depth/stencil into a
 * separate immutable object, so neither can be set per draw the way the guest
 * sets its registers. Both are therefore cached on the guest state that
 * produces them. The caches are tiny and linear because a title uses a handful
 * of distinct render states, not thousands: JSRF's own combiner trace reports
 * three distinct configurations across a whole frame.
 *
 * REFUSAL, NOT SUBSTITUTION. If any field fails to translate, these return nil
 * and the caller falls back to the software-state path for that draw. A
 * pipeline built from a guessed blend factor draws the frame with the wrong
 * equation and reports nothing; falling back is visibly slower and correct.
 * The counters below say how often it happens so "the hardware path is on"
 * never silently means "for some of the draws". */
_Static_assert(MTLBlendFactorZero==NV2A_MTL_BLEND_ZERO
    && MTLBlendFactorOne==NV2A_MTL_BLEND_ONE
    && MTLBlendFactorSourceColor==NV2A_MTL_BLEND_SRC_COLOR
    && MTLBlendFactorOneMinusSourceColor==NV2A_MTL_BLEND_ONE_MINUS_SRC_COLOR
    && MTLBlendFactorSourceAlpha==NV2A_MTL_BLEND_SRC_ALPHA
    && MTLBlendFactorOneMinusSourceAlpha==NV2A_MTL_BLEND_ONE_MINUS_SRC_ALPHA
    && MTLBlendFactorDestinationAlpha==NV2A_MTL_BLEND_DST_ALPHA
    && MTLBlendFactorOneMinusDestinationAlpha==NV2A_MTL_BLEND_ONE_MINUS_DST_ALPHA
    && MTLBlendFactorDestinationColor==NV2A_MTL_BLEND_DST_COLOR
    && MTLBlendFactorOneMinusDestinationColor==NV2A_MTL_BLEND_ONE_MINUS_DST_COLOR
    && MTLBlendFactorSourceAlphaSaturated==NV2A_MTL_BLEND_SRC_ALPHA_SATURATED,
    "nv2a_metal_state.h blend constants must equal the MTLBlendFactor values");
_Static_assert(MTLCompareFunctionNever==NV2A_MTL_CMP_NEVER
    && MTLCompareFunctionLess==NV2A_MTL_CMP_LESS
    && MTLCompareFunctionEqual==NV2A_MTL_CMP_EQUAL
    && MTLCompareFunctionLessEqual==NV2A_MTL_CMP_LESS_EQUAL
    && MTLCompareFunctionGreater==NV2A_MTL_CMP_GREATER
    && MTLCompareFunctionNotEqual==NV2A_MTL_CMP_NOT_EQUAL
    && MTLCompareFunctionGreaterEqual==NV2A_MTL_CMP_GREATER_EQUAL
    && MTLCompareFunctionAlways==NV2A_MTL_CMP_ALWAYS,
    "nv2a_metal_state.h compare constants must equal MTLCompareFunction");
_Static_assert(MTLStencilOperationKeep==NV2A_MTL_STENCIL_KEEP
    && MTLStencilOperationZero==NV2A_MTL_STENCIL_ZERO
    && MTLStencilOperationReplace==NV2A_MTL_STENCIL_REPLACE
    && MTLStencilOperationIncrementClamp==NV2A_MTL_STENCIL_INCR_CLAMP
    && MTLStencilOperationDecrementClamp==NV2A_MTL_STENCIL_DECR_CLAMP
    && MTLStencilOperationInvert==NV2A_MTL_STENCIL_INVERT
    && MTLStencilOperationIncrementWrap==NV2A_MTL_STENCIL_INCR_WRAP
    && MTLStencilOperationDecrementWrap==NV2A_MTL_STENCIL_DECR_WRAP,
    "nv2a_metal_state.h stencil constants must equal MTLStencilOperation");

unsigned long long g_hw_pipeline_misses, g_hw_state_refusals;

/* The real depth and stencil attachments, and the guest RAM they mirror.
 *
 * This is the pair that frees the colour attachment's alpha channel, which is
 * what the whole hardware-state path is for: with depth somewhere else there
 * is a destination alpha again, so blending can go back to the blend unit, and
 * depth and stencil can go back to the depth unit, and the fragment shader
 * stops reading its own attachments through a raster order group.
 *
 * Guest depth is D24S8: byte 0 stencil, bytes 1..3 the 24-bit depth, little
 * endian -- the same layout nv2a_metal_sync already writes back today, just
 * read out of alpha instead of out of here. */
static id<MTLTexture> hw_depth_tex, hw_stencil_tex;
static int batch_encoder_hw;
unsigned long long g_hw_draws;

/* MEASURED, and it is the first frame-time win this renderer has produced.
 *
 *     RECOMP_METAL_HW=0   31.56  31.78  34.62 ms   (mean 32.65, n=3)
 *     RECOMP_METAL_HW=1   28.83  30.20        ms   (mean 29.52, n=2)
 *     ranges do not overlap: 9.6% less frame time
 *
 * and the column that moved is the one the mechanism predicts:
 *
 *     clear   9.61 / 10.25 / 11.12 ms   ->   5.76 ms
 *
 * A clear's cost here is mostly waiting for the GPU to drain. Without
 * raster_order_group(0) the overdrawn fragments no longer serialise, so there
 * is less outstanding work to wait for. submit and vsh did not move, which is
 * what should happen: this change touches neither.
 *
 * The arms were verified distinct rather than assumed -- [METAL] hw draws=0
 * against 742443 and 773039, five pipelines, ZERO refusals, so every draw took
 * this path and no state had to fall back.
 *
 * STILL OFF BY DEFAULT. One run of six faulted, and although it never left the
 * title screen (scene=12, ord175=9859 -- the USB driver had already stalled)
 * so the renderer was barely running in it, and although the pre-existing
 * fault rate in the OHCI path is ~11.8%, one fault at n=3 is not evidence
 * either way. A default flip here wants a person playing it, the same gate
 * RECOMP_METAL_BATCH is still waiting on. */
static int hw_state_on(void);

/* A true RGB565 colour attachment: two bytes a pixel against sixteen.
 *
 * It is what xemu allocates for a 565 guest surface on both of its backends
 * (GL_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16) and what the software rasteriser
 * already blends at, so it is the faithful choice rather than merely the cheap
 * one -- and it removes the double rounding a wider attachment forces, because
 * the attachment IS the guest format. Upload and readback stop converting and
 * become 16-bit copies.
 *
 * Only available on the hardware path: the legacy one still packs depth into
 * the colour alpha, and this format has no alpha at all. That is also why it
 * costs nothing here -- DST_ALPHA factors are already refused by the shared
 * accept test, and SRC_ALPHA comes from the fragment's own alpha, not the
 * attachment's.
 *
 * Verified renderable, blendable and CPU-readable on this device before being
 * written, rather than assumed.
 *
 * MEASURED, pooled over two rounds, both arms on the hardware path so this
 * isolates the format alone:
 *
 *     RECOMP_METAL_565=0   29.63 - 33.55 ms   (n=5, mean 31.87)
 *     RECOMP_METAL_565=1   26.73 - 27.54 ms   (n=3, mean 27.23)
 *     ranges do not overlap: 14.5% less frame time
 *
 * and again the columns that moved are the ones the mechanism names:
 *
 *     submit  9.33 -> 6.03 ms      the per-pixel upload conversion, deleted
 *     sync    0.73 -> 0.28 ms      the per-pixel readback conversion, deleted
 *
 * Both become 16-bit row copies because the attachment is the guest's own
 * format. vsh moved 9.89 -> 11.72 in the same pair, which is noise: nothing
 * here touches the vertex stage.
 *
 * RECOMP_METAL_565=1. Still default off, with RECOMP_METAL_HW, pending a
 * person playing it. */
static int hw_565_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_METAL_565");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on && hw_state_on();
}

/* !!! THE IMAGE IS WRONG AT A REAL MISSION. DO NOT DEFAULT THIS ON. !!!
 *
 * 15 Sep 2026, reported by a person watching the game: a flickering grid across
 * the background, with solid black rectangles appearing and disappearing frame
 * to frame. Reproduced in a framebuffer dump from a scene-verified mission with
 * RECOMP_METAL_HW=1 RECOMP_METAL_565=1: torn rectangular tiles carrying content
 * from elsewhere in the scene, and a large black band.
 *
 * It is not draws being dropped. That run reports hw draws=900426, refusals=0,
 * 0 software fallbacks, 0 texture rejects, [VSH] rejected=45 of 239005. Every
 * draw took this path and succeeded. The surface CONTENTS are wrong.
 *
 * AND metal_hw_check.sh PASSES. That gate scores a synthetic 40-draw test
 * against the software rasteriser and says 1 pixel differs by 1 step. So the
 * gate does not exercise whatever this is.
 *
 * The frame-time numbers recorded below are real measurements OF A
 * CONFIGURATION THAT RENDERS THE GAME WRONG. They are not a reason to ship it.
 * I reported them as wins before ever looking at a frame from a real run, which
 * is the hole in the method: ab_switch.sh scores frame time, scene and crashes,
 * and nothing scores the image.
 *
 * BOTH SUSPECTS RECORDED HERE HAVE SINCE BEEN TESTED AND NEITHER SURVIVED.
 * Reviewed 15 Sep 2026; see
 * docs/jsrf/handovers/REVIEW_RESPONSE_2026-09-15_THE_GATE_NEVER_TESTED_DEPTH.txt.
 *
 *   The multi-surface swap. metal_batch_test now draws 120 triangles across
 *   THREE alternating surfaces and scores them against the rasteriser, and
 *   every arm -- software state, hardware state, hardware+565 -- lands on the
 *   same 29 pixels and the same 109 depth bytes. The retained-surface
 *   machinery is shared by both paths and behaves identically in both. (The
 *   note that "the synthetic test changes surface once" was wrong: phase D
 *   already alternated two targets forty times. What was missing was a SCORE,
 *   because the only oracle comparison was phase A, which never swaps.)
 *
 *   setDepthClipMode:MTLDepthClipModeClamp. Not speculative and not
 *   hardware-path-only: it is applied unconditionally to the encoder below,
 *   before the hw branch, and has been since the 13 Sep depth-clipping
 *   retraction. The call inside the hw branch is a second call on the same
 *   encoder object with the same value. Reverting it changes nothing, and
 *   reverting the unconditional one changes both paths equally, so neither can
 *   explain a difference between them.
 *
 * What the gate still does not exercise, and a mission does: register
 * combiners, alpha test, stencil, multi-texture. Look there. */
static int hw_state_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_METAL_HW");
        on = e ? (atoi(e) != 0) : 0;
    }
    return on;
}

/* Guest D24S8 -> the two attachments. Returns 0 if either texture could not be
 * made, and the caller falls back; a half-populated depth buffer would render
 * a plausible wrong image, which is worse than being slow. */
static int hw_depth_upload(const uint8_t *zram, unsigned w, unsigned h,
                           unsigned pitch)
{
    size_t px = (size_t)w * h;
    float *dep = malloc(px * sizeof *dep);
    uint8_t *ste = malloc(px);
    if (!dep || !ste) { free(dep); free(ste); return 0; }
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        size_t at = (size_t)y * w + x;
        if (zram) {
            const uint8_t *z = zram + (size_t)y * pitch + x * 4;
            uint32_t q = (uint32_t)z[1] | (uint32_t)z[2] << 8 | (uint32_t)z[3] << 16;
            dep[at] = (float)q / 16777215.0f;
            ste[at] = z[0];
        } else { dep[at] = 1.0f; ste[at] = 0; }
    }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModeShared;
    hw_depth_tex = [device newTextureWithDescriptor:dd];
    MTLTextureDescriptor *sd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8
        width:w height:h mipmapped:NO];
    sd.usage = MTLTextureUsageRenderTarget; sd.storageMode = MTLStorageModeShared;
    hw_stencil_tex = [device newTextureWithDescriptor:sd];
    if (!hw_depth_tex || !hw_stencil_tex) {
        hw_depth_tex = nil; hw_stencil_tex = nil;
        free(dep); free(ste); return 0;
    }
    [hw_depth_tex replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0
        withBytes:dep bytesPerRow:w * sizeof *dep];
    [hw_stencil_tex replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0
        withBytes:ste bytesPerRow:w];
    free(dep); free(ste);
    return 1;
}

/* And back, in the guest's own layout, so nothing downstream can tell which
 * path produced it. */
static void hw_depth_readback(uint8_t *zram, unsigned w, unsigned h,
                              unsigned pitch)
{
    if (!zram || !hw_depth_tex || !hw_stencil_tex) return;
    size_t px = (size_t)w * h;
    float *dep = malloc(px * sizeof *dep);
    uint8_t *ste = malloc(px);
    if (!dep || !ste) { free(dep); free(ste); return; }
    [hw_depth_tex getBytes:dep bytesPerRow:w * sizeof *dep
        fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    [hw_stencil_tex getBytes:ste bytesPerRow:w
        fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        size_t at = (size_t)y * w + x;
        uint32_t q = (uint32_t)((double)fminf(1.0f, fmaxf(0.0f, dep[at]))
                                * 16777215.0 + 0.5);
        uint8_t *p = zram + (size_t)y * pitch + x * 4;
        p[0] = ste[at]; p[1] = (uint8_t)q; p[2] = (uint8_t)(q >> 8);
        p[3] = (uint8_t)(q >> 16);
    }
    free(dep); free(ste);
}

#define HW_CACHE 16
static struct { uint32_t blend,src,dst; id<MTLRenderPipelineState> pso; }
    hw_pso[HW_CACHE];
static unsigned hw_pso_n;

static id<MTLRenderPipelineState> hw_pipeline_for(const NV2ATextureCopy *s)
{
    unsigned i;
    for (i = 0; i < hw_pso_n; ++i)
        if (hw_pso[i].blend == s->blend && hw_pso[i].src == s->blend_src
            && hw_pso[i].dst == s->blend_dst) return hw_pso[i].pso;
    if (hw_pso_n >= HW_CACHE) { ++g_hw_state_refusals; return nil; }

    int sf = MTLBlendFactorOne, df = MTLBlendFactorZero;
    if (s->blend) {
        sf = nv2a_metal_blend_factor(s->blend_src);
        df = nv2a_metal_blend_factor(s->blend_dst);
        if (sf < 0 || df < 0) { ++g_hw_state_refusals; return nil; }
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = hw_vs; d.fragmentFunction = hw_fs;
    /* STILL RGBA32Float, AND THE REASON RECORDED HERE BEFORE WAS WRONG.
     *
     * That comment said the software rasteriser prefers float because float
     * keeps blend intermediates exact. It does not keep anything exact: it
     * reads its blend destination out of guest RAM with unpack565() and stores
     * RGB565 back after every pixel of every draw (nv2a_texture_copy.c:691-711).
     * It is a framebuffer-precision blender, and always was. xemu is the same
     * on both of its backends -- GL_RGB565 and VK_FORMAT_R5G6B5_UNORM_PACK16,
     * a true two-byte attachment -- and blends there with the GPU's own unit.
     *
     * So float is the one candidate NOBODY models: it is the only format that
     * never requantises between draws, which is the single thing the NV2A
     * certainly does. If intermediate precision drove the ranking below, RGBA8
     * would be CLOSEST to the oracle and float furthest. It is the exact
     * inverse, so that ranking measures double rounding at the readback
     * boundary, not blending.
     *
     * Both narrower formats were built and scored against the oracle:
     *
     *     RGBA32Float      1 of 65536 pixels differ
     *     RGBA16Unorm     24
     *     RGBA8Unorm    5705
     *
     * The initial RGB565 round trip is exact in all three -- the cost is
     * INTERMEDIATE. Overlapping blended draws read the attachment back and
     * blend again, and every such step requantises at the attachment's
     * precision, where the float target kept them exact. At 8 bits that is
     * plainly visible; at 16 it is 24 pixels of tie-break rounding.
     *
     * It also scored the wrong quantity. A COUNT cannot be acted on: this
     * project admitted the D3D11 backend under "matches the CPU rasteriser to
     * within one RGB565 channel step", with 48% of pixels differing on a
     * single unblended draw. metal_batch_test now reports worst-step as well,
     * and the next move is to re-score the narrow formats by magnitude.
     *
     * The format that should win is neither: MTLPixelFormatB5G6R5Unorm, two
     * bytes a pixel, an 8x cut, no double rounding at all because the
     * attachment IS the guest format -- and it is exactly what xemu allocates.
     * It is only available on this path, because the legacy one still needs
     * alpha for depth. */
    d.colorAttachments[0].pixelFormat = hw_565_on() ? MTLPixelFormatB5G6R5Unorm
                                                    : MTLPixelFormatRGBA32Float;
    /* SEPARATE depth and stencil textures, not a combined format. Measured on
     * this host before choosing: Depth32Float, Stencil8 and
     * Depth32Float_Stencil8 all accept MTLStorageModeShared and all accept
     * replaceRegion/getBytes on Apple Silicon -- so the combined format buys
     * nothing and separate ones keep the guest's D24S8 unpack trivial, with no
     * blit encoder and no MTLBlitOptionDepthFromDepthStencil dance to move
     * bytes in and out of a private texture. */
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    if (s->blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        /* The guest's only blend equation here is ADD; nv2a_texture_copy's
         * accept test rejects anything else before a draw reaches us. */
        d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)sf;
        d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)df;
        d.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)sf;
        d.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)df;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) { ++g_hw_state_refusals; return nil; }
    ++g_hw_pipeline_misses;
    hw_pso[hw_pso_n].blend = s->blend; hw_pso[hw_pso_n].src = s->blend_src;
    hw_pso[hw_pso_n].dst = s->blend_dst; hw_pso[hw_pso_n].pso = pso;
    ++hw_pso_n;
    return pso;
}

static struct { uint32_t key[9]; id<MTLDepthStencilState> dss; }
    hw_dss[HW_CACHE];
static unsigned hw_dss_n;

static id<MTLDepthStencilState> hw_depth_state_for(const NV2ATextureCopy *s)
{
    uint32_t k[9] = { s->depth_test, s->depth_write, s->depth_func,
                      s->stencil_test, s->stencil_write, s->stencil_mask,
                      s->stencil_func, s->stencil_fail, s->stencil_zpass };
    unsigned i;
    for (i = 0; i < hw_dss_n; ++i)
        if (!memcmp(hw_dss[i].key, k, sizeof k)) return hw_dss[i].dss;
    if (hw_dss_n >= HW_CACHE) { ++g_hw_state_refusals; return nil; }

    /* An unwritten NV097_SET_DEPTH_FUNC reads 0, and the shader's cmpf treats
     * that as LEQUAL. The hardware path has to agree or a title that never
     * wrote the register would depth-test differently on the two paths. */
    uint32_t func = s->depth_func ? s->depth_func : NV2A_GUEST_DEPTH_FUNC_DEFAULT;
    int cmp = s->depth_test ? nv2a_metal_compare_func(func)
                            : NV2A_MTL_CMP_ALWAYS;
    /* AN UNRECOGNISED COMPARE BECOMES ALWAYS, because that is precisely what
     * the shader this path replaces does: cmpf()'s switch ends in
     * "default: return true". Mirroring it is not guessing -- it is the whole
     * requirement, because the two paths have to produce the same image before
     * either can be preferred, and a path that refuses where the other passes
     * is a different renderer, not a faster one.
     *
     * It is reachable. metal_batch_test sets depth_func = 4, a D3D-style
     * D3DCMP_LESSEQUAL that the NV2A never emits -- its own encoding is the
     * 0x200 range -- and the shader has been treating it as ALWAYS ever since.
     * Whether that default should be `true` at all is a real question about
     * the software path, with its own evidence to gather; it is not a question
     * this commit gets to answer by quietly diverging. */
    if (cmp < 0) { ++g_hw_state_refusals; cmp = NV2A_MTL_CMP_ALWAYS; }

    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = (MTLCompareFunction)cmp;
    d.depthWriteEnabled = s->depth_write ? YES : NO;
    if (s->stencil_test || s->stencil_write) {
        int sc = s->stencil_test ? nv2a_metal_compare_func(s->stencil_func)
                                 : NV2A_MTL_CMP_ALWAYS;
        int fail = nv2a_metal_stencil_op(s->stencil_fail);
        int pass = nv2a_metal_stencil_op(s->stencil_zpass);
        int zfail = nv2a_metal_stencil_op(s->stencil_zfail);
        /* Same rule, same reason: stop()'s switch ends in "default: return
         * old", which is KEEP. */
        if (sc < 0) { ++g_hw_state_refusals; sc = NV2A_MTL_CMP_ALWAYS; }
        if (fail < 0) { ++g_hw_state_refusals; fail = NV2A_MTL_STENCIL_KEEP; }
        if (pass < 0) { ++g_hw_state_refusals; pass = NV2A_MTL_STENCIL_KEEP; }
        if (zfail < 0) { ++g_hw_state_refusals; zfail = NV2A_MTL_STENCIL_KEEP; }
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = (MTLCompareFunction)sc;
        sd.stencilFailureOperation = (MTLStencilOperation)fail;
        sd.depthStencilPassOperation = (MTLStencilOperation)pass;
        sd.depthFailureOperation = (MTLStencilOperation)zfail;
        sd.readMask = s->stencil_func_mask & 255;
        sd.writeMask = s->stencil_write ? (s->stencil_mask & 255) : 0;
        d.frontFaceStencil = sd; d.backFaceStencil = sd;
    }
    id<MTLDepthStencilState> dss = [device newDepthStencilStateWithDescriptor:d];
    if (!dss) { ++g_hw_state_refusals; return nil; }
    memcpy(hw_dss[hw_dss_n].key, k, sizeof k);
    hw_dss[hw_dss_n].dss = dss; ++hw_dss_n;
    return dss;
}

static id<MTLBuffer> texture_buffer(const uint8_t *data,size_t size)
{
    if(!size) {
        if(!dummy_buffer){uint8_t zero=0;dummy_buffer=[device newBufferWithBytes:&zero length:1 options:MTLResourceStorageModeShared];}
        return dummy_buffer;
    }
    ++texture_requests;
    TextureBuffer *slot=NULL,*oldest=&texture_cache[0];
    for(unsigned i=0;i<TEXTURE_CACHE_SIZE;i++) {
        TextureBuffer *entry=&texture_cache[i];
        if(entry->source==data&&entry->size==size) {
            slot=entry;
            if(entry->buffer&&!memcmp(entry->buffer.contents,data,size)) {
                entry->stamp=++texture_clock;++texture_hits;return entry->buffer;
            }
            break;
        }
        if(!entry->buffer){if(!slot)slot=entry;}
        else if(entry->stamp<oldest->stamp)oldest=entry;
    }
    if(!slot)slot=oldest;
    id<MTLBuffer> buffer=[device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    if(!buffer)return nil;
    slot->source=data;slot->size=size;slot->buffer=buffer;slot->stamp=++texture_clock;++texture_uploads;
    return buffer;
}

/* Vertex and index staging: one ring of persistent slabs, not a driver
 * allocation per batch.
 *
 * The old code asked Metal for a fresh MTLBuffer every time a batch exceeded
 * the 4 KB setVertexBytes inline limit, and another one for the indices past
 * 4 KB. A 170 s gameplay run reported 304,980 of those against 65,630 inline
 * batches -- five batches in six paying for a driver allocation, a page table
 * update and, at release, a free, on the one thread the guest is waiting on.
 * nv2a_metal_draw was the heaviest non-idle entry in a CPU profile of that
 * run after the thread-local accessor.
 *
 * So: RING_SLABS persistent buffers of RING_SLAB_BYTES each, sub-allocated by
 * a bump pointer and bound with setVertexBuffer:offset:. Both halves of a
 * batch are reserved in one contiguous span, so a batch never straddles two
 * slabs and one in-flight count per batch is enough. A batch is bounded:
 * nv2a_metal_draw rejects count>4096 and a Vertex is 112 bytes, so vertices
 * cost at most 448 KB, and indices are bounded by the 12288-entry assembly
 * array at 48 KB. Half a megabyte, worst case, into a two-megabyte slab.
 *
 * WHY THIS IS SAFE AGAINST IN-FLIGHT GPU WORK -- the part that has to be
 * right. A command buffer is committed per draw and nothing waits on it until
 * the next nv2a_metal_sync, so at any moment several committed command
 * buffers may still be reading vertices. The bump pointer only moves forward
 * inside a slab, so bytes handed to one batch are never touched again while
 * that slab is current; the only way to reach them a second time is to wrap
 * round to that slab, and the wrap is gated. Every command buffer that reads
 * a slab increments that slab's in-flight count before it is committed and
 * decrements it from addCompletedHandler; a wrap blocks on a condition
 * variable until the count of the slab it is about to reuse reads zero. A
 * slab's contents are therefore overwritten only after Metal has told us that
 * every command buffer which referenced it has finished.
 *
 * Note what this deliberately does NOT assume: that command buffers committed
 * to one queue complete in commit order. nv2a_metal_sync already leans on
 * that when it waits for last_command alone and then reads the surface back,
 * but a wrong guess there costs a stale frame, whereas a wrong guess here is
 * a batch rasterised from half-overwritten vertices -- intermittent, scene
 * dependent, and certain to be blamed on something else a month later. An
 * atomic increment and a completion block per draw buy the assumption away --
 * 0.20 us against the 4.9-8.2 us the allocation they replace was measured to
 * cost -- so it is bought.
 *
 * That argument was not left as an argument. A verbatim copy of ring_reserve
 * and ring_pin, driven through 20,000 blit command buffers that each read
 * their own reservation back, reports zero corrupted reservations; with the
 * pinning removed and the ring squeezed to two slabs the same test reports
 * 147. The positive control is the point: the test can fail.
 *
 * Writing the CPU side of a shared-storage buffer while the GPU reads a
 * different range of the same buffer is the ordinary dynamic-buffer pattern
 * and needs no explicit synchronisation here; what needs synchronising is the
 * range, and the in-flight count is what protects the range.
 *
 * Reserving is single-threaded. nv2a_metal_draw is only ever reached from the
 * pusher thread -- the same assumption the texture cache, last_command and
 * all of the surface state already make -- so ring_current and ring_offset
 * are plain statics. The in-flight counts are the exception, because the
 * completion handler runs on a Metal-owned thread, so those are atomics and
 * the mutex below exists only to carry the sleep.
 *
 * If a slab cannot be allocated the reserve fails and the caller falls back
 * to the old per-batch allocation, which is slow but always correct. */
#define RING_SLABS 8
#define RING_SLAB_BYTES (2u<<20)
#define RING_ALIGN 256u
#define RING_ALIGN_UP(n) (((n)+RING_ALIGN-1)&~(size_t)(RING_ALIGN-1))

static id<MTLBuffer> ring_slab[RING_SLABS];
/* How many of the slabs are in use. RING_SLABS in every normal run; the ring
 * self-test squeezes it to two, because a wrap that takes 36 draws to come
 * round is a wrap the GPU has always finished with by the time it matters, and
 * a hazard that cannot be reached cannot be tested for. */
static unsigned ring_limit = RING_SLABS;
static void (*ring_wrap_hook)(void);
static unsigned ring_current;
static size_t ring_offset;
static pthread_mutex_t ring_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond=PTHREAD_COND_INITIALIZER;
/* Sequentially consistent by default, and deliberately so: the lost-wakeup
 * argument in ring_pin below is stated in terms of one total order over these
 * four operations, and a weaker order would not support it. Measured, they
 * are not what this costs -- a committed-and-pinned empty command buffer runs
 * 0.20 us slower than a bare one on this machine, essentially all of it the
 * heap copy of the completion block. Taking ring_mutex on the producer side
 * measured free; taking it inside the completion handler on every draw did
 * not, because that thread then contends with the draw thread, which is the
 * whole reason the handler only reaches for the lock when somebody is
 * actually asleep on it. */
static _Atomic unsigned ring_inflight[RING_SLABS];
static _Atomic unsigned ring_waiters;
/* Opt-in accounting (RECOMP_METAL_RING_AUDIT), read-only. The increments are
 * unconditional because they cost what the existing inline/allocated vertex
 * counters cost -- nothing measurable next to a draw -- but the report line
 * is gated, so a normal run's output does not change. */
static uint64_t ring_reserves,ring_bytes,ring_wraps,ring_waits,ring_fallbacks,ring_slabs_live;
static uint64_t sync_calls,sync_clean,sync_color,sync_depth,surface_uploads;
/* Nanoseconds inside nv2a_metal_sync, split: waiting for the GPU to drain
 * versus reading the surface back and converting it. See the comment at the
 * wait. */
static uint64_t g_sync_drain_ns, g_sync_read_ns;

static int ring_audit_on(void)
{static int on=-1;if(on<0)on=getenv("RECOMP_METAL_RING_AUDIT")?1:0;return on;}

/* Reserve `bytes` of slab storage. Returns the slab to bind, the byte offset
 * to bind it at, a CPU pointer to fill, and which slab was used so the caller
 * can pin it to the command buffer. Returns nil if the ring cannot serve the
 * request at all, and the caller must then allocate privately. */
static id<MTLBuffer> ring_reserve(size_t bytes,size_t*offset_out,void**cpu_out,unsigned*slab_out)
{
    if(!bytes||bytes>RING_SLAB_BYTES){++ring_fallbacks;return nil;}
    size_t need=RING_ALIGN_UP(bytes);
    if(ring_offset+need>RING_SLAB_BYTES){
        unsigned next=(ring_current+1u)%ring_limit;
        ++ring_wraps;
        /* Before looking at the next slab's in-flight count: see the flushing
         * paragraph above. An open batch holds unpinned reservations.
         *
         * A batch therefore NEVER spans a wrap, which is what makes pinning a
         * slab once per batch sufficient. The hook lets the ring self-test
         * reproduce that ordering instead of testing a pattern the draw path
         * cannot produce. */
        batch_flush();
        if (ring_wrap_hook) ring_wrap_hook();
        /* Announce before looking, so a handler that drains the slab after we
         * have looked and before we sleep is guaranteed to see us and shout.
         * It takes ring_mutex to shout and we hold it from the look to the
         * sleep, so the shout cannot slip through the gap either. */
        atomic_fetch_add(&ring_waiters,1);
        pthread_mutex_lock(&ring_mutex);
        if(atomic_load(&ring_inflight[next])){
            ++ring_waits;
            while(atomic_load(&ring_inflight[next]))pthread_cond_wait(&ring_cond,&ring_mutex);
        }
        pthread_mutex_unlock(&ring_mutex);
        atomic_fetch_sub(&ring_waiters,1);
        ring_current=next;ring_offset=0;
    }
    id<MTLBuffer>slab=ring_slab[ring_current];
    if(!slab){
        slab=[device newBufferWithLength:RING_SLAB_BYTES options:MTLResourceStorageModeShared];
        if(!slab){++ring_fallbacks;return nil;}
        ring_slab[ring_current]=slab;++ring_slabs_live;
    }
    *offset_out=ring_offset;*cpu_out=(uint8_t*)slab.contents+ring_offset;*slab_out=ring_current;
    ring_offset+=need;++ring_reserves;ring_bytes+=need;
    return slab;
}

/* Pin a slab to a command buffer for as long as the GPU may read it. Must be
 * called before commit, so the handler cannot be installed on an already
 * finished buffer and miss its own decrement. */
static void ring_pin(id<MTLCommandBuffer>command,unsigned slab)
{
    atomic_fetch_add(&ring_inflight[slab],1);
    [command addCompletedHandler:^(id<MTLCommandBuffer>done){(void)done;
        /* The lock is only for the sleeping case. Suppose a reserve is about
         * to wrap onto this slab and sees a nonzero count: in the single total
         * order over these sequentially consistent operations, its read of the
         * count precedes the decrement that empties the slab, and its earlier
         * announcement precedes that read -- so this load of ring_waiters,
         * which follows the decrement, must see the announcement, and the
         * broadcast happens. The sleeper holds ring_mutex from its read of the
         * count until pthread_cond_wait releases it, so a broadcast can never
         * land in between. */
        if(atomic_fetch_sub(&ring_inflight[slab],1)==1&&atomic_load(&ring_waiters)){
            pthread_mutex_lock(&ring_mutex);
            pthread_cond_broadcast(&ring_cond);
            pthread_mutex_unlock(&ring_mutex);}}];
}

static void batch_flush(void)
{
    if(!batch_encoder)return;
    [batch_encoder endEncoding];
    for(unsigned i=0;i<RING_SLABS;i++)if(batch_pins&(1u<<i))ring_pin(batch_command,i);
    mtl_cb_gpu_watch(batch_command);
    [batch_command commit];
    last_command=batch_command;
    if(batch_draws>batch_longest)batch_longest=batch_draws;
    batch_draws_total+=batch_draws;++batch_flushes;
    batch_command=nil;batch_encoder=nil;batch_pins=0;batch_draws=0;
}

void nv2a_metal_report(void)
{
    fprintf(stderr,"[METAL] sync %llu calls (%llu already clean): %.1f ms draining"
            " the GPU, %.1f ms reading back and converting."
            "  A resident clear could remove the second only.\n",
            (unsigned long long)sync_calls,(unsigned long long)sync_clean,
            g_sync_drain_ns/1e6,g_sync_read_ns/1e6);
    /* A full clear discards the surface instead of syncing it, so each of
     * these is one drain and one 4.9 MB readback that did not happen. Printed
     * with the state of the switch so an A/B can see the arms differ. */
    /* Draws that actually took the hardware path, against the states it had to
     * refuse. "The switch is on" and "the draws used it" are different facts,
     * and a mixed frame -- some draws writing depth to the attachment, the
     * rest to the colour alpha -- reads as a depth bug rather than as a
     * fallback, which is exactly how this was first misread. */
    fprintf(stderr,"[METAL] hw draws=%llu pipelines=%llu refusals=%llu (metal_hw %s)\n",
            (unsigned long long)g_hw_draws,
            (unsigned long long)g_hw_pipeline_misses,
            (unsigned long long)g_hw_state_refusals,
            hw_state_on()?"on":"OFF");
    /* The colour attachment's format, named so an A/B can verify its arms
     * differ rather than assume the environment took. ab_score.py harvests
     * this; it could not for RECOMP_METAL_565 and said so. */
    fprintf(stderr,"[METAL] colour attachment: %s\n",
            hw_565_on()?"B5G6R5Unorm (metal_565 on)"
                       :"RGBA32Float (metal_565 OFF)");
    fprintf(stderr,"[METAL] clear discards=%llu (clear_discard %s)\n",
            (unsigned long long)g_mtl_discards,
            g_mtl_discards?"used":"unused");
    fprintf(stderr,"[METAL] texture buffers: %llu requests, %llu cache hits, %llu uploads; vertices: %llu inline, %llu allocated\n",
        (unsigned long long)texture_requests,(unsigned long long)texture_hits,
        (unsigned long long)texture_uploads,(unsigned long long)inline_vertex_batches,
        (unsigned long long)allocated_vertex_batches);
    if(ring_audit_on())
        fprintf(stderr,"[METAL] ring audit: %llu reservations, %llu MiB staged, "
            "%llu slabs live, %llu wraps of which %llu had to wait, %llu fallbacks "
            "to a private allocation | syncs: %llu calls, %llu already clean, "
            "%llu colour read-backs, %llu depth read-backs; %llu surface re-uploads\n",
            (unsigned long long)ring_reserves,(unsigned long long)(ring_bytes>>20),
            (unsigned long long)ring_slabs_live,(unsigned long long)ring_wraps,
            (unsigned long long)ring_waits,(unsigned long long)ring_fallbacks,
            (unsigned long long)sync_calls,(unsigned long long)sync_clean,
            (unsigned long long)sync_color,(unsigned long long)sync_depth,
            (unsigned long long)surface_uploads);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: %llu triangles submitted, discarded whole "
            "by near=%llu far=%llu side=%llu | %llu vertices, w<0=%llu w==0=%llu "
            "w=NaN=%llu, z<0=%llu z>max=%llu, z in [%g %g], w in [%g %g]\n",
            (unsigned long long)audit_tris,(unsigned long long)audit_out_near,
            (unsigned long long)audit_out_far,(unsigned long long)audit_out_side,
            (unsigned long long)audit_verts,(unsigned long long)audit_w_neg,
            (unsigned long long)audit_w_zero,(unsigned long long)audit_w_nan,
            (unsigned long long)audit_z_below,(unsigned long long)audit_z_above,
            audit_z_min,audit_z_max,audit_w_min,audit_w_max);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: %llu triangles fully on screen, of which "
            "%llu discarded by the clip volume; %llu of all discards have w<0\n",
            (unsigned long long)audit_onscreen,(unsigned long long)audit_onscreen_lost,
            (unsigned long long)audit_wneg_lost);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: %llu discarded with every w>0 and every "
            "vertex on screen (near=%llu side=%llu)\n",
            (unsigned long long)audit_clean_lost,(unsigned long long)audit_clean_near,
            (unsigned long long)audit_clean_side);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: %llu triangles assembled, dropped before "
            "the encoder: nonfinite=%llu texcoord-q=%llu degenerate=%llu culled=%llu\n",
            (unsigned long long)audit_asm_total,(unsigned long long)audit_asm_nonfinite,
            (unsigned long long)audit_asm_texq,(unsigned long long)audit_asm_degenerate,
            (unsigned long long)audit_asm_culled);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: degenerate split: nonfinite-area=%llu "
            "strip-stitch(duplicate vertex)=%llu collinear-distinct=%llu, of which "
            "%llu were on screen with every w>0\n",
            (unsigned long long)audit_deg_nonfinite,(unsigned long long)audit_deg_dup,
            (unsigned long long)audit_deg_collinear,(unsigned long long)audit_deg_lost);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: cull state seen: none=%llu front=%llu "
            "back=%llu both=%llu other=%llu | front_cw=%llu front_ccw=%llu\n",
            (unsigned long long)audit_cull_none,(unsigned long long)audit_cull_front,
            (unsigned long long)audit_cull_back,(unsigned long long)audit_cull_both,
            (unsigned long long)audit_cull_other,(unsigned long long)audit_front_cw,
            (unsigned long long)audit_front_ccw);
    if(clip_audit_on())
        fprintf(stderr,"[METAL] clip audit: w sign per triangle: all-positive=%llu "
            "MIXED=%llu all-negative=%llu | mixed culled=%llu | odd-negative=%llu "
            "of which culled ONLY because the winding is inverted=%llu\n",
            (unsigned long long)audit_w_allpos,(unsigned long long)audit_w_mixed,
            (unsigned long long)audit_w_allneg,(unsigned long long)audit_mixed_culled,
            (unsigned long long)audit_signflip,(unsigned long long)audit_signflip_culled);
}

typedef struct{float p[4],d0[4],d1[4],t[4][4];}Vertex;
typedef struct{uint32_t width,height,dither,untextured,combiner_count,texture_mask,add_specular,alpha_test,alpha_ref,modulate,blend,blend_src,blend_dst,depth_test,depth_write,depth_func;
    uint32_t z_cull; float z_lo,z_hi;
    uint32_t stencil_test,stencil_write,stencil_mask,stencil_ref,stencil_func_mask,stencil_func,stencil_fail,stencil_zfail,stencil_zpass;
    uint32_t tw[4],th[4],pitch[4],linear[4],rgba8[4],dxt1[4],dxt3[4],repeat[4],levels[4],min_filter[4];
    float lod_bias[4];
    uint32_t color_icw[8],alpha_icw[8],color_ocw[8],alpha_ocw[8];}Params;

/* Does the ring actually protect staging memory from the GPU?
 *
 * This exercises the REAL ring_reserve and ring_pin, not a copy of them. An
 * earlier version of this evidence was a throwaway harness that was never
 * committed, so the claim in the comment above -- 20,000 blit command buffers,
 * zero corrupted reservations, 147 with the pinning removed -- could not be
 * re-run by anyone, including by me when batching changed how pinning works.
 * A copy would drift from the original; a hook does not.
 *
 * Each iteration reserves a span, stamps every word of it with the iteration
 * number, and blits it into its own slot of a destination buffer WITHOUT
 * waiting. Only at the end is everything waited on and checked, so a slab
 * handed back while the GPU was still reading it shows up as the wrong stamp.
 *
 * pin_mode is the point: 0 pins nothing (the positive control -- this MUST
 * corrupt, or the test is measuring nothing), 1 pins per command buffer (what
 * the per-draw path does), 2 pins each slab once from a bitmask (what batching
 * does, and the thing that previously had no test at all).
 *
 * The wrap hook matters for mode 2. In the draw path a batch can never span a
 * wrap, because ring_reserve flushes before it waits; without reproducing that
 * ordering the test would drive a pattern the real code cannot produce, and
 * would report a hazard that does not exist. */
static id<MTLCommandBuffer> st_cmd;
static id<MTLBlitCommandEncoder> st_blit;
static unsigned st_pins, st_in_batch, st_pin_mode;

static void selftest_flush(void)
{
    if (!st_cmd) return;
    [st_blit endEncoding];
    if (st_pin_mode == 2) {
        unsigned b;
        for (b = 0; b < RING_SLABS; ++b) if (st_pins & (1u << b)) ring_pin(st_cmd, b);
    }
    [st_cmd commit];
    last_command = st_cmd;
    st_cmd = nil; st_blit = nil; st_pins = 0; st_in_batch = 0;
}

int nv2a_metal_ring_selftest(unsigned slabs, int pin_mode, unsigned iters,
                             unsigned per_batch, unsigned *corrupt_out)
{
    const size_t span = 64u * 1024u;       /* 32 to a 2 MB slab */
    unsigned saved_limit = ring_limit, corrupt = 0, i;
    id<MTLBuffer> dst;
    int failed = 0;

    if (!initialize()) return 0;
    if (slabs < 2 || slabs > RING_SLABS || !iters || !per_batch) return 0;
    dst = [device newBufferWithLength:span * iters options:MTLResourceStorageModeShared];
    if (!dst) return 0;
    memset(dst.contents, 0, span * iters);

    ring_limit = slabs;
    ring_current = 0; ring_offset = 0;
    st_cmd = nil; st_blit = nil; st_pins = 0; st_in_batch = 0; st_pin_mode = (unsigned)pin_mode;
    ring_wrap_hook = selftest_flush;

    for (i = 0; i < iters; ++i) {
        size_t at = 0; void *cpu = NULL; unsigned slot = 0;
        id<MTLBuffer> slab;
        uint32_t *w; size_t n, k;
        if (!st_cmd) {
            st_cmd = [queue commandBuffer];
            st_blit = st_cmd ? [st_cmd blitCommandEncoder] : nil;
            if (!st_cmd || !st_blit) { failed = 1; break; }
        }
        /* Reserve AFTER the command buffer exists, so a wrap inside the
         * reservation flushes a real open batch, as it does in the draw path. */
        slab = ring_reserve(span, &at, &cpu, &slot);
        if (!slab) { failed = 1; break; }
        if (!st_cmd) {                      /* the wrap hook flushed us */
            st_cmd = [queue commandBuffer];
            st_blit = st_cmd ? [st_cmd blitCommandEncoder] : nil;
            if (!st_cmd || !st_blit) { failed = 1; break; }
        }
        w = (uint32_t *)cpu; n = span / 4;
        for (k = 0; k < n; ++k) w[k] = 0xA5000000u | i;
        [st_blit copyFromBuffer:slab sourceOffset:at
                       toBuffer:dst destinationOffset:(size_t)i * span size:span];
        if (pin_mode == 1) ring_pin(st_cmd, slot);
        else if (pin_mode == 2) st_pins |= 1u << slot;
        if (++st_in_batch >= per_batch) selftest_flush();
    }
    selftest_flush();
    ring_wrap_hook = NULL;
    [last_command waitUntilCompleted];

    if (!failed) {
        const uint32_t *w = (const uint32_t *)dst.contents;
        for (i = 0; i < iters; ++i) {
            size_t base = (size_t)i * span / 4, k;
            for (k = 0; k < span / 4; ++k)
                if (w[base + k] != (0xA5000000u | i)) { ++corrupt; break; }
        }
    }
    ring_limit = saved_limit;
    ring_current = 0; ring_offset = 0;
    if (corrupt_out) *corrupt_out = failed ? 0xFFFFFFFFu : corrupt;
    return 1;
}

int nv2a_metal_sync(void)
{
    unsigned long long _t_sync = mtl_now_ns(), _t_drained = 0;
    @autoreleasepool{
        ++sync_calls;
        /* BEFORE the dirty test and before the wait. An open batch is work the
         * GPU has not been told about, so last_command would be the previous
         * committed buffer and waiting on it would read a surface that is
         * missing every draw in the batch. */
        batch_flush();
        if(!surface_dirty&&!depth_dirty){++sync_clean;
            g_sync_drain_ns += mtl_now_ns()-_t_sync; return 1;}
        [last_command waitUntilCompleted];
        /* The split that decides whether a resident clear is worth building.
         *
         * clear_surface pays 11-15 ms a frame, and all of it is attributed to
         * `clear` because that is where the stall lands -- but the wait above
         * is a drain of command buffers the DRAWS submitted, and no clear
         * implementation can remove that. What a resident clear WOULD remove
         * is everything below: the readback, the two per-pixel conversion
         * loops, and the re-upload the invalidate forces on the next draw.
         *
         * So time the two halves separately. If the readback half is large,
         * the case is arithmetic. If the drain is nearly all of it, the clear
         * is only paying for the draws and the only available win is
         * pipelining, which is a different piece of work. Reported as a
         * breakdown OF the clear, never added to it. */
        _t_drained = mtl_now_ns();
        g_sync_drain_ns += _t_drained - _t_sync;
        if(last_command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"[METAL] command failed: %s\n",last_command.error.description.UTF8String);return 0;}
        size_t pixels=(size_t)surface_width*surface_height;
        float *rgba=malloc(pixels*16);
        if(!rgba)return 0;
        int fmt565=hw_565_on();
        [surface getBytes:rgba bytesPerRow:surface_width*(fmt565?2:16) fromRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0];
        if(surface_dirty&&fmt565) {
            /* Straight back out, no conversion and no rounding -- which is the
             * whole point: a wider attachment has to round twice, here and
             * again at the 565 grid. */
            ++sync_color;
            const uint16_t*w16=(const uint16_t*)rgba;
            for(unsigned y=0;y<surface_height;++y)
                memcpy(surface_target+(size_t)y*surface_pitch,w16+(size_t)y*surface_width,(size_t)surface_width*2);
            surface_dirty=0;
        } else if(surface_dirty) {
            ++sync_color;
            for(unsigned y=0;y<surface_height;++y) for(unsigned x=0;x<surface_width;++x) {
                size_t at=((size_t)y*surface_width+x)*4;
                unsigned c=(unsigned)(fminf(1,fmaxf(0,rgba[at]))*31+.5f)<<11|(unsigned)(fminf(1,fmaxf(0,rgba[at+1]))*63+.5f)<<5|(unsigned)(fminf(1,fmaxf(0,rgba[at+2]))*31+.5f);
                uint8_t*p=surface_target+(size_t)y*surface_pitch+x*2;
                p[0]=c;p[1]=c>>8;
            }
            surface_dirty=0;
        }
        /* Hardware path: depth and stencil live in their own attachments, so
         * they come back from there rather than out of the colour texture's
         * alpha. Written in the guest's own D24S8 layout either way, so
         * nothing downstream can tell which path produced the frame. */
        if(depth_dirty&&depth_target&&hw_state_on()&&hw_depth_tex) {
            ++sync_depth;
            hw_depth_readback(depth_target,surface_width,surface_height,depth_pitch);
            depth_dirty=0;
        } else if(depth_dirty&&depth_target) {
            ++sync_depth;
            uint8_t *stencil=malloc(pixels);
            if(!stencil){free(rgba);return 0;}
            [stencil_surface getBytes:stencil bytesPerRow:surface_width fromRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0];
            for(unsigned y=0;y<surface_height;++y) for(unsigned x=0;x<surface_width;++x) {
                size_t at=((size_t)y*surface_width+x)*4;
                uint32_t q=(uint32_t)((double)fminf(1,fmaxf(0,rgba[at+3]))*16777215.0+0.5);
                uint8_t*p=depth_target+(size_t)y*depth_pitch+x*4;
                p[0]=stencil[(size_t)y*surface_width+x];p[1]=q;p[2]=q>>8;p[3]=q>>16;
            }
            free(stencil);
            depth_dirty=0;
        }
        free(rgba);
        g_sync_read_ns += mtl_now_ns()-_t_drained;
        return 1;}
}

/* DROP THE RETAINED SURFACE WITHOUT READING IT BACK.
 *
 * nv2a_metal_invalidate syncs first, because in general guest RAM has to
 * receive whatever the GPU rendered before the CPU is allowed to write there.
 * A FULL clear is the one case where that is provably pointless: the CPU is
 * about to overwrite every byte of the region with a constant, so every byte
 * the readback writes is dead on arrival.
 *
 * clear_surface's own CPU loops run over clip_w x clip_h, which is exactly the
 * region this surface covers, so coverage is not in question -- the only
 * question is whether BOTH halves are being cleared, because colour and depth
 * share one RGBA32Float texture here (depth is packed into alpha). A
 * colour-only clear still needs depth preserved and vice versa, so the caller
 * only reaches this when the guest cleared both.
 *
 * What is skipped, per clear: [last_command waitUntilCompleted], a 4.9 MB
 * getBytes, and two 307k-pixel conversion loops. A sampling profile put 12.8%
 * and 14.5% of the rendering thread under one clear_surface path, "most of it
 * waiting for a command buffer or in the readback" -- this is that cost, and
 * for a full clear it buys nothing.
 *
 * No wait is needed before releasing the textures: Metal keeps a texture alive
 * for any command buffer still referencing it, so committing the open batch
 * and dropping our reference is safe. dirty is cleared too, so a later sync
 * cannot try to read back a surface that has been abandoned. */

void nv2a_metal_discard(void)
{
    @autoreleasepool{
        batch_flush();
        ++g_mtl_discards;
        surface_valid=depth_valid=0;
        surface_dirty=depth_dirty=0;
    }
}

void nv2a_metal_invalidate(uint8_t *target)
{if(!target||target==surface_target||target==depth_target){nv2a_metal_sync();surface_valid=depth_valid=0;}}
const char *nv2a_metal_last_reject(void){return reject_reason?reject_reason:"none";}
static int reject(const char *reason){reject_reason=reason;nv2a_metal_sync();surface_valid=depth_valid=0;return-1;}
static float area(const float a[4],const float b[4],const float c[4])
{return(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);}
static int vertex_valid(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned i)
{for(unsigned k=0;k<4;k++)if(!isfinite(v[i][0][k])||!isfinite(v[i][3][k])||!isfinite(v[i][4][k]))return 0;
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){for(unsigned k=0;k<4;k++)if(!isfinite(v[i][9+u][k]))return 0;if(v[i][9+u][3]<=0)return 0;}return 1;}
/* Same predicate, split so a rejection can name itself: 1 non-finite
 * position/colour, 2 texture coordinate q<=0 or non-finite, 0 valid. */
static int vertex_why(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned i)
{for(unsigned k=0;k<4;k++)if(!isfinite(v[i][0][k])||!isfinite(v[i][3][k])||!isfinite(v[i][4][k]))return 1;
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){for(unsigned k=0;k<4;k++)if(!isfinite(v[i][9+u][k]))return 2;if(v[i][9+u][3]<=0)return 2;}return 0;}
static void triangle(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned*out,unsigned*n,unsigned a,unsigned b,unsigned c)
{int audit=clip_audit_on();if(audit){++audit_asm_total;
    switch(s->cull_face){case 0:++audit_cull_none;break;case 0x404:++audit_cull_front;break;
    case 0x405:++audit_cull_back;break;case 0x408:++audit_cull_both;break;
    default:++audit_cull_other;}
    if(s->front_cw)++audit_front_cw;else++audit_front_ccw;}
 if(!vertex_valid(s,v,a)||!vertex_valid(s,v,b)||!vertex_valid(s,v,c)){
    if(audit){int w=vertex_why(s,v,a);if(!w)w=vertex_why(s,v,b);if(!w)w=vertex_why(s,v,c);
              if(w==2)++audit_asm_texq;else++audit_asm_nonfinite;}
    return;}
 float ar=area(v[a][0],v[b][0],v[c][0]);if(!isfinite(ar)||ar==0){
    if(audit){++audit_asm_degenerate;
        if(!isfinite(ar))++audit_deg_nonfinite;
        else{const float*pa=v[a][0],*pb=v[b][0],*pc=v[c][0];
            int dup=(pa[0]==pb[0]&&pa[1]==pb[1])||(pb[0]==pc[0]&&pb[1]==pc[1])
                   ||(pa[0]==pc[0]&&pa[1]==pc[1]);
            if(dup)++audit_deg_dup;
            else{++audit_deg_collinear;
                int on=1,wn=0;const float*q[3]={pa,pb,pc};
                for(int k=0;k<3;k++){if(!(q[k][0]>=0&&q[k][0]<=(float)s->clip_w
                        &&q[k][1]>=0&&q[k][1]<=(float)s->clip_h))on=0;
                    if(q[k][3]<0)wn=1;}
                if(on&&!wn)++audit_deg_lost;}}}
    return;}
 int front=nv2a_texture_copy_front_facing(s,ar,v[a][0][3],v[b][0][3],v[c][0][3]);
 int culled=nv2a_texture_copy_culled(s,front);
 if(audit){const float*pa=v[a][0],*pb=v[b][0],*pc=v[c][0];
    int neg=(pa[3]<0)+(pb[3]<0)+(pc[3]<0);
    if(!neg)++audit_w_allpos;else if(neg==3)++audit_w_allneg;else{++audit_w_mixed;if(culled)++audit_mixed_culled;}
    if(neg&1){++audit_signflip;
        int front2=((ar>0)^1)==(s->front_cw!=0);
        int culled2=s->cull_face==0x408||(s->cull_face==0x404&&front2)||(s->cull_face==0x405&&!front2);
        if(culled&&!culled2){++audit_signflip_culled;
            /* The geometry itself, for the first few. A count says how big the
             * population is; these say what the population IS, and whether it
             * looks like the large near-camera ground quads the symptom
             * points at. Screen x,y are pixels; w is the guest's oPos.w. */
            static unsigned shown;
            if(shown<6){++shown;
                fprintf(stderr,"  [METAL] winding-flip example %u: area=%.1f cull=0x%X front_cw=%u\n",
                    shown,ar,s->cull_face,s->front_cw);
                const float*q[3]={pa,pb,pc};
                for(int k=0;k<3;k++)
                    fprintf(stderr,"  [METAL]   v%d x=%9.2f y=%9.2f z=%14.1f w=%12.4f\n",
                        k,q[k][0],q[k][1],q[k][2],q[k][3]);
                fflush(stderr);}}}}
 if(culled){if(audit)++audit_asm_culled;return;}out[(*n)++]=a;out[(*n)++]=b;out[(*n)++]=c;}

/* Replay one captured frame through both submission paths and time them.
 *
 * WHY THIS EXISTS. The live A/B could not settle whether batching makes frames
 * faster, and more runs were not going to fix it. Every playthrough emits the
 * same first few reports and then follows one of a small number of paths, so
 * runs are not draws from one distribution; and the periodic reports are spaced
 * by wall clock, so as soon as two runs differ in speed -- the thing being
 * measured -- report i of one covers a different slice of the scene from report
 * i of the other. Matching on scene object counts was too loose: two reports in
 * one bucket were 22% apart in triangles. Matching on triangles per frame
 * answers a weaker question than it looks like, because triangle count is not
 * rendering cost -- overdraw, texture access, blending and the number of draws
 * all vary independently of it. And matching on a RATE would be worse again: a
 * rate depends on how fast the frame ran, so matching it discards the effect.
 *
 * So: capture one real frame -- every draw, its state, its texture bytes, its
 * vertices, and the colour and depth contents those draws start from -- and
 * replay that identical input through per-draw and batched submission. Same
 * commands, same resources, same starting pixels, alternating trials. The only
 * thing left different is the submission path.
 *
 * What is timed: the CPU cost of issuing the frame, and the elapsed time until
 * the GPU has finished it. The diagnostic completion handlers are forced OFF
 * for the duration -- they run once per command buffer, and there are about
 * thirteen times as many of those in the per-draw path, so leaving them on
 * taxes one arm far more than the other.
 *
 * What this does NOT establish: that the difference reaches gameplay. It
 * measures the rendering path on one captured frame. The live runs remain the
 * evidence for integration behaviour and stability.
 *
 *   RECOMP_METAL_FRAME_BENCH=<flip>    capture the frame after this flip
 *   RECOMP_METAL_FRAME_BENCH_TRIALS=n  timed trials per arm (default 20)
 */
#define BENCH_MAX_DRAWS 4096
#define BENCH_MAX_SURF  8
typedef struct {
    NV2ATextureCopy state, extra[3];
    uint8_t *tex[4]; size_t tex_size[4];
    float (*verts)[16][4]; unsigned count, primitive;
    int target_slot, depth_slot;
} BenchDraw;
typedef struct { const uint8_t *guest; size_t size; uint8_t *initial, *work; } BenchSurf;

static BenchDraw *bench_draw;
static BenchSurf bench_surf[BENCH_MAX_SURF];
static unsigned bench_n, bench_surfs, bench_flip_at, bench_trials, bench_min_draws;

/* Throw away a captured frame and try the next one. A frame boundary reached
 * at an arbitrary flip is as likely to be a menu as gameplay -- the first
 * attempt captured three draws -- and three draws do not exercise submission
 * at all. */
static void bench_discard(void)
{
    unsigned i, u;
    for (i = 0; i < bench_n; ++i) {
        for (u = 0; u < 4; ++u) free(bench_draw[i].tex[u]);
        free(bench_draw[i].verts);
    }
    for (i = 0; i < bench_surfs; ++i) { free(bench_surf[i].initial); free(bench_surf[i].work); }
    memset(bench_draw, 0, BENCH_MAX_DRAWS * sizeof *bench_draw);
    memset(bench_surf, 0, sizeof bench_surf);
    bench_n = 0; bench_surfs = 0;
}
static int bench_state;          /* 0 unset, -1 off, 1 armed, 2 capturing, 3 done */
static unsigned long long bench_flips;

static int bench_slot(const uint8_t *guest, size_t size)
{
    unsigned i;
    if (!guest || !size) return -1;
    for (i = 0; i < bench_surfs; ++i)
        if (bench_surf[i].guest == guest && bench_surf[i].size == size) return (int)i;
    if (bench_surfs >= BENCH_MAX_SURF) return -1;
    bench_surf[bench_surfs].guest = guest;
    bench_surf[bench_surfs].size = size;
    bench_surf[bench_surfs].initial = malloc(size);
    bench_surf[bench_surfs].work = malloc(size);
    if (!bench_surf[bench_surfs].initial || !bench_surf[bench_surfs].work) return -1;
    /* Contents BEFORE this frame touched it: snapshot on first sight, so every
     * trial starts from the same pixels the real frame started from. */
    memcpy(bench_surf[bench_surfs].initial, guest, size);
    return (int)bench_surfs++;
}

static void bench_capture(const NV2ATextureCopy *st, const uint8_t *texture, size_t texture_size,
                          uint8_t *target, size_t target_size, uint8_t *depth, size_t depth_size,
                          const float (*v)[16][4], unsigned count, unsigned primitive)
{
    BenchDraw *d;
    unsigned u;
    if (bench_n >= BENCH_MAX_DRAWS) return;
    d = &bench_draw[bench_n];
    memset(d, 0, sizeof *d);
    d->state = *st;
    if (st->extra_stages) { memcpy(d->extra, st->extra_stages, sizeof d->extra);
                            d->state.extra_stages = d->extra; }
    for (u = 0; u < 4; ++u) {
        const uint8_t *src = u ? st->extra_texture[u-1] : texture;
        size_t n = u ? st->extra_size[u-1] : texture_size;
        if (!src || !n) continue;
        d->tex[u] = malloc(n); if (!d->tex[u]) return;
        memcpy(d->tex[u], src, n); d->tex_size[u] = n;
        if (u) { d->state.extra_texture[u-1] = d->tex[u]; d->state.extra_size[u-1] = n; }
    }
    d->verts = malloc((size_t)count * sizeof *d->verts);
    if (!d->verts) return;
    memcpy(d->verts, v, (size_t)count * sizeof *d->verts);
    d->count = count; d->primitive = primitive;
    d->target_slot = bench_slot(target, target_size);
    d->depth_slot  = bench_slot(depth, depth_size);
    if (d->target_slot < 0) return;
    ++bench_n;
}

static unsigned long long bench_replay(int batched, unsigned long long *cpu_ns)
{
    unsigned long long t0, t1, t2;
    unsigned i;
    batch_force = batched ? 1 : -1;
    for (i = 0; i < bench_surfs; ++i)
        memcpy(bench_surf[i].work, bench_surf[i].initial, bench_surf[i].size);
    nv2a_metal_invalidate(NULL);       /* identical starting pixels every trial */
    t0 = mtl_now_ns();
    for (i = 0; i < bench_n; ++i) {
        BenchDraw *d = &bench_draw[i];
        uint8_t *tgt = bench_surf[d->target_slot].work;
        uint8_t *dep = d->depth_slot >= 0 ? bench_surf[d->depth_slot].work : NULL;
        size_t ds = d->depth_slot >= 0 ? bench_surf[d->depth_slot].size : 0;
        nv2a_metal_draw(&d->state, d->tex[0], d->tex_size[0], tgt,
                        bench_surf[d->target_slot].size, dep, ds,
                        (const float (*)[16][4])d->verts, d->count, d->primitive);
    }
    t1 = mtl_now_ns();
    nv2a_metal_sync();                 /* through final GPU completion */
    t2 = mtl_now_ns();
    batch_force = 0;
    if (cpu_ns) *cpu_ns = t1 - t0;
    return t2 - t0;
}

static int bench_cmp(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

static void bench_run(void)
{
    unsigned long long *per, *bat, *perc, *batc;
    unsigned t, trials = bench_trials, mismatch = 0, i;
    uint8_t **shot_a, **shot_b;
    int saved_gpu = mtl_cb_gpu_force;

    fprintf(stderr, "  [FRAME-BENCH] captured %u draws over %u surfaces\n",
            bench_n, bench_surfs);
    if (!bench_n) { bench_state = 3; return; }

    /* Handlers off: see the header comment. */
    mtl_cb_gpu_force = -1;

    /* Correctness before speed. */
    shot_a = calloc(bench_surfs, sizeof *shot_a);
    shot_b = calloc(bench_surfs, sizeof *shot_b);
    if (shot_a && shot_b) {
        bench_replay(0, NULL);
        for (i = 0; i < bench_surfs; ++i) {
            shot_a[i] = malloc(bench_surf[i].size);
            if (shot_a[i]) memcpy(shot_a[i], bench_surf[i].work, bench_surf[i].size);
        }
        bench_replay(1, NULL);
        for (i = 0; i < bench_surfs; ++i) {
            shot_b[i] = malloc(bench_surf[i].size);
            if (shot_b[i]) memcpy(shot_b[i], bench_surf[i].work, bench_surf[i].size);
            if (shot_a[i] && shot_b[i] && memcmp(shot_a[i], shot_b[i], bench_surf[i].size))
                ++mismatch;
        }
        fprintf(stderr, "  [FRAME-BENCH] output equality over %u surfaces: %s\n",
                bench_surfs, mismatch ? "DIFFERENT -- do not read the timings below"
                                      : "identical");
    }

    for (t = 0; t < 3; ++t) { bench_replay(0, NULL); bench_replay(1, NULL); }   /* warm both */

    per  = malloc(trials * sizeof *per);  bat  = malloc(trials * sizeof *bat);
    perc = malloc(trials * sizeof *perc); batc = malloc(trials * sizeof *batc);
    if (per && bat && perc && batc) {
        for (t = 0; t < trials; ++t) {
            per[t] = bench_replay(0, &perc[t]);
            bat[t] = bench_replay(1, &batc[t]);
        }
        qsort(per, trials, sizeof *per, bench_cmp);
        qsort(bat, trials, sizeof *bat, bench_cmp);
        qsort(perc, trials, sizeof *perc, bench_cmp);
        qsort(batc, trials, sizeof *batc, bench_cmp);
        fprintf(stderr,
            "  [FRAME-BENCH] %u alternating trials of %u draws, handlers off.\n"
            "  [FRAME-BENCH] Each trial restores the starting pixels and invalidates,\n"
            "  [FRAME-BENCH] so both arms pay one full surface re-upload; that fixed\n"
            "  [FRAME-BENCH] cost is in both columns and not in the difference.\n"
            "  [FRAME-BENCH] Spread is min/p25/median/p75/max over the %u trials --\n"
            "  [FRAME-BENCH] quoting only min and median would hide the tails.\n"
            "  [FRAME-BENCH]   per-draw issue    %.3f %.3f %.3f %.3f %.3f ms  (%.1f us/draw)\n"
            "  [FRAME-BENCH]   batched  issue    %.3f %.3f %.3f %.3f %.3f ms  (%.1f us/draw)\n"
            "  [FRAME-BENCH]   per-draw gpu-done %.3f %.3f %.3f %.3f %.3f ms\n"
            "  [FRAME-BENCH]   batched  gpu-done %.3f %.3f %.3f %.3f %.3f ms\n"
            "  [FRAME-BENCH]   median difference   issue %+.3f ms   to-gpu-done %+.3f ms\n",
            trials, bench_n, trials,
            perc[0]/1e6, perc[trials/4]/1e6, perc[trials/2]/1e6,
            perc[(3*trials)/4]/1e6, perc[trials-1]/1e6,
            perc[trials/2]/1e3/(double)bench_n,
            batc[0]/1e6, batc[trials/4]/1e6, batc[trials/2]/1e6,
            batc[(3*trials)/4]/1e6, batc[trials-1]/1e6,
            batc[trials/2]/1e3/(double)bench_n,
            per[0]/1e6, per[trials/4]/1e6, per[trials/2]/1e6,
            per[(3*trials)/4]/1e6, per[trials-1]/1e6,
            bat[0]/1e6, bat[trials/4]/1e6, bat[trials/2]/1e6,
            bat[(3*trials)/4]/1e6, bat[trials-1]/1e6,
            ((double)batc[trials/2]-(double)perc[trials/2])/1e6,
            ((double)bat[trials/2]-(double)per[trials/2])/1e6);
    }
    mtl_cb_gpu_force = saved_gpu;
    free(per); free(bat); free(perc); free(batc);
    if (shot_a) { for (i = 0; i < bench_surfs; ++i) free(shot_a[i]); free(shot_a); }
    if (shot_b) { for (i = 0; i < bench_surfs; ++i) free(shot_b[i]); free(shot_b); }
    fflush(stderr);
}

void nv2a_metal_frame_bench_flip(void)
{
    if (!bench_state) {
        const char *e = getenv("RECOMP_METAL_FRAME_BENCH");
        if (!e) { bench_state = -1; return; }
        bench_flip_at = (unsigned)strtoul(e, NULL, 0);
        e = getenv("RECOMP_METAL_FRAME_BENCH_TRIALS");
        bench_trials = e ? (unsigned)strtoul(e, NULL, 0) : 20u;
        if (!bench_trials) bench_trials = 20u;
        e = getenv("RECOMP_METAL_FRAME_BENCH_MIN_DRAWS");
        bench_min_draws = e ? (unsigned)strtoul(e, NULL, 0) : 40u;
        bench_draw = calloc(BENCH_MAX_DRAWS, sizeof *bench_draw);
        bench_state = bench_draw ? 1 : -1;
    }
    if (bench_state < 1 || bench_state == 3) return;
    ++bench_flips;
    if (bench_state == 1) { if (bench_flips >= bench_flip_at) bench_state = 2; return; }
    if (bench_state == 2 && bench_n < bench_min_draws) {
        /* Not a representative frame: drop it and capture the next one. */
        bench_discard();
        return;
    }
    if (bench_state == 2) {
        /* Close capture BEFORE replaying. The replays go through
         * nv2a_metal_draw like everything else, so leaving the state at
         * "capturing" made them capture themselves: three draws captured and
         * eight surfaces reported, and an output comparison against a moving
         * target that duly came out DIFFERENT. */
        bench_state = 3;
        bench_run();
    }
}

int nv2a_metal_draw(const NV2ATextureCopy*s,const uint8_t*texture,size_t texture_size,
 uint8_t*target,size_t target_size,uint8_t*depth,size_t depth_size,
 const float(*vertices)[16][4],unsigned count,unsigned primitive)
{
 if(!s)return reject("null-state");
 if((s->texture_mask&1)&&!texture)return reject("missing-texture");
 if(!target)return reject("missing-target");
 if(!vertices||count<3||count>4096)return reject("vertex-count");
 if(s->target_bpp!=2)return reject("target-format");
 if(!s->clip_w||!s->clip_h||s->clip_x||s->clip_y||s->clip_w>4096||s->clip_h>4096)return reject("clip");
 if(s->target_pitch<(uint64_t)s->clip_w*2)return reject("target-pitch");
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){
    if(u&&!s->extra_stages)return reject("missing-stage-state");
    const NV2ATextureCopy*t=u?&s->extra_stages[u-1]:s;
    const uint8_t*data=u?s->extra_texture[u-1]:texture;
    size_t bytes=u?s->extra_size[u-1]:texture_size;
    if(!data)return reject("missing-texture");
    if(!t->width||!t->height||t->width>4096||t->height>4096||!t->levels||t->levels>13)return reject("texture-size");
    if(nv2a_texture_copy_texture_bytes(t)>bytes)return reject("texture-bounds");
 }
 if((uint64_t)s->target_pitch*s->clip_h>target_size)return reject("target-bounds");
 if((s->depth_test||s->stencil_test)&&(!depth||s->depth_pitch<(uint64_t)s->clip_w*4||(uint64_t)s->depth_pitch*s->clip_h>depth_size))return reject("depth-bounds");
 unsigned indices[12288],n=0;
 switch(primitive){case 5:for(unsigned i=0;i+2<count;i+=3)triangle(s,vertices,indices,&n,i,i+1,i+2);break;
 case 6:for(unsigned i=0;i+2<count;i++)triangle(s,vertices,indices,&n,i+(i&1),i+1-(i&1),i+2);break;
 case 7:for(unsigned i=1;i+1<count;i++)triangle(s,vertices,indices,&n,0,i,i+1);break;
 case 8:for(unsigned i=0;i+3<count;i+=4){triangle(s,vertices,indices,&n,i,i+1,i+2);triangle(s,vertices,indices,&n,i,i+2,i+3);}break;
 case 9:for(unsigned i=0;i+3<count;i+=2){triangle(s,vertices,indices,&n,i,i+1,i+3);triangle(s,vertices,indices,&n,i,i+3,i+2);}break;default:return reject("primitive");}
 if(!n){reject_reason=NULL;return 0;}
 if(clip_audit_on())clip_audit(vertices,indices,n,s->clip_w,s->clip_h);
 if(bench_state==2)bench_capture(s,texture,texture_size,target,target_size,depth,depth_size,vertices,count,primitive);
 @autoreleasepool{if(!initialize())return reject("initialization");
  int use_zeta=s->depth_test||s->stencil_test;uint8_t*next_depth=use_zeta?depth:NULL;uint32_t next_depth_pitch=use_zeta?s->depth_pitch:0;size_t next_depth_size=use_zeta?depth_size:0;
  /* Staging. Anything that fits Metal's 4 KB inline limit still goes through
   * setVertexBytes, which costs no allocation at all; everything larger comes
   * out of the slab ring above in a single contiguous reservation covering
   * both the vertices and, when they are too big to inline too, the indices.
   * The private-allocation path is kept as the fallback for a ring that could
   * not allocate, because a slow correct draw beats a rejected one. */
  Vertex small_vertices[36];size_t vertex_bytes=count*sizeof(Vertex),index_bytes=n*sizeof(indices[0]);
  int want_vb=vertex_bytes>sizeof(small_vertices),want_ib=index_bytes>4096;
  id<MTLBuffer>vb=nil,ib=nil;size_t vb_offset=0,ib_offset=0;Vertex*v=small_vertices;
  void*ib_cpu=NULL;int pinned_slab=-1;
  if(want_vb)++allocated_vertex_batches;else ++inline_vertex_batches;
  if(want_vb||want_ib){
   size_t vneed=want_vb?RING_ALIGN_UP(vertex_bytes):0,ineed=want_ib?RING_ALIGN_UP(index_bytes):0;
   size_t at=0;void*cpu=NULL;unsigned slot=0;
   id<MTLBuffer>slab=ring_reserve(vneed+ineed,&at,&cpu,&slot);
   if(slab){
    pinned_slab=(int)slot;
    if(want_vb){vb=slab;vb_offset=at;v=(Vertex*)cpu;}
    if(want_ib){ib=slab;ib_offset=at+vneed;ib_cpu=(uint8_t*)cpu+vneed;}
   }else{
    if(want_vb){vb=[device newBufferWithLength:vertex_bytes options:MTLResourceStorageModeShared];if(!vb)return reject("buffer-allocation");v=vb.contents;}
    if(want_ib){ib=[device newBufferWithBytes:indices length:index_bytes options:MTLResourceStorageModeShared];if(!ib)return reject("buffer-allocation");}
   }
  }
  if(ib_cpu)memcpy(ib_cpu,indices,index_bytes);
  id<MTLBuffer>tb[4];
  for(unsigned u=0;u<4;u++){
   int active=(s->texture_mask&(1u<<u))!=0;const NV2ATextureCopy*t=u&&active?&s->extra_stages[u-1]:s;
   const uint8_t*data=active?(u?s->extra_texture[u-1]:texture):NULL;size_t bytes=active?nv2a_texture_copy_texture_bytes(t):0;
   tb[u]=texture_buffer(data,bytes);if(!tb[u])return reject("buffer-allocation");}
  for(unsigned i=0;i<count;i++){memcpy(v[i].p,vertices[i][0],16);memcpy(v[i].d0,vertices[i][3],16);memcpy(v[i].d1,vertices[i][4],16);for(unsigned u=0;u<4;u++)memcpy(v[i].t[u],vertices[i][9+u],16);}
  if(!surface_valid||!depth_valid||surface_target!=target||surface_width!=s->clip_w||surface_height!=s->clip_h||surface_pitch!=s->target_pitch||surface_target_size!=target_size||depth_target!=next_depth||depth_pitch!=next_depth_pitch||depth_target_size!=next_depth_size){
   ++surface_uploads;if(!nv2a_metal_sync())return reject("surface-sync");MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:hw_565_on()?MTLPixelFormatB5G6R5Unorm:MTLPixelFormatRGBA32Float width:s->clip_w height:s->clip_h mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;surface=[device newTextureWithDescriptor:td];MTLTextureDescriptor*sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:s->clip_w height:s->clip_h mipmapped:NO];sd.usage=MTLTextureUsageRenderTarget;sd.storageMode=MTLStorageModeShared;stencil_surface=[device newTextureWithDescriptor:sd];if(!surface||!stencil_surface)return reject("surface-allocation");
   surface_target=target;surface_target_size=target_size;surface_width=s->clip_w;surface_height=s->clip_h;surface_pitch=s->target_pitch;size_t pixels=(size_t)surface_width*surface_height;float*rgba=malloc(pixels*16);uint8_t*stencil=malloc(pixels);if(!rgba||!stencil){free(rgba);free(stencil);return reject("upload-allocation");}
   if(hw_565_on()){
    /* The attachment is the guest's format, so there is nothing to convert:
     * copy the rows in and let the shader's channel swap put each component
     * where the hardware will pack it back. */
    uint16_t*w16=(uint16_t*)rgba;
    for(unsigned y=0;y<surface_height;y++)
     memcpy(w16+(size_t)y*surface_width,target+(size_t)y*surface_pitch,(size_t)surface_width*2);
    [surface replaceRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0 withBytes:w16 bytesPerRow:surface_width*2];
   }else
   for(unsigned y=0;y<surface_height;y++)for(unsigned x=0;x<surface_width;x++){const uint8_t*p=target+(size_t)y*surface_pitch+x*2;unsigned c=p[0]|(unsigned)p[1]<<8;size_t at=((size_t)y*surface_width+x)*4;rgba[at]=(float)(c>>11)/31;rgba[at+1]=(float)((c>>5)&63)/63;rgba[at+2]=(float)(c&31)/31;if(next_depth){const uint8_t*z=next_depth+(size_t)y*next_depth_pitch+x*4;uint32_t q=(uint32_t)z[1]|(uint32_t)z[2]<<8|(uint32_t)z[3]<<16;rgba[at+3]=(float)q/16777215;stencil[(size_t)y*surface_width+x]=z[0];}else{rgba[at+3]=1;stencil[(size_t)y*surface_width+x]=0;}}
   if(!hw_565_on())[surface replaceRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0 withBytes:rgba bytesPerRow:surface_width*16];[stencil_surface replaceRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0 withBytes:stencil bytesPerRow:surface_width];free(rgba);free(stencil);surface_valid=depth_valid=1;surface_dirty=depth_dirty=0;depth_target=next_depth;depth_target_size=next_depth_size;depth_pitch=next_depth_pitch;
   /* The real attachments are built from the same guest bytes, at the same
    * moment, so the two paths start from identical depth. If this fails the
    * textures are left nil and every draw below falls back to the software
    * path -- slower, and correct. */
   if(hw_state_on()&&!hw_depth_upload(next_depth,surface_width,surface_height,next_depth_pitch)){hw_depth_tex=nil;hw_stencil_tex=nil;}}
  /* The hardware-state path attaches real depth and stencil buffers and drops
   * the second colour attachment the software path used to carry stencil in.
   * Chosen per draw rather than per surface because a state this path cannot
   * translate falls back, and a fallback draw needs the old descriptor. */
  int hw = hw_state_on() && hw_depth_tex && hw_stencil_tex;
  id<MTLRenderPipelineState> hw_pso_use = hw ? hw_pipeline_for(s) : nil;
  id<MTLDepthStencilState> hw_dss_use = hw ? hw_depth_state_for(s) : nil;
  /* DEPTH OWNERSHIP IS EXCLUSIVE, so there is no "fall back for this draw".
   *
   * The software path stores depth in the colour attachment's alpha; the
   * hardware path stores it in the depth attachment. Letting individual draws
   * choose puts one quantity in two places, updated on different schedules,
   * and nv2a_metal_sync can then only read one of them -- which is exactly
   * what happened: every differing pixel had software 0x000000 against
   * hardware 0xFFFFFF, the hardware attachment still holding its uploaded
   * value because the draws that wrote depth had quietly used the other path.
   *
   * So an untranslatable state rejects the draw instead. reject() is the
   * existing, counted route the pushbuffer executor already handles, and a
   * rejected draw is visible; a silently mixed frame is not. */
  if (hw && (!hw_pso_use || !hw_dss_use))
    return reject("hw-state-untranslatable");
  MTLRenderPassDescriptor*pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=surface;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
  if(hw){pass.depthAttachment.texture=hw_depth_tex;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;
         pass.stencilAttachment.texture=hw_stencil_tex;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}
  else{pass.colorAttachments[1].texture=stencil_surface;pass.colorAttachments[1].loadAction=MTLLoadActionLoad;pass.colorAttachments[1].storeAction=MTLStoreActionStore;}
  /* A batch encoder is opened from ONE pass descriptor, so a draw that needs
   * the other attachment layout cannot join it. batch_flush() before switching
   * keeps that invariant; without it the hardware path would silently render
   * into the software path's descriptor.
   *
   * IT HAS TO HAPPEN BEFORE THE ENCODER IS BOUND, and it used to happen after.
   * batch_flush() calls endEncoding, commits the command buffer and nils
   * batch_encoder -- but `encoder` had already been bound to that object a few
   * lines above, so every setRenderPipelineState and drawPrimitives below went
   * to an encoder that was already ended and a command buffer already
   * committed. RECOMP_METAL_HW=1 with RECOMP_METAL_BATCH=1 segfaulted on its
   * first draw, because batch_encoder_hw starts at 0 and the first hardware
   * draw therefore always takes this branch. Neither gate combined the two
   * switches, so nothing saw it. */
  if(batch_on()&&batch_encoder&&hw!=batch_encoder_hw){batch_flush();}
  batch_encoder_hw=hw;
  unsigned long long _t0=mtl_cb_stats()?mtl_now_ns():0;
  id<MTLCommandBuffer>command;id<MTLRenderCommandEncoder>encoder;
  if(batch_on()){
   /* An encoder already open is one whose pass descriptor still describes the
    * live surface: the only thing that changes it is the surface upload above,
    * which syncs, and a sync flushes. */
   if(!batch_encoder){
    batch_command=[queue commandBuffer];
    batch_encoder=batch_command?[batch_command renderCommandEncoderWithDescriptor:pass]:nil;
    if(!batch_command||!batch_encoder){batch_command=nil;batch_encoder=nil;return reject("command-encoder");}
    if(mtl_cb_stats())g_mtl_cbufs++;
   }
   command=batch_command;encoder=batch_encoder;
  }else{
   command=[queue commandBuffer];encoder=command?[command renderCommandEncoderWithDescriptor:pass]:nil;
   if(!command||!encoder)return reject("command-encoder");
   if(mtl_cb_stats())g_mtl_cbufs++;
  }
  if(mtl_cb_stats()){g_mtl_ns_create+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
  Params p={0};p.width=s->clip_w;p.height=s->clip_h;p.dither=s->dither;p.untextured=s->untextured;p.combiner_count=s->combiner_count;p.texture_mask=s->texture_mask;p.add_specular=s->add_specular;p.alpha_test=s->alpha_test;p.alpha_ref=s->alpha_ref;p.modulate=s->modulate;p.blend=s->blend;p.blend_src=s->blend_src;p.blend_dst=s->blend_dst;p.depth_test=s->depth_test;p.depth_write=s->depth_test&&s->depth_write;p.depth_func=s->depth_func;{static int legacy=-1;if(legacy<0)legacy=recomp_switch_on("RECOMP_LEGACY_ZCLAMP");
   p.z_cull=legacy?0u:s->z_cull;}p.z_lo=s->z_clip_min;p.z_hi=s->z_clip_max;p.stencil_test=s->stencil_test;p.stencil_write=s->stencil_write;p.stencil_mask=s->stencil_mask;p.stencil_ref=s->stencil_ref;p.stencil_func_mask=s->stencil_func_mask;p.stencil_func=s->stencil_func;p.stencil_fail=s->stencil_fail;p.stencil_zfail=s->stencil_zfail;p.stencil_zpass=s->stencil_zpass;for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){const NV2ATextureCopy*t=u?&s->extra_stages[u-1]:s;p.tw[u]=t->width;p.th[u]=t->height;p.pitch[u]=t->pitch;p.linear[u]=t->linear;p.rgba8[u]=t->rgba8;p.dxt1[u]=t->dxt1;p.dxt3[u]=t->dxt3;p.repeat[u]=t->repeat;p.levels[u]=t->levels;p.min_filter[u]=t->min_filter;p.lod_bias[u]=t->lod_bias;}memcpy(p.color_icw,s->color_icw,sizeof(p.color_icw));memcpy(p.alpha_icw,s->alpha_icw,sizeof(p.alpha_icw));memcpy(p.color_ocw,s->color_ocw,sizeof(p.color_ocw));memcpy(p.alpha_ocw,s->alpha_ocw,sizeof(p.alpha_ocw));
  /* Depth clipping is NOT losing geometry. Retracted 13 Sep 2026, measured.
   *
   * This switch and the paragraph that used to stand here claimed the opposite:
   * that Metal's 0 <= z <= w volume was discarding background geometry, on an
   * A/B that read 18.46M triangles clamped against 15.26M unclamped. That A/B
   * could not have measured clipping. nv2a_metal_draw returns n/3 -- triangles
   * assembled and culled, counted BEFORE this encoder exists -- so the clip
   * mode is invisible to it, and the two numbers differ only because the two
   * runs got different distances through the title. Read a counter's trigger
   * before trusting its value.
   *
   * What the volume actually does, measured with RECOMP_METAL_CLIP_AUDIT over
   * 37.7M triangles at the Corn tutorial:
   *
   *     discarded by the far plane                                0
   *     discarded while fully on screen with every w > 0          0
   *
   * Zero, both. The far plane cannot fire at all: the vertex shader clamps z
   * into [0,16777215] and only then multiplies by w, so 0 <= z*w <= w holds
   * identically for any positive w. And for a triangle with every w > 0 the
   * side planes reduce to "every vertex beyond one screen edge", which is
   * correct culling. Every discard the audit finds has w < 0 -- geometry
   * behind the camera, which the guest's own perspective divide has mirrored
   * through the viewport origin, so it can land on screen looking legitimate.
   * Discarding it is right, and that is all the clamp ever recovered: it drew
   * mirrored garbage over large areas, which is why it halved the frame rate.
   *
   * The switch stays, opt-in and off, as the positive control for this class
   * of claim -- turn it on and the lost geometry does NOT come back. The real
   * whole-draw loss was elsewhere: a DST_COLOR/ZERO multiply blend refused by
   * prepare_texture_copy, which deleted the batch outright. See the comment on
   * blend_factor in nv2a_texture_copy.c. */
  /* ALWAYS clamp, never clip. The fragment shader now applies the guest's
   * own SET_CLIP_MIN/MAX and ZMIN_MAX_CONTROL policy, so hardware z
   * clipping would pre-empt a decision that is no longer ours to make --
   * and would discard the straddling ground polygons before the shader
   * could judge them. This is what xemu does unconditionally
   * (glEnable(GL_DEPTH_CLAMP) / depthClampEnable = VK_TRUE).
   *
   * Behind-camera geometry is NOT resurrected by this: a vertex with w<0
   * cannot satisfy -w<=x<=w, so the side planes still remove it.
   * RECOMP_LEGACY_ZCLAMP=1 forces the old saturate-instead-of-discard
   * policy, to A/B the change in one binary. */
  [encoder setDepthClipMode:MTLDepthClipModeClamp];
  if(hw){[encoder setRenderPipelineState:hw_pso_use];[encoder setDepthStencilState:hw_dss_use];
         /* CLAMP, not clip. Metal's default discards a fragment whose z leaves
          * [0,1]; the software path clamped it and drew it anyway, which is
          * also what the guest's CLAMP z-range policy asks for. The vertex
          * shader deliberately does not clamp -- "clamping before the
          * perspective multiply corrupts the endpoints the hardware clipper
          * interpolates from", which is what made straddling ground polygons
          * wrong -- so the clamp has to happen here, at the rasteriser, or
          * geometry crossing the near plane renders differently on the two
          * paths. */
         [encoder setDepthClipMode:MTLDepthClipModeClamp];
         [encoder setStencilReferenceValue:(uint32_t)(s->stencil_ref&255)];++g_hw_draws;}
  else [encoder setRenderPipelineState:pipeline];if(vb)[encoder setVertexBuffer:vb offset:vb_offset atIndex:0];else[encoder setVertexBytes:v length:vertex_bytes atIndex:0];[encoder setVertexBytes:&p length:sizeof(p) atIndex:1];if(ib)[encoder setVertexBuffer:ib offset:ib_offset atIndex:2];else[encoder setVertexBytes:indices length:index_bytes atIndex:2];[encoder setFragmentBuffer:tb[0] offset:0 atIndex:0];[encoder setFragmentBuffer:tb[1] offset:0 atIndex:2];[encoder setFragmentBuffer:tb[2] offset:0 atIndex:3];[encoder setFragmentBuffer:tb[3] offset:0 atIndex:4];[encoder setFragmentBytes:&p length:sizeof(p) atIndex:1];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:n];
  if(batch_on()){
   if(pinned_slab>=0)batch_pins|=1u<<(unsigned)pinned_slab;
   ++batch_draws;
   if(mtl_cb_stats()){g_mtl_ns_encode+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
   if(batch_cap()&&batch_draws>=batch_cap())batch_flush();
   if(mtl_cb_stats())g_mtl_ns_commit+=mtl_now_ns()-_t0;
  }else{
   [encoder endEncoding];if(mtl_cb_stats()){g_mtl_ns_encode+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
   if(pinned_slab>=0)ring_pin(command,(unsigned)pinned_slab);
   mtl_cb_gpu_watch(command);
   [command commit];if(mtl_cb_stats())g_mtl_ns_commit+=mtl_now_ns()-_t0;last_command=command;
  }
  surface_dirty=1;if((s->depth_test&&s->depth_write)||(s->stencil_test&&s->stencil_write))depth_dirty=1;reject_reason=NULL;return(int)(n/3);}
}
