#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nv2a_metal.h"
#include "nv2a_ff.h"
#include "../recomp_switch.h"
#include "nv2a_metal_state.h"
#include "nv2a_vsh.h"
#include "nv2a_texture_decode.h"
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
/* The three regions of nv2a_metal_draw that ran BEFORE the first timer.
 * Measured 21 Sep 2026: create+encode+commit came to 0.133 ms of a
 * 2.89 ms submit -- 4.6% -- so 95% of the draw cost was in code no
 * counter covered. Same RECOMP_METAL_CB_STATS gate, so a run either has
 * all six or none and the arithmetic always closes. */
unsigned long long g_mtl_ns_valid, g_mtl_ns_pack, g_mtl_ns_state;
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
static id<MTLFunction> hw_vs, hw_fs, hw_fs_blend, hw_fs_early, hw_fs_early_nw;
/* The shared library the fragment tails above came out of, kept so the
 * combiner-specialised variants can be fetched from it later. */
static id<MTLLibrary> hw_library;
/* A combiner program as a pipeline key: every word shade() branches on per
 * draw, canonicalised so draws that shade identically share a pipeline. See
 * COMBINER SPECIALISATION in the shader and spec_pipeline_for. */
typedef struct CombKey {
    uint32_t cc, tmask, hw, flags;
    uint32_t ci[8], ai[8], co[8], ao[8];
} CombKey;
static id<MTLFunction> frag_fn(id<MTLLibrary> lib, NSString *name, const CombKey *ck);
static unsigned long long g_mtl_discards;
static id<MTLCommandBuffer> batch_command;
static id<MTLRenderCommandEncoder> batch_encoder;
static unsigned batch_draws, batch_pins;
static uint64_t batch_flushes, batch_draws_total, batch_longest;
static void batch_flush(void);
/* -1 forces off, 1 forces on, 0 defers to the switch. The frame benchmark
 * drives both paths inside one process, so it cannot use the environment. */
static int batch_force;
/* READ batch_on() BELOW BEFORE TRUSTING THIS PARAGRAPH: THE DEFAULT IS 1.
 *
 * The decision recorded here was to go back to opt-in, and it was never
 * carried out. `batch_on()` reads `(e && *e) ? (atoi(e)!=0) : 1`, so unset
 * means ON, and no player paths.conf sets the switch -- every interactive
 * session since 14 Sep 2026 has run batched, including all five that hung or
 * froze on 21 Sep. Noted 21 Sep 2026 night; the default is left alone here
 * because changing it is a decision, not a documentation fix, and it wants
 * the A/B below. See docs/jsrf/STATUS.md, "Metal command-buffer batching".
 *
 * The decision, for the record: it was turned on by default on the evidence
 * below, and then a person playing the title interactively got stuck on the
 * SEGA screen. That is a report from the only test that actually matters, and
 * it is not something the scripted runs saw -- a scripted boot on the same
 * binary reaches NtOpenFile 1408 and 61 scene nodes. So the default was to go
 * back to off until that is either reproduced and fixed or shown to be
 * something else. Nothing below is retracted; a measured rendering win does
 * not outrank a title that will not boot for the user, and the switch costs
 * nothing to keep.
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
/* ON BY DEFAULT, AND IT IS A CORRECTNESS DEFAULT RATHER THAN A FAST ONE.
 *
 * Off, every draw is its own render pass and its own command buffer: load the
 * whole surface into tile memory, draw one batch, store it back. A gameplay
 * frame issues tens of thousands of those against the same texture, and
 * RECOMP_METAL_CB_GPU measures 664,933 command buffers starting before the
 * previous one ended (out-of-order 0). The tiles therefore resolve out of
 * step, and each 64x64 tile of the presented frame keeps whichever command
 * buffer stored it last -- a mosaic of different moments, which is what a
 * person watching it called the background being cut and jumbled.
 *
 * Measured on real gameplay frames, counting strong vertical seams and asking
 * how many land on a multiple of 64 against what chance would give:
 *
 *     hardware, per-draw    248 seams, 66 on a 64 boundary   17.0x chance
 *     hardware, batched      29 seams,  0                     0.0x
 *     software control      227 seams,  3                     0.8x
 *
 * Not reduced, gone. In a screen recording the seams sit on a 64 grid in both
 * axes -- x = 0,64,192,256,320,384,448,640 and y = 128,256,384 -- which is the
 * tile grid and not anything in the scene.
 *
 * It only ever showed on the hardware-state path, for the same reason that
 * path is quick: the software fragment tail serialises overdrawn fragments
 * with raster_order_group(0), which keeps the GPU shallow enough that the
 * per-draw passes rarely overlap. Removing that serialisation is where the
 * 9.6% came from and is what let them race.
 *
 * No device test could catch this. metal_batch_test submits a few hundred
 * draws and reads back immediately, so the GPU is never behind -- its own
 * header says the first ring phase was "toothless" for exactly that reason.
 * The gate for this is a real frame, and metal_batch_check.sh still holds the
 * invariant that matters here: batched output is byte-for-byte the per-draw
 * output on the device test.
 *
 * =0 restores the per-draw path, which is the arm that has to ask for itself
 * now. Keep it working: it is the control for this whole result. */
static int batch_on(void)
{static int on=-1;if(batch_force)return batch_force>0;
 if(on<0){const char*e=getenv("RECOMP_METAL_BATCH");on=(e&&*e)?(atoi(e)!=0):1;}
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
    fprintf(stderr,
            "  [METAL-CB] before the command buffer: validate+assemble %.2fms,"
            " vertex pack %.2fms, state+texture %.2fms (totals) -- this is the"
            " 95%% the six-counter split was missing\n",
            g_mtl_ns_valid / 1e6, g_mtl_ns_pack / 1e6, g_mtl_ns_state / 1e6);
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
/* THE GPU TEXTURE IS AUTHORITATIVE, NOT GUEST RAM.
 *
 * The backend retained ONE surface and the title uses THREE -- measured, not
 * assumed: RECOMP_SURFACE_AUDIT reports "3 distinct colour surfaces bound",
 * and at 55% of flips and 50% of clears the surface the guest names is not the
 * one being held. Every swap therefore tore the retained surface down: sync it
 * out to guest RAM, reallocate four textures, and upload the new one back in.
 * 12,448 of those in a 280 s run, costing 16.1 s reading back and converting
 * on top of 33.8 s draining -- about 18% of the run.
 *
 * It was ALSO my candidate for where content goes missing, and that half was
 * wrong -- recorded because the reasoning was good and the conclusion was not.
 * A person watching the game reports that characters, text and logos are
 * unaffected while the world behind them is not, which fits this exactly:
 * world geometry is drawn FIRST, so it makes the round trip through guest RAM
 * on the next swap, while the character and HUD drawn afterwards never do. The
 * fit was convincing and the fix did not change the artefact. Keep the change
 * for what it demonstrably does -- remove 12,448 round trips -- and do not
 * carry the explanation forward.
 *
 * So keep each surface's textures and rebind them on a swap instead of
 * rebuilding from guest RAM. A slot stays valid only while guest RAM cannot
 * have changed underneath it -- nv2a_metal_invalidate, which every clear goes
 * through, drops the slots it names -- so the cache can never serve a surface
 * the CPU has written since.
 *
 * RECOMP_METAL_SURFACE_CACHE=0 restores the rebuild-every-swap behaviour, and
 * is the control for any measurement of this. */
/* HOW MANY RENDER TARGETS THE CACHE CAN HOLD AT ONCE.
 *
 * Four, since this cache was written, and never measured against what the
 * title actually keeps live. The player's 21 Sep session logged 92 EVICTIONS
 * and 455 rebuilds: an eviction only happens when a fifth surface is wanted
 * while four are held, so the working set demonstrably exceeds four. A
 * render target evicted and rebuilt mid-effect is a frame drawn against a
 * surface that has just been re-read from guest RAM, which is what a flicker
 * looks like -- and "boost dash flickers" is an open report.
 *
 * AND MORE SLOTS IS NOT THE FIX. Measured 21 Sep: at eight the title renders
 * a BLACK SCREEN -- [FB] sum=00000000 nonzero=0/153600 -- and never leaves the
 * logos. The reason is `owes_guest_ram` below: a slot that was cleared
 * residently holds pixels guest RAM has not got, and EVICTION IS WHAT WRITES
 * THEM BACK. Raise the slot count and evictions stop; stop evictions and guest
 * RAM never catches up, so everything that reads it reads zeroes.
 *
 * So the cache size is not independently tunable. It is coupled to the
 * writeback policy, and a bigger cache needs writeback on some trigger other
 * than eviction before it can be considered. That is the real work here, and
 * it is not done.
 *
 * The array is eight and SURFACE_SLOTS_USED is a RUNTIME cap, DEFAULTING TO
 * FOUR, so the experiment is repeatable without shipping its result. A slot
 * is a few pointers and texture handles, not pixels, so the unused ones cost
 * nothing. */
#define SURFACE_SLOTS 8
static unsigned surface_slots_used(void)
{
    static unsigned n;
    if (!n) {
        const char *e = getenv("RECOMP_METAL_SURFACE_SLOTS");
        long v = e ? strtol(e, NULL, 10) : 4;   /* measured: see above */
        if (v < 1) v = 1;
        if (v > SURFACE_SLOTS) v = SURFACE_SLOTS;
        n = (unsigned)v;
    }
    return n;
}
static struct {
    uint8_t *target; size_t target_size;
    uint32_t w, h, pitch;
    uint8_t *depth; uint32_t depth_pitch; size_t depth_size;
    id<MTLTexture> colour, stencil, hw_depth, hw_stencil;
    uint64_t stamp; int valid;
    /* THE GPU HOLDS PIXELS GUEST RAM HAS NOT GOT.
     *
     * Set when this slot's texture is cleared residently while some OTHER
     * surface is bound -- the GPU writes the constant, and the CPU clear that
     * would have written guest RAM is skipped. From that moment the texture is
     * the authority for this surface and guest RAM is behind it.
     *
     * It has exactly one consequence and surface_cache_drop is where it lands:
     * a slot that owes guest RAM cannot simply be dropped, because dropping it
     * throws the only copy away. This is the same class of mistake that cost
     * 24,848 pixels when nv2a_metal_discard dropped a retained surface without
     * reading it back; the difference is that this one is written down and
     * counted before it can happen. */
    int owes_guest_ram;
} surf_slot[SURFACE_SLOTS];
static uint64_t surf_clock;
static uint64_t surface_hits, surface_evictions;

/* G3 A2: DO NOT WRITE THE OUTGOING SURFACE BACK JUST BECAUSE WE ARE LEAVING IT.
 *
 * RECOMP_METAL_DEFER_SWAP, default OFF.
 *
 * Measured, 240 s of gameplay: 18,942 surface swaps against 9,504 flip syncs,
 * and 18,937 of 18,942 swaps are REBINDS at a 100% cache hit rate -- both
 * surfaces already resident on the GPU. Each one drains the queue and reads
 * 614 KB back so that guest RAM holds pixels which are sitting safely in the
 * slot we are about to keep. Per-frame that is [SYNC] p50 = 9.0 ms of an
 * 18.5 ms median frame, and [NOSYNC] says the median would be 8.0 ms without
 * it, inside the 16.68 ms budget.
 *
 * So mark the debt instead of paying it: the slot holding the outgoing texture
 * takes owes_guest_ram, surface_dirty is cleared, and nv2a_metal_sync then
 * takes its already-clean early return -- no drain, no read-back. The flip
 * names the range it is about to read and nv2a_metal_sync_range pays there
 * (A1, which is why the order is not negotiable), and surface_cache_drop pays
 * on eviction as it always has.
 *
 * REFUSES RATHER THAN GUESSES, in two cases, both counted:
 *   - the outgoing surface is not in a slot, so deferring would throw the only
 *     copy of those pixels away;
 *   - depth is dirty. surface_slot_writeback carries COLOUR only, and a slot's
 *     depth textures are retained for a rebind but never written to guest RAM,
 *     so deferring a dirty depth would lose it to anything that reads it --
 *     including the re-upload a cache MISS performs.
 *
 * THIS CHANGES WHEN GUEST RAM BECOMES CORRECT, which is why it ships off. A
 * guest CPU read of the surface range that is not the flip is not intercepted;
 * that is the same exposure the resident-clear deferral has always accepted,
 * but it is now on the hot path rather than on clears. */
static int defer_swap_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_DEFER_SWAP"); return on; }
static uint64_t g_swap_deferred, g_swap_defer_depth, g_swap_defer_noslot;

static int surface_cache_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_METAL_SURFACE_CACHE");
                  on = (e && *e) ? (atoi(e) != 0) : 1; }
    return on;
}

/* Drop every slot that could describe memory the CPU is about to write. NULL
 * means "all of them", which is what a full invalidate asks for. */
static void surface_slot_writeback(unsigned i);
static int clear_encode(MTLRenderPassDescriptor *pass);
static void surface_cache_drop(const uint8_t *target)
{
    for (unsigned i = 0; i < surface_slots_used(); ++i) {
        if (!surf_slot[i].valid) continue;
        if (target && surf_slot[i].target != target && surf_slot[i].depth != target)
            continue;
        /* A slot that owes guest RAM is the only copy of those pixels. Drop it
         * without reading it back and the clear -- or whatever else the GPU
         * wrote while this surface was unbound -- is simply gone. */
        if (surf_slot[i].owes_guest_ram) surface_slot_writeback(i);
        surf_slot[i].valid = 0; surf_slot[i].owes_guest_ram = 0;
        surf_slot[i].colour = nil; surf_slot[i].stencil = nil;
        surf_slot[i].hw_depth = nil; surf_slot[i].hw_stencil = nil;
    }
}

/* RECOMP_METAL_FENCE -- declared here because initialize() is the first user.
 * See the long note at the encoder, which is where it is actually reasoned
 * about. */
static id<MTLFence> g_pass_fence;
static uint64_t g_fence_waits;
static int pass_fence_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_FENCE"); return on; }

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
    uint64_t verified_frame;   /* texture_frame when the full compare last matched */
    /* G27: the same bytes as an MTLTexture, built from `buffer` on first
     * hardware use and dropped whenever `buffer` is replaced. hw_key says
     * which reading of the bytes it holds: format, width, height, level-0
     * pitch and mip count -- one guest allocation can be bound two ways. */
    id<MTLTexture> hw;
    uint32_t hw_key[5];
    uint8_t hw_min_a, hw_max_a;   /* G38c: alpha range over every uploaded level */
} TextureBuffer;
static TextureBuffer texture_cache[TEXTURE_CACHE_SIZE];
static id<MTLBuffer> dummy_buffer;
static uint64_t texture_clock,texture_requests,texture_hits,texture_uploads;
/* The entry texture_buffer() last returned, so texture_hw() can attach its
 * MTLTexture to the same cache slot. Single pushbuffer thread; see ring. */
static TextureBuffer *texture_last_slot;
/* The cache used to memcmp the whole texture against guest RAM on EVERY
 * request, and a frame requests the same textures hundreds of times: a 5 s
 * sample of the combo trick stage (23 Sep 2026) put that memcmp at 28% of the
 * pushbuffer thread's busy time, 13.1 M requests against 50 K uploads in a
 * session. Now the full compare runs once per frame per entry; every other
 * hit in the same frame compares a fixed sample of the bytes -- both ends and
 * eight strided lines -- so a wholesale rewrite mid-frame is still caught at
 * once, and a partial edit is caught at the next frame's full compare.
 * texture_partial_caught counts the first kind: a change the once-per-frame
 * rule alone would have drawn stale. A nonzero count in a session is the
 * signal to revisit this. */
static uint64_t texture_frame=1,texture_full_compares,texture_partial_compares,
                texture_partial_caught,texture_changed;
#define TEXTURE_PROBE 64
static int texture_probe_differs(const uint8_t *a,const uint8_t *b,size_t size)
{
    if(size<=4*TEXTURE_PROBE) return memcmp(a,b,size)!=0;
    if(memcmp(a,b,TEXTURE_PROBE)) return 1;
    if(memcmp(a+size-TEXTURE_PROBE,b+size-TEXTURE_PROBE,TEXTURE_PROBE)) return 1;
    size_t step=(size-2*TEXTURE_PROBE)/9;
    for(unsigned i=1;i<=8;i++) {
        size_t off=TEXTURE_PROBE+step*i;
        if(memcmp(a+off,b+off,TEXTURE_PROBE)) return 1;
    }
    return 0;
}
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
/* Defined with area(), far below: triangles whose float cross product
 * collapsed to zero while the double one did not. Counted unconditionally so
 * a run with the repair OFF still reports what the old arithmetic lost. */
static uint64_t g_area_rescued;
static int area_double_on(void);
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

/* One Metal function constant per combiner word; see COMBINER SPECIALISATION
 * in the shader. Undefined reads as 0, which only the generic path ever sees
 * and never reads. */
#define FC_WORD(name, idx) \
    "constant uint fc_" name "_ [[function_constant(" #idx ")]];" \
    "constant uint FC_" name " = is_function_constant_defined(fc_" name "_) ? fc_" name "_ : 0u;\n"
static NSString *const shader =
@"#include <metal_stdlib>\n"
 "using namespace metal;\n"
 "struct Vertex { float4 p,d0,d1,t0,t1,t2,t3; };\n"
 "struct Params { uint width,height,dither,untextured,combiner_count,texture_mask,add_specular,alpha_test,alpha_ref,modulate,blend,blend_src,blend_dst,depth_test,depth_write,depth_func;"
 "  uint z_cull; float z_lo,z_hi;"
 " uint stencil_test,stencil_write,stencil_mask,stencil_ref,stencil_func_mask,stencil_func,stencil_fail,stencil_zfail,stencil_zpass;"
 " uint tw[4],th[4],pitch[4],linear[4],rgba8[4],dxt1[4],dxt3[4],repeat[4],levels[4],min_filter[4]; float lod_bias[4];"
 " uint color_icw[8]; uint alpha_icw[8]; uint color_ocw[8]; uint alpha_ocw[8];"
 " uint const0[8]; uint const1[8]; uint frag_force; uint hw[4]; uint ez_proven; };\n"
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
 /* G27: THE SAME SAMPLE, BY THE SAMPLER HARDWARE. The texture holds exactly
  * what texel() would have returned for each texel (nv2a_texture_decode.c,
  * checked by texture_decode_test.c), with the mip chain present only when
  * sample_lod() would have walked it; the sampler carries the filter, mip
  * and wrap choices sample_lod() makes by hand, and bias() the LOD bias. */
 "float4 hw_sample(texture2d<float> h,sampler q,float4 tc,uint u,constant Params&s){return h.sample(q,tc.xy/tc.w,bias(s.lod_bias[u]));}\n"
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
 "float cmap1(uint m,float x){switch(m){case 1:return x-0.5f;case 2:return x*2.0f;case 3:return (x-0.5f)*2.0f;case 4:return x*4.0f;case 6:return x*0.5f;default:return x;}}\n"
 "float3 cmap3(uint m,float3 v){return float3(cmap1(m,v.x),cmap1(m,v.y),cmap1(m,v.z));}\n"
 /* COMBINER SPECIALISATION: THE PROGRAM AS FUNCTION CONSTANTS.
  *
  * shade() interprets the combiners per pixel: a loop over a run-time stage
  * count, four input switches per channel, and a float4 r[14] written at
  * indices known only at run time, which forces r[] into memory. A title
  * uses a handful of combiner programs -- JSRF's fixed-function draws, 13 in
  * a whole tutorial -- so the host builds one pipeline per program with the
  * words below set (see spec_pipeline_for), and the compiler unrolls the
  * stages, folds every switch and keeps r[] in registers.
  *
  * FC_SPEC false, or undefined, is the generic interpreter, and every entry
  * point is fetched with it set false (frag_fn), because a function that
  * reads a function constant cannot be used unspecialised at all. The stage
  * body is ONE function, comb_stage, called by both the interpreter loop and
  * the unrolled calls, so the two cannot drift: the same float operations in
  * the same order, which is what metal_combiner_spec_test holds byte-exact.
  * Array constants are not allowed, hence one constant per word. const0 and
  * const1 stay in Params: they change per draw. */
 "constant bool fc_spec_ [[function_constant(0)]];\n"
 "constant bool FC_SPEC = is_function_constant_defined(fc_spec_) && fc_spec_;\n"
 FC_WORD("CC",1) FC_WORD("TMASK",2) FC_WORD("HW",3) FC_WORD("FLAGS",4)
 FC_WORD("ci0",8) FC_WORD("ci1",9) FC_WORD("ci2",10) FC_WORD("ci3",11)
 FC_WORD("ci4",12) FC_WORD("ci5",13) FC_WORD("ci6",14) FC_WORD("ci7",15)
 FC_WORD("ai0",16) FC_WORD("ai1",17) FC_WORD("ai2",18) FC_WORD("ai3",19)
 FC_WORD("ai4",20) FC_WORD("ai5",21) FC_WORD("ai6",22) FC_WORD("ai7",23)
 FC_WORD("co0",24) FC_WORD("co1",25) FC_WORD("co2",26) FC_WORD("co3",27)
 FC_WORD("co4",28) FC_WORD("co5",29) FC_WORD("co6",30) FC_WORD("co7",31)
 FC_WORD("ao0",32) FC_WORD("ao1",33) FC_WORD("ao2",34) FC_WORD("ao3",35)
 FC_WORD("ao4",36) FC_WORD("ao5",37) FC_WORD("ao6",38) FC_WORD("ao7",39)
 "inline void comb_stage(thread float4 *r,uint stage,uint ciw,uint aiw,uint cw,uint aw,constant Params&s){"
 " uint k0=s.const0[stage],k1=s.const1[stage];"
 " r[1]=float4(float((k0>>16)&255),float((k0>>8)&255),float(k0&255),float((k0>>24)&255))/255.0f;"
 " r[2]=float4(float((k1>>16)&255),float((k1>>8)&255),float(k1&255),float((k1>>24)&255))/255.0f;"
 " float4 ab,cd;for(uint k=0;k<4;k++){uint word=k==3?aiw:ciw;uint ch=k==3?2:k;"
 " float a=input(word>>24,ch,r),b=input((word>>16)&255,ch,r),cc=input((word>>8)&255,ch,r),d=input(word&255,ch,r);"
 " ab[k]=a*b;cd[k]=cc*d;}"
 " float3 abr=((cw>>13)&1)?float3(ab.r+ab.g+ab.b):ab.rgb;"
 " float3 cdr=((cw>>12)&1)?float3(cd.r+cd.g+cd.b):cd.rgb;"
 " uint mx=(cw>>14)&1,mp=(cw>>15)&7;"
 " float3 sm=mx?((r[12].a>=0.5f)?abr:cdr):(abr+cdr);"
 " abr=cmap3(mp,abr);cdr=cmap3(mp,cdr);sm=cmap3(mp,sm);"
 " uint dcd=cw&15,dab=(cw>>4)&15,dsm=(cw>>8)&15;"
 " if(dcd)r[dcd].rgb=clamp(cdr,-1.0f,1.0f);"
 " if(dab)r[dab].rgb=clamp(abr,-1.0f,1.0f);"
 " if(dsm)r[dsm].rgb=clamp(sm,-1.0f,1.0f);"
 " uint amx=(aw>>14)&1,amp=(aw>>15)&7;"
 " uint acd=aw&15,aab=(aw>>4)&15,asum=(aw>>8)&15;"
 " if(acd)r[acd].a=clamp(cmap1(amp,cd.a),-1.0f,1.0f);"
 " if(aab)r[aab].a=clamp(cmap1(amp,ab.a),-1.0f,1.0f);"
 " if(asum)r[asum].a=clamp(cmap1(amp,amx?((r[12].a>=0.5f)?ab.a:cd.a):(ab.a+cd.a)),-1.0f,1.0f);"
 " if(((cw>>19)&1)&&dab)r[dab].a=clamp(abr.b,-1.0f,1.0f);"
 " if(((cw>>18)&1)&&dcd)r[dcd].a=clamp(cdr.b,-1.0f,1.0f);}\n"
 "float4 shade(Out i, const device uchar*t0, constant Params&s, const device uchar*t1, const device uchar*t2, const device uchar*t3,"
 " texture2d<float> h0, texture2d<float> h1, texture2d<float> h2, texture2d<float> h3, sampler q0, sampler q1, sampler q2, sampler q3){\n"
 /* Every per-draw branch shade() takes, from the constants when specialised
  * and from Params otherwise. Under FC_SPEC == false each of these folds to
  * exactly the Params read it replaced. */
 " uint tmask=FC_SPEC?FC_TMASK:s.texture_mask,ncomb=FC_SPEC?FC_CC:s.combiner_count;"
 " bool hw0=FC_SPEC?(FC_HW&1u)!=0:s.hw[0]!=0,hw1=FC_SPEC?(FC_HW&2u)!=0:s.hw[1]!=0,"
 "hw2=FC_SPEC?(FC_HW&4u)!=0:s.hw[2]!=0,hw3=FC_SPEC?(FC_HW&8u)!=0:s.hw[3]!=0;"
 " bool addspec=FC_SPEC?(FC_FLAGS&1u)!=0:s.add_specular!=0,untex=FC_SPEC?(FC_FLAGS&2u)!=0:s.untextured!=0,"
 "modul=FC_SPEC?(FC_FLAGS&4u)!=0:s.modulate!=0;"
 " float4 d0=i.d0,d1=i.d1,c=float4(1),tex=float4(0);"
 " if(tmask&1)tex=hw0?hw_sample(h0,q0,i.t0,0,s):sample_lod(t0,i.t0,0,s);"
 " if(ncomb){float4 r[14];for(uint n=0;n<14;n++)r[n]=float4(0);r[4]=d0;r[5]=d1;r[8]=tex;"
 " if(tmask&2)r[9]=hw1?hw_sample(h1,q1,i.t1,1,s):sample_lod(t1,i.t1,1,s);if(tmask&4)r[10]=hw2?hw_sample(h2,q2,i.t2,2,s):sample_lod(t2,i.t2,2,s);if(tmask&8)r[11]=hw3?hw_sample(h3,q3,i.t3,3,s):sample_lod(t3,i.t3,3,s);"
 " r[12].a=(tmask&1)?r[8].a:1;"
 " if(FC_SPEC){"
 " if(FC_CC>0u)comb_stage(r,0u,FC_ci0,FC_ai0,FC_co0,FC_ao0,s);"
 " if(FC_CC>1u)comb_stage(r,1u,FC_ci1,FC_ai1,FC_co1,FC_ao1,s);"
 " if(FC_CC>2u)comb_stage(r,2u,FC_ci2,FC_ai2,FC_co2,FC_ao2,s);"
 " if(FC_CC>3u)comb_stage(r,3u,FC_ci3,FC_ai3,FC_co3,FC_ao3,s);"
 " if(FC_CC>4u)comb_stage(r,4u,FC_ci4,FC_ai4,FC_co4,FC_ao4,s);"
 " if(FC_CC>5u)comb_stage(r,5u,FC_ci5,FC_ai5,FC_co5,FC_ao5,s);"
 " if(FC_CC>6u)comb_stage(r,6u,FC_ci6,FC_ai6,FC_co6,FC_ao6,s);"
 " if(FC_CC>7u)comb_stage(r,7u,FC_ci7,FC_ai7,FC_co7,FC_ao7,s);"
 " }else for(uint stage=0;stage<s.combiner_count;stage++)"
 "comb_stage(r,stage,s.color_icw[stage],s.alpha_icw[stage],s.color_ocw[stage],s.alpha_ocw[stage],s);"
 " c=clamp(r[12]+(addspec?float4(r[5].rgb,0):float4(0)),0.0f,1.0f);}"
 " else if(!untex){c=tex;c.a=clamp(d0.a,0.0f,1.0f)*(modul?c.a:1);if(modul)c.rgb*=max(float3(0),d0.rgb);}"
  /* RECOMP_FRAG_FORCE -- REPLACE THE FRAGMENT WITH ONE OF ITS OWN INPUTS.
  *
  * The intro attract camera draws a scene in which the distant city is
  * correct and textured while the near geometry is SOLID BLACK, and the
  * magenta clear proved those pixels are covered and written as zero rather
  * than left bare. So the question is which input to this function is black,
  * `tex` or `d0`, and nothing in the tree can answer it: the combiner trace
  * reports the CONFIGURATION and RECOMP_FF_BATCH_DUMP_TEX reports the texture
  * OFFSET. Neither reports a colour, and both are wired into the
  * fixed-function branches -- which this title does not draw through. See the
  * note at RECOMP_GLYPH_DUMP, which records that same blindness.
  *
  * This returns the input itself, so the answer is a picture:
  *   1  TEXTURE0 raw          still black -> the texture is black
  *   2  PRIMARY_COLOR raw     still black -> the diffuse is black
  *   3  white                 which pixels this draw covers at all
  *   4  TEXCOORD0 after the perspective divide, red/green, with BLUE where
  *      w <= 0. A flat single colour across a whole object is the collapsed-q
  *      defect: sample_lod divides by tc.w and sample_level then clamps, so
  *      one corner texel is smeared over the primitive.
  *
  * It sits at the END of shade(), after the combiner, so one branch overrides
  * every path -- combiner, modulate and untextured alike. The blend, alpha
  * test and dither downstream are deliberately LEFT ALONE: they are the next
  * question, not this one, and changing two things at once is how five
  * theories died on 21 Sep 2026.
  *
  * Renders incorrectly by construction. Off unless set. */
 " if(s.frag_force==1u)return float4(tex.rgb,1);"
 " if(s.frag_force==2u)return float4(clamp(d0.rgb,0.0f,1.0f),1);"
 " if(s.frag_force==3u)return float4(1,1,1,1);"
  /* FRACT, NOT CLAMP, and the first version of this got it wrong.
  *
  * Clamping made "constant across the primitive" and "off the end of the
  * texture" the SAME flat colour, and those two have opposite fixes: one is a
  * vertex stage that never varied the coordinate, the other is a tiled
  * surface whose wrap mode we dropped. fract keeps a gradient visible however
  * far out of range the coordinate runs, and BLUE marks the pixels that are
  * outside [0,1]. So: flat and blue is one texel smeared over a primitive;
  * striped and blue is a tiled surface addressed correctly; flat and black is
  * a coordinate that genuinely never changed. */
 " if(s.frag_force==4u){float2 q=(i.t0.w!=0.0f)?i.t0.xy/i.t0.w:float2(0);"
 " bool oor=q.x<0.0f||q.x>1.0f||q.y<0.0f||q.y>1.0f;"
 " return float4(fract(q),oor?1.0f:0.0f,1);}"
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
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]],"
 " texture2d<float> h0 [[texture(0)]],texture2d<float> h1 [[texture(1)]],texture2d<float> h2 [[texture(2)]],texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]],sampler q1 [[sampler(1)]],sampler q2 [[sampler(2)]],sampler q3 [[sampler(3)]],"
 " device atomic_uint *ezv [[buffer(5)]]){\n"
 " float4 c=shade(i,t0,s,t1,t2,t3,h0,h1,h2,h3,q0,q1,q2,q3);"
 /* discard_fragment() does NOT return in MSL -- execution continues and the
  * write is dropped at the end -- so each discard returns explicitly. The
  * returned value is immaterial; the explicit return is what stops the rest
  * of the shader running for a fragment that is already gone. */
 " float zg=i.p.z*16777215.0f;"
 " if(s.z_cull&&(zg<s.z_lo||zg>s.z_hi)){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
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
 /* THE THIRD ENTRY POINT: hardware depth and stencil, blending in the shader.
  *
  * It exists for one reason. The NV2A dithers at the ROP, AFTER blending, and
  * so does fs() and so does nv2a_texture_copy.c at its store. fs_hw cannot:
  * the blend unit runs after the fragment shader, so a bias added there is
  * multiplied by the source blend factor and mixed with a destination that
  * already carries its own -- instead of being the bounded +/-0.5 LSB nudge at
  * quantisation that ordered dither is. Measured at 8192 of 65536 pixels wrong
  * against the rasteriser, on the dither matrix's own 4x4 lattice, where the
  * software path scored 0. That is the flickering checkerboard.
  *
  * So for a draw that BOTH blends and dithers, the blend comes back into the
  * shader and the dither follows it, exactly as fs() orders them. Depth and
  * stencil stay where the hardware path put them -- real attachments and an
  * MTLDepthStencilState -- because that is the part that was right, and
  * because depth ownership is exclusive: a draw that put depth back in the
  * colour alpha here would mix two representations in one frame.
  *
  * WHAT IT COSTS, and only for these draws: reading the destination needs
  * raster_order_group(0), so overlapping fragments serialise again. A draw
  * that blends without dithering keeps the blend unit and pays nothing; so
  * does a draw that dithers without blending, because then the bias IS the
  * last thing before quantisation and fs_hw is already correct.
  *
  * The alpha it returns is the shaded alpha, not a blended one, matching
  * fs(): bfactor() takes its SRC_ALPHA from c.a and nothing downstream reads
  * this attachment's alpha on this path -- depth comes from hw_depth_readback
  * and the 565 attachment has no alpha at all. */
 "fragment float4 fs_hw_blend(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]],"
 " texture2d<float> h0 [[texture(0)]],texture2d<float> h1 [[texture(1)]],texture2d<float> h2 [[texture(2)]],texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]],sampler q1 [[sampler(1)]],sampler q2 [[sampler(2)]],sampler q3 [[sampler(3)]],"
 " device atomic_uint *ezv [[buffer(5)]]){\n"
 " float4 c=shade(i,t0,s,t1,t2,t3,h0,h1,h2,h3,q0,q1,q2,q3);"
 " float zg=i.p.z*16777215.0f;"
 " if(s.z_cull&&(zg<s.z_lo||zg>s.z_hi)){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
 " if(s.blend){float3 d=dst.rgb;c.rgb=c.rgb*bfactor(s.blend_src,c,d)+d*bfactor(s.blend_dst,c,d);}"
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}"
 " return float4(c.rgb,clamp(c.a,0.0f,1.0f));}\n"
 /* G27b -- EARLY TESTS FOR A DRAW THAT WRITES NEITHER DEPTH NOR STENCIL.
  *
  * The objection to early tests is a write made for a fragment the shader
  * then discards. A draw with depth writes off and no stencil write makes no
  * such write: the early test only decides which fragments get shaded, and
  * one that passes and is then discarded loses its colour exactly as it
  * would have after a late test. So both discard branches stay, verbatim
  * from fs_hw_blend, and the order is still exact. hw_early_z() returns 2
  * for exactly these draws. */
 "[[early_fragment_tests]] fragment float4 fs_hw_early_nw(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]],"
 " texture2d<float> h0 [[texture(0)]],texture2d<float> h1 [[texture(1)]],texture2d<float> h2 [[texture(2)]],texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]],sampler q1 [[sampler(1)]],sampler q2 [[sampler(2)]],sampler q3 [[sampler(3)]],"
 " device atomic_uint *ezv [[buffer(5)]]){\n"
 " float4 c=shade(i,t0,s,t1,t2,t3,h0,h1,h2,h3,q0,q1,q2,q3);"
 " float zg=i.p.z*16777215.0f;"
 " if(s.z_cull&&(zg<s.z_lo||zg>s.z_hi)){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
 " if(s.alpha_test&&uint(clamp(c.a,0.0f,1.0f)*255+.5f)<=s.alpha_ref){if(s.ez_proven)atomic_fetch_add_explicit(ezv,1u,memory_order_relaxed);discard_fragment();return c;}"
 " if(s.blend){float3 d=dst.rgb;c.rgb=c.rgb*bfactor(s.blend_src,c,d)+d*bfactor(s.blend_dst,c,d);}"
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}"
 " return float4(c.rgb,clamp(c.a,0.0f,1.0f));}\n"
/* THE SAME FUNCTION WITH THE DEPTH TEST IN FRONT OF IT.
  *
  * WHY THERE HAS TO BE A SECOND ONE. fs_hw_blend can call discard_fragment(),
  * for the guest's z-range CULL policy and for the alpha test, and a fragment
  * function that may discard forces Metal to run the shader BEFORE the depth
  * test -- otherwise the depth write would already have happened for a
  * fragment the shader then throws away. So every occluded fragment in the
  * scene pays the whole combiner chain and up to four texture samples first,
  * and those samples are not hardware samples: sample_lod walks a software
  * Morton address, decodes DXT1/DXT3 by hand and does its own bilinear, four
  * texel fetches at a time, out of a `device uchar*`. Losing early-Z in front
  * of that is the expensive kind of losing it.
  *
  * A draw with s.alpha_test == 0 and s.z_cull == 0 cannot reach either
  * discard, so for that draw the two orderings are indistinguishable and
  * [[early_fragment_tests]] is exact rather than approximate. This is that
  * draw's entry point, with both dead branches removed so the attribute is
  * not merely ignored. Everything else -- shade(), the blend, the dither --
  * is the same text.
  *
  * The COLOUR read stays. It is raster_order_group(0) and it is what shader
  * blend mode 3 exists for; dropping it here would reintroduce the corruption
  * that mode measured, and early-Z is orthogonal to it. */
 "[[early_fragment_tests]] fragment float4 fs_hw_early(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]],"
 " texture2d<float> h0 [[texture(0)]],texture2d<float> h1 [[texture(1)]],texture2d<float> h2 [[texture(2)]],texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]],sampler q1 [[sampler(1)]],sampler q2 [[sampler(2)]],sampler q3 [[sampler(3)]],"
 " device atomic_uint *ezv [[buffer(5)]]){\n"
 " float4 c=shade(i,t0,s,t1,t2,t3,h0,h1,h2,h3,q0,q1,q2,q3);"
 " if(s.blend){float3 d=dst.rgb;c.rgb=c.rgb*bfactor(s.blend_src,c,d)+d*bfactor(s.blend_dst,c,d);}"
 " if(s.dither){constexpr uint b[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};int2 xy=int2(i.p.xy);"
 " float bias=(float(b[(xy.y&3)*4+(xy.x&3)])+.5f)/16-.5f;c.rgb+=bias/float3(31,63,31);}"
 " return float4(c.rgb,clamp(c.a,0.0f,1.0f));}\n"
 /* The software-state path: everything the hardware is not being allowed to
  * do. Unchanged in behaviour; it just calls shade() for its colour now. */
 "fragment Frag fs(Out i [[stage_in]], float4 dst [[color(0),raster_order_group(0)]],uint stencil [[color(1),raster_order_group(0)]],"
 " const device uchar*t0 [[buffer(0)]],constant Params&s [[buffer(1)]],const device uchar*t1 [[buffer(2)]],const device uchar*t2 [[buffer(3)]],const device uchar*t3 [[buffer(4)]],"
 " texture2d<float> h0 [[texture(0)]],texture2d<float> h1 [[texture(1)]],texture2d<float> h2 [[texture(2)]],texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]],sampler q1 [[sampler(1)]],sampler q2 [[sampler(2)]],sampler q3 [[sampler(3)]],"
 " device atomic_uint *ezv [[buffer(5)]]){\n"
 " Frag o;o.color=dst;o.stencil=stencil;float4 c=shade(i,t0,s,t1,t2,t3,h0,h1,h2,h3,q0,q1,q2,q3);"
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

/* ===== THE GUEST'S VERTEX PROGRAM, ON THE GPU ==========================
 *
 * WHY. Measured at gameplay on 16 Sep 2026 with RECOMP_VSH_SPLIT, over
 * 206,203,274 vertices in one run: fetching the guest's attributes out of RAM
 * costs 8.25 s and RUNNING ITS PROGRAM costs 77.19 s. Execution is 89% of the
 * vertex stage, and the vertex stage is the largest item left in the frame at
 * about 10.7 ms of 26.3. Moving execution to the GPU is the whole remaining
 * distance to 60 fps; moving the fetch as well would buy a ninth of it for a
 * great deal more risk, so the CPU keeps fetching and this runs the program.
 *
 * WHAT MAKES IT BELIEVABLE. nv2a_vsh_msl.c emits MSL for these programs and
 * had never been executed until vsh_msl_diff_test.m dispatched it on a device
 * against nv2a_vsh_execute over the 126 programs read out of the title's own
 * default.xbe: 8064 vectors, 0 residual disagreements, with an injected-fault
 * control that still reports 64 of 64. It found three real defects doing it,
 * including one where a zero normal produced a NaN that reached oPos and
 * deleted the triangle.
 *
 * THE ONE HAZARD THAT WOULD CORRUPT GEOMETRY SILENTLY, and why it is handled
 * the way it is. The emitter's VS_OUT is not the Out that fs, fs_hw and
 * fs_hw_blend consume -- different field count, different order -- and a
 * vertex function in one MTLLibrary feeding a fragment function in another
 * matches by position. Getting that wrong does not fail to compile; it draws
 * the wrong thing. So the program is compiled INTO THE SAME LIBRARY as the
 * fragment tails, its entry point is rewritten to a plain function, and a
 * wrapper repacks VS_OUT into Out explicitly, field by field. One library per
 * program costs recompiling the shared source 126 times across a session;
 * that is a first-use hitch, not a per-frame cost, and correctness first. */
/* DEFAULT ON since 16 Sep 2026, measured before flipping it:
 *
 *     CPU interpreter   vsh 12.90  submit 5.28  sync 8.30   29.79 ms  33.6 fps
 *     GPU programs      vsh  3.29  submit 4.99  sync 6.40   18.32 ms  54.6 fps
 *
 * 408,205,400 vertices transformed on the GPU across 639,089 draws, 21 distinct
 * programs compiled, nothing refused by the emitter, the compiler or either
 * cache, and 24 of 24 gameplay frames clean with the artefact fingerprint at
 * 31% against the software control's 30%.
 *
 * =0 takes the CPU interpreter and stays a real arm: it is the oracle this
 * path was verified against, it is what runs for any program the emitter
 * cannot express, and it is the control for every frame-time claim above. */
static int vsh_gpu_on(void)
{ static int on=-1; if(on<0){const char*e=getenv("RECOMP_METAL_VSH");
                             on = (e && *e) ? (atoi(e)!=0) : 1;} return on; }
/* Both are defined below; this block sits above them because the program cache
 * has to be declared before nv2a_metal_draw, which is above them too. */
static int hw_state_on(void);
static int initialize(void);

typedef struct { float f[4]; } float4v;
#define VSH_CACHE 192
typedef struct {
    uint32_t words[NV2A_VS_MAX_INSTRUCTIONS][4];
    int length;
    uint32_t hash;
    uint16_t inputs;
    unsigned nattrs;
    id<MTLLibrary> library;
    id<MTLFunction> fn;
    int refused;            /* the emitter or the compiler said no; never retry */
    /* words[] holds an NV2AFFKey rather than a vertex program. The array is
     * NV2A_VS_MAX_INSTRUCTIONS*16 bytes and the key is 32, so it fits with
     * room to spare and the cache stays one table. This file never looks
     * inside the key: it hashes it, memcmp's it, and hands it back to
     * nv2a_ff_generate_msl, exactly as it does a program's words. */
    int is_ff;
    unsigned keysize;
    /* G38c: oD0.w and oD1.w as small expression trees over the constant file,
     * built once from the parsed program (vsh_out_alpha_build). od_ready: 0 not
     * built, 1 built, -1 unusable (fixed function, parse failure, too deep). */
    int od_ready;
    int16_t od_root[2];
    struct { uint8_t op, neg, comp; int16_t a, b, c; uint16_t idx; } od_node[96];
    int od_n;
} VshSlot;
static VshSlot vsh_slot[VSH_CACHE];
static unsigned vsh_slot_n;
static VshSlot *vsh_active;          /* the program this draw will use, or NULL */

/* ---- G38c: the diffuse/specular alpha a guest vertex program writes, as a range ----
 *
 * experiments/d3d8_boundary/vsh_od0w.c measured the title's 126 programs:
 * oD0.w is built from constant registers alone in 79, never written in 43 (the
 * emitter's default, float4(0,0,0,1): alpha 1), from input registers in 3 and
 * computed in 1. So for almost every draw the value is a function of the
 * constant file this draw uploads, and can be evaluated here. The emitter
 * saturates both colour outputs, so the result is clamped to [0,1]. Anything
 * the tree cannot see through -- an input register, an a0-relative constant,
 * a dot product, an ILU op other than MOV -- evaluates to "unknown", which
 * makes the draw ineligible, never wrong. */
enum { OD_UNK = 0, OD_CONST, OD_ONE, OD_MOV, OD_MUL, OD_ADD, OD_MAD, OD_MIN, OD_MAX };
static NV2AVshProgram g_od_prog;   /* pusher thread only */

static int od_new(VshSlot *v, uint8_t op)
{
    if (v->od_n >= (int)(sizeof v->od_node / sizeof v->od_node[0])) return -1;
    memset(&v->od_node[v->od_n], 0, sizeof v->od_node[0]);
    v->od_node[v->od_n].op = op;
    return v->od_n++;
}
static int od_temp(VshSlot *v, int before, int reg, int lane, int depth);
static int od_src(VshSlot *v, int slot, const NV2AVshSrcOperand *o, int lane, int depth)
{
    int c = lane == 0 ? o->swizzle.x : lane == 1 ? o->swizzle.y : lane == 2 ? o->swizzle.z : o->swizzle.w;
    int n;
    if (o->reg_type == NV2A_VSH_REG_CONST && !o->rel_addr) {
        if ((n = od_new(v, OD_CONST)) < 0) return -1;
        v->od_node[n].idx = (uint16_t)o->reg_index; v->od_node[n].comp = (uint8_t)c;
        v->od_node[n].neg = (uint8_t)(o->negate != 0);
        return n;
    }
    if (o->reg_type == NV2A_VSH_REG_TEMP) {
        int t = od_temp(v, slot, o->reg_index, c, depth + 1);
        if (t < 0 || !o->negate) return t;
        if ((n = od_new(v, OD_MOV)) < 0) return -1;
        v->od_node[n].a = (int16_t)t; v->od_node[n].neg = 1;
        return n;
    }
    return od_new(v, OD_UNK);
}
static int od_mac(VshSlot *v, int slot, int lane, int depth)
{
    const NV2AVshInstruction *in = &g_od_prog.insns[slot];
    int n, a = -1, b = -1, c = -1; uint8_t op;
    switch (in->mac_op) {
    case NV2A_VSH_MAC_MOV: return od_src(v, slot, &in->mac_src[0], lane, depth);
    case NV2A_VSH_MAC_MUL: op = OD_MUL; break;
    case NV2A_VSH_MAC_ADD: op = OD_ADD; break;
    case NV2A_VSH_MAC_MAD: op = OD_MAD; break;
    case NV2A_VSH_MAC_MIN: op = OD_MIN; break;
    case NV2A_VSH_MAC_MAX: op = OD_MAX; break;
    default: return od_new(v, OD_UNK);
    }
    a = od_src(v, slot, &in->mac_src[0], lane, depth);
    if (op != OD_ADD) b = od_src(v, slot, &in->mac_src[1], lane, depth);
    if (op == OD_ADD || op == OD_MAD) c = od_src(v, slot, &in->mac_src[2], lane, depth);
    if (a < 0 || (op != OD_ADD && b < 0) || ((op == OD_ADD || op == OD_MAD) && c < 0)) return -1;
    if ((n = od_new(v, op)) < 0) return -1;
    v->od_node[n].a = (int16_t)a; v->od_node[n].b = (int16_t)b; v->od_node[n].c = (int16_t)c;
    return n;
}
static int od_temp(VshSlot *v, int before, int reg, int lane, int depth)
{
    if (depth > 24) return od_new(v, OD_UNK);
    for (int i = before - 1; i >= 0; --i) {
        const NV2AVshInstruction *in = &g_od_prog.insns[i];
        uint8_t bit = (uint8_t)(1u << (3 - lane));
        if (in->mac_op != NV2A_VSH_MAC_NOP && in->mac_dst.temp_reg == reg && (in->mac_dst.write_mask & bit))
            return od_mac(v, i, lane, depth);
        if (in->ilu_op != NV2A_VSH_ILU_NOP && in->ilu_dst.temp_reg == reg && (in->ilu_dst.write_mask & bit))
            return in->ilu_op == NV2A_VSH_ILU_MOV ? od_src(v, i, &in->ilu_src, lane, depth) : od_new(v, OD_UNK);
    }
    return od_new(v, OD_UNK);
}
static void vsh_out_alpha_build(VshSlot *v)
{
    if (v->od_ready) return;
    v->od_ready = -1; v->od_n = 0;
    if (v->is_ff || nv2a_vsh_parse(&v->words[0][0], v->length * 4, &g_od_prog) < 0) return;
    for (int k = 0; k < 2; ++k) {
        NV2AVshOutputReg out = k ? NV2A_VSH_OUT_D1 : NV2A_VSH_OUT_D0;
        int last = -1, mac = 0, root;
        for (int i = 0; i < g_od_prog.length; ++i) {
            const NV2AVshInstruction *in = &g_od_prog.insns[i];
            if (in->mac_op != NV2A_VSH_MAC_NOP && in->mac_dst.output_reg == out && (in->mac_dst.output_mask & 1)) { last = i; mac = 1; }
            if (in->ilu_op != NV2A_VSH_ILU_NOP && in->ilu_dst.output_reg == out && (in->ilu_dst.output_mask & 1)) { last = i; mac = 0; }
        }
        if (last < 0) root = od_new(v, OD_ONE);          /* the emitter's default: float4(0,0,0,1) */
        else if (mac) root = od_mac(v, last, 3, 0);
        else root = g_od_prog.insns[last].ilu_op == NV2A_VSH_ILU_MOV
                  ? od_src(v, last, &g_od_prog.insns[last].ilu_src, 3, 0) : od_new(v, OD_UNK);
        if (root < 0) return;
        v->od_root[k] = (int16_t)root;
    }
    v->od_ready = 1;
}
typedef struct { float lo, hi; } Rng;
static Rng rng_mul(Rng a, Rng b)
{
    float p[4] = { a.lo*b.lo, a.lo*b.hi, a.hi*b.lo, a.hi*b.hi }; Rng r = { p[0], p[0] };
    for (int i = 1; i < 4; ++i) { if (p[i] < r.lo) r.lo = p[i]; if (p[i] > r.hi) r.hi = p[i]; }
    if (r.lo != r.lo || r.hi != r.hi) { r.lo = -INFINITY; r.hi = INFINITY; }   /* 0*inf */
    return r;
}
static Rng od_eval(const VshSlot *v, int n, const float (*c)[4])
{
    const __typeof__(v->od_node[0]) *d = &v->od_node[n]; Rng r, x, y, z;
    switch (d->op) {
    case OD_CONST: if (!c || d->idx >= 192) { r.lo = -INFINITY; r.hi = INFINITY; return r; }
                   r.lo = r.hi = d->neg ? -c[d->idx][d->comp] : c[d->idx][d->comp];
                   if (r.lo != r.lo) { r.lo = -INFINITY; r.hi = INFINITY; }
                   return r;
    case OD_ONE:   r.lo = r.hi = 1.0f; return r;
    case OD_MOV:   x = od_eval(v, d->a, c); if (d->neg) { r.lo = -x.hi; r.hi = -x.lo; return r; } return x;
    case OD_MUL:   return rng_mul(od_eval(v, d->a, c), od_eval(v, d->b, c));
    case OD_ADD:   x = od_eval(v, d->a, c); z = od_eval(v, d->c, c); r.lo = x.lo + z.lo; r.hi = x.hi + z.hi; return r;
    case OD_MAD:   x = rng_mul(od_eval(v, d->a, c), od_eval(v, d->b, c)); z = od_eval(v, d->c, c);
                   r.lo = x.lo + z.lo; r.hi = x.hi + z.hi; return r;
    case OD_MIN:   x = od_eval(v, d->a, c); y = od_eval(v, d->b, c);
                   r.lo = fminf(x.lo, y.lo); r.hi = fminf(x.hi, y.hi); return r;
    case OD_MAX:   x = od_eval(v, d->a, c); y = od_eval(v, d->b, c);
                   r.lo = fmaxf(x.lo, y.lo); r.hi = fmaxf(x.hi, y.hi); return r;
    default:       r.lo = -INFINITY; r.hi = INFINITY; return r;
    }
}
/* The saturated output alpha range of oD0 (k=0) or oD1 (k=1); [0,1] if unknown. */
static Rng vsh_out_alpha(VshSlot *v, int k, const float (*c)[4])
{
    Rng r = { 0.0f, 1.0f };
    if (!v) return r;
    vsh_out_alpha_build(v);
    if (v->od_ready != 1) return r;
    r = od_eval(v, v->od_root[k], c);
    if (!(r.lo >= 0.0f)) r.lo = 0.0f; if (r.lo > 1.0f) r.lo = 1.0f;    /* saturate(), NaN-safe */
    if (!(r.hi <= 1.0f)) r.hi = 1.0f; if (r.hi < 0.0f) r.hi = 0.0f;
    return r;
}
static const float (*vsh_constants)[4];
static uint64_t g_vsh_gpu_draws, g_vsh_cpu_draws, g_vsh_compiles, g_vsh_hits;
static uint64_t g_vsh_refused_emit, g_vsh_refused_compile, g_vsh_cache_full;
static uint64_t g_vsh_gpu_vertices;

static uint32_t vsh_hash(const uint32_t (*w)[4], int len)
{   /* FNV-1a over the words. The hash is a FAST REJECT ONLY -- every candidate
     * is confirmed with a full word compare below, because a cache that hands
     * a draw the wrong vertex program produces wrong geometry and no error
     * anywhere. trace_selected_program() has identified programs by a 32-bit
     * hash for months without a collision, which is not the same fact. */
    uint32_t h = 2166136261u; int i, k;
    for (i = 0; i < len; ++i) for (k = 0; k < 4; ++k) {
        h ^= w[i][k]; h *= 16777619u;
    }
    return h;
}

/* Rewrite the emitted program so it can live beside the fragment tails.
 *
 * Three textual changes, all of them structural rather than cosmetic:
 *   - the entry point becomes a plain function, because MSL cannot call a
 *     `vertex` function and the wrapper has to call it;
 *   - [[stage_in]] becomes an indexed read out of a plain buffer, so this path
 *     needs no MTLVertexDescriptor and no drawIndexedPrimitives -- it keeps
 *     the index-in-the-shader shape the existing `vs` already uses and that
 *     the rest of nv2a_metal_draw is built around;
 *   - a wrapper repacks VS_OUT into Out field by field.
 * Returns 0 if the emitted text is not the shape this expects, which is a
 * refusal and not a guess. */
static int vsh_wrap(const char *src, uint16_t inputs, unsigned nattrs,
                    char *out, size_t outsize)
{
    const char *sig_in  = "vertex VS_OUT vsh_main(VS_IN input [[stage_in]],\n"
                          "                      constant float4 *c [[buffer(1)]],\n"
                          "                      constant VSH_Viewport &viewport [[buffer(2)]]) {\n";
    const char *sig_non = "vertex VS_OUT vsh_main(constant float4 *c [[buffer(1)]],\n"
                          "                      constant VSH_Viewport &viewport [[buffer(2)]]) {\n";
    const char *sig = inputs ? sig_in : sig_non;
    const char *at = strstr(src, sig);
    size_t used = 0; unsigned i, slot = 0;
    if (!at) return 0;
    /* head, then the rewritten signature */
    used = (size_t)(at - src);
    if (used + 4096 > outsize) return 0;
    memcpy(out, src, used);
    used += (size_t)snprintf(out + used, outsize - used,
        "static VS_OUT vsh_body(const device float4 *raw, uint base,\n"
        "                       constant float4 *c,\n"
        "                       constant VSH_Viewport &viewport) {\n");
    /* body, with the stage_in aliases replaced */
    {
        const char *body = at + strlen(sig);
        size_t blen = strlen(body);
        if (used + blen + 4096 > outsize) return 0;
        memcpy(out + used, body, blen); out[used + blen] = 0;
        for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
            char from[64], to[64], *hit;
            if (!(inputs & (1u << i))) continue;
            snprintf(from, sizeof from, "    float4 v%u = input.v%u;\n", i, i);
            snprintf(to,   sizeof to,   "    float4 v%u = raw[base + %u];\n", i, slot++);
            hit = strstr(out + used, from);
            if (!hit) return 0;
            memmove(hit + strlen(to), hit + strlen(from),
                    strlen(hit + strlen(from)) + 1);
            memcpy(hit, to, strlen(to));
        }
        used += strlen(out + used);
    }
    /* the wrapper. `Out` and the index buffer are the existing pipeline's, so
     * everything downstream of the vertex stage is untouched. */
    used += (size_t)snprintf(out + used, outsize - used,
        "\nvertex Out vs_gpu(uint id [[vertex_id]],\n"
        "                  const device float4 *raw [[buffer(0)]],\n"
        "                  constant float4 *c [[buffer(1)]],\n"
        "                  constant VSH_Viewport &vp [[buffer(2)]],\n"
        "                  const device uint *indices [[buffer(3)]]) {\n"
        "  VS_OUT o = vsh_body(raw, indices[id] * %uu, c, vp);\n"
        "  Out r; r.p=o.oPos; r.d0=o.oD0; r.d1=o.oD1;\n"
        "  r.t0=o.oT0; r.t1=o.oT1; r.t2=o.oT2; r.t3=o.oT3;\n"
        "  return r;\n}\n", nattrs ? nattrs : 1u);
    return used < outsize;
}

/* Find or build the MTLFunction for this program. Called from the executor
 * BEFORE it decides whether to run the interpreter, so a refusal here is a
 * clean CPU draw rather than a half-transformed batch. */
/* ONE CACHE, TWO KINDS OF VERTEX SHADER.
 *
 * A guest program is (words,length); a fixed-function shader is an opaque
 * NV2AFFKey blob. They differ only in how the entry is keyed and which emitter
 * produces the MSL -- everything after that is identical, which is the whole
 * reason the fixed-function path was shaped this way. vsh_wrap(), the pipeline
 * lookup, the vertex packing and the buffer bindings are untouched: the two
 * emitters produce a byte-compatible signature block and alias lines, and
 * nv2a_ff_constants is float[192][4], exactly the 192*16 bytes already sent at
 * buffer(1). */
static VshSlot *vsh_lookup_ex(const void *blob, int length, unsigned keysize,
                              int is_ff, uint16_t inputs)
{
    uint32_t h = is_ff ? vsh_hash((const uint32_t (*)[4])blob, (int)(keysize / 16))
                       : vsh_hash((const uint32_t (*)[4])blob, length);
    size_t bytes = is_ff ? (size_t)keysize : (size_t)length * 16;
    unsigned i, n;
    for (i = 0; i < vsh_slot_n; ++i) {
        VshSlot *v = &vsh_slot[i];
        if (v->hash != h || v->inputs != inputs) continue;
        if (v->is_ff != is_ff) continue;
        if (is_ff ? (v->keysize != keysize) : (v->length != length)) continue;
        if (memcmp(v->words, blob, bytes)) continue;   /* full compare */
        ++g_vsh_hits;
        return v->refused ? NULL : v;
    }
    if (vsh_slot_n >= VSH_CACHE) { ++g_vsh_cache_full; return NULL; }
    if (is_ff) { if (!keysize || keysize > sizeof vsh_slot[0].words) return NULL; }
    else if (length <= 0 || length > NV2A_VS_MAX_INSTRUCTIONS) return NULL;

    {   /* Build it. Everything below happens once per distinct program. */
        VshSlot *v = &vsh_slot[vsh_slot_n++];
        NV2AVshProgram prog;
        char *emitted = malloc(262144), *wrapped = malloc(524288);
        memcpy(v->words, blob, bytes);
        v->length = is_ff ? 0 : length; v->hash = h; v->inputs = inputs;
        v->is_ff = is_ff; v->keysize = keysize;
        for (n = 0, i = 0; i < NV2A_VS_MAX_INPUTS; ++i)
            if (inputs & (1u << i)) ++n;
        v->nattrs = n;
        v->refused = 1;                    /* until proven otherwise */
        if (!emitted || !wrapped) { free(emitted); free(wrapped); return NULL; }
        int emitted_ok;
        if (is_ff) {
            emitted_ok = nv2a_ff_generate_msl((const NV2AFFKey *)v->words,
                                              emitted, 262144);
        } else {
            nv2a_vsh_parse((const uint32_t (*)[4])blob, length, &prog);
            emitted_ok = prog.valid
                      && nv2a_vsh_generate_msl(&prog, emitted, 262144);
        }
        if (!emitted_ok
            || !vsh_wrap(emitted, inputs, v->nattrs, wrapped, 524288)) {
            ++g_vsh_refused_emit; free(emitted); free(wrapped); return NULL;
        }
        @autoreleasepool {
            /* The program is appended to the SHARED source, so vs_gpu and the
             * fragment tails come out of one library and Out means the same
             * type on both sides of the stage boundary. */
            NSString *whole = [shader stringByAppendingString:
                                 [NSString stringWithUTF8String:wrapped]];
            NSError *err = nil;
            MTLCompileOptions *opt = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            /* SAFE MATH, not the default, and not a style preference: the
             * differential test measured this emitter against the interpreter
             * under safe math only. Under fast math rsqrt, pow and the
             * reciprocals are different functions and that result does not
             * transfer. */
            if (@available(macOS 15.0,*)) opt.mathMode = MTLMathModeSafe;
            else opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
            v->library = [device newLibraryWithSource:whole
                          options:opt error:&err];
            if (!v->library) {
                static int told;
                if (!told++) fprintf(stderr,
                    "[METAL] vsh compile failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "(no error)");
                ++g_vsh_refused_compile;
            } else {
                v->fn = [v->library newFunctionWithName:@"vs_gpu"];
                if (v->fn) { v->refused = 0; ++g_vsh_compiles; }
                else ++g_vsh_refused_compile;
            }
        }
        free(emitted); free(wrapped);
        return v->refused ? NULL : v;
    }
}

int nv2a_metal_vsh_ready(const uint32_t (*words)[4], int length,
                         uint16_t inputs_read)
{
    if (!vsh_gpu_on() || !hw_state_on()) return 0;
    if (!initialize()) return 0;
    vsh_active = vsh_lookup_ex(words, length, 0, 0, inputs_read);
    return vsh_active != NULL;
}

/* THE FIXED-FUNCTION UNIT, ON THE GPU. Same contract as the call above, one
 * layer down: the executor asks BEFORE it decides whether to run
 * nv2a_ff_vertex on the CPU, and a 0 here is a clean CPU batch rather than a
 * half-transformed one. 62.8% of this title's index slots are transformed on
 * the CPU for want of this path -- 520 million of 829 million in one gameplay
 * run -- because only programmable-shader draws were ever GPU-shaded.
 *
 * The key is opaque here on purpose. It is hashed and memcmp'd like a
 * program's words and handed straight to nv2a_ff_generate_msl, so this file
 * holds no opinion about fixed-function state and cannot drift from the one
 * that does.
 *
 * GATED: RECOMP_METAL_FF, default off, and the emitter has a differential test
 * against nv2a_ff_vertex itself -- jsrf_ff_msl_diff_test, residual 0 over 15
 * states in both modes with an injected fault firing in every one. Do not turn
 * the switch on for a build whose test has not passed. A subtly wrong MSL
 * expression compiles clean and outputs black; this tree has lost two builds
 * that way. */
int nv2a_metal_ff_ready(const void *key, unsigned keysize, uint16_t inputs_read)
{
    if (!vsh_gpu_on() || !hw_state_on()) return 0;
    if (!initialize()) return 0;
    vsh_active = vsh_lookup_ex(key, 0, keysize, 1, inputs_read);
    return vsh_active != NULL;
}

void nv2a_metal_vsh_constants(const float (*c)[4]) { vsh_constants = c; }
void nv2a_metal_vsh_clear(void) { vsh_active = NULL; }

static int initialize(void)
{
    pthread_mutex_lock(&initialization_mutex);
    if(attempted){int ready=pipeline!=nil;pthread_mutex_unlock(&initialization_mutex);return ready;}
    device=MTLCreateSystemDefaultDevice();if(!device){attempted=1;pthread_mutex_unlock(&initialization_mutex);return 0;}
    if(pass_fence_on())g_pass_fence=[device newFence];
    NSError *error=nil;MTLCompileOptions *options=[MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if(@available(macOS 15.0,*)) options.mathMode=MTLMathModeSafe; else options.fastMathEnabled=NO;
#pragma clang diagnostic pop
    id<MTLLibrary> library=[device newLibraryWithSource:shader options:options error:&error];
    if(library){MTLRenderPipelineDescriptor *desc=[MTLRenderPipelineDescriptor new];
        /* Every fragment entry point reads the combiner-specialisation
         * constants through shade(), so each is fetched through frag_fn with
         * FC_SPEC false -- Metal refuses to build a pipeline from a function
         * whose constants were never given values. */
        hw_library=library;
        desc.vertexFunction=[library newFunctionWithName:@"vs"];desc.fragmentFunction=frag_fn(library,@"fs",NULL);
        /* Retained for the hardware-state pipelines, which are built lazily
         * per distinct blend state rather than once here. */
        hw_vs=[library newFunctionWithName:@"vs"];hw_fs=frag_fn(library,@"fs_hw",NULL);
        hw_fs_blend=frag_fn(library,@"fs_hw_blend",NULL);
        hw_fs_early=frag_fn(library,@"fs_hw_early",NULL);
        hw_fs_early_nw=frag_fn(library,@"fs_hw_early_nw",NULL);
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
/* Draws that took the SOFTWARE tail while the hardware path was switched on,
 * and depth uploads that failed and caused it. Neither was counted, and the
 * mixed frame they produce is the one the depth-ownership comment describes:
 * "every differing pixel had software 0x000000 against hardware 0xFFFFFF".
 * refusals= stays 0 through all of it, because reject() is never reached. */
static unsigned long long g_hw_mixed, g_hw_upload_fail;
/* How many depth/stencil states each bisect arm actually altered. Counted at
 * the build, not the draw -- hw_depth_state_for returns a cached state before
 * reaching either -- so a non-zero value says the arm reached the descriptor,
 * which is the fact the report cannot otherwise carry. */
static unsigned long long g_hw_depth_always_states, g_hw_no_stencil_states;

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

/* A DRAW THAT SAMPLES THE SURFACE IT IS RENDERING INTO. RECOMP_METAL_FEEDBACK_SYNC.
 *
 * Measured on xemu, 21 Sep 2026, last 800 flips of the Load-screen trace:
 * 21,573 draws target the render target and 15,162 of them bind that SAME
 * surface as TEXTURE0 (linear R5G6B5, 640x480) -- nineteen a flip. The title
 * screen does it too, 829 times in 2,688 flips, on its fades. On the NV2A the
 * sample reads the surface as it stands at that draw.
 *
 * texture_buffer uploads from GUEST RAM, and the bound surface's rendering
 * is only written back to guest RAM at a sync -- a flip, a clear, a target
 * change. So every one of those draws sampled the surface as it was at the
 * last sync, not as it is: the frame's fresh content was never in the bytes
 * it read. surface_pay_debt_for_range cannot help, because the bound
 * surface's debt is surface_dirty, not owes_guest_ram, and it walks the
 * latter.
 *
 * MEASURED AND REFUTED THE SAME NIGHT. The nineteen were a tally reading
 * the LATCHED texture offset on untextured draws; our counter and xemu's
 * own render_to_texture line both say ONE such draw a flip, and it is the
 * composite. The switch stays as a negative control (it changed nothing);
 * the counter stays because one-a-flip is the cheapest sighting of the
 * composite there is.
 *
 * g_feedback_draws counts the aliasing draws whether or not the switch is
 * on, so a run says how many there were before anyone argues about the
 * cost. With the switch on, each such draw syncs the bound surface first --
 * a drain and a 614 KB read-back per draw, nineteen a flip on that screen,
 * which is why it is opt-in until a run has said what it buys. */
static uint64_t g_feedback_draws, g_feedback_syncs;
static int feedback_sync_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_FEEDBACK_SYNC");
    return on;
}

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
        on = (e && *e) ? (atoi(e) != 0) : 1;
    }
    return on && hw_state_on();
}

/* DEFAULT ON since 16 Sep 2026. THE BLOCK BELOW USED TO SAY "DO NOT DEFAULT
 * THIS ON" AND ITS EVIDENCE HAS BEEN SUPERSEDED, NOT IGNORED. Read on.
 *
 * What it described -- "torn rectangular tiles carrying content from elsewhere
 * in the scene, and a large black band" -- is the artefact that was fixed on
 * 16 Sep by making every hardware draw read the colour attachment under
 * raster_order_group(0). It was never a 565 defect. 565 was measured beside it
 * and inherited the blame, because at the time EVERY hardware-path arm was
 * corrupt and there was no clean baseline to score against.
 *
 * Scored against a clean baseline, on the same binary and pad, 24 captured
 * gameplay frames: clean by eye, and the artefact's own structural fingerprint
 * -- the fraction of dark blocks whose left edge lands on a 4-pixel grid --
 * reads 32% against the software control's 30%, where the broken renderer read
 * 43% and chance is 25%.
 *
 * It is also FASTER and MORE CORRECT, which is why this is not a trade. Two
 * bytes a pixel instead of sixteen, in the guest's own format: both per-pixel
 * conversion loops become row memcpys, every colour transfer shrinks
 * eightfold, and the frame stops requantising between draws, so the double
 * rounding at the read-back boundary is gone. Measured 18.32 -> 16.37 ms,
 * 54.6 -> 61.1 fps.
 *
 * =0 takes RGBA32Float and remains the control arm.
 *
 * ------------------------------------------------------------------------
 * THE ORIGINAL NOTE FOLLOWS, kept because its METHOD lesson is still the
 * right one and is the reason the fingerprint above exists at all.
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
        /* DEFAULT ON since 16 Sep 2026, and the reason it was off until then
         * is now fixed. This path renders with real Depth32Float and Stencil8
         * attachments and the ROP, against the software tail's depth-in-alpha
         * and in-shader everything. It was default OFF because it rendered
         * gameplay visibly corrupt -- axis-aligned blocks holding fragments of
         * other scene content, on every frame -- which is the defect
         * hw_shader_blend's comment describes and which is fixed by every
         * hardware draw reading the colour attachment.
         *
         * MEASURED BEFORE FLIPPING IT, 16 Sep 2026, gameplay windows past
         * t=140 s, weighted by flips, one pinned binary and one pad:
         *
         *     hardware path, this default      31.75 ms   31.5 fps   clean
         *     software path, the control       32.58 ms   30.7 fps   clean
         *
         * and the artefact's own structural fingerprint -- the fraction of
         * dark blocks whose left edge lands on a 4-pixel grid -- reads 31% on
         * this path against the software control's 30%, where the broken
         * version read 43%.
         *
         * =0 takes the software path, and it is a real arm rather than a dead
         * one: it is the reference this path is scored against, and it is the
         * only path that works when a draw's state cannot be translated. */
        const char *e = getenv("RECOMP_METAL_HW");
        on = (e && *e) ? (atoi(e) != 0) : 1;
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

/* 32, NOT 16, BECAUSE THE KEY JUST GAINED A DIMENSION.
 *
 * The old 16 was sized from "JSRF uses three" and a real mission was later
 * measured at pipelines=4..5 with refusals=0, so it had comfortable headroom.
 * Keying on the shader-blend selector as well can double the set -- the same
 * blend state appears both dithered and not -- so the same measurement now
 * implies up to ten. A miss is not a small slowdown here: hw_pipeline_for
 * returns nil, the draw is rejected, and the pushbuffer executor falls back to
 * the CPU rasteriser for it. 32 keeps the margin the 16 used to have, at
 * 32 pointers.
 *
 * Still not evidence for a mission, which is what it should be sized from:
 * RECOMP_METAL_HW reports pipelines= and refusals= on every run, and the
 * number to watch is refusals staying at 0. */
#define HW_CACHE 32
/* EVERY HARDWARE DRAW READS THE COLOUR ATTACHMENT. That is the fix, and this
 * function is where it lives.
 *
 * WHAT WAS WRONG. fs_hw writes colour(0) and never reads it. fs_hw_blend
 * declares `float4 dst [[color(0), raster_order_group(0)]]`, exactly as the
 * software tail fs() does. Until 16 Sep 2026 the hardware path chose between
 * them per draw -- fs_hw_blend only for a draw that both blended and dithered
 * -- so the great majority of draws, all the opaque world geometry, wrote the
 * attachment with no destination read and no raster ordering. At gameplay that
 * renders visibly corrupt: axis-aligned blocks, median 5x5 px, holding black
 * or fragments of other scene content, on every frame.
 *
 * HOW IT IS KNOWN, and the shape of the evidence matters because six image
 * metrics failed on this artefact before one worked. Four arms, one pinned
 * binary, one pad, run serially, 24 captured gameplay frames each. The score
 * is `x0 mod 4`: the fraction of corruption blocks whose left edge lands on a
 * 4-pixel grid, against 25% by chance -- a within-frame structural ratio,
 * which is the only kind of statistic that has ever separated arms here.
 *
 *     mode 0  no draw reads dst, and no mixing either   36%   corrupt
 *     mode 1  dithered AND blended draws (the old one)  43%   corrupt
 *     mode 2  every blended draw                        40%   corrupt
 *     mode 3  every draw                                30%   CLEAN
 *     --      the software path, as the control         30%   clean
 *
 * Monotone in COVERAGE and nothing else. Mode 2 already takes every blended
 * draw off the fixed-function blend unit and gives it the read, and is still
 * corrupt; mode 0 removes fs_hw_blend entirely so the encoder never mixes two
 * blending modes, and is still corrupt. Only full coverage goes clean, landing
 * on the software control's own number.
 *
 * WHAT IT IS NOT. Not depth (compare forced ALWAYS: corrupt), not stencil
 * (ignored: corrupt), not the surface cache (off: corrupt), not the read-back
 * (19,777 gameplay syncs audited, 0 pixels changed after a full queue drain),
 * not the queue (22,072 explicit drains: corrupt), and not the tile: at 21 and
 * 7 bytes per pixel a 32 KB tile is 32x32 and 64x64, and the measured
 * granularity is the same 4x4 in both arms.
 *
 * THE COST. For an unblended draw s.blend is 0, so fs_hw_blend skips the
 * in-shader blend and produces byte-identical colour; only the dependency
 * changes. raster_order_group(0) serialises overlapping fragments, which is a
 * real cost and is the first thing to measure if this path disappoints -- see
 * the frame-time note beside the arms below.
 *
 * THE VALUE STILL SELECTS, so the defect is reproducible in one binary. */
static int hw_shader_blend_mode(int mode, int blend, int dither)
{
    /* Pure, so it can be tested without a device. metal_state_test walks all
     * four modes against all four (blend, dither) combinations; a gate that
     * needs a GPU is a gate that does not run. */
    switch (mode) {
    case 0:  return 0;              /* nothing reads dst -- the "before" arm.
                                     * Also puts the dither on the wrong side
                                     * of the blend, measured at 8192 of 65536
                                     * pixels on the dither's own 4x4 lattice,
                                     * so it carries two defects at once. */
    case 1:  return blend && dither;/* what shipped until 16 Sep 2026 */
    case 2:  return blend;          /* every blended draw */
    default: return 1;              /* every draw -- the fix */
    }
}

static int hw_shader_blend_mode_env(void)
{
    /* A VALUE, not presence: =0 has to mean off, which is the whole point of
     * an arm, and recomp_switch.h exists because three switches here were
     * presence-tested and their control arms silently ran with the guard on.
     * The default is 3 and the report prints the mode, so an A/B can verify
     * which arm actually ran rather than which one was asked for. */
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_METAL_SHADER_BLEND");
                  on = e ? atoi(e) : 3; }
    return on;
}

/* THE TWO DIAGNOSTIC SWITCHES THAT DECIDE WHETHER THE SHADER CAN DISCARD.
 *
 * Both were read inline, at the one place that builds Params, with a local
 * static each. The pipeline key now has to ask the same two questions -- the
 * early-Z variant is only sound for a draw that cannot reach a discard -- and
 * two copies of "what does this switch mean" is exactly how a pipeline ends
 * up selected for state the shader was not given. One reader each, both
 * callers. */
static int no_alpha_test_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_NO_ALPHA_TEST");
  return on; }
static int legacy_zclamp_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_LEGACY_ZCLAMP");
  return on; }

/* RECOMP_FRAG_FORCE=<mode> -- the modes are documented in shade(), which is
 * where they act. Read DIRECTLY rather than through recomp_switch.h, for the
 * same reason RECOMP_CLEAR_COLOR_FORCE is: the value carries meaning, and
 * recomp_switch_on maps every string that is not "0" to 1, which would
 * collapse all four modes into mode 1 without saying so. */
/* RECOMP_FRAG_FORCE_AFTER=<seconds> -- ARRIVE LATE, BECAUSE THE MENU HAS TO BE
 * READABLE TO GET TO THE SCREEN UNDER TEST.
 *
 * Mode 3 paints every fragment white, including the main menu, so a run that
 * needs a person to drive "title -> START -> main menu -> LOAD" cannot be
 * driven: there is nothing on screen to navigate by. The 15:57 run is the
 * evidence -- 66 seconds, presented coverage pinned at 307200/307200
 * throughout, and NOT ONE window at the Load screen's 28 batches per frame.
 * The experiment never reached its own condition and the result was worth
 * nothing.
 *
 * So the force holds off until this wall-clock second, which is the same
 * device RECOMP_GLYPH_DUMP_AFTER and RECOMP_FF_BATCH_DUMP_AFTER already use
 * for the same reason. Drive the menus normally, stop on the screen under
 * test, and the force arrives on top of it. */
/* RECOMP_FRAG_FORCE_ON_BLACK=<reports> -- ARM OFF THE DEFECT, NOT OFF A CLOCK.
 *
 * RECOMP_FRAG_FORCE_AFTER fixed the "the menu is white so it cannot be
 * navigated" problem and left a second one: the person driving has to reach
 * the screen under test BEFORE the second you guessed. They took 30 s to
 * reach the Load screen in one run and 50 s in the next, so any fixed
 * `after` is either early enough to blind the menu or late enough to miss the
 * window. That is the same failure that made the white test worthless twice.
 *
 * This arms on the thing being investigated instead. The presented frame is
 * all-zero for 33-35 seconds on the Load screen and is never all-zero on a
 * menu, so N consecutive black reports IS the condition -- it cannot fire
 * early and it cannot be missed. Once armed it LATCHES: the force turns the
 * screen white, the black that armed it goes away, and switching off then
 * would destroy the reading.
 *
 * Read it as: black, then armed, then either the screen goes WHITE -- the
 * batches do cover the presented surface and the defect is shading -- or it
 * STAYS BLACK, and they never covered it. The run carries its own positive
 * control, because the same run shows the menu rendering beforehand. */
static int s_frag_force_armed;

void nv2a_metal_frag_force_arm(void)
{
  if(s_frag_force_armed) return;
  s_frag_force_armed=1;
  fprintf(stderr,"  [FRAG-FORCE] ARMED by the black itself. Everything from"
      " here is painted by the force, so a white screen now means the batches"
      " were covering the surface all along.\n");
  fflush(stderr);
}

static uint32_t frag_force_mode(void)
{ static int init; static uint32_t mode; static double after; static long on_black;
  extern double xbox_TraceSeconds(void);
  if(!init){ const char*e=getenv("RECOMP_FRAG_FORCE");
             mode=(e&&*e)?(uint32_t)strtoul(e,NULL,10):0u; init=1;
             { const char*a=getenv("RECOMP_FRAG_FORCE_AFTER");
               after=(a&&*a)?atof(a):0.0; }
             { const char*b=getenv("RECOMP_FRAG_FORCE_ON_BLACK");
               on_black=(b&&*b)?strtol(b,NULL,10):0; }
             if(mode) fprintf(stderr,"  [FRAG-FORCE] mode %u -- every fragment"
                 " is replaced by %s. THIS RENDERS INCORRECTLY.\n", mode,
                 mode==1?"its TEXTURE0 sample":mode==2?"its PRIMARY_COLOR":
                 mode==3?"white":mode==4?"its TEXCOORD0 after the divide":
                 "nothing -- shade() ignores an unknown mode");
             if(mode&&after>0.0)
                 fprintf(stderr,"  [FRAG-FORCE] holding off until t=%.0fs --"
                     " drive the menus normally until then.\n", after);
             if(mode&&on_black>0)
                 fprintf(stderr,"  [FRAG-FORCE] holding off until the presented"
                     " frame has been BLACK for %ld reports. Drive the menus"
                     " normally; the defect arms this itself.\n", on_black); }
  /* Checked per draw, not cached: the whole point is that it changes. */
  if(mode&&on_black>0&&!s_frag_force_armed) return 0u;
  if(mode&&after>0.0&&xbox_TraceSeconds()<after) return 0u;
  return mode; }

/* RECOMP_METAL_EARLY_Z -- put the depth test in front of the shader for the
 * draws where that is exact.
 *
 * A fragment function that may call discard_fragment() cannot have its depth
 * test run first: the write would already have happened for a fragment the
 * shader then throws away. Metal therefore runs the whole shader for every
 * occluded fragment, and on this backend the whole shader is the register
 * combiner chain plus up to four SOFTWARE texture samples -- Morton address
 * arithmetic, hand-decoded DXT, hand-written bilinear, four `device uchar*`
 * fetches per sample. That is a great deal to spend on a fragment the depth
 * buffer was going to reject.
 *
 * fs_hw_blend discards for exactly two reasons, the guest's z-range CULL
 * policy and the alpha test, and both are Params fields. A draw with neither
 * cannot discard, so for that draw early and late tests are
 * indistinguishable and fs_hw_early is not an approximation. The predicate
 * has to mirror the Params build exactly, including the two diagnostic
 * switches that force either field to zero -- hence the readers above.
 *
 * Off by default until a scene-matched A/B has scored it: a pipeline variant
 * doubles the key space, and the number to watch beside any frame-time change
 * is refusals staying at 0. */
static int early_z_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_EARLY_Z");
  return on; }
static uint64_t g_early_z_draws, g_late_z_draws, g_early_nw_draws;
/* Latin font pages seen by the text detector above: distinct texture
 * addresses whose cells match the 21x34 Latin grid, and how many character
 * quads each drew. Two is correct; one means page 1 is never bound. */
static uint32_t g_latin_tex[16]; static uint64_t g_latin_tex_quads[16];
static unsigned g_latin_tex_n;
/* WHY the predicate refused, not just that it did. The 22 Sep A/B read
 * "0 draws early" in BOTH arms with the switch demonstrably on -- a failed
 * positive control, and the report could not say which of the two terms was
 * responsible across 761,548 draws. These only move when early_z_on(), so
 * they read zero in the control arm by construction. */
static uint64_t g_ez_no_alpha, g_ez_no_alpha_ref0, g_ez_no_zcull, g_ez_eligible;

/* RECOMP_METAL_EARLY_Z_REF0 -- MEASURES A CEILING, AND IS NOT A CANDIDATE
 * DEFAULT.
 *
 * Measured 22 Sep 2026: every one of 1,368,416 alpha-test refusals in a
 * scene-matched tutorial run had alpha_ref == 0. The title enables the alpha
 * test globally and never sets a cutout threshold, so the shader's discard is
 * `alpha <= 0` -- it rejects a FULLY TRANSPARENT fragment and nothing else.
 * That single state bit is what makes early-Z unreachable on every draw.
 *
 * With this on, such a draw is treated as non-discarding so its depth test
 * moves in front. That is NOT exact: a fully transparent fragment would write
 * depth and could occlude what is behind it. The point is to find out what
 * early-Z is worth before anyone pays for a correct version of it -- a depth
 * pre-pass, or splitting the alpha-zero texels out. If the ceiling is small
 * the whole item dies here; if it is large, it justifies the real work.
 *
 * The picture is the gate, not the frame time. */
static int early_z_ref0_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_EARLY_Z_REF0");
  return on; }

/* G27b: the draws that may still discard but write nothing an early test
 * could get wrong -- see fs_hw_early_nw. Mirrors the Params build: depth is
 * written only when the depth test is on, stencil only when its test is. */
static uint64_t g_ez_nowrite, g_ez_alpha_writes_cc0, g_ez_alpha_writes_cc;
static int hw_draw_writes_nothing_early(const NV2ATextureCopy *s)
{
    return !(s->depth_test && s->depth_write) && !(s->stencil_test && s->stencil_write);
}

/* G38 -- WHICH COMBINER ALPHA PROGRAMS DO THE LATE, ALPHA-TESTED, DEPTH-WRITING
 * DRAWS RUN? RECOMP_METAL_ALPHA_CENSUS=1, opt-in, read-only.
 *
 * Every one of those draws uses the combiners (23 Sep 2026: 0 without), so
 * whether a fragment can reach alpha 0 -- the only discard at alpha_ref 0 --
 * is a property of the alpha chain: combiner_count, each stage's alpha input
 * and output words, and the constant alphas it can read. Counted per distinct
 * configuration at the draw, top entries printed by nv2a_metal_report. */
static int alpha_census_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_ALPHA_CENSUS"); return on; }
#define ALPHA_CENSUS_SLOTS 512
static struct { uint64_t key; uint64_t draws; uint32_t cc, icw[8], ocw[8], k0a, k1a, tmask; } g_alpha_cfg[ALPHA_CENSUS_SLOTS];
static uint64_t g_alpha_cfg_overflow;
static void alpha_census_note(const NV2ATextureCopy *s)
{
    uint64_t h = 1469598103934665603ull; uint32_t k0a = 0, k1a = 0;
    unsigned cc = s->combiner_count > 8 ? 8 : s->combiner_count;
    for (unsigned i = 0; i < cc; ++i) {
        h = (h ^ s->alpha_icw[i]) * 1099511628211ull; h = (h ^ s->alpha_ocw[i]) * 1099511628211ull;
        k0a |= ((s->const0[i] >> 24) ? 1u : 0u) << i; k1a |= ((s->const1[i] >> 24) ? 1u : 0u) << i;
    }
    h = (h ^ cc) * 1099511628211ull; h = (h ^ k0a) * 1099511628211ull;
    h = (h ^ k1a) * 1099511628211ull; h = (h ^ s->texture_mask) * 1099511628211ull;
    for (unsigned n = 0; n < ALPHA_CENSUS_SLOTS; ++n) {
        unsigned i = (unsigned)((h + n) % ALPHA_CENSUS_SLOTS);
        if (g_alpha_cfg[i].draws && g_alpha_cfg[i].key != h) continue;
        if (!g_alpha_cfg[i].draws) {
            g_alpha_cfg[i].key = h; g_alpha_cfg[i].cc = cc; g_alpha_cfg[i].k0a = k0a;
            g_alpha_cfg[i].k1a = k1a; g_alpha_cfg[i].tmask = s->texture_mask;
            memcpy(g_alpha_cfg[i].icw, s->alpha_icw, sizeof g_alpha_cfg[i].icw);
            memcpy(g_alpha_cfg[i].ocw, s->alpha_ocw, sizeof g_alpha_cfg[i].ocw);
        }
        ++g_alpha_cfg[i].draws; return;
    }
    ++g_alpha_cfg_overflow;
}
static void alpha_census_report(void)
{
    if (!alpha_census_on()) return;
    uint64_t total = 0; unsigned used = 0;
    for (unsigned i = 0; i < ALPHA_CENSUS_SLOTS; ++i) if (g_alpha_cfg[i].draws) { total += g_alpha_cfg[i].draws; ++used; }
    fprintf(stderr, "[ALPHA-CENSUS] RECOMP_METAL_ALPHA_CENSUS=on late alpha-tested depth-writing draws=%llu configs=%u overflow=%llu\n",
            (unsigned long long)total, used, (unsigned long long)g_alpha_cfg_overflow);
    static unsigned char taken[ALPHA_CENSUS_SLOTS];
    memset(taken, 0, sizeof taken);
    for (unsigned r = 0; r < 24; ++r) {
        unsigned best = ALPHA_CENSUS_SLOTS; uint64_t bd = 0;
        for (unsigned i = 0; i < ALPHA_CENSUS_SLOTS; ++i)
            if (!taken[i] && g_alpha_cfg[i].draws > bd) { bd = g_alpha_cfg[i].draws; best = i; }
        if (best == ALPHA_CENSUS_SLOTS) break;
        fprintf(stderr, "[ALPHA-CENSUS] #%u draws=%llu (%.1f%%) cc=%u tmask=%X k0a=%X k1a=%X",
                r, (unsigned long long)bd, total ? 100.0 * bd / total : 0.0, g_alpha_cfg[best].cc,
                g_alpha_cfg[best].tmask, g_alpha_cfg[best].k0a, g_alpha_cfg[best].k1a);
        for (unsigned k = 0; k < g_alpha_cfg[best].cc; ++k)
            fprintf(stderr, " [%u]icw=%08X ocw=%08X", k, g_alpha_cfg[best].icw[k], g_alpha_cfg[best].ocw[k]);
        fprintf(stderr, "\n");
        taken[best] = 1;               /* rank without disturbing the counts: this runs periodically */
    }
}

/* G38c -- RECOMP_METAL_EARLY_Z_EXACT=1 (needs RECOMP_METAL_EARLY_Z=1 and
 * RECOMP_METAL_HW_TEX=1). An alpha-tested draw at alpha_ref 0 discards only a
 * fragment whose final alpha is below 1/510. When a lower bound on that alpha,
 * over every fragment the draw can produce, is at least 1/255, no fragment can
 * be discarded and fs_hw_early is exact. The bound comes from running the
 * draw's combiner alpha chain -- shade()'s exact steps -- over [lo,hi] ranges:
 * textures by the alpha range recorded at decode, diffuse/specular by the
 * vertex program's traced output (vsh_out_alpha). Anything not known is its
 * full possible range, which can only make a draw ineligible. */
static int early_z_exact_mode(void)   /* 0 off, 1 on, 2 audit: prove, stay late, count violations */
{ static int m=-1;
  if(m<0){const char*e=getenv("RECOMP_METAL_EARLY_Z_EXACT");
          if(!e||e[0]==0||!strcmp(e,"0"))m=0;          /* default OFF; empty is the default */
          else if(!strcmp(e,"audit"))m=2;
          else if(!strcmp(e,"audit-control"))m=3;   /* positive control: mark EVERY alpha-tested draw */
          else m=1;
          fprintf(stderr,"[METAL] RECOMP_METAL_EARLY_Z_EXACT=%s\n",m==3?"audit-control":m==2?"audit":m?"on":"off");}
  return m; }
static int early_z_exact_on(void) { return early_z_exact_mode() != 0; }
static id<MTLBuffer> g_ez_violation_buf;   /* device atomic_uint, fragment buffer 5 */
static float g_draw_alpha_floor = -1.0f;   /* this draw's proven alpha lower bound; -1 none */
static int hw_zcull_cannot_fire(const NV2ATextureCopy *s)
{
    return !(s->z_cull && !legacy_zclamp_on())
        || (s->z_clip_min <= 0.0f && s->z_clip_max >= 16777215.0f);
}
static uint64_t g_ez_exact, g_ez_exact_refused;

static Rng rng_c(float lo, float hi) { Rng r = { lo, hi }; return r; }
static float cl(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static Rng comb_input(uint32_t code, const Rng *alpha, uint32_t k0, uint32_t k1)
{
    uint32_t src = code & 15u; Rng x;
    if (code & 16u) x = alpha[src];
    else x = src == 0 ? rng_c(0, 0) : src == 1 ? rng_c((k0 & 255u) / 255.0f, (k0 & 255u) / 255.0f)
           : src == 2 ? rng_c((k1 & 255u) / 255.0f, (k1 & 255u) / 255.0f) : rng_c(-1.0f, 1.0f);   /* blue lane */
    float plo = fmaxf(0.0f, x.lo), phi = fmaxf(0.0f, x.hi);
    switch (code >> 5) {
    case 0: return rng_c(plo, phi);
    case 1: return rng_c(1.0f - fminf(1.0f, phi), 1.0f - fminf(1.0f, plo));
    case 2: return rng_c(2 * plo - 1, 2 * phi - 1);
    case 3: return rng_c(1 - 2 * phi, 1 - 2 * plo);
    case 4: return rng_c(plo - 0.5f, phi - 0.5f);
    case 5: return rng_c(0.5f - phi, 0.5f - plo);
    case 6: return x;
    default: return rng_c(-x.hi, -x.lo);
    }
}
static Rng comb_map(uint32_t m, Rng x)   /* cmap1, monotone increasing; then the [-1,1] clamp */
{
    float lo = x.lo, hi = x.hi;
    switch (m) { case 1: lo -= .5f; hi -= .5f; break; case 2: lo *= 2; hi *= 2; break;
                 case 3: lo = (lo - .5f) * 2; hi = (hi - .5f) * 2; break; case 4: lo *= 4; hi *= 4; break;
                 case 6: lo *= .5f; hi *= .5f; break; default: break; }
    return rng_c(cl(lo, -1, 1), cl(hi, -1, 1));
}
static float comb_alpha_floor(const NV2ATextureCopy *s, Rng d0, Rng d1, const Rng tex[4])
{
    Rng a[16];
    if (!s->combiner_count || s->combiner_count > 8) return -1.0f;
    for (int i = 0; i < 16; ++i) a[i] = rng_c(0, 0);
    a[4] = d0; a[5] = d1;
    for (int u = 0; u < 4; ++u) a[8 + u] = (s->texture_mask & (1u << u)) ? tex[u] : rng_c(0, 0);
    a[12] = (s->texture_mask & 1u) ? tex[0] : rng_c(1, 1);
    for (uint32_t st = 0; st < s->combiner_count; ++st) {
        uint32_t k0 = s->const0[st], k1 = s->const1[st], w = s->alpha_icw[st];
        uint32_t aw = s->alpha_ocw[st], cw = s->color_ocw[st];
        a[1] = rng_c((k0 >> 24) / 255.0f, (k0 >> 24) / 255.0f);
        a[2] = rng_c((k1 >> 24) / 255.0f, (k1 >> 24) / 255.0f);
        Rng A = comb_input(w >> 24, a, k0, k1), B = comb_input((w >> 16) & 255u, a, k0, k1);
        Rng C = comb_input((w >> 8) & 255u, a, k0, k1), D = comb_input(w & 255u, a, k0, k1);
        Rng ab = rng_mul(A, B), cd = rng_mul(C, D);
        uint32_t amx = (aw >> 14) & 1u, amp = (aw >> 15) & 7u, acd = aw & 15u, aab = (aw >> 4) & 15u, asum = (aw >> 8) & 15u;
        if (acd) a[acd] = comb_map(amp, cd);
        if (aab) a[aab] = comb_map(amp, ab);
        if (asum) {
            Rng sum;
            if (!amx) sum = rng_c(ab.lo + cd.lo, ab.hi + cd.hi);
            else if (a[12].lo >= 0.5f) sum = ab;
            else if (a[12].hi < 0.5f) sum = cd;
            else sum = rng_c(fminf(ab.lo, cd.lo), fmaxf(ab.hi, cd.hi));
            a[asum] = comb_map(amp, sum);
        }
        /* blue-to-alpha from the colour half, written last in shade() */
        if (((cw >> 19) & 1u) && ((cw >> 4) & 15u)) a[(cw >> 4) & 15u] = rng_c(-1, 1);
        if (((cw >> 18) & 1u) && (cw & 15u)) a[cw & 15u] = rng_c(-1, 1);
    }
    return cl(a[12].lo, 0.0f, 1.0f);   /* c = clamp(r[12] + ..., 0, 1); alpha gets no specular */
}

/* 0: late tests. 1: early, the draw cannot discard (fs_hw_early).
 * 2: early, the draw may discard but writes no depth or stencil
 *    (fs_hw_early_nw). Both early variants are exact. */
static int hw_early_z(const NV2ATextureCopy *s)
{
    if (!early_z_on()) return 0;
    int may_discard = (s->alpha_test && !no_alpha_test_on())
                   || (s->z_cull && !legacy_zclamp_on());
    if (may_discard && hw_draw_writes_nothing_early(s)) { ++g_ez_nowrite; ++g_ez_eligible; return 2; }
    /* G38c: an alpha test that provably cannot fire, and a z-range cull that
     * cannot either because its range is the whole depth range -- this title
     * sets CULL with [0, 16777215] on every draw (see z_clip_min). Whether a
     * fragment's z can still fall outside [0,1] here is checked, not assumed:
     * the audit counts z-cull discards of proven draws too. */
    if (early_z_exact_on() && s->alpha_test && !no_alpha_test_on() && s->alpha_ref == 0
        && hw_zcull_cannot_fire(s)) {
        if (g_draw_alpha_floor >= 1.0f / 255.0f) {
            ++g_ez_exact;
            if (early_z_exact_mode() >= 2) return 0;   /* audit: stay late, the shader counts */
            ++g_ez_eligible; return 1;
        }
        ++g_ez_exact_refused;
    }
    if (s->alpha_test && !no_alpha_test_on()) {
        /* What an exact version for the remaining draws would have to see
         * through: with no combiners the fragment alpha is diffuse alpha times
         * (at most) texture alpha; with combiners it is whatever the alpha
         * chain computes. */
        if (s->combiner_count) ++g_ez_alpha_writes_cc; else ++g_ez_alpha_writes_cc0;
        /* Split by the reference value. The shader discards on
         * alpha <= alpha_ref, so ref==0 rejects only a FULLY transparent
         * fragment: those draws are the ones where early-Z might be made
         * reachable, and ref>0 is a real cutout that never can be. */
        ++g_ez_no_alpha;
        if (s->alpha_ref == 0) {
            ++g_ez_no_alpha_ref0;
            if (early_z_ref0_on()) { ++g_ez_eligible; return 1; }
        }
        return 0;
    }
    if (s->z_cull && !legacy_zclamp_on())     { ++g_ez_no_zcull;  return 0; }
    ++g_ez_eligible;
    return 1;
}

static int hw_shader_blend(const NV2ATextureCopy *s)
{
    return hw_shader_blend_mode(hw_shader_blend_mode_env(), s->blend, s->dither);
}

int nv2a_metal_shader_blend_mode(void) { return hw_shader_blend_mode_env(); }
int nv2a_metal_shader_blend_for(int mode, int blend, int dither)
{ return hw_shader_blend_mode(mode, blend, dither); }

int nv2a_metal_shader_blend_on(void)
{ NV2ATextureCopy probe; memset(&probe, 0, sizeof probe);
  probe.blend = 1; probe.dither = 1; return hw_shader_blend(&probe); }

/* THE SELECTOR IS PART OF THE KEY. Two draws with identical blend state but
 * differing dither need DIFFERENT pipelines now -- one with the blend unit
 * enabled and fs_hw, one with it disabled and fs_hw_blend. Keying on blend
 * alone would hand the second draw the first one's pipeline and blend twice,
 * or not at all. */
static struct { uint32_t blend,src,dst,sblend,early; id<MTLRenderPipelineState> pso; }
    hw_pso[HW_CACHE];
static unsigned hw_pso_n;

/* A pipeline for a generated vertex program.
 *
 * Same shape as hw_pipeline_for -- fixed array, linear scan, refuse when full,
 * never evict -- with the program added to the key, because two draws with
 * identical blend state and different programs need different pipelines and a
 * key that cannot tell them apart hands one of them the other's geometry.
 * Sized from the same census as the program cache: 126 programs in the image,
 * a handful of blend states, and a refusal is a counted CPU draw rather than a
 * stall, so a generous fixed size costs pointers and nothing else. */
#define VSH_PSO_CACHE 256
static struct { const void *fn; uint32_t blend,src,dst,sblend,early;
                id<MTLRenderPipelineState> pso; } vsh_pso[VSH_PSO_CACHE];
static unsigned vsh_pso_n;
static uint64_t g_vsh_pso_full;

/* sblend and early come from the caller, which asks hw_shader_blend and
 * hw_early_z ONCE per draw: hw_early_z counts as it answers, and the
 * specialised lookup in front of this one asks the same question. */
static id<MTLRenderPipelineState> vsh_pipeline_for(const NV2ATextureCopy *s,
                                                   VshSlot *prog,
                                                   uint32_t sblend, uint32_t early)
{
    /* The variant is part of the key, not a property of the draw: two draws
     * with identical blend state and different discard state need different
     * pipelines, and a key that cannot tell them apart would hand an
     * alpha-tested draw the function whose depth write has already happened. */
    unsigned i;
    for (i = 0; i < vsh_pso_n; ++i)
        if (vsh_pso[i].fn == (__bridge const void *)prog->fn
            && vsh_pso[i].blend == s->blend && vsh_pso[i].src == s->blend_src
            && vsh_pso[i].dst == s->blend_dst && vsh_pso[i].sblend == sblend
            && vsh_pso[i].early == early)
            return vsh_pso[i].pso;
    if (vsh_pso_n >= VSH_PSO_CACHE) { ++g_vsh_pso_full; return nil; }
    {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        id<MTLRenderPipelineState> pso; NSError *err = nil;
        int sf = MTLBlendFactorOne, df = MTLBlendFactorZero;
        if (s->blend) {
            if (!nv2a_texture_copy_blend_factor_supported(s->blend_src)
             || !nv2a_texture_copy_blend_factor_supported(s->blend_dst)) return nil;
            sf = nv2a_metal_blend_factor(s->blend_src);
            df = nv2a_metal_blend_factor(s->blend_dst);
            if (sf < 0 || df < 0) return nil;
        }
        d.vertexFunction = prog->fn;
        /* THE FRAGMENT FUNCTION COMES OUT OF THE PROGRAM'S OWN LIBRARY, not
         * the shared one. Both libraries contain a function of that name
         * compiled from identical text, but `Out` is a distinct type per
         * library and a pipeline that straddles them matches its stage_in by
         * position rather than by name. Taking both halves from one library is
         * what makes the repacking wrapper sound. */
        d.fragmentFunction = frag_fn(prog->library,
                                early == 2 ? @"fs_hw_early_nw"
                              : early  ? @"fs_hw_early"
                              : sblend ? @"fs_hw_blend" : @"fs_hw", NULL);
        if (!d.fragmentFunction) return nil;
        d.colorAttachments[0].pixelFormat = hw_565_on() ? MTLPixelFormatB5G6R5Unorm
                                                        : MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        d.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
        if (s->blend && !sblend) {
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
            d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)sf;
            d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)df;
            d.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)sf;
            d.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)df;
        }
        pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) { ++g_hw_state_refusals; return nil; }
        vsh_pso[vsh_pso_n].fn = (__bridge const void *)prog->fn;
        vsh_pso[vsh_pso_n].blend = s->blend; vsh_pso[vsh_pso_n].src = s->blend_src;
        vsh_pso[vsh_pso_n].dst = s->blend_dst; vsh_pso[vsh_pso_n].sblend = sblend;
        vsh_pso[vsh_pso_n].early = early;
        vsh_pso[vsh_pso_n].pso = pso; ++vsh_pso_n;
        return pso;
    }
}

static id<MTLRenderPipelineState> hw_pipeline_for(const NV2ATextureCopy *s,
                                                  uint32_t sblend, uint32_t early)
{
    unsigned i;
    for (i = 0; i < hw_pso_n; ++i)
        if (hw_pso[i].blend == s->blend && hw_pso[i].src == s->blend_src
            && hw_pso[i].dst == s->blend_dst && hw_pso[i].sblend == sblend
            && hw_pso[i].early == early)
            return hw_pso[i].pso;
    if (hw_pso_n >= HW_CACHE) { ++g_hw_state_refusals; return nil; }

    int sf = MTLBlendFactorOne, df = MTLBlendFactorZero;
    if (s->blend) {
        /* THE SHARED ACCEPT TEST DECIDES, not the enum map.
         *
         * nv2a_metal_blend_factor is a faithful NV2A-to-Metal translation and
         * knows DST_ALPHA, ONE_MINUS_DST_ALPHA and SRC_ALPHA_SATURATED.
         * nv2a_texture_copy_blend_factor_supported deliberately does not, and
         * the software fragment tail's bfactor() implements exactly the eight
         * it allows -- anything else falls through its default and contributes
         * nothing. Two lists, and only one of them was consulted here.
         *
         * Nothing reaches a sink with those three today, because the accept
         * test refuses them in prepare_texture_copy first. The hazard is that
         * widening the accept test silently widens THIS path and not the other
         * one: on the hardware path a DST_ALPHA factor would read the colour
         * attachment's alpha, which holds the shaded alpha, or the guest's
         * DEPTH immediately after a surface upload, or -- under 565 -- does not
         * exist at all.
         *
         * So the sink asks the shared test, and the enum map stays a pure
         * translation with no policy in it. nv2a_metal_state.c has no title
         * knowledge and is the piece most ready to go upstream; this is the
         * right side of that line for the decision to live on. */
        if (!nv2a_texture_copy_blend_factor_supported(s->blend_src)
         || !nv2a_texture_copy_blend_factor_supported(s->blend_dst)) {
            ++g_hw_state_refusals; return nil;
        }
        sf = nv2a_metal_blend_factor(s->blend_src);
        df = nv2a_metal_blend_factor(s->blend_dst);
        if (sf < 0 || df < 0) { ++g_hw_state_refusals; return nil; }
    }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = hw_vs;
    d.fragmentFunction = early == 2 ? hw_fs_early_nw : early ? hw_fs_early : sblend ? hw_fs_blend : hw_fs;
    if (!d.fragmentFunction) { ++g_hw_state_refusals; return nil; }
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
    if (s->blend && !sblend) {
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
    hw_pso[hw_pso_n].dst = s->blend_dst; hw_pso[hw_pso_n].sblend = sblend;
    hw_pso[hw_pso_n].early = early;
    hw_pso[hw_pso_n].pso = pso;
    ++hw_pso_n;
    return pso;
}

/* ===== COMBINER SPECIALISATION: A PIPELINE PER COMBINER PROGRAM ==========
 *
 * RECOMP_METAL_SPECIALISE_COMBINERS, default ON; =0 is the control arm and is
 * the generic interpreter exactly as it ran before this existed.
 *
 * WHY. shade() interprets the register combiners for every pixel of every
 * draw: a loop over a run-time stage count, four input switches per channel,
 * and a float4 r[14] written at run-time indices, which puts the register
 * file in memory. Measured 24 Sep 2026 with the player's configuration: the
 * tutorial spends ~10.4 ms of a 17.7 ms frame executing on the GPU for ~71
 * draws at 640x480, and over the whole tutorial the fixed-function draws use
 * only 13 distinct combiner setups. A program known when the pipeline is
 * built lets the compiler unroll the stages, resolve every switch, and keep
 * r[] in registers -- and the whole cost is one compile per program.
 *
 * EXACT, NOT CLOSE. Both arms run the same comb_stage() on the same values in
 * the same order, so specialising must not move a single bit;
 * metal_combiner_spec_check.sh holds that byte-for-byte over the D3D
 * fixed-function setups and a set of arbitrary programs, with a positive
 * control (RECOMP_METAL_SPECIALISE_COMBINERS_CONTROL, below) that must differ.
 *
 * THE KEY is the combiner program (CombKey: stage count, texture mask, which
 * units sample in hardware, add_specular/untextured/modulate, and the four
 * words of each live stage -- dead stages' words are zeroed so they cannot
 * split the cache), plus everything the generic pipeline caches key on: the
 * vertex function (the fixed `vs` or the guest program's), the blend state,
 * the shader-blend selector and the early-Z variant.
 *
 * BOUNDED, and the bound is a fallback, never a refusal: when the table is
 * full or a compile fails, the draw takes the generic pipeline, which is
 * always correct. A failed compile is remembered so it is not retried every
 * draw. The compile is SYNCHRONOUS, on the draw thread, so the first draw of
 * each new program stalls for it -- see [METAL] combiner specialisation's
 * compile time and worst; asynchronous compilation is the follow-up if the
 * worst case shows up as a hitch. */
static int spec_combiners_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on_default("RECOMP_METAL_SPECIALISE_COMBINERS",1); return on; }
/* THE POSITIVE CONTROL. Specialises on deliberately WRONG words -- stage 0's
 * colour input A with its mapping's low bit flipped, which turns
 * UNSIGNED_IDENTITY into UNSIGNED_INVERT -- so a scene that cannot tell a
 * wrong specialisation from a right one says so. Renders incorrectly by
 * construction; the check script is its only user. */
static int spec_control_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_SPECIALISE_COMBINERS_CONTROL"); return on; }

#define SPEC_FN_CACHE  512
#define SPEC_PSO_CACHE 512
static struct { const void *lib; NSString *name; int generic; uint64_t h; CombKey ck;
                id<MTLFunction> fn; } spec_fn[SPEC_FN_CACHE];
static unsigned spec_fn_n;
static struct { const void *vfn; uint32_t blend,src,dst,sblend,early; uint64_t h; CombKey ck;
                id<MTLRenderPipelineState> pso; int failed; } spec_pso[SPEC_PSO_CACHE];
static unsigned spec_pso_n;
static uint64_t g_spec_built, g_spec_hits, g_spec_fns, g_spec_generic_fns;
static uint64_t g_spec_fb_full, g_spec_fb_failed, g_spec_fb_wide, g_spec_compile_ns, g_spec_compile_max_ns;

static uint64_t spec_hash(const void *p, size_t n, uint64_t h)
{ const uint8_t *b = p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } return h; }

/* Returns 0 for a program the shader cannot be specialised on: a stage count
 * past the eight words it has constants for. */
static int comb_key(const NV2ATextureCopy *s, unsigned hwmask, CombKey *k)
{
    if (s->combiner_count > 8) return 0;
    memset(k, 0, sizeof *k);
    k->cc = s->combiner_count;
    k->tmask = s->texture_mask & 15u;
    k->hw = hwmask & k->tmask;
    if (k->cc) {
        /* untextured and modulate are only read when there are no stages */
        k->flags = s->add_specular ? 1u : 0u;
        for (uint32_t i = 0; i < k->cc; ++i) {
            k->ci[i] = s->color_icw[i]; k->ai[i] = s->alpha_icw[i];
            k->co[i] = s->color_ocw[i]; k->ao[i] = s->alpha_ocw[i];
        }
    } else
        k->flags = (s->untextured ? 2u : 0u) | (s->modulate ? 4u : 0u);
    return 1;
}

/* A fragment entry point out of `lib`: specialised on `ck`, or with ck NULL
 * the generic interpreter (FC_SPEC false). Cached per library, because the
 * guest-program pipelines take their fragment function out of the program's
 * own library (see vsh_pipeline_for) and asking Metal for a specialised
 * function compiles it. nil on failure; a full table still answers the
 * generic request, uncached, because the generic function is the fallback. */
static id<MTLFunction> frag_fn(id<MTLLibrary> lib, NSString *name, const CombKey *ck)
{
    const void *lp = (__bridge const void *)lib;
    uint64_t h = spec_hash(&lp, sizeof lp, 1469598103934665603ull);
    unsigned i;
    h = spec_hash(name.UTF8String, strlen(name.UTF8String), h);
    if (ck) h = spec_hash(ck, sizeof *ck, h);
    for (i = 0; i < spec_fn_n; ++i)
        if (spec_fn[i].h == h && spec_fn[i].lib == lp && spec_fn[i].generic == !ck
            && [spec_fn[i].name isEqualToString:name]
            && (!ck || !memcmp(&spec_fn[i].ck, ck, sizeof *ck)))
            return spec_fn[i].fn;
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    bool on = ck != NULL;
    [cv setConstantValue:&on type:MTLDataTypeBool atIndex:0];
    if (ck) {
        CombKey k = *ck;
        if (spec_control_on()) k.ci[0] ^= 0x20000000u;
        [cv setConstantValue:&k.cc type:MTLDataTypeUInt atIndex:1];
        [cv setConstantValue:&k.tmask type:MTLDataTypeUInt atIndex:2];
        [cv setConstantValue:&k.hw type:MTLDataTypeUInt atIndex:3];
        [cv setConstantValue:&k.flags type:MTLDataTypeUInt atIndex:4];
        [cv setConstantValues:k.ci type:MTLDataTypeUInt withRange:NSMakeRange(8, 8)];
        [cv setConstantValues:k.ai type:MTLDataTypeUInt withRange:NSMakeRange(16, 8)];
        [cv setConstantValues:k.co type:MTLDataTypeUInt withRange:NSMakeRange(24, 8)];
        [cv setConstantValues:k.ao type:MTLDataTypeUInt withRange:NSMakeRange(32, 8)];
    }
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:name constantValues:cv error:&err];
    if (!fn) {
        static int told;
        if (!told++) fprintf(stderr, "[METAL] %s fragment function %s failed: %s\n",
                             ck ? "specialised" : "generic", name.UTF8String,
                             err ? err.localizedDescription.UTF8String : "(no error)");
        return nil;
    }
    if (ck) ++g_spec_fns; else ++g_spec_generic_fns;
    if (spec_fn_n < SPEC_FN_CACHE) {
        spec_fn[spec_fn_n].lib = lp; spec_fn[spec_fn_n].name = name;
        spec_fn[spec_fn_n].generic = !ck; spec_fn[spec_fn_n].h = h;
        if (ck) spec_fn[spec_fn_n].ck = *ck; else memset(&spec_fn[spec_fn_n].ck, 0, sizeof(CombKey));
        spec_fn[spec_fn_n].fn = fn; ++spec_fn_n;
    }
    return fn;
}

/* The specialised pipeline for this draw, or nil for "use the generic one".
 * prog is the guest vertex program when one is active, else the fixed `vs`.
 * Blend and attachment setup mirror hw_pipeline_for/vsh_pipeline_for; an
 * untranslatable blend returns nil uncounted so the generic lookup refuses it
 * with its own counter, exactly as before. */
static id<MTLRenderPipelineState> spec_pipeline_for(const NV2ATextureCopy *s, unsigned hwmask,
                                                    VshSlot *prog, uint32_t sblend, uint32_t early)
{
    CombKey ck;
    const void *vfn;
    uint64_t h;
    unsigned i;
    if (!spec_combiners_on()) return nil;
    if (!comb_key(s, hwmask, &ck)) { ++g_spec_fb_wide; return nil; }
    vfn = prog ? (__bridge const void *)prog->fn : (__bridge const void *)hw_vs;
    h = spec_hash(&ck, sizeof ck, spec_hash(&vfn, sizeof vfn, 1469598103934665603ull));
    {   uint32_t b[5] = { s->blend, s->blend_src, s->blend_dst, sblend, early };
        h = spec_hash(b, sizeof b, h); }
    for (i = 0; i < spec_pso_n; ++i)
        if (spec_pso[i].h == h && spec_pso[i].vfn == vfn
            && spec_pso[i].blend == s->blend && spec_pso[i].src == s->blend_src
            && spec_pso[i].dst == s->blend_dst && spec_pso[i].sblend == sblend
            && spec_pso[i].early == early && !memcmp(&spec_pso[i].ck, &ck, sizeof ck)) {
            if (spec_pso[i].failed) { ++g_spec_fb_failed; return nil; }
            ++g_spec_hits; return spec_pso[i].pso;
        }
    if (spec_pso_n >= SPEC_PSO_CACHE) { ++g_spec_fb_full; return nil; }
    int sf = MTLBlendFactorOne, df = MTLBlendFactorZero;
    if (s->blend) {
        if (!nv2a_texture_copy_blend_factor_supported(s->blend_src)
         || !nv2a_texture_copy_blend_factor_supported(s->blend_dst)) return nil;
        sf = nv2a_metal_blend_factor(s->blend_src);
        df = nv2a_metal_blend_factor(s->blend_dst);
        if (sf < 0 || df < 0) return nil;
    }
    unsigned long long t0 = mtl_now_ns();
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLRenderPipelineState> pso = nil;
    NSError *err = nil;
    d.vertexFunction = prog ? prog->fn : hw_vs;
    d.fragmentFunction = frag_fn(prog ? prog->library : hw_library,
                                 early == 2 ? @"fs_hw_early_nw"
                               : early  ? @"fs_hw_early"
                               : sblend ? @"fs_hw_blend" : @"fs_hw", &ck);
    if (d.vertexFunction && d.fragmentFunction) {
        d.colorAttachments[0].pixelFormat = hw_565_on() ? MTLPixelFormatB5G6R5Unorm
                                                        : MTLPixelFormatRGBA32Float;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        d.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
        if (s->blend && !sblend) {
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
            d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)sf;
            d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)df;
            d.colorAttachments[0].sourceAlphaBlendFactor = (MTLBlendFactor)sf;
            d.colorAttachments[0].destinationAlphaBlendFactor = (MTLBlendFactor)df;
        }
        pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) {
            static int told;
            if (!told++) fprintf(stderr, "[METAL] specialised pipeline failed: %s\n",
                                 err ? err.localizedDescription.UTF8String : "(no error)");
        }
    }
    {   unsigned long long dt = mtl_now_ns() - t0;
        g_spec_compile_ns += dt; if (dt > g_spec_compile_max_ns) g_spec_compile_max_ns = dt; }
    spec_pso[spec_pso_n].vfn = vfn; spec_pso[spec_pso_n].h = h; spec_pso[spec_pso_n].ck = ck;
    spec_pso[spec_pso_n].blend = s->blend; spec_pso[spec_pso_n].src = s->blend_src;
    spec_pso[spec_pso_n].dst = s->blend_dst; spec_pso[spec_pso_n].sblend = sblend;
    spec_pso[spec_pso_n].early = early;
    spec_pso[spec_pso_n].pso = pso; spec_pso[spec_pso_n].failed = pso == nil;
    ++spec_pso_n;
    if (!pso) { ++g_spec_fb_failed; return nil; }
    ++g_spec_built;
    return pso;
}

static void spec_report(void)
{
    uint64_t fb = g_spec_fb_full + g_spec_fb_failed + g_spec_fb_wide;
    fprintf(stderr, "[METAL] combiner specialisation: built=%llu hits=%llu fallbacks=%llu"
            " (cache full %llu, compile failed %llu, >8 stages %llu); %llu specialised"
            " functions, %llu generic; %.1f ms compiling, worst %.1f ms"
            " (metal_specialise_combiners %s)%s\n",
            (unsigned long long)g_spec_built, (unsigned long long)g_spec_hits,
            (unsigned long long)fb, (unsigned long long)g_spec_fb_full,
            (unsigned long long)g_spec_fb_failed, (unsigned long long)g_spec_fb_wide,
            (unsigned long long)g_spec_fns, (unsigned long long)g_spec_generic_fns,
            g_spec_compile_ns / 1e6, g_spec_compile_max_ns / 1e6,
            spec_combiners_on() ? "on" : "OFF",
            spec_control_on() ? " -- POSITIVE CONTROL: specialised on WRONG words,"
                                " renders incorrectly" : "");
}

/* THE TWO BISECT SWITCHES, AS PREDICATES THAT CAN NAME THEMSELVES.
 *
 * They were read inline in hw_depth_state_for, where nothing outside that
 * function could see their state -- so a run taking one of these arms produced
 * a report indistinguishable from a run that did not, and the arm could only
 * be asserted from the command line that launched it. This project has already
 * paid for that shape three times (recomp_switch.h lists them): an A/B whose
 * control arm silently ran with the guard on. An earlier pass of exactly this
 * bisect was reported as "still broken" and then had to be voided.
 *
 * So the state is a function, and nv2a_metal_report prints it. "I set the
 * variable" and "the model read it" are different facts. */
static int hw_depth_always_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_HW_DEPTH_ALWAYS");
    return on;
}

static int hw_no_stencil_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_HW_NO_STENCIL");
    return on;
}

/* G3: IS THE DEPTH WRITE-BACK BUYING ANYTHING?
 *
 * RECOMP_METAL_NO_DEPTH_SYNC=1, default OFF, diagnostic in the same sense as
 * RECOMP_METAL_HW_NO_STENCIL above: a path that never returns depth to guest
 * RAM is wrong by construction if anything reads it.
 *
 * WHY IT IS WORTH ASKING. A2 (RECOMP_METAL_DEFER_SWAP) refuses to defer a swap
 * whenever depth is dirty, because surface_slot_writeback carries colour only.
 * Measured on a 160 s gameplay run, that refusal fired 5,747-12,656 times
 * against ONE successful deferral -- so the depth refusal is the whole reason
 * A2 is inert, and the whole reason [SYNC] still owns the median frame.
 *
 * The same run says where sync's time actually goes:
 *
 *     sync 38089 calls (12696 already clean): 83847.0 ms draining the GPU,
 *                                             22728.8 ms reading back
 *
 * The DRAIN is 79% of it, not the read-back. The G3 plan was written as though
 * the 4.9 MB copy were the cost; it is the wait. That matters here because
 * skipping the depth read-back removes a drain as well as a copy.
 *
 * This switch answers "does anything read depth from guest RAM" the only way
 * that is cheap: stop writing it and look. If the frame is unchanged, the
 * write-back is buying nothing and A2's refusal can be narrowed. If the frame
 * breaks, it says exactly what depends on it.
 *
 * Counted either way, so a run with it OFF still reports how many write-backs
 * it would have skipped -- which is the number that says whether the switch is
 * worth an A/B at all. */
/* DEFAULT ON since 18 Sep 2026, on a player's look and a scene-matched A/B.
 *
 *   speed        9.1% less frame time, ranges NOT overlapping, eight usable
 *                runs, arms verified distinct. ~27,000 write-backs skipped per
 *                160 s run, each one a GPU drain as well as a 4.9 MB copy.
 *   correctness  zero non-MATCH blit checks across twelve runs, AND the
 *                player played a session with it on and reported the picture
 *                clean -- no sorting errors, nothing through walls.
 *
 * That second line is the one that matters. This tree has shipped two switches
 * on test evidence without a look and backed both out, and the rule that came
 * from it is that a player-facing default needs a picture or a listen. This
 * one has a picture.
 *
 * RECOMP_METAL_NO_DEPTH_SYNC=0 restores the write-back. The grammar is
 * empty-value-safe on purpose: `(e && *e)` rather than `e`, because an
 * exported-but-empty variable must not read as "off" -- that exact bug sat in
 * RECOMP_APU_FEDEC_HOLD for a week.
 *
 * WHAT THIS DOES NOT DO: reach 60 fps. The same session ran 15.5 ms for ~35
 * windows and then degraded to 21 ms, so the median across a session is not
 * reliably inside the 16.68 ms budget. The 9% is real and it is not the whole
 * gap. */
static int no_depth_sync_on(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_switch_on_default("RECOMP_METAL_NO_DEPTH_SYNC", 1);
    return on;
}
static uint64_t g_depth_syncs_skipped, g_depth_syncs_taken;
/* Deferrals that only happened because the depth write-back is off. */
static uint64_t g_swap_deferred_no_depth;

/* G3: AND IS THE COLOUR WRITE-BACK BUYING ANYTHING?
 *
 * RECOMP_METAL_NO_COLOUR_SYNC=1, default OFF, diagnostic in exactly the sense
 * RECOMP_METAL_HW_NO_STENCIL and RECOMP_METAL_NO_DEPTH_SYNC above are: a path
 * that never returns colour to guest RAM is wrong by construction IF anything
 * reads it, and the only cheap way to learn whether anything does is to stop
 * writing it and look.
 *
 * WHY THIS IS THE NEXT QUESTION AFTER DEPTH. no_depth_sync asked it of depth
 * and won 9.1% of the frame -- ~27,000 write-backs skipped per 160 s run, no
 * crash, eight usable runs with non-overlapping ranges. The same 160 s run
 * says where what is left of sync's time goes:
 *
 *     sync 38089 calls (12696 already clean): 83847.0 ms draining the GPU,
 *                                             22728.8 ms reading back
 *
 * The swap drain is 79% of it, and it is SCENE-driven rather than thermal: a
 * 1,523 s session at constant workload held frame time and drain-per-call flat
 * to within 2%, so it is a cost the WORK is paying and not the machine giving
 * up. Colour is the only remaining thing the swap writes back.
 *
 * THERE IS OUTSIDE EVIDENCE THE ANSWER CAN BE "NOTHING READS IT". Microsoft's
 * own Xbox backward-compatibility packages ship a per-title switch granting
 * permission to elide resolves -- `xoallowtitletoskipresolves`. Same cost,
 * same question, and decided PER TITLE, which is the shape of an answer that
 * is a property of the game rather than of the hardware. It is a reason to
 * ask; it is not an answer for this title.
 *
 * WHAT IS ACTUALLY SKIPPED. The two per-pixel conversion loops and the row
 * copies, and -- when nothing else wants the pixels -- the 4.9 MB getBytes
 * that feeds them. The DRAIN above is NOT removed directly: it is paid before
 * the dirty flags are examined, as it must be while anything might still be
 * read back. It comes off indirectly, exactly the way depth's did: clearing
 * surface_dirty without paying it lets every later sync that has no new draw
 * behind it take the `!surface_dirty && !depth_dirty` early return, which is
 * the one path in this function that skips the wait.
 *
 * COUNTED IN BOTH ARMS, which is the property that decides whether the A/B is
 * worth taking at all. With the switch OFF, g_color_syncs_taken IS the number
 * of write-backs the ON arm would have skipped; with it on,
 * g_color_syncs_skipped is the number it did. So either arm's report sizes the
 * question on its own, before a single paired run is spent on it -- and if the
 * taken count in a control run came back small, the honest move is to not run
 * the A/B at all.
 *
 * THE HONEST LIMIT, and it is the same one no_depth_sync carries: "IT RAN AND
 * PRESENTED" IS NOT "IT RENDERED CORRECTLY". Neither instrument that would
 * have to notice can see a colour-dependent artifact. d3d8_gl.c's blit check
 * samples ONE pixel -- the drawable centre against the guest's centre pixel --
 * and [FB] is a SUM over the framebuffer, which cancels as readily as it
 * differs. Neither can see mid-frame content that depended on the previous
 * frame's colour having reached guest RAM: a motion trail, a feedback blur, a
 * flare composited from the last frame, anything read back as a texture. A
 * green report from those two means "nothing crashed and the centre pixel
 * agreed", and that is the whole of what it means.
 *
 * So this stays default OFF until a player has LOOKED at a session with it on.
 * That is not caution for its own sake: this tree has shipped two switches on
 * test evidence without a look and backed both out, and no_depth_sync only
 * became a default because it had a picture behind it as well as eight runs.
 *
 * Grammar note: recomp_switch_on(), so "0" and empty are off and "1", "on",
 * "yes" are on -- the one grammar stated in recomp_switch.h, not a fourth
 * hand-rolled getenv. */
static int no_colour_sync_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_NO_COLOUR_SYNC");
    return on;
}
/* Both arms, so the OFF arm still reports the size of the ON arm's saving.
 * g_color_readbacks_elided is the narrower fact: syncs where the skip also
 * removed the 4.9 MB getBytes, because no depth write-back wanted the alpha
 * out of the same buffer. Separate because "skipped the loop" and "skipped
 * the copy" are different amounts of time and a reader must not have to guess
 * which one a number describes. */
static uint64_t g_color_syncs_skipped, g_color_syncs_taken;
static uint64_t g_color_readbacks_elided;

/* ELEVEN, and it was nine. The key has to name every field the descriptor
 * below reads, or the cache serves a state built for a different draw.
 * stencil_zfail feeds depthFailureOperation and stencil_func_mask feeds
 * readMask, and neither was in the key -- so two draws differing only in one
 * of those got whichever state was built first, on the hardware path only,
 * silently. metal_batch_test phase I measures it at 8056 of 65536 pixels. */
static struct { uint32_t key[11]; id<MTLDepthStencilState> dss; }
    hw_dss[HW_CACHE];
static unsigned hw_dss_n;

static id<MTLDepthStencilState> hw_depth_state_for(const NV2ATextureCopy *s)
{
    uint32_t k[11] = { s->depth_test, s->depth_write, s->depth_func,
                       s->stencil_test, s->stencil_write, s->stencil_mask,
                       s->stencil_func, s->stencil_fail, s->stencil_zpass,
                       s->stencil_zfail, s->stencil_func_mask };
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
    /* RECOMP_METAL_HW_DEPTH_ALWAYS=1 neuters the depth test on this path only.
     *
     * A bisect instrument, not a mode. The hardware path loses large
     * rectangular regions of a real frame that the software path renders
     * correctly, and the two candidate shapes are "the draws never reach the
     * attachment" and "the draws reach it and the depth unit rejects them
     * against contents that are wrong". Forcing ALWAYS separates those in one
     * run: if the missing regions come back, the uploaded depth is the
     * problem; if they stay missing, depth is not involved and the stencil,
     * the blend unit or the store is.
     *
     * It renders incorrectly by construction -- everything draws over
     * everything -- so it is only ever a diagnostic. */
    if (hw_depth_always_on()) { cmp = NV2A_MTL_CMP_ALWAYS; ++g_hw_depth_always_states; }
    d.depthCompareFunction = (MTLCompareFunction)cmp;
    /* GATED ON THE TEST, like the software tail and like the hardware.
     *
     * This read s->depth_write alone, while the fragment tail derives its flag
     * as depth_test && depth_write and nv2a_metal_draw marks depth_dirty on
     * the same pair. With the test off, cmp is forced to ALWAYS above, so a
     * draw with the mask still set -- an overlay, a UI quad -- stamped its own
     * depth over everything it covered on this path and over nothing on the
     * other. The NV2A does not update depth when the test is disabled, and
     * neither does nv2a_texture_copy.c.
     *
     * The second-order half is worse than the divergence: depth_dirty is not
     * set for such a draw, so whatever this wrote was never read back and the
     * next surface swap re-uploaded over it from stale guest RAM. */
    d.depthWriteEnabled = (s->depth_test && s->depth_write) ? YES : NO;
    {   /* RECOMP_METAL_HW_NO_STENCIL=1: the same bisect, one stage along.
         * Depth was ruled out by forcing ALWAYS and watching the regions stay
         * missing; this asks the question of the stencil unit. Diagnostic
         * only -- a path that ignores stencil renders wrongly by construction. */
        if (hw_no_stencil_on()) { ++g_hw_no_stencil_states; goto no_stencil; }
    }
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
no_stencil:
    ;
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
            if(entry->buffer) {
                const uint8_t *have=entry->buffer.contents;
                if(entry->verified_frame==texture_frame) {
                    ++texture_partial_compares;
                    if(!texture_probe_differs(have,data,size)) {
                        entry->stamp=++texture_clock;++texture_hits;texture_last_slot=entry;return entry->buffer;
                    }
                    ++texture_partial_caught;
                } else {
                    ++texture_full_compares;
                    if(!memcmp(have,data,size)) {
                        entry->verified_frame=texture_frame;
                        entry->stamp=++texture_clock;++texture_hits;texture_last_slot=entry;return entry->buffer;
                    }
                }
                ++texture_changed;
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
    slot->verified_frame=texture_frame;   /* just copied from data: equal by construction */
    slot->hw=nil;                         /* built from the old bytes */
    texture_last_slot=slot;
    return buffer;
}

/* G27 -- SAMPLE TEXTURES IN HARDWARE. RECOMP_METAL_HW_TEX=1, OFF BY DEFAULT.
 *
 * The fragment shader has always decoded guest textures itself out of a raw
 * MTLBuffer (texel/sample_level/sample_lod above): Morton addressing, DXT
 * block decode, bilinear and mip blending as arithmetic, up to eight software
 * texel fetches per sample. Measured 22 Sep 2026: that is the bulk of `sync`,
 * 8.9 ms of GPU execution per 640x480 frame on an M1 Max.
 *
 * Here the same bytes become an RGBA8 MTLTexture, decoded ONCE per upload by
 * nv2a_texture_decode_rgba8(), whose output is checked texel-for-texel
 * against the shader's own texel() rules by texture_decode_test.c; and the
 * sampler hardware does the filtering. Scope, deliberately narrow: the
 * swizzled formats whose coordinates sample_level() normalises -- RGBA8
 * (0x06/0x07), DXT1, DXT3 -- which are all but a few thousand of the title's
 * samples. Everything else (linear 565, the swizzled 16-bit formats) keeps the
 * software path, per unit, in the same draw.
 *
 * Off by default until the goal's exit criteria both pass: a scene-matched
 * A/B with `sync` down, AND image equality against the software sampler. */
static int hw_tex_on(void)
{
    static int on=-1;
    if(on<0){on=recomp_switch_on("RECOMP_METAL_HW_TEX");
        fprintf(stderr,"[METAL] RECOMP_METAL_HW_TEX=%s (G27 hardware texture sampling)\n",on?"on":"off");}
    return on;
}
static uint64_t hw_tex_builds,hw_tex_reuses,hw_tex_units,hw_tex_failed,hw_tex_bytes;
static id<MTLTexture> hw_dummy_texture;
static id<MTLSamplerState> hw_samplers[64],hw_default_sampler;

static int hw_tex_format(const NV2ATextureCopy *t)
{
    return t->dxt1?NV2A_TEXFMT_DXT1:t->dxt3?NV2A_TEXFMT_DXT3:t->rgba8?NV2A_TEXFMT_RGBA8:0;
}
/* sample_lod() walks the mip chain only when min_filter selects a mip mode
 * (3..6) and there is more than one level; otherwise it always reads level 0. */
static int hw_tex_mipped(const NV2ATextureCopy *t){return t->min_filter>=3&&t->levels>=2;}

/* Build (or reuse) the MTLTexture for the slot texture_buffer() just returned. */
static uint8_t hw_last_min_a = 0, hw_last_max_a = 255;  /* of the texture texture_hw() last returned */
static id<MTLTexture> texture_hw(const NV2ATextureCopy *t)
{
    TextureBuffer *slot=texture_last_slot;
    int fmt=hw_tex_format(t);
    unsigned mips=hw_tex_mipped(t)?t->levels:1;
    uint32_t key[5]={(uint32_t)fmt,t->width,t->height,t->pitch,mips};
    if(!slot||!slot->buffer||!fmt)return nil;
    if(slot->hw&&!memcmp(slot->hw_key,key,sizeof key)){++hw_tex_reuses;
        hw_last_min_a=slot->hw_min_a;hw_last_max_a=slot->hw_max_a;return slot->hw;}
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:t->width height:t->height mipmapped:NO];
    d.mipmapLevelCount=mips;d.usage=MTLTextureUsageShaderRead;d.storageMode=MTLStorageModeShared;
    id<MTLTexture> tex=[device newTextureWithDescriptor:d];
    uint8_t *rgba=malloc((size_t)t->width*t->height*4);
    const uint8_t *src=slot->buffer.contents;size_t left=slot->size,off=0;
    unsigned w=t->width,h=t->height,pitch=t->pitch;
    int ok=tex&&rgba;
    uint8_t amin=255,amax=0;
    for(unsigned l=0;ok&&l<mips;l++){
        size_t level=nv2a_texture_level_bytes(fmt,w,h,pitch);
        if(off>left||!nv2a_texture_decode_rgba8(src+off,left-off,w,h,pitch,fmt,rgba)){ok=0;break;}
        [tex replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:l withBytes:rgba bytesPerRow:(NSUInteger)w*4];
        for(size_t q=3;q<(size_t)w*h*4;q+=4){uint8_t a=rgba[q];if(a<amin)amin=a;if(a>amax)amax=a;}
        hw_tex_bytes+=(uint64_t)w*h*4;
        off+=level;pitch=nv2a_texture_next_pitch(fmt,w,pitch);w=w>1?w/2:1;h=h>1?h/2:1;
    }
    free(rgba);
    if(!ok){++hw_tex_failed;return nil;}
    slot->hw=tex;memcpy(slot->hw_key,key,sizeof key);++hw_tex_builds;
    slot->hw_min_a=amin;slot->hw_max_a=amax;hw_last_min_a=amin;hw_last_max_a=amax;
    return tex;
}

/* The choices sample_lod() makes by hand, as sampler state:
 *   magnification: s.linear (mag filter 2 = linear)
 *   minification : (min_filter & 1) == 0 is linear -- 1,3,5 nearest; 2,4,6 linear
 *   mip          : none unless hw_tex_mipped(); 3,4 nearest level; 5,6 blend two
 *   wrap         : repeat, else clamp to edge (texel() clamps the coordinate) */
static id<MTLSamplerState> hw_sampler(const NV2ATextureCopy *t)
{
    unsigned mipped=(unsigned)hw_tex_mipped(t);
    unsigned key=(t->linear?1u:0u)|((t->min_filter&7u)<<1)|(mipped<<4)|((t->repeat?1u:0u)<<5);
    if(hw_samplers[key&63])return hw_samplers[key&63];
    MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];
    d.magFilter=t->linear?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
    d.minFilter=(t->min_filter&1u)==0?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
    d.mipFilter=!mipped?MTLSamplerMipFilterNotMipmapped:t->min_filter>=5?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
    d.sAddressMode=d.tAddressMode=t->repeat?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeClampToEdge;
    d.normalizedCoordinates=YES;
    return hw_samplers[key&63]=[device newSamplerStateWithDescriptor:d];
}

/* Always bind SOMETHING to texture(u)/sampler(u): the shader only reads them
 * under s.hw[u], but an unbound argument is a validation error. */
static void hw_bind_defaults(void)
{
    if(!hw_dummy_texture){
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        d.usage=MTLTextureUsageShaderRead;hw_dummy_texture=[device newTextureWithDescriptor:d];
        uint32_t zero=0;[hw_dummy_texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
    }
    if(!hw_default_sampler)hw_default_sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
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
 * nv2a_metal_draw rejects count>NV2A_METAL_MAX_VERTICES and a Vertex is 112
 * bytes, so vertices cost at most 1.75 MB, and indices are bounded by the
 * assembly array at 3x that count, 192 KB. Just under two megabytes worst case,
 * which is why the slab below is eight and not the two it was at the old cap.
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
/* Vertices in one batch. Must match NV_MAX_INDICES in nv2a_pb_exec.c: the
 * executor assembles up to that many and hands them here, and a smaller limit
 * turns every large batch into a CPU-rasterised one. */
#define NV2A_METAL_MAX_VERTICES 16384
#define RING_SLABS 8
#define RING_SLAB_BYTES (8u<<20)
#define RING_ALIGN 256u
#define RING_ALIGN_UP(n) (((n)+RING_ALIGN-1)&~(size_t)(RING_ALIGN-1))

static id<MTLBuffer> ring_slab[RING_SLABS];
/* How many of the slabs are in use. RING_SLABS in every normal run; the ring
 * self-test squeezes it to two, because a wrap that takes 36 draws to come
 * round is a wrap the GPU has always finished with by the time it matters, and
 * a hazard that cannot be reached cannot be tested for. */
static unsigned ring_limit = RING_SLABS;
/* THE SAME SQUEEZE, IN THE LIVE TITLE. RECOMP_METAL_RING_SLABS=<2..8>.
 *
 * The self-test can only drive a copy of the draw path's SHAPE. The question
 * a real run has to answer is different: with eight 8 MB slabs, does the ring
 * ever come round at all while the game is drawing? A run whose wrap count is
 * zero has PROVED that no staging byte was ever handed out twice, and no
 * amount of argument about pins and fences is needed to explain a defect --
 * the ring cannot have caused it.
 *
 * That proof is only worth having with a positive control beside it, because
 * "wraps=0" is an absence measurement and this project has been burned by
 * those. Squeezing the ring to two slabs makes wraps unavoidable in the real
 * title, so a run with RECOMP_METAL_RING_SLABS=2 says what a wrapping ring
 * looks like, and RECOMP_METAL_RING_NOPIN=1 below says what an UNPROTECTED
 * wrapping ring looks like. Three arms, and only the third may corrupt.
 *
 * Read once, in ring_configure(), because the self-test assigns ring_limit
 * directly and must not have it read back out from under it. */
static int ring_limit_configured;
static int ring_selftest_active;
/* HOW MUCH OF A SLAB A RUN IS ALLOWED TO USE. RECOMP_METAL_RING_SLAB_KB=<n>.
 *
 * Squeezing the slab COUNT is not by itself a stress, and the control run
 * says why: this backend drains the GPU on every surface swap -- 9,388 of
 * them in a 150 s gameplay run, about 63 a second -- so the CPU cannot get a
 * whole slab ahead however few slabs there are. 8 x 8 MB came round 2,501
 * times in that run and the in-flight count was zero on every one of them.
 *
 * A hazard the stress cannot reach is a hazard the stress says nothing
 * about, so the usable span of a slab is settable too. At 256 KB the ring
 * comes round several times a frame instead of twice a second, which is the
 * only regime in which "the GPU is still reading this" can be true. The slab
 * is still ALLOCATED at RING_SLAB_BYTES -- only the bump pointer's ceiling
 * moves -- so a batch too large for the squeezed span takes the private
 * allocation fallback and is counted there rather than silently overrunning. */
static size_t ring_usable = RING_SLAB_BYTES;
/* RECOMP_METAL_RING_NOPIN=1 -- A DELIBERATE CORRUPTION ARM. It renders
 * incorrectly by construction and exists only to be the positive control for
 * the arms above: with the pin removed the wrap gate can never block, so
 * staging memory is handed back while the GPU is still reading it. If a
 * defect blamed on the ring does not appear in THIS arm, the ring is not what
 * produces it. Never leave it on. */
static int ring_nopin_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_RING_NOPIN"); return on; }
static void ring_configure(void)
{
    const char *e;
    if (ring_limit_configured) return;
    ring_limit_configured = 1;
    e = getenv("RECOMP_METAL_RING_SLABS");
    if (e && *e) {
        int n = atoi(e);
        if (n < 2) n = 2;
        if (n > RING_SLABS) n = RING_SLABS;
        ring_limit = (unsigned)n;
    }
    e = getenv("RECOMP_METAL_RING_SLAB_KB");
    if (e && *e) {
        long kb = atol(e);
        if (kb < 64) kb = 64;                 /* still holds a real batch */
        if ((size_t)kb * 1024u > RING_SLAB_BYTES) kb = RING_SLAB_BYTES / 1024u;
        ring_usable = (size_t)kb * 1024u;
    }
    if (ring_limit != RING_SLABS || ring_usable != RING_SLAB_BYTES
        || ring_nopin_on())
        fprintf(stderr, "[METAL] RING DIAGNOSTIC ARM: %u of %d slabs, %zu KiB"
                " usable of %u%s. This is not a mode -- with the pin removed"
                " the ring hands staging memory back while the GPU is still"
                " reading it, and the frame is wrong by construction.\n",
                ring_limit, RING_SLABS, ring_usable >> 10,
                (unsigned)(RING_SLAB_BYTES >> 10),
                ring_nopin_on() ? ", PINNING REMOVED" : ", pinning intact");
}
static void (*ring_wrap_hook)(void);
static unsigned ring_current;
static size_t ring_offset;
static pthread_mutex_t ring_mutex=PTHREAD_MUTEX_INITIALIZER;

/* IS THE SINGLE-THREAD ASSUMPTION TRUE? The block above states it -- "nv2a_
 * metal_draw is only ever reached from the pusher thread -- the same
 * assumption the texture cache, last_command and all of the surface state
 * already make" -- and states it as an assumption. Nothing has ever checked
 * it, and it is load-bearing for four separate pieces of mutable state.
 *
 * If it is false the consequence is precisely the defect being hunted on
 * 16 Sep 2026: ring_offset is a plain static, so two threads reserving at
 * once hand out OVERLAPPING spans and one batch's vertices are written over
 * another's. The picture that produces is a handful of primitives drawn at
 * some other batch's coordinates, once, rarely -- which is what the GPU
 * fixed-function arm did to two quote glyphs in Gum's tutorial text
 * (frame-bisect/ffglyph2/ff-on/fb/flip013.bmp) while the vertex data the CPU
 * fetched was byte-identical on every draw.
 *
 * One pointer compare per draw, always on, and it names both threads once.
 * A run that never prints this has PROVED the assumption for that run, which
 * is worth more than the comment above; a run that does print it has found
 * the bug. */
static _Atomic(pthread_t) g_draw_thread;
static _Atomic(unsigned long) g_draw_other_thread;
static void draw_thread_check(void)
{
    pthread_t me = pthread_self(), none = (pthread_t)0, owner;
    owner = atomic_load(&g_draw_thread);
    if (!owner) {
        if (atomic_compare_exchange_strong(&g_draw_thread, &none, me)) return;
        owner = atomic_load(&g_draw_thread);
    }
    if (pthread_equal(owner, me)) return;
    if (atomic_fetch_add(&g_draw_other_thread, 1) == 0)
        fprintf(stderr, "[METAL] THE SINGLE-THREAD ASSUMPTION IS FALSE:"
                " nv2a_metal_draw reached from thread %p, having been claimed"
                " by %p. ring_offset, the texture cache, last_command and the"
                " surface state are all plain statics under that assumption;"
                " overlapping ring reservations draw one batch's vertices at"
                " another's coordinates.\n", (void *)me, (void *)owner);
}
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
static uint64_t ring_reserves,ring_bytes,ring_wraps,ring_waits,ring_fallbacks,ring_slabs_live,ring_pins_skipped;
static uint64_t sync_calls,sync_clean,sync_color,sync_depth,surface_uploads;
/* Nanoseconds inside nv2a_metal_sync, split: waiting for the GPU to drain
 * versus reading the surface back and converting it. See the comment at the
 * wait. */
static uint64_t g_sync_drain_ns, g_sync_read_ns;
/* THE READ-BACK AUDIT. See the block in nv2a_metal_sync.
 *
 * races  = syncs whose two reads disagreed at all
 * diff   = total pixels that changed between the two reads
 * audits = syncs the audit actually ran on, which is the positive control:
 *          diff=0 with audits=0 measures nothing. */
static uint64_t g_readback_races, g_readback_diff, g_readback_audits;
static uint64_t g_queue_drains;
/* Clears the GPU performed itself. Zero with the hardware path on means every
 * clear refused and took the read-back route, which is a measurement, not a
 * silence: read it before believing a frame-time number. */
static uint64_t g_resident_color_clears, g_resident_depth_clears;
static uint64_t g_resident_unbound_clears, g_slot_writebacks, g_slot_writeback_skipped;
static uint64_t g_resident_depth_sibling_clears;   /* see nv2a_metal_clear_depth_stencil */
static uint64_t g_scissored_draws;   /* draws whose window clip was smaller than the surface */
/* WHO ASKS FOR THE STALL.
 *
 * A 150 s gameplay run reported 24,293 sync calls costing 52.7 s of draining
 * and 12.5 s of readback -- 8.06 ms per frame against a 16.05 ms frame, i.e.
 * HALF THE FRAME. The instrument printed one total for every caller, so the
 * attribution had to be done by arithmetic across two other report lines
 * (16,181 surface swaps + 8,098 flips = 24,279, against 24,293 syncs). That
 * worked, but it is not a measurement anyone else will repeat. These are. */
static uint64_t g_sync_by_swap, g_sync_by_invalidate, g_sync_by_frame_end;
/* AND WHAT EACH CALLER COSTS, WHICH THE COUNTS ABOVE DO NOT SAY.
 *
 * Measured 19 Sep 2026, new_game.pad, 4,913 flips: 9,810 surface swaps, 4,928
 * external (the flip), 4,924 "already clean", 24,831 ms of draining. Those
 * four numbers admit two readings that differ by the whole bill -- the swaps
 * drain and the flip is clean, or one swap a frame is clean and the flip
 * drains -- and nothing printed could tell them apart. An afternoon of
 * arithmetic over three lines is what this replaces.
 *
 * `who` is set by the caller immediately before the call and consumed on
 * entry, so anything that does not set it is attributed to `external`, which
 * is what the flip and the diagnostic dumps are. Two adds against ~15,000
 * syncs a run: unmeasurable beside a 2.5 ms drain. */
typedef enum { SYNC_WHO_EXTERNAL, SYNC_WHO_SWAP, SYNC_WHO_INVALIDATE,
               SYNC_WHO_FRAME_END, SYNC_WHO_N } SyncWho;
static SyncWho g_sync_who = SYNC_WHO_EXTERNAL;
static uint64_t g_sync_who_calls[SYNC_WHO_N], g_sync_who_drained[SYNC_WHO_N];
static uint64_t g_sync_who_drain_ns[SYNC_WHO_N], g_sync_who_read_ns[SYNC_WHO_N];
static const char *const g_sync_who_name[SYNC_WHO_N] = {
    "external", "swap", "invalidate", "frame-end" };
/* Syncs that found dirty flags nobody was going to pay, and so skipped the
 * wait. See the block at the top of nv2a_metal_sync. */
static uint64_t g_sync_drainless;
/* Deferred slot debts paid because something was about to READ those guest
 * bytes as a texture, or rebuild the surface from them. Zero is the expected
 * reading and it is also the positive control for the walk: see
 * surface_pay_debt_for_range. */
static uint64_t g_debt_paid_on_read, g_debt_paid_on_rebuild, g_debt_paid_on_evict;
/* WHY A RESIDENT CLEAR REFUSED, by reason, because the counts alone said the
 * colour half was refusing 8 times for every one it took and nothing said
 * which test threw it out. A refusal is not free: clear_surface then calls
 * nv2a_gpu_invalidate_range, which drops every surface cache slot naming that
 * pointer, so a refusing colour clear costs the whole retained-surface
 * machinery for that frame as well as the read-back it was meant to avoid. */
static uint64_t g_clear_refuse_mask, g_clear_refuse_softtail, g_clear_refuse_invalid,
                g_clear_refuse_target, g_clear_refuse_geom, g_clear_color_calls,
                g_clear_depth_calls, g_clear_refuse_rect, g_clear_refuse_components;
static int queue_drain_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_DRAIN"); return on; }
static int readback_audit_on(void)
{ static int on=-1; if(on<0) on=recomp_switch_on("RECOMP_METAL_READBACK_AUDIT");
  return on; }

static int ring_audit_on(void)
{static int on=-1;if(on<0)on=getenv("RECOMP_METAL_RING_AUDIT")?1:0;return on;}

/* Reserve `bytes` of slab storage. Returns the slab to bind, the byte offset
 * to bind it at, a CPU pointer to fill, and which slab was used so the caller
 * can pin it to the command buffer. Returns nil if the ring cannot serve the
 * request at all, and the caller must then allocate privately. */
static id<MTLBuffer> ring_reserve(size_t bytes,size_t*offset_out,void**cpu_out,unsigned*slab_out)
{
    ring_configure();
    if(!bytes||bytes>ring_usable){++ring_fallbacks;return nil;}
    size_t need=RING_ALIGN_UP(bytes);
    if(ring_offset+need>ring_usable){
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
    /* The corruption arm. Gated on the self-test flag as well, because that
     * test drives ring_pin directly and its own pin_mode 0 is already the
     * control it needs -- an environment variable must not be able to turn
     * its two protected cases into a third copy of that control. */
    if(!ring_selftest_active&&ring_nopin_on()){++ring_pins_skipped;return;}
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
    if(pass_fence_on()&&g_pass_fence){
        [batch_encoder updateFence:g_pass_fence afterStages:MTLRenderStageFragment];
        ++g_fence_waits;}
    [batch_encoder endEncoding];
    for(unsigned i=0;i<RING_SLABS;i++)if(batch_pins&(1u<<i))ring_pin(batch_command,i);
    mtl_cb_gpu_watch(batch_command);
    [batch_command commit];
    last_command=batch_command;
    if(batch_draws>batch_longest)batch_longest=batch_draws;
    batch_draws_total+=batch_draws;++batch_flushes;
    batch_command=nil;batch_encoder=nil;batch_pins=0;batch_draws=0;
}

/* WRITE BACK THE SURFACE THE CALLER ASKED FOR, NOT THE ONE THAT HAPPENS TO BE
 * BOUND.
 *
 * nv2a_pb_exec.c defines, on this host:
 *
 *     #define nv2a_gpu_sync_range(target, bytes) nv2a_metal_sync()
 *
 * -- it takes the range and throws it away. snapshot_surface() passes the real
 * flipped range and gets back whichever surface was bound, and the title
 * rotates surfaces constantly (402,784 rebinds in one session at a 100% cache
 * hit rate). The D3D11 backend has had the honest version since it was
 * written: nv2a_d3d11.c:1224 sync_range_inner walks its cache and syncs every
 * surface the range touches.
 *
 * THIS CHANGES NOTHING TODAY, AND THAT IS DELIBERATE. Every draw sets
 * surface_dirty, and every surface swap syncs before it rebinds, so guest RAM
 * is already current for every surface and no slot ever owes it anything for
 * rendered content. A range walk therefore finds nothing to pay and this
 * behaves exactly as nv2a_metal_sync() does -- which is the point: it lands
 * with zero behavioural risk, and is the piece that makes deferring the swap's
 * writeback SAFE rather than a way to present stale pixels.
 *
 * Without it, deferring would mean the flip asking for address X and receiving
 * whatever was bound. With it, the flip names the range it is about to read
 * and anything owing that range pays first. */
static size_t surface_slot_bytes(unsigned i)
{
    return (size_t)surf_slot[i].pitch * surf_slot[i].h;
}

/* Subtraction after ordering, so an end pointer cannot overflow. */
static int guest_ranges_overlap(const uint8_t *a, size_t a_size,
                                const uint8_t *b, size_t b_size)
{
    uintptr_t av, bv;
    if (!a || !b || !a_size || !b_size) return 0;
    av = (uintptr_t)a; bv = (uintptr_t)b;
    return av <= bv ? bv - av < a_size : av - bv < b_size;
}

/* Calls, and slots that actually owed the range something. paid=0 across a
 * whole run is the expected reading until the swap starts deferring; it is
 * also the positive control that says this walked and found nothing, rather
 * than that it never ran. */
static uint64_t g_sync_range_calls, g_sync_range_paid;

int nv2a_metal_sync_range(uint8_t *target, size_t bytes)
{
    ++g_sync_range_calls;
    /* No range named means "all of it", which is what a full invalidate and
     * every internal caller wants. */
    if (!target || !bytes) return nv2a_metal_sync();

    /* The bound surface first. It is the only one whose DEPTH can be dirty,
     * and nv2a_metal_sync is the only path that writes depth back. */
    if (!nv2a_metal_sync()) return 0;

    for (unsigned i = 0; i < surface_slots_used(); ++i) {
        if (!surf_slot[i].valid || !surf_slot[i].owes_guest_ram) continue;
        if (!guest_ranges_overlap(target, bytes,
                                  surf_slot[i].target, surface_slot_bytes(i)))
            continue;
        /* Pays the debt and clears it. A slot that is also the bound surface
         * has already been written by the sync above, and its debt was
         * transferred to surface_dirty when it was rebound, so it cannot
         * double-pay. */
        surface_slot_writeback(i);
        ++g_sync_range_paid;
    }
    return 1;
}

/* THE SAME PAYMENT, FOR EVERY OTHER READER OF THOSE GUEST BYTES.
 *
 * nv2a_metal_sync_range closes the hole for the ONE reader that names its
 * range: the flip. Deferring a swap's write-back creates a slot whose pixels
 * exist only on the GPU, and defer_swap's own header admits the gap it leaves
 * -- "a guest CPU read of the surface range that is not the flip is not
 * intercepted". Inside this backend there are exactly two such readers and
 * both are reachable on an ordinary frame:
 *
 *   - a TEXTURE whose guest bytes are a surface this title has rendered into
 *     and not yet handed back. texture_buffer uploads straight from guest RAM
 *     and would upload the pre-render contents, which is render-to-texture
 *     silently sampling last frame.
 *   - a surface REBUILD. The slot lookup is exact on seven fields, so a
 *     surface whose depth pointer moved misses the cache and is re-uploaded
 *     from guest RAM while a slot still owes those very bytes.
 *
 * Four slots and a pointer-range compare, so the walk costs nothing when
 * nothing is owed -- which, with the deferral off, is always. g_debt_paid_*
 * are the positive controls: they say this ran and found nothing, rather than
 * that it never ran. */
static void surface_pay_debt_for_range(const uint8_t *p, size_t bytes,
                                       uint64_t *counter)
{
    if (!p || !bytes) return;
    for (unsigned i = 0; i < surface_slots_used(); ++i) {
        if (!surf_slot[i].valid || !surf_slot[i].owes_guest_ram) continue;
        if (!guest_ranges_overlap(p, bytes, surf_slot[i].target,
                                  surface_slot_bytes(i)))
            continue;
        surface_slot_writeback(i);
        ++*counter;
    }
}

/* THE ONE NUMBER THE CUMULATIVE TOTAL CANNOT GIVE.
 *
 * g_sync_drain_ns + g_sync_read_ns already says sync costs 7.9 ms per frame
 * averaged over a run, which is how G3 got its "48% of the frame". An average
 * cannot say whether that cost sits on the MEDIAN frame or only on the tail,
 * and p50 against the 16.68 ms budget is the number that has to move --
 * removing a stall from frames that were already fast buys nothing. So hand
 * the running total to the only code that knows where a frame ends, and let
 * it difference. Read-only, and it allocates nothing: a flip can afford it. */
unsigned long long nv2a_metal_sync_ns(void)
{
    return (unsigned long long)(g_sync_drain_ns + g_sync_read_ns);
}

void nv2a_metal_report(void)
{
    fprintf(stderr,"[METAL] sync %llu calls (%llu already clean): %.1f ms draining"
            " the GPU, %.1f ms reading back and converting."
            "  A resident clear could remove the second only.\n",
            (unsigned long long)sync_calls,(unsigned long long)sync_clean,
            g_sync_drain_ns/1e6,g_sync_read_ns/1e6);
    /* The RANGE walk, whose two counters were written as a positive control
     * -- their own comment says "paid=0 across a whole run is the expected
     * reading ... it is also the positive control that says this walked and
     * found nothing, rather than that it never ran" -- and were then printed
     * nowhere, which is the one thing that makes that control useless.
     *
     * This is also the line G3 wants. The goals note says to size direct
     * Metal presentation from the copy, the conversion and the re-upload and
     * never from [STAGE] sync; paid is how many of those writebacks actually
     * happened. */
    fprintf(stderr,"[METAL] sync_range %llu calls, %llu slot writeback(s)"
            " paid%s\n",
            (unsigned long long)g_sync_range_calls,
            (unsigned long long)g_sync_range_paid,
            g_sync_range_calls == 0
                ? "   <- never called: nothing asked for a ranged sync"
                : (g_sync_range_paid == 0
                    ? " (walked and found nothing owing -- expected until the"
                      " swap defers)" : ""));
    /* A full clear discards the surface instead of syncing it, so each of
     * these is one drain and one 4.9 MB readback that did not happen. Printed
     * with the state of the switch so an A/B can see the arms differ. */
    /* Draws that actually took the hardware path, against the states it had to
     * refuse. "The switch is on" and "the draws used it" are different facts,
     * and a mixed frame -- some draws writing depth to the attachment, the
     * rest to the colour alpha -- reads as a depth bug rather than as a
     * fallback, which is exactly how this was first misread. */
    /* Named unconditionally, in both states, so ab_score.py can verify an A/B's
     * arms actually differed. It could not for this switch, which is the one
     * that turned out to decide whether the frame is correct. */
    fprintf(stderr,"[METAL] one encoder per batch: %s (metal_batch %s)\n",
            batch_on()?"yes":"no (a render pass per draw)",
            batch_on()?"on":"OFF");
    {   /* The MODE, not a boolean. =0 off, =1 dithered blended draws only,
         * =2 every blended draw, =3 every hardware draw. A report that
         * collapses four states into "on" cannot verify which arm ran. */
        int mode = hw_shader_blend_mode_env();
        fprintf(stderr,"[METAL] shader blend mode %d (%s)%s\n",
                mode,
                mode==0?"off -- NO draw reads the destination":
                mode==1?"dithered blended draws only":
                mode==2?"every blended draw":"every hardware draw",
                mode>=3?" (default)":" -- A CONTROL ARM, renders incorrectly");
    }
    fprintf(stderr,"[METAL] MIXED draws (software tail while hw on)=%llu, "
            "depth uploads failed=%llu\n",
            (unsigned long long)g_hw_mixed,(unsigned long long)g_hw_upload_fail);
    fprintf(stderr,"[METAL] hw draws=%llu pipelines=%llu refusals=%llu (metal_hw %s)\n",
            (unsigned long long)g_hw_draws,
            (unsigned long long)g_hw_pipeline_misses,
            (unsigned long long)g_hw_state_refusals,
            hw_state_on()?"on":"OFF");
    spec_report();
    /* THE BISECT ARMS NAME THEMSELVES, in both states and unconditionally.
     *
     * Each of these renders incorrectly by construction, so a run carrying one
     * is a diagnostic and never a result about the renderer. The states= count
     * is the positive control: the switch being "on" is what the environment
     * says, and states> 0 is the backend saying it reached the descriptor. A
     * zero there with the switch on means no hardware draw ever built a state
     * -- read that as "this arm did nothing", not as "the arm changed
     * nothing". */
    fprintf(stderr,"[METAL] bisect: depth forced ALWAYS %s (states=%llu), "
            "stencil ignored %s (states=%llu)\n",
            hw_depth_always_on()?"on":"OFF",
            (unsigned long long)g_hw_depth_always_states,
            hw_no_stencil_on()?"on":"OFF",
            (unsigned long long)g_hw_no_stencil_states);
    /* The colour attachment's format, named so an A/B can verify its arms
     * differ rather than assume the environment took. ab_score.py harvests
     * this; it could not for RECOMP_METAL_565 and said so. */
    if (readback_audit_on())
        fprintf(stderr,"[METAL] read-back audit: %llu syncs checked, %llu raced,"
                " %llu pixels changed after a full queue drain\n",
                (unsigned long long)g_readback_audits,
                (unsigned long long)g_readback_races,
                (unsigned long long)g_readback_diff);
    /* WHAT THE TEXTURE SAYS IT IS, not what the descriptor asked for.
     *
     * Metal synchronises two passes over the same texture automatically only
     * for TRACKED resources; a resource sub-allocated from an MTLHeap defaults
     * to UNTRACKED and gets no dependency at all. Everything here is created
     * straight from the device, so this should read tracked -- but "should"
     * is what every retracted claim in this repo was built on, and the object
     * can be asked. */
    if (surface)
        fprintf(stderr,"[METAL] surface hazard tracking: %s (queue drains %llu,"
                " metal_drain %s)\n",
                surface.hazardTrackingMode==MTLHazardTrackingModeTracked
                    ? "tracked" : "UNTRACKED -- Metal orders nothing",
                (unsigned long long)g_queue_drains,
                queue_drain_on()?"on":"OFF");
    if(pass_fence_on())
        fprintf(stderr,"[METAL] pass fence: %s, %llu updates (metal_fence on)\n",
                g_pass_fence?"created":"NOT CREATED -- this arm did nothing",
                (unsigned long long)g_fence_waits);
    fprintf(stderr,"[METAL] colour attachment: %s\n",
            hw_565_on()?"B5G6R5Unorm (metal_565 on)"
                       :"RGBA32Float (metal_565 OFF)");
    /* COLOUR CLEARS ARE SERVED BY TWO PATHS AND THIS LINE USED TO NAME ONLY
     * ONE. g_resident_color_clears counts the bound-surface path;
     * g_resident_unbound_clears counts the slot cache, which picks up exactly
     * the clears clear_resident_ok refused for `target` because the guest was
     * clearing a surface other than the bound one. Printing the first against
     * the total, with "each one a drain, a 4.9 MB read-back and a 6.4 MB
     * re-upload that did not happen" attached, reads as though the remainder
     * pays all of that. It does not: measured 2,115 + 12,293 = 14,408 of
     * 14,420, so 99.9% of colour clears are already on the GPU and the
     * `target=12,323` refusals are served a few lines later rather than lost.
     * An afternoon went into chasing that gap before the two counters were
     * added up. Print the sum, and the split behind it. */
    fprintf(stderr,"[METAL] resident clears: %llu of %llu colour (%llu bound +"
            " %llu via the slot cache), %llu of %llu depth/stencil -- each one"
            " a drain, a 4.9 MB read-back and a 6.4 MB re-upload that did not"
            " happen\n",
            (unsigned long long)(g_resident_color_clears
                                 + g_resident_unbound_clears),
            (unsigned long long)g_clear_color_calls,
            (unsigned long long)g_resident_color_clears,
            (unsigned long long)g_resident_unbound_clears,
            (unsigned long long)g_resident_depth_clears,
            (unsigned long long)g_clear_depth_calls);
    fprintf(stderr,"[METAL] scissored draws (window clip smaller than the surface): %llu\n",
            (unsigned long long)g_scissored_draws);
    fprintf(stderr,"[METAL] depth clears applied to a sibling slot sharing the same"
            " guest depth buffer: %llu (the render target's copy, which the"
            " bound-surface clear used to miss)\n",
            (unsigned long long)g_resident_depth_sibling_clears);
    /* THE ARM NAMES ITSELF, in both states, because ab_score.py can only check
     * a switch that does. gpu draws moving while vertices stays at zero would
     * mean the counter is on the wrong side of the branch, which is why both
     * are printed. */
    /* BOTH ARMS NAME THEMSELVES. With the switch off every draw is counted
     * late, which is the control that says this counter is on the draw path
     * at all; with it on, the split is how much of the scene can take the
     * variant, and that bound is a property of the title rather than of the
     * change. */
    if(g_latin_tex_n){
     fprintf(stderr,"[METAL] Latin font pages bound for text: %u distinct"
             " texture(s)%s\n", g_latin_tex_n,
             g_latin_tex_n<2?"  <<< JSRF HAS TWO LATIN PAGES. Only one was"
                             " ever bound, so every glyph on the other page"
                             " (v w x y z) sampled the wrong sheet.":"");
     for(unsigned i=0;i<g_latin_tex_n;i++)
      fprintf(stderr,"[METAL]   page texture %08X: %llu character quads\n",
              g_latin_tex[i],(unsigned long long)g_latin_tex_quads[i]);
    }
    fprintf(stderr,"[METAL] depth test before the shader: %llu draws early"
            " (%llu of them through fs_hw_early_nw, discard kept), %llu late (early_z %s)\n",
            (unsigned long long)g_early_z_draws,(unsigned long long)g_early_nw_draws,
            (unsigned long long)g_late_z_draws,
            early_z_on()?"on":"OFF");
    if (early_z_on() && early_z_ref0_on())
        fprintf(stderr,"[METAL]   early_z_ref0 ON -- alpha_ref==0 draws are"
                " treated as non-discarding. CEILING MEASUREMENT, not exact:"
                " a fully transparent fragment can write depth\n");
    if (early_z_on())
        fprintf(stderr,"[METAL]   early-Z predicate: %llu eligible, refused"
                " %llu for alpha_test (of which %llu have alpha_ref==0,"
                " i.e. they reject only a fully transparent fragment),"
                " %llu for z_cull -- a draw that can discard cannot have its"
                " depth test moved in front\n",
                (unsigned long long)g_ez_eligible,
                (unsigned long long)g_ez_no_alpha,
                (unsigned long long)g_ez_no_alpha_ref0,
                (unsigned long long)g_ez_no_zcull);
    if (early_z_on())
        fprintf(stderr,"[METAL]   early-Z G27b: %llu eligible because they write neither depth nor"
                " stencil (fs_hw_early_nw, exact with discard); alpha-tested draws that DO write:"
                " %llu without combiners, %llu with combiners\n",
                (unsigned long long)g_ez_nowrite,(unsigned long long)g_ez_alpha_writes_cc0,
                (unsigned long long)g_ez_alpha_writes_cc);
    if (early_z_on() && early_z_exact_on())
        fprintf(stderr,"[METAL]   early-Z G38c (RECOMP_METAL_EARLY_Z_EXACT=on): %llu predicate calls proven"
                " alpha >= 1/255 (fs_hw_early), %llu alpha-tested calls not proven\n",
                (unsigned long long)g_ez_exact,(unsigned long long)g_ez_exact_refused);
    if (early_z_exact_mode() >= 2)
        fprintf(stderr,"[METAL]   early-Z G38c AUDIT%s: %u fragments of proven draws were DISCARDED"
                " (must be 0; any nonzero value means the proof is wrong)\n",
                early_z_exact_mode() == 3 ? " POSITIVE CONTROL (every alpha-tested draw marked; must be NONZERO)" : "",
                g_ez_violation_buf ? *(const uint32_t *)g_ez_violation_buf.contents : 0u);
    alpha_census_report();
    /* Defined beside nv2a_metal_draw, which is where it measures. */
    { extern void t0_census_report(void); t0_census_report(); }
    fprintf(stderr,"[METAL] vsh: %s (metal_vsh %s)\n",
            vsh_gpu_on()?"guest programs on the GPU":"CPU interpreter",
            vsh_gpu_on()?"on":"OFF");
    fprintf(stderr,"[METAL] vsh draws: %llu GPU, %llu CPU; %llu vertices on the"
            " GPU\n",
            (unsigned long long)g_vsh_gpu_draws,
            (unsigned long long)g_vsh_cpu_draws,
            (unsigned long long)g_vsh_gpu_vertices);
    fprintf(stderr,"[METAL] vsh programs: %llu compiled, %llu cache hits,"
            " refused: %llu emitter, %llu compiler, %llu cache full,"
            " %llu pipeline cache full\n",
            (unsigned long long)g_vsh_compiles, (unsigned long long)g_vsh_hits,
            (unsigned long long)g_vsh_refused_emit,
            (unsigned long long)g_vsh_refused_compile,
            (unsigned long long)g_vsh_cache_full,
            (unsigned long long)g_vsh_pso_full);
    /* The attribution, beside the cost, so nobody has to cross-reference two
     * lines to learn where half the frame went. "external" is the executor's
     * own calls -- the flip's snapshot and the diagnostic surface dump -- which
     * reach the backend through a macro rather than from inside this file. */
    fprintf(stderr,"[METAL] sync callers: %llu surface swap, %llu invalidate,"
            " %llu frame end, %llu external (of %llu calls, %llu already"
            " clean)\n",
            (unsigned long long)g_sync_by_swap,
            (unsigned long long)g_sync_by_invalidate,
            (unsigned long long)g_sync_by_frame_end,
            (unsigned long long)(sync_calls - g_sync_by_swap
                                 - g_sync_by_invalidate - g_sync_by_frame_end),
            (unsigned long long)sync_calls, (unsigned long long)sync_clean);
    /* AND WHAT EACH OF THEM PAID. The line above says who called; without
     * this one the 24.8 s bill has to be attributed by arithmetic across
     * three counters, and the two readings that arithmetic admits differ by
     * all of it. `drained` is the calls that actually took
     * waitUntilCompleted -- the rest either found nothing outstanding or
     * found nothing anybody was going to collect. */
    for (unsigned w = 0; w < SYNC_WHO_N; ++w) {
        if (!g_sync_who_calls[w]) continue;
        fprintf(stderr,"[METAL] sync cost by caller: %-10s %llu calls,"
                " %llu drained, %.1f ms waiting, %.1f ms reading back\n",
                g_sync_who_name[w],
                (unsigned long long)g_sync_who_calls[w],
                (unsigned long long)g_sync_who_drained[w],
                g_sync_who_drain_ns[w]/1e6, g_sync_who_read_ns[w]/1e6);
    }
    fprintf(stderr,"[METAL] syncs that skipped the wait because nothing was"
            " going to be written back: %llu\n",
            (unsigned long long)g_sync_drainless);
    fprintf(stderr,"[METAL] deferred debts paid on demand: %llu texture read,"
            " %llu surface rebuild, %llu eviction\n",
            (unsigned long long)g_debt_paid_on_read,
            (unsigned long long)g_debt_paid_on_rebuild,
            (unsigned long long)g_debt_paid_on_evict);
    /* "the depth refusal is why defer_swap is inert" stood here and was
     * wrong, or at least a generation out of date: the refusal had already
     * been narrowed to "depth is dirty AND somebody is going to write it",
     * and the deferral still bought nothing because clearing surface_dirty
     * alone never reached nv2a_metal_sync's early return -- depth_dirty was
     * still set, so the wait was taken anyway. The wait is now decided by
     * what will actually be written; see the block at the top of that
     * function. */
    fprintf(stderr,"[METAL] depth write-backs: %llu taken, %llu skipped"
            " (no_depth_sync %s). Each one is a drain as well as a copy.\n",
            (unsigned long long)g_depth_syncs_taken,
            (unsigned long long)g_depth_syncs_skipped,
            no_depth_sync_on()?"on":"OFF");
    /* THE SAME SHAPE, AND THE SAME REASON FOR THE SHAPE. Printed in BOTH
     * states: with no_colour_sync OFF the `taken` count is precisely what the
     * other arm would skip, so one control run sizes the A/B before it is
     * run. The token is unconditional so ab_score.py's generic METAL_SWITCH_RE
     * can harvest it and the identical-arms VOID check can actually run --
     * defer_swap's A/B was scored with that check silently skipped because its
     * token was invisible to the regex of the day. */
    fprintf(stderr,"[METAL] colour write-backs: %llu taken, %llu skipped"
            " (of which %llu also elided the 4.9 MB read-back, the rest still"
            " owing it to the depth path's alpha) (no_colour_sync %s)."
            "  With it OFF, taken IS what the other arm would skip.\n",
            (unsigned long long)g_color_syncs_taken,
            (unsigned long long)g_color_syncs_skipped,
            (unsigned long long)g_color_readbacks_elided,
            no_colour_sync_on()?"on":"OFF");
    fprintf(stderr,"[METAL] swap writeback deferred: %llu (of which %llu only"
            " because depth is not written back at all) (refused: %llu depth"
            " dirty, %llu no slot) (defer_swap %s)\n",
            (unsigned long long)g_swap_deferred,
            (unsigned long long)g_swap_deferred_no_depth,
            (unsigned long long)g_swap_defer_depth,
            (unsigned long long)g_swap_defer_noslot,
            defer_swap_on()?"on":"OFF");
    fprintf(stderr,"[METAL] unbound-surface clears: %llu served from the cache,"
            " %llu slot write-backs (%llu skipped)\n",
            (unsigned long long)g_resident_unbound_clears,
            (unsigned long long)g_slot_writebacks,
            (unsigned long long)g_slot_writeback_skipped);
    fprintf(stderr,"[METAL] clear refused: mask=%llu components=%llu rect=%llu"
            " soft-tail=%llu invalidated=%llu target=%llu geometry=%llu\n",
            (unsigned long long)g_clear_refuse_mask,
            (unsigned long long)g_clear_refuse_components,
            (unsigned long long)g_clear_refuse_rect,
            (unsigned long long)g_clear_refuse_softtail,
            (unsigned long long)g_clear_refuse_invalid,
            (unsigned long long)g_clear_refuse_target,
            (unsigned long long)g_clear_refuse_geom);
    fprintf(stderr,"[METAL] clear discards=%llu (clear_discard %s)\n",
            (unsigned long long)g_mtl_discards,
            g_mtl_discards?"used":"unused");
    {   unsigned long other = atomic_load(&g_draw_other_thread);
        fprintf(stderr,"[METAL] draw thread: %s (%lu draws from another"
                " thread)\n", other ? "MORE THAN ONE -- see the warning above"
                : "one, as the ring assumes", other); }
    fprintf(stderr,"[METAL] texture buffers: %llu requests, %llu cache hits, %llu uploads; vertices: %llu inline, %llu allocated\n",
        (unsigned long long)texture_requests,(unsigned long long)texture_hits,
        (unsigned long long)texture_uploads,(unsigned long long)inline_vertex_batches,
        (unsigned long long)allocated_vertex_batches);
    fprintf(stderr,"[METAL] texture cache: %llu full compares, %llu partial compares, %llu changed;"
        " partial-caught=%llu (a mid-frame rewrite the once-per-frame rule alone would have missed)\n",
        (unsigned long long)texture_full_compares,(unsigned long long)texture_partial_compares,
        (unsigned long long)texture_changed,(unsigned long long)texture_partial_caught);
    fprintf(stderr,"[METAL] hw textures (RECOMP_METAL_HW_TEX=%s): %llu built, %llu reused, %llu units sampled in hardware,"
        " %llu decode failures (fell back to software), %.1f MiB decoded\n",
        hw_tex_on()?"on":"off",(unsigned long long)hw_tex_builds,(unsigned long long)hw_tex_reuses,
        (unsigned long long)hw_tex_units,(unsigned long long)hw_tex_failed,hw_tex_bytes/1048576.0);
    if(ring_audit_on())
        fprintf(stderr,"[METAL] ring audit: %llu reservations, %llu MiB staged, "
            "%llu slabs live, %llu wraps of which %llu had to wait, %llu fallbacks "
            "to a private allocation; %u of %d slabs in use, %zu KiB usable, %llu pins skipped%s | syncs: %llu calls, %llu already clean, "
            "%llu colour read-backs, %llu depth read-backs; %llu surface re-uploads\n",
            (unsigned long long)ring_reserves,(unsigned long long)(ring_bytes>>20),
            (unsigned long long)ring_slabs_live,(unsigned long long)ring_wraps,
            (unsigned long long)ring_waits,(unsigned long long)ring_fallbacks,
            ring_limit,RING_SLABS,ring_usable>>10,
            (unsigned long long)ring_pins_skipped,
            ring_nopin_on()?", PINNING REMOVED -- this frame is wrong by construction":"",
            (unsigned long long)sync_calls,(unsigned long long)sync_clean,
            (unsigned long long)sync_color,(unsigned long long)sync_depth,
            (unsigned long long)surface_uploads);
    /* surface_uploads counts every SWAP -- it is incremented before the cache
     * lookup, not after -- so printing it as "rebuilds" overstates them by the
     * number of hits and quietly hides the cache's hit rate. A reader who took
     * 21,614 as rebuilds against 5,765 rebinds would conclude the cache barely
     * helps; the real split is 15,849 rebuilds and a 27% hit rate, which is a
     * different problem with a different fix. Print the swap total, the split,
     * and the rate, so none of the three has to be inferred. */
    fprintf(stderr,"[METAL] feedback reads: %llu draws sampled the surface they"
            " render into, %llu synced it first (feedback_sync %s)\n",
            (unsigned long long)g_feedback_draws,
            (unsigned long long)g_feedback_syncs,
            feedback_sync_on()?"on":"OFF");
    fprintf(stderr,"[METAL] surface cache: %llu swaps = %llu rebinds + %llu "
            "rebuilds (%.0f%% hit), %llu evictions (surface_cache %s)\n",
            (unsigned long long)surface_uploads,
            (unsigned long long)surface_hits,
            (unsigned long long)(surface_uploads - surface_hits),
            surface_uploads ? 100.0 * (double)surface_hits / (double)surface_uploads : 0.0,
            (unsigned long long)surface_evictions,
            surface_cache_on()?"on":"OFF");
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
    fprintf(stderr,"[METAL] clip audit: float area collapsed to zero on %llu"
            " triangles that have a non-zero area in double (area_double %s)"
            " -- these are the thin far-scenery triangles the old arithmetic"
            " dropped\n",(unsigned long long)g_area_rescued,
            area_double_on()?"on":"OFF");
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
    uint32_t color_icw[8],alpha_icw[8],color_ocw[8],alpha_ocw[8];
    uint32_t const0[8],const1[8];
    uint32_t frag_force;
    uint32_t hw[4];   /* G27: unit u samples texture(u) with sampler(u), not the buffer */
    uint32_t ez_proven;  /* G38c audit: the alpha test was proven unable to fire */
}Params;

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
    unsigned saved_limit, corrupt = 0, i;
    size_t saved_usable;
    id<MTLBuffer> dst;
    int failed = 0;

    if (!initialize()) return 0;
    /* Settle the environment's own ring configuration first, so saved_limit
     * below restores what a real run would have had rather than the compiled
     * default. */
    ring_configure();
    saved_limit = ring_limit;
    saved_usable = ring_usable;
    if (slabs < 2 || slabs > RING_SLABS || !iters || !per_batch) return 0;
    dst = [device newBufferWithLength:span * iters options:MTLResourceStorageModeShared];
    if (!dst) return 0;
    memset(dst.contents, 0, span * iters);

    /* Lock the corruption arm out: this test brings its own control, and an
     * environment variable must not be able to turn its two protected cases
     * into a third copy of it. */
    ring_selftest_active = 1;
    ring_limit = slabs;
    ring_usable = RING_SLAB_BYTES;   /* the test sizes its own reservations */
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
    ring_usable = saved_usable;
    ring_selftest_active = 0;
    ring_current = 0; ring_offset = 0;
    if (corrupt_out) *corrupt_out = failed ? 0xFFFFFFFFu : corrupt;
    return 1;
}

int nv2a_metal_sync(void)
{
    unsigned long long _t_sync = mtl_now_ns(), _t_drained = 0;
    SyncWho who = g_sync_who;
    g_sync_who = SYNC_WHO_EXTERNAL;
    ++texture_frame;   /* every sync is at least a frame boundary for the texture cache */
    @autoreleasepool{
        ++sync_calls; ++g_sync_who_calls[who];
        /* BEFORE the dirty test and before the wait. An open batch is work the
         * GPU has not been told about, so last_command would be the previous
         * committed buffer and waiting on it would read a surface that is
         * missing every draw in the batch. */
        batch_flush();
        if(!surface_dirty&&!depth_dirty){++sync_clean;
            g_sync_drain_ns += mtl_now_ns()-_t_sync;
            g_sync_who_drain_ns[who] += mtl_now_ns()-_t_sync; return 1;}
        /* THE WAIT EXISTS ONLY TO MAKE A READ-BACK VALID, SO A SYNC THAT IS
         * GOING TO READ NOTHING BACK MUST NOT TAKE IT.
         *
         * The dirty test above asks "is anything outstanding"; it does not ask
         * "is anybody going to collect it". Those came apart the moment the
         * two write-backs became skippable. With RECOMP_METAL_NO_DEPTH_SYNC on
         * -- which is the default and is what the player runs -- a batch that
         * wrote depth or stencil leaves depth_dirty set, the three branches
         * below then SKIP the write-back and clear the flag, and the only
         * thing the flag bought was `[last_command waitUntilCompleted]`. That
         * is the drain, and the drain is 89% of this function's cost: measured
         * 19 Sep 2026 over 4,913 flips, 24,831 ms draining against 3,204 ms
         * reading back and converting.
         *
         * It is also exactly why RECOMP_METAL_DEFER_SWAP was inert. Its own
         * header says it clears surface_dirty "and nv2a_metal_sync then takes
         * its already-clean early return". It did not: depth_dirty was still
         * set, so the early return was never reached and every deferred swap
         * still paid the full stall for a read-back it had just cancelled. The
         * deferral saved the 614 KB copy and none of the 2.5 ms wait.
         *
         * So resolve the skip predicates HERE, before the wait, in the same
         * order and from the same state the branches below use -- the one
         * thing that must not happen is a sync that skips the wait and then
         * finds a branch that wanted the pixels. depth_dirty is left alone
         * when there is no depth_target, because that is the one case the
         * branches below also leave alone, and a flag this function does not
         * own is not a flag it should clear. Nothing is lost by that: without
         * a target no branch can write depth either, so no drain is taken for
         * it on this call or on any later one. */
        {
            int will_colour = surface_dirty && !no_colour_sync_on();
            int will_depth  = depth_dirty && depth_target && !no_depth_sync_on();
            if (!will_colour && !will_depth) {
                if (surface_dirty) {
                    ++g_color_syncs_skipped; ++g_color_readbacks_elided;
                    surface_dirty = 0;
                }
                if (depth_dirty && depth_target) {
                    ++g_depth_syncs_skipped; depth_dirty = 0;
                }
                ++g_sync_drainless;
                g_sync_drain_ns += mtl_now_ns()-_t_sync;
                g_sync_who_drain_ns[who] += mtl_now_ns()-_t_sync;
                return 1;
            }
        }
        ++g_sync_who_drained[who];
        [last_command waitUntilCompleted];
        /* RECOMP_METAL_DRAIN=1 -- wait for the QUEUE, not for one buffer.
         *
         * The line above is this backend's oldest load-bearing assumption:
         * that waiting on the most recently committed command buffer means
         * every earlier one has finished. Apple does not document that.
         * MTLCommandBuffer.commit() promises only that "the GPU STARTS the
         * command buffer after it starts any command buffers that are ahead of
         * it in the same command queue" -- start order, not completion order --
         * and the resource-synchronization guide says plainly that "by design,
         * GPUs can run multiple commands in parallel". getBytes() and
         * replaceRegion() both carry the same instruction in Apple's own
         * words: "ensure ALL operations that write or render to the texture
         * complete" first. Waiting on one buffer is not that.
         *
         * An empty command buffer committed now cannot start before everything
         * already on the queue has started, and waiting on it therefore costs
         * one round trip to establish what the code above assumes for free.
         *
         * Opt-in, because it is a candidate FIX and a candidate fix has to be
         * A/B-able against the picture it claims to repair. */
        if (queue_drain_on()) {
            id<MTLCommandBuffer> drain = [queue commandBuffer];
            [drain commit];
            [drain waitUntilCompleted];
            ++g_queue_drains;
        }
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
        g_sync_who_drain_ns[who] += _t_drained - _t_sync;
        /* RECOMP_METAL_READBACK_AUDIT=1 -- see the header comment on
         * g_readback_diff. Drains the queue a second time, properly, and
         * compares. */
        if (readback_audit_on() && surface_dirty) {
            size_t px = (size_t)surface_width * surface_height;
            size_t stride = hw_565_on() ? 2 : 16;
            uint8_t *a = malloc(px * stride), *b = malloc(px * stride);
            if (a && b) {
                MTLRegion r = MTLRegionMake2D(0,0,surface_width,surface_height);
                [surface getBytes:a bytesPerRow:surface_width*stride
                       fromRegion:r mipmapLevel:0];
                {   /* An empty command buffer committed now cannot be
                     * scheduled before everything already on the queue, so
                     * waiting on it drains the queue -- which waiting on
                     * last_command alone only does if the queue is in order.
                     * That is the assumption under test. */
                    id<MTLCommandBuffer> drain = [queue commandBuffer];
                    [drain commit];
                    [drain waitUntilCompleted];
                }
                [surface getBytes:b bytesPerRow:surface_width*stride
                       fromRegion:r mipmapLevel:0];
                ++g_readback_audits;
                if (memcmp(a, b, px * stride)) {
                    size_t i, differ = 0;
                    for (i = 0; i < px; ++i)
                        if (memcmp(a + i*stride, b + i*stride, stride)) ++differ;
                    g_readback_diff += differ;
                    ++g_readback_races;
                }
            }
            free(a); free(b);
        }
        if(last_command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"[METAL] command failed: %s\n",last_command.error.description.UTF8String);return 0;}
        size_t pixels=(size_t)surface_width*surface_height;
        int fmt565=hw_565_on();
        /* RECOMP_METAL_NO_COLOUR_SYNC -- see the header comment on
         * no_colour_sync_on. Resolved ONCE, here, rather than at each of the
         * three places below that need it: the branches must agree about
         * whether this sync is writing colour back, and re-asking a predicate
         * per branch is how a skip that clears the dirty flag ends up paired
         * with a read-back that still ran.
         *
         * WHAT STILL NEEDS THE PIXELS WHEN COLOUR DOES NOT. On the software
         * tail depth lives in the colour attachment's ALPHA, so the depth
         * write-back below reads rgba[at+3] out of this same buffer. Eliding
         * the getBytes while that branch is live would hand it uninitialised
         * heap and write it into the guest's D24S8 surface -- a corruption
         * with no symptom here at all, since this function would still return
         * 1. So the elision asks for the state of the depth path rather than
         * assuming it: hardware depth reads its own texture and wants nothing
         * from here, and a skipped depth write-back reads nothing at all. */
        int skip_color=surface_dirty&&no_colour_sync_on();
        int alpha_depth=depth_dirty&&depth_target&&!no_depth_sync_on()
                        &&!(hw_state_on()&&hw_depth_tex);
        int want_pixels=(surface_dirty&&!skip_color)||alpha_depth;
        /* SIZED FOR THE FORMAT. This allocated pixels*16 unconditionally, so a
         * 565 run asked for 4.9 MB per sync and used 614 KB of it -- a malloc,
         * a page-fault storm and a free, once a frame, for nothing. */
        float *rgba=NULL;
        if(want_pixels) {
            rgba=malloc(pixels*(fmt565?2:16));
            if(!rgba)return 0;
            [surface getBytes:rgba bytesPerRow:surface_width*(fmt565?2:16) fromRegion:MTLRegionMake2D(0,0,surface_width,surface_height) mipmapLevel:0];
        } else if(skip_color) ++g_color_readbacks_elided;
        if(skip_color) {
            /* Counted, and the flag is cleared -- the same pair no_depth_sync
             * needs. Leaving surface_dirty set would make the next sync try
             * again, so the switch would measure nothing and the frame would
             * still pay for the write-back at the next opportunity. */
            ++g_color_syncs_skipped;
            surface_dirty=0;
        } else if(surface_dirty&&fmt565) {
            /* Straight back out, no conversion and no rounding -- which is the
             * whole point: a wider attachment has to round twice, here and
             * again at the 565 grid. */
            ++sync_color; ++g_color_syncs_taken;
            const uint16_t*w16=(const uint16_t*)rgba;
            for(unsigned y=0;y<surface_height;++y)
                memcpy(surface_target+(size_t)y*surface_pitch,w16+(size_t)y*surface_width,(size_t)surface_width*2);
            surface_dirty=0;
        } else if(surface_dirty) {
            ++sync_color; ++g_color_syncs_taken;
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
        if(depth_dirty&&depth_target&&no_depth_sync_on()) {
            /* Counted, and the flag is cleared: leaving it set would make the
             * next sync try again and the switch would measure nothing. */
            ++g_depth_syncs_skipped;
            depth_dirty=0;
        } else if(depth_dirty&&depth_target&&hw_state_on()&&hw_depth_tex) {
            ++sync_depth; ++g_depth_syncs_taken;
            hw_depth_readback(depth_target,surface_width,surface_height,depth_pitch);
            depth_dirty=0;
        } else if(depth_dirty&&depth_target&&rgba) {
            /* `&&rgba` states the invariant `alpha_depth` was computed from --
             * this is the one branch that reads depth out of the colour
             * buffer's alpha, so it is the one branch that keeps the getBytes
             * alive. The two must agree, and if they ever stop agreeing the
             * failure is a retry on the next sync (depth_dirty stays set)
             * rather than a D24S8 surface written from uninitialised heap. */
            ++sync_depth; ++g_depth_syncs_taken;
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
        g_sync_who_read_ns[who] += mtl_now_ns()-_t_drained;
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

/* WHAT IS THE BACKEND HOLDING RIGHT NOW? Read-only, for RECOMP_SURFACE_AUDIT.
 *
 * Every question about state surviving a frame boundary reduces to this one:
 * the backend retains ONE surface, the guest binds and clears whichever it
 * likes, and the two only meet on a draw. Nothing could compare them from
 * outside, so every answer so far has come from reading this file rather than
 * from a run. `owed` is -1 when nothing is retained, 0 when the retained
 * surface matches guest RAM, and 1 when it holds rendering guest RAM has not
 * seen yet -- which is the only state in which losing it costs pixels. */
/* G51.1: READ-ONLY -- the depth the hardware path holds for a depth surface.
 *
 * Under RECOMP_METAL_NO_DEPTH_SYNC (default on) depth never returns to guest
 * RAM, so the only copy of the depth a draw is tested against is the
 * Depth32Float attachment here. The host's 2D shadow needs exactly that copy.
 * This finds the attachment belonging to `depth` (the bound one, or a cached
 * slot's), completes queued work, and copies the top-left w x h values out.
 * It writes nothing the executor reads: no dirty flag, no debt, no guest RAM.
 * The one side effect is committing an open batch early, which changes how
 * draws are grouped into command buffers and not what they draw. Only the
 * shadow calls it; nothing on the default path does. */
int nv2a_metal_depth_peek(const uint8_t *depth, unsigned w, unsigned h, float *out)
{
    id<MTLTexture> tex = nil;
    if (!depth || !out || !w || !h) return 0;
    if (depth_target == depth && hw_depth_tex) tex = hw_depth_tex;
    for (unsigned i = 0; !tex && i < surface_slots_used(); i++)
        if (surf_slot[i].valid && surf_slot[i].depth == depth && surf_slot[i].hw_depth)
            tex = surf_slot[i].hw_depth;
    if (!tex || tex.width < w || tex.height < h) return 0;
    batch_flush();
    if (last_command) [last_command waitUntilCompleted];
    [tex getBytes:out bytesPerRow:(NSUInteger)w * sizeof *out
        fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    return 1;
}

/* G51.1 draw mode. See nv2a_metal.h. Nothing on the executor's own path
 * calls it. It uses the executor's queue, so its pass is ordered after every
 * draw the executor has committed and before every one it commits next, and
 * Metal's hazard tracking orders the attachment accesses. The fence pair
 * mirrors a per-draw command buffer's, so RECOMP_METAL_PASS_FENCE sees it as
 * one more draw. */
int nv2a_metal_external_draw(const uint8_t *target, const uint8_t *depth, int writes_depth,
                             int (*encode)(void *encoder, unsigned w, unsigned h, void *ctx), void *ctx)
{
    draw_thread_check();
    if (!encode || !target || !hw_state_on() || !hw_565_on()) return -1;
    if (!surface || !surface_valid || !hw_depth_tex || !hw_stencil_tex) return -2;
    if (surface_target != target) return -3;
    if (depth && depth_target != depth) return -4;
    batch_flush();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = surface;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = hw_depth_tex;
    pass.depthAttachment.loadAction = MTLLoadActionLoad; pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = hw_stencil_tex;
    pass.stencilAttachment.loadAction = MTLLoadActionLoad; pass.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = cb ? [cb renderCommandEncoderWithDescriptor:pass] : nil;
    if (!cb || !enc) return 0;
    if (pass_fence_on() && g_pass_fence) [enc waitForFence:g_pass_fence beforeStages:MTLRenderStageVertex];
    int ok = encode((__bridge void *)enc, surface_width, surface_height, ctx);
    if (pass_fence_on() && g_pass_fence) { [enc updateFence:g_pass_fence afterStages:MTLRenderStageFragment]; ++g_fence_waits; }
    [enc endEncoding];
    mtl_cb_gpu_watch(cb);
    [cb commit];
    last_command = cb;
    if (ok) { surface_dirty = 1; if (writes_depth) depth_dirty = 1; }
    return ok != 0;
}

void nv2a_metal_retained(const uint8_t **color, const uint8_t **depth, int *owed)
{
    if (color) *color = surface_target;
    if (depth) *depth = depth_target;
    if (owed)  *owed  = (!surface_valid && !depth_valid) ? -1
                      : ((surface_dirty || depth_dirty) ? 1 : 0);
}

/* THE RESIDENT CLEAR: a full-surface clear that never leaves the GPU.
 *
 * WHAT IT REPLACES, and the cost is measured rather than assumed. A clear
 * currently drains the GPU, reads the whole 4.9 MB colour surface back,
 * converts it per pixel into guest RAM, lets the CPU memset 1.8 MB of guest
 * RAM, and then makes the next draw re-upload 6.4 MB with a second per-pixel
 * conversion. Twice a frame. Measured 16 Sep 2026 at gameplay, that whole
 * stage is 9.57 ms of a 31.75 ms frame -- 30% of the budget, against a 16.67
 * ms target -- and the run total is 28,651 ms of read-back and conversion in
 * 280 s. None of it is necessary when the CPU is about to write one constant
 * over every byte: the GPU can write the constant itself.
 *
 * WHY BOTH HALVES OR NEITHER. clear_surface calls the depth half first and the
 * colour half second, and each falls back to nv2a_gpu_invalidate_range when
 * its resident clear refuses. Those two calls are the ONLY invalidate sites in
 * the executor, so if both halves go resident nothing invalidates and the
 * surface stays valid across frames -- which is what makes this compound
 * instead of applying once. But if the depth half refuses and invalidates, a
 * resident colour clear would be re-uploaded away from guest RAM that never
 * received the constant, and the clear would be LOST. So each half refuses
 * unless the surface is still valid, which makes the pair self-limiting: the
 * first clear after any invalidate takes the slow, correct path.
 *
 * WHY HARDWARE-STATE ONLY. On the software tail the colour attachment's ALPHA
 * IS THE DEPTH BUFFER, so clearing the colour attachment would wipe depth. The
 * hardware path keeps depth and stencil in their own attachments, which is the
 * whole reason it exists, and is the path this is for.
 *
 * GUEST RAM IS LEFT STALE ON PURPOSE, and marked dirty. The next
 * nv2a_metal_sync writes it back, and that sync already happens once a frame
 * at the guest's own FLIP_STALL because the presenter reads guest RAM. So two
 * read-back-and-re-upload cycles a frame become one read-back.
 *
 * REFUSING IS FREE AND COUNTED. Returning 0 tells clear_surface its fast path
 * did not apply and it performs the byte-exact CPU clear it always did, so
 * every refusal is a correct frame rather than a wrong one. */
/* Pay a slot's debt to guest RAM: read its colour texture back, in the guest's
 * own RGB565 layout, exactly as nv2a_metal_sync does for the bound surface.
 *
 * Called only from surface_cache_drop, so it happens when something is about to
 * write that guest memory from the CPU and the GPU's copy is about to stop
 * being reachable. It costs a drain and a read-back -- the very things the
 * resident clear exists to avoid -- which is the point: the debt is paid once,
 * at the moment it has to be, instead of twice a frame whether or not anyone
 * ever looks. */
static void surface_slot_writeback(unsigned i)
{
    uint8_t *dst = surf_slot[i].target;
    id<MTLTexture> tex = surf_slot[i].colour;
    uint32_t w = surf_slot[i].w, h = surf_slot[i].h, pitch = surf_slot[i].pitch;
    if (!dst || !tex || !w || !h) { ++g_slot_writeback_skipped; return; }
    @autoreleasepool {
        size_t px = (size_t)w * h;
        int fmt565 = hw_565_on();
        uint8_t *buf = malloc(px * (fmt565 ? 2 : 16));
        if (!buf) { ++g_slot_writeback_skipped; return; }
        batch_flush();
        [last_command waitUntilCompleted];
        [tex getBytes:buf bytesPerRow:w * (fmt565 ? 2 : 16)
           fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
        if (fmt565) {
            const uint16_t *src = (const uint16_t *)buf;
            for (uint32_t y = 0; y < h; ++y)
                memcpy(dst + (size_t)y * pitch, src + (size_t)y * w, (size_t)w * 2);
        } else {
            const float *src = (const float *)buf;
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
                size_t at = ((size_t)y * w + x) * 4;
                unsigned c = (unsigned)(fminf(1,fmaxf(0,src[at]))*31+.5f)<<11
                           | (unsigned)(fminf(1,fmaxf(0,src[at+1]))*63+.5f)<<5
                           | (unsigned)(fminf(1,fmaxf(0,src[at+2]))*31+.5f);
                uint8_t *q = dst + (size_t)y * pitch + x * 2;
                q[0] = (uint8_t)c; q[1] = (uint8_t)(c >> 8);
            }
        }
        free(buf);
        ++g_slot_writebacks;
    }
    surf_slot[i].owes_guest_ram = 0;
}

/* THE CENSUS: what every retained colour surface holds, on the GPU and in
 * guest RAM, side by side. RECOMP_SURFACE_CENSUS. See NV2ASurfaceCensus in
 * nv2a_metal_state.h for why guest RAM alone cannot answer the question and
 * why the D3D11 branch's nv2a_gpu_surface_report() does not exist here.
 *
 * READ-ONLY, and it must stay that way. It drains and getBytes: exactly what
 * surface_slot_writeback does, minus the store into guest RAM and minus
 * clearing owes_guest_ram. Paying the debt here would destroy the very
 * disagreement the census is for -- the run would report a healthy frame
 * BECAUSE the instrument had repaired it -- so the pixels are counted and
 * thrown away.
 *
 * IT IS NOT FREE. Each report drains the GPU and reads back every held
 * surface. At 240 fps that is unaffordable every frame, which is why the
 * caller strides it; the cost is the reason the switch takes a stride and an
 * after-time rather than being a plain on/off trace. */
void nv2a_metal_surface_census_report(const uint8_t *guest_base,
                                      uint32_t presented_offset)
{
    NV2ASurfaceCensus census[SURFACE_SLOTS];
    unsigned n = 0, i;

    if (!hw_state_on() || !initialize()) {
        fprintf(stderr, "  [SURFACE-CENSUS] the hardware path is not up; "
                        "nothing to census\n");
        return;
    }
    @autoreleasepool {
        int fmt565 = hw_565_on();
        batch_flush();
        [last_command waitUntilCompleted];
        for (i = 0; i < surface_slots_used() && n < SURFACE_SLOTS; ++i) {
            NV2ASurfaceCensus *e;
            uint32_t w = surf_slot[i].w, h = surf_slot[i].h;
            uint32_t pitch = surf_slot[i].pitch;
            const uint8_t *target = surf_slot[i].target;
            uint8_t *buf;
            uint32_t x, y;

            if (!surf_slot[i].valid || !surf_slot[i].colour || !w || !h)
                continue;
            e = &census[n++];
            memset(e, 0, sizeof *e);
            e->target = (guest_base && target >= guest_base)
                      ? (uint32_t)(size_t)(target - guest_base) : 0;
            e->w = w; e->h = h;
            e->bound = (target == surface_target);
            e->presented = (e->target && e->target == presented_offset);
            e->owes_guest_ram = surf_slot[i].owes_guest_ram;
            e->gpu_nonzero = -1;
            e->guest_nonzero = -1;

            /* Guest RAM, in the guest's own RGB565 layout -- the same bytes
             * [FB] and the presenter read, so a disagreement with the texture
             * below is a disagreement with what the player sees. */
            if (target && pitch) {
                long nz = 0;
                for (y = 0; y < h; ++y) {
                    const uint8_t *row = target + (size_t)y * pitch;
                    for (x = 0; x < w; ++x)
                        if (row[x * 2] || row[x * 2 + 1]) ++nz;
                }
                e->guest_nonzero = nz;
            }

            /* The backend's own texture. The float arm converts to RGB565
             * before counting so both numbers mean the same thing: "pixels the
             * guest would see as nonzero". Counting raw float bits instead
             * would call a pixel that rounds to black nonzero, and the whole
             * point of the line is that the two counts are comparable. */
            buf = malloc((size_t)w * h * (fmt565 ? 2 : 16));
            if (!buf) continue;
            [surf_slot[i].colour getBytes:buf bytesPerRow:w * (fmt565 ? 2 : 16)
                              fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
            {
                long nz = 0;
                if (fmt565) {
                    const uint16_t *src = (const uint16_t *)buf;
                    for (y = 0; y < (size_t)w * h; ++y) if (src[y]) ++nz;
                } else {
                    const float *src = (const float *)buf;
                    for (y = 0; y < (size_t)w * h; ++y) {
                        size_t at = (size_t)y * 4;
                        unsigned c = (unsigned)(fminf(1,fmaxf(0,src[at]))*31+.5f)<<11
                                   | (unsigned)(fminf(1,fmaxf(0,src[at+1]))*63+.5f)<<5
                                   | (unsigned)(fminf(1,fmaxf(0,src[at+2]))*31+.5f);
                        if (c) ++nz;
                    }
                }
                e->gpu_nonzero = nz;
            }
            free(buf);
        }
    }
    for (i = 0; i < n; ++i)
        fprintf(stderr, "  [SURFACE-CENSUS]   surface 0x%08X %ux%u%s%s%s"
                " gpu_nonzero=%ld guest_nonzero=%ld of %u\n",
                census[i].target, census[i].w, census[i].h,
                census[i].bound ? " BOUND" : "",
                census[i].presented ? " PRESENTED" : "",
                census[i].owes_guest_ram ? " owes-guest-ram" : "",
                census[i].gpu_nonzero, census[i].guest_nonzero,
                census[i].w * census[i].h);
    fprintf(stderr, "  [SURFACE-CENSUS]   %u surface(s) held; verdict: %s\n",
            n, nv2a_surface_census_verdict_text(
                    nv2a_surface_census_verdict(census, n)));
    fflush(stderr);
}

/* CLEAR A SURFACE THE GUEST NAMES BUT THE BACKEND IS NOT CURRENTLY BOUND TO.
 *
 * Measured at gameplay, 16 Sep 2026: of 14,603 colour clears, 12,507 -- 86% --
 * named a colour surface other than the bound one, and every one of them fell
 * back to the CPU. That is not an accident of this title, it is how the guest
 * works: the colour offset register moves when the guest writes it, while
 * surface_target only moves on the next DRAW, so a clear issued in that window
 * names one surface while the backend holds another. nv2a_metal_discard's
 * comment describes the same window, and RECOMP_SURFACE_AUDIT measured 8,843
 * of 17,646 clears in it.
 *
 * JSRF rotates three colour surfaces and the cache holds four, so the surface
 * being cleared is usually one we already have a texture for -- just not the
 * bound one. Clearing that texture is the same load action; the only thing
 * that changes is which slot it lands in and who then owes guest RAM.
 *
 * WHY THIS IS THE EXPENSIVE HALF. Refusing did not merely cost the read-back
 * saving. clear_surface falls back to nv2a_gpu_invalidate_range, which drops
 * every cache slot naming that pointer -- so 86% of clears were emptying the
 * surface cache, which is why its hit rate sat at exactly 50% with 10,352
 * rebuilds in a run. Each rebuild reallocates four textures and re-uploads
 * 6.4 MB. The refusal was costing more than the clear.
 *
 * Returns 0 for anything it cannot serve, and the byte-exact CPU clear then
 * runs as it always did. */
static int clear_unbound_slot(uint8_t *target, uint32_t pitch,
                              uint32_t width, uint32_t height, uint16_t v)
{
    unsigned i;
    if (!surface_cache_on() || !hw_state_on() || !initialize()) return 0;
    for (i = 0; i < surface_slots_used(); ++i) {
        if (!surf_slot[i].valid || surf_slot[i].target != target) continue;
        if (surf_slot[i].w != width || surf_slot[i].h != height) return 0;
        if (surf_slot[i].pitch != pitch) return 0;
        if (!surf_slot[i].colour) return 0;
        break;
    }
    if (i == surface_slots_used()) return 0;     /* not a surface we hold */
    @autoreleasepool {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        batch_flush();
        pass.colorAttachments[0].texture = surf_slot[i].colour;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake((double)(v >> 11) / 31.0,
                              (double)((v >> 5) & 63) / 63.0,
                              (double)(v & 31) / 31.0, 1.0);
        /* No depth or stencil attachment: this slot's depth textures belong to
         * whatever geometry last used it and the colour clear must not disturb
         * them. A pass with a colour attachment alone is legal and clears
         * exactly what it names. */
        if (!clear_encode(pass)) return 0;
        /* The CPU clear is now skipped, so guest RAM for this surface is behind
         * the texture until the slot is rebound or dropped. */
        surf_slot[i].owes_guest_ram = 1;
        ++g_resident_unbound_clears;
    }
    return 1;
}

static int clear_resident_ok(const uint8_t *target, uint32_t pitch,
                             uint32_t width, uint32_t height)
{
    if (!hw_state_on()) { ++g_clear_refuse_softtail; return 0; }
    if (!initialize()) { ++g_clear_refuse_softtail; return 0; }
    if (!surface_valid || !depth_valid) { ++g_clear_refuse_invalid; return 0; }
    if (!surface || !hw_depth_tex || !hw_stencil_tex) { ++g_clear_refuse_invalid; return 0; }
    if (surface_target != target) { ++g_clear_refuse_target; return 0; }
    if (surface_width != width || surface_height != height) { ++g_clear_refuse_geom; return 0; }
    if (surface_pitch != pitch) { ++g_clear_refuse_geom; return 0; }
    return 1;
}

/* One empty render pass whose load action is the clear. No draws: the tile is
 * initialised to the constant and stored, which is the cheapest way a GPU can
 * write a constant over an attachment. */
static int clear_encode(MTLRenderPassDescriptor *pass)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc =
        cb ? [cb renderCommandEncoderWithDescriptor:pass] : nil;
    if (!cb || !enc) return 0;
    [enc endEncoding];
    mtl_cb_gpu_watch(cb);
    [cb commit];
    last_command = cb;
    return 1;
}

int nv2a_metal_clear_color(uint8_t *target, size_t target_size, uint32_t pitch,
                           uint32_t width, uint32_t height,
                           uint32_t param, uint32_t value)
{
    (void)target_size;
    /* Only a clear that writes all three colour channels over the WHOLE
     * surface can become a load action; anything partial refuses and the CPU
     * loop runs. NV097_CLEAR_SURFACE's R/G/B bits are 0x10/0x20/0x40. */
    ++g_clear_color_calls;
    if ((param & 0x70u) != 0x70u) { ++g_clear_refuse_mask; return 0; }
    if (!clear_resident_ok(target, pitch, width, height)) {
        /* Not the bound surface -- but very probably one we hold. See
         * clear_unbound_slot: 86% of this title's colour clears land here. */
        return clear_unbound_slot(target, pitch, width, height,
                                  (uint16_t)value);
    }
    @autoreleasepool {
        /* SET_COLOR_CLEAR_VALUE arrives already in the surface's own format,
         * so a 16-bit surface takes the low half verbatim as R5G6B5 -- the
         * same reading clear_surface's CPU loop uses, and for the same reason:
         * D3D reduced the D3DCOLOR before it reached the pushbuffer, so
         * reducing again drops the red field. */
        uint16_t v = (uint16_t)value;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        batch_flush();
        pass.colorAttachments[0].texture = surface;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake((double)(v >> 11) / 31.0,
                              (double)((v >> 5) & 63) / 63.0,
                              (double)(v & 31) / 31.0, 1.0);
        /* The depth and stencil attachments are LOADED, not cleared: this call
         * is the colour half and must not touch depth. A pass that names them
         * with MTLLoadActionLoad/StoreActionStore leaves them exactly as they
         * were. */
        pass.depthAttachment.texture = hw_depth_tex;
        pass.depthAttachment.loadAction = MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.texture = hw_stencil_tex;
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        if (!clear_encode(pass)) return 0;
        ++g_resident_color_clears;
        /* Guest RAM no longer matches the attachment. The flip's sync carries
         * it across; nothing else reads it in between. */
        surface_dirty = 1;
    }
    return 1;
}

int nv2a_metal_clear_depth_stencil(uint8_t *target, size_t target_size,
        uint32_t pitch, uint32_t width, uint32_t height,
        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
        uint32_t components, uint32_t value)
{
    (void)target_size;
    /* Both halves of the D24S8 word, over the whole surface, or refuse.
     * components is NV097_CLEAR_SURFACE's low two bits: 1 depth, 2 stencil. */
    ++g_clear_depth_calls;
    if ((components & 3u) != 3u) { ++g_clear_refuse_components; return 0; }
    if (x0 != 0 || y0 != 0 || x1 != width || y1 != height) { ++g_clear_refuse_rect; return 0; }
    /* EVERY SLOT THAT SHARES THIS DEPTH BUFFER, NOT ONLY THE BOUND ONE.
     *
     * The slot key is exact on seven fields including the depth pointer, and
     * a draw that neither depth-tests nor stencil-tests keeps the previous
     * depth binding (see "A DRAW WITH NO DEPTH DOES NOT NEED THE RETAINED
     * DEPTH THROWN AWAY" in nv2a_metal_draw). So this title's per-frame
     * composite -- render target sampled into a back buffer, depth off --
     * builds the back buffer's slot with the RENDER TARGET's depth pointer,
     * and from then on two slots hold two separate depth textures for one
     * guest depth buffer. The frame's depth clear arrives while the back
     * buffer is bound, passes the bound-surface test below, and clears the
     * back buffer's copy. The render target's copy is never cleared, never
     * re-uploaded on a rebind, and never written back, so it keeps last
     * frame's depth for good.
     *
     * Measured 21 Sep 2026 on the Load screen: 24 consecutive draws, every
     * triangle accepted, every one with the depth test on, and the render
     * target stays black; RECOMP_METAL_HW_DEPTH_ALWAYS=1 brings the whole
     * screen back. A static screen is the worst case -- identical depth every
     * frame fails LESS everywhere -- but every scene pays some of it.
     *
     * So the clear is applied to every valid slot whose depth pointer is the
     * one being cleared, at the same geometry. The bound slot is handled by
     * the path below as before; the others get a depth/stencil-only pass,
     * which Metal permits. Counted separately so the report can say how many
     * copies existed. */
    for (unsigned i = 0; i < surface_slots_used(); ++i) {
        if (!surf_slot[i].valid || surf_slot[i].depth != target) continue;
        if (surf_slot[i].depth_pitch != pitch || surf_slot[i].w != width || surf_slot[i].h != height) continue;
        if (!surf_slot[i].hw_depth || !surf_slot[i].hw_stencil) continue;
        if (surf_slot[i].hw_depth == hw_depth_tex) continue;   /* the bound copy, below */
        @autoreleasepool {
            uint32_t q = (value >> 8) & 0xFFFFFFu;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            batch_flush();
            pass.depthAttachment.texture = surf_slot[i].hw_depth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.depthAttachment.clearDepth = (double)q / 16777215.0;
            pass.stencilAttachment.texture = surf_slot[i].hw_stencil;
            pass.stencilAttachment.loadAction = MTLLoadActionClear;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = value & 0xFFu;
            if (clear_encode(pass)) ++g_resident_depth_sibling_clears;
        }
    }
    if (!clear_resident_ok(depth_target == target ? surface_target : NULL,
                           surface_pitch, width, height)) return 0;
    if (depth_target != target || depth_pitch != pitch) return 0;
    @autoreleasepool {
        /* The guest's dword is stencil in byte 0 and 24-bit depth above it,
         * which is how hw_depth_upload reads it and how hw_depth_readback
         * writes it back. Normalising by 16777215 here keeps the three in
         * agreement; a mismatch would show as a depth test that passes on one
         * path and fails on the other. */
        uint32_t q = (value >> 8) & 0xFFFFFFu;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        batch_flush();
        pass.colorAttachments[0].texture = surface;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = hw_depth_tex;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = (double)q / 16777215.0;
        pass.stencilAttachment.texture = hw_stencil_tex;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = value & 0xFFu;
        if (!clear_encode(pass)) return 0;
        ++g_resident_depth_clears;
        depth_dirty = 1;
    }
    return 1;
}

int nv2a_metal_discard(const uint8_t *color, const uint8_t *depth)
{
    /* ONLY IF THE SURFACE BEING CLEARED IS THE ONE BEING HELD.
     *
     * Dropping without a readback is sound because the CPU is about to write a
     * constant over every byte -- but only over the CLEARED surface's bytes.
     * This used to take no arguments and drop whatever was retained, and the
     * two are not the same surface as often as it looks: the bound colour
     * offset moves when the guest writes the register, while surface_target
     * only moves on the next DRAW, so a clear issued in that window names one
     * surface and threw away another. metal_batch_test phase G measures it at
     * 24848 of 65536 pixels of finished rendering lost.
     *
     * nv2a_metal_invalidate takes a target for exactly this reason -- "it used
     * to be one nv2a_gpu_invalidate(NULL) here ... that blast radius meant no
     * surface ever survived a frame" -- and this path, added later, did not.
     *
     * Returning 0 leaves the retained surface alone AND tells the caller its
     * fast path did not apply, so it invalidates normally. Pointer equality is
     * the same test invalidate uses; a surface reached through a non-zero DMA
     * base simply fails it and takes the slow, correct route. */
    if (surface_target != color || depth_target != depth)
        return 0;
    @autoreleasepool{
        batch_flush();
        ++g_mtl_discards;
        /* AND DROP THE CACHED SLOTS, for the reason the cache's own header
         * gives: a slot is valid only while guest RAM cannot have changed
         * underneath it. This is the one clear that does not go through
         * nv2a_metal_invalidate -- that is the whole point of it -- so it has
         * to do invalidate's cache work itself. Without this the CPU writes
         * the clear into guest RAM, the next draw's slot lookup matches, the
         * PRE-CLEAR textures are rebound, and surface_dirty is 0 so nothing
         * re-uploads the cleared bytes: the clear is lost entirely. */
        surface_cache_drop(color);
        surface_cache_drop(depth);
        surface_valid=depth_valid=0;
        surface_dirty=depth_dirty=0;
    }
    return 1;
}

void nv2a_metal_invalidate(uint8_t *target)
{/* The cached slots describe guest memory, so whatever this invalidates in the
  * live binding it must also invalidate in the cache -- otherwise a later swap
  * back would rebind a texture for memory the CPU has since overwritten. */
 surface_cache_drop(target);
 if(!target||target==surface_target||target==depth_target){++g_sync_by_invalidate;g_sync_who=SYNC_WHO_INVALIDATE;nv2a_metal_sync();surface_valid=depth_valid=0;}}
const char *nv2a_metal_last_reject(void){return reject_reason?reject_reason:"none";}
/* A REJECTED DRAW HANDS THE BATCH TO THE CPU RASTERISER, which renders it into
 * guest RAM -- so the retained textures are stale from that moment, and the
 * cache has to be told. It was not: invalidate drops slots, this did not, and
 * a later swap back would rebind a texture for memory the CPU has since
 * written, silently discarding everything the fallback drew.
 * reject("hw-state-untranslatable") is a hardware-path-only route into it. */
static int reject(const char *reason){reject_reason=reason;nv2a_metal_sync();
    surface_cache_drop(surface_target);surface_cache_drop(depth_target);
    surface_valid=depth_valid=0;return-1;}
/* THE SIGNED AREA, AND WHY IT IS COMPUTED IN DOUBLE.
 *
 * This is a 2D cross product of differences, which is the textbook shape for
 * catastrophic cancellation: for a THIN triangle the two products are nearly
 * equal and their float32 difference collapses to exactly 0. The caller then
 * treats ar==0 as degenerate and drops the triangle.
 *
 * Thin is exactly what DISTANT geometry is. Near surfaces are made of fat
 * triangles and survive; a fence or a bridge across the level is a long thin
 * structure whose triangles are slivers by the time they reach the screen, so
 * it loses most of them and renders as a skeletal outline that flickers as the
 * camera moves. That is the player-visible symptom, and it is why it is always
 * the far scenery rather than the thing in front of you.
 *
 * MEASURED, and the instrument that says so was already here: the degenerate
 * split distinguishes a duplicate vertex (a legitimate triangle-strip stitch)
 * from three DISTINCT positions that still produce zero area. One 180 s
 * gameplay run:
 *
 *     strip-stitch(duplicate vertex)=30337822
 *     collinear-distinct=265785, of which 260714 were on screen with
 *                               every w>0
 *
 * 260,714 triangles a run, on screen, in front of the camera, three distinct
 * corners, dropped for having no area. Real meshes do not contain a quarter of
 * a million exactly-collinear triangles; float32 does.
 *
 * The inputs stay float -- they are the guest's own transformed positions and
 * promoting them changes nothing -- but the arithmetic is done in double, which
 * has enough headroom that the cancellation cannot reach zero for any triangle
 * whose corners genuinely differ. The result narrows back to float for the
 * facing test, which only reads its sign.
 *
 * THE HYPOTHESIS ABOVE IS WRONG, AND THE COUNTER BELOW IS HOW WE KNOW.
 * Measured over a full gameplay run: area_rescued = 0. NOT ONE triangle has a
 * zero area in float and a non-zero area in double, so the cancellation never
 * manufactures a false degenerate and this change rescues nothing. The 161,586
 * on-screen collinear-distinct triangles are EXACTLY zero in double too --
 * they are genuinely collinear in screen space, which means something upstream
 * is flattening those vertices onto a line, and that is a transform question
 * rather than a precision one. Default OFF because it demonstrably does
 * nothing; kept, with its counter, so the next person does not spend the same
 * afternoon on the same idea.
 *
 * RECOMP_METAL_AREA_DOUBLE=1 enables the double arithmetic for an A/B. The
 * DETECTION is unconditional: area_rescued counts triangles that are zero in
 * float and non-zero in double, so a run with the repair OFF still reports
 * exactly how much geometry the old arithmetic was throwing away. */
static int area_double_on(void)
{ static int on=-1;
  if(on<0){const char*e=getenv("RECOMP_METAL_AREA_DOUBLE");
           on = e ? (atoi(e)!=0) : 0;}   /* DEFAULT OFF -- MEASURED A NO-OP */
  return on; }
static float area(const float a[4],const float b[4],const float c[4])
{float f=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
 double d=((double)b[0]-a[0])*((double)c[1]-a[1])
         -((double)b[1]-a[1])*((double)c[0]-a[0]);
 if(f==0.0f&&d!=0.0)++g_area_rescued;
 return area_double_on()?(float)d:f;}
static int vertex_valid(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned i)
{for(unsigned k=0;k<4;k++)if(!isfinite(v[i][0][k])||!isfinite(v[i][3][k])||!isfinite(v[i][4][k]))return 0;
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){for(unsigned k=0;k<4;k++)if(!isfinite(v[i][9+u][k]))return 0;if(v[i][9+u][3]<=0)return 0;}return 1;}
/* Same predicate, split so a rejection can name itself: 1 non-finite
 * position/colour, 2 texture coordinate q<=0 or non-finite, 0 valid. */
static int vertex_why(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned i)
{for(unsigned k=0;k<4;k++)if(!isfinite(v[i][0][k])||!isfinite(v[i][3][k])||!isfinite(v[i][4][k]))return 1;
 for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){for(unsigned k=0;k<4;k++)if(!isfinite(v[i][9+u][k]))return 2;if(v[i][9+u][3]<=0)return 2;}return 0;}
/* CULLING NEEDS THE TRANSFORMED POSITION, WHICH IS THE THING THAT MOVED.
 *
 * Everything below reads v[i][0] as a SCREEN-SPACE position: area() takes its
 * winding, front_facing() takes the w components, vertex_valid() tests the
 * transformed texture coordinates. With the guest's program running on the GPU
 * that slot holds attribute 0 -- an object-space position -- and every one of
 * those tests is then computing on the wrong numbers. It does not fail loudly;
 * it culls the wrong faces, which is a character rendered as a silhouette.
 *
 * So on that path the CPU emits the indices and the GPU does the culling,
 * through setCullMode and setFrontFacingWinding at the encoder. That is where
 * it belongs anyway: Metal decides facing after the perspective divide, where
 * the NV2A's own rule needs winding_flipped() to patch up triangles straddling
 * the camera plane -- a correction this tree's own header calls an open
 * question and whose obvious sign fix is recorded as WRONG. */
static int vsh_gpu_culling;
static void triangle(const NV2ATextureCopy*s,const float(*v)[16][4],unsigned*out,unsigned*n,unsigned a,unsigned b,unsigned c)
{if(vsh_gpu_culling){out[(*n)++]=a;out[(*n)++]=b;out[(*n)++]=c;return;}
 int audit=clip_audit_on();if(audit){++audit_asm_total;
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
    ++g_sync_by_frame_end;
    g_sync_who = SYNC_WHO_FRAME_END;
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

/* ── RECOMP_T0_CENSUS: WHICH DRAWS CARRY ONE TEXTURE COORDINATE ─────────
 *
 * WHAT IT IS FOR. On 21 Sep 2026 RECOMP_FRAG_FORCE=4 painted the attract
 * scene with its own TEXCOORD0 and the answer was visible in one frame: the
 * plaza floor is a wrapping gradient, and the Rokkaku-dai disc and the window
 * mullions are ONE FLAT COLOUR, in range, with no blue. A primitive whose
 * every vertex carries the same texture coordinate samples ONE TEXEL over its
 * whole area, and a large object painted in one texel is exactly what the
 * handovers have been calling a silhouette.
 *
 * That ruled the combiner, the diffuse, the clear, the wrap mode and the
 * negative-q collapse out one at a time. What it cannot say is WHICH draws
 * they are or why their coordinate never varies, because a picture has no
 * batch numbers in it. This counts them.
 *
 * WHY IT SITS HERE and not in the fixed-function batch watcher: this title
 * draws through guest vertex programs, and RECOMP_FF_BATCH_WATCH_TEX has both
 * of its call sites inside the fixed-function branches -- the same blindness
 * the note at RECOMP_GLYPH_DUMP records. nv2a_metal_draw is the common entry
 * for both paths, so it sees every draw either way.
 *
 * FLAT IS NOT AUTOMATICALLY WRONG. A two-triangle quad drawn with a single
 * solid colour texel is a legitimate way to fill a rectangle, and the NV2A
 * does the same thing when no coordinate array is bound: every vertex takes
 * the "current" register. So the report prints the AREA as well as the count
 * -- what needs explaining is a flat coordinate over a large object, not a
 * flat coordinate as such.
 *
 *   RECOMP_T0_CENSUS=<max lines>   off unless set; 0 counts without printing
 *
 * Read-only. */
#define T0_CENSUS_KEYS 96
static struct { uint32_t tex, w, h; float u, v;
                unsigned long draws; double area;
                /* THE SUM IS THE WRONG STATISTIC and the first run proved it:
                 * every key came back with a big total built out of draws a
                 * few pixels across, and the object actually painted in one
                 * texel was nowhere in the table. Keep the biggest single
                 * draw and its screen box. */
                double maxarea; float bx0,by0,bx1,by1;
                unsigned maxverts; } g_t0_flat[T0_CENSUS_KEYS];
static unsigned g_t0_flat_n;
static unsigned long g_t0_draws, g_t0_flat_draws, g_t0_flat_overflow;
static double g_t0_flat_area, g_t0_varying_area;

static int t0_census_cap(void)
{ static int cap=-1;
  if(cap<0){ const char*e=getenv("RECOMP_T0_CENSUS"); cap=(e&&*e)?atoi(e):-1;
             if(cap<0&&e) cap=0; }
  return cap; }

void t0_census_report(void)
{
    if(!g_t0_draws) return;
    fprintf(stderr,"[T0] textured draws=%lu, of which ONE COORDINATE ON EVERY"
            " VERTEX=%lu (%.1f%%); screen area flat=%.0f varying=%.0f\n",
            g_t0_draws, g_t0_flat_draws,
            g_t0_draws?100.0*(double)g_t0_flat_draws/(double)g_t0_draws:0.0,
            g_t0_flat_area, g_t0_varying_area);
    if(g_t0_flat_overflow)
        fprintf(stderr,"[T0]   %lu flat draws did not fit the %u-key table\n",
                g_t0_flat_overflow,(unsigned)T0_CENSUS_KEYS);
    /* Sorted by AREA, because one flat draw covering a third of the screen is
     * the defect and ten thousand covering four pixels each are not. */
    for(unsigned n=0;n<g_t0_flat_n;n++){
        unsigned best=0; double top=-1.0;
        for(unsigned i=0;i<g_t0_flat_n;i++)
            if(g_t0_flat[i].maxarea>top){top=g_t0_flat[i].maxarea;best=i;}
        if(top<0.0) break;
        fprintf(stderr,"[T0]   tex %08X %ux%u  draws=%lu  biggest=%.0f px"
                " (%u verts, screen %.0f,%.0f..%.0f,%.0f)  total=%.0f"
                "  coord=(%.4f,%.4f)\n",
                g_t0_flat[best].tex,g_t0_flat[best].w,g_t0_flat[best].h,
                g_t0_flat[best].draws,g_t0_flat[best].maxarea,
                g_t0_flat[best].maxverts,
                (double)g_t0_flat[best].bx0,(double)g_t0_flat[best].by0,
                (double)g_t0_flat[best].bx1,(double)g_t0_flat[best].by1,
                g_t0_flat[best].area,
                (double)g_t0_flat[best].u,(double)g_t0_flat[best].v);
        g_t0_flat[best].maxarea=-1.0;
    }
}

int nv2a_metal_draw(const NV2ATextureCopy*s,const uint8_t*texture,size_t texture_size,
 uint8_t*target,size_t target_size,uint8_t*depth,size_t depth_size,
 const float(*vertices)[16][4],unsigned count,unsigned primitive)
{
 draw_thread_check();
 unsigned long long _tf=mtl_cb_stats()?mtl_now_ns():0;
 if(!s)return reject("null-state");
 if((s->texture_mask&1)&&!texture)return reject("missing-texture");
 if(!target)return reject("missing-target");
 if(!vertices||count<3||count>NV2A_METAL_MAX_VERTICES)return reject("vertex-count");
 if(s->target_bpp!=2)return reject("target-format");
 if(!s->clip_w||!s->clip_h||s->clip_x||s->clip_y||s->clip_w>4096||s->clip_h>4096)return reject("clip");
 /* The window clip (scissor). prepare() has intersected it with the surface
  * clip and refused an empty one, so it lies inside the target; clamp again
  * against what Metal will actually be bound to, because a scissor outside
  * the attachment is a validation failure, not a clip. */
 uint32_t wc_x0,wc_y0,wc_x1,wc_y1;nv2a_texture_copy_window(s,&wc_x0,&wc_y0,&wc_x1,&wc_y1);
 if(wc_x1<wc_x0||wc_y1<wc_y0||wc_x0>=s->clip_w||wc_y0>=s->clip_h)return reject("window-clip");
 if(s->target_pitch<(uint64_t)s->clip_w*2)return reject("target-pitch");
 /* See the note on g_t0_flat. Bounded: one pass over the batch's vertices,
  * and only when the switch has been read as set. */
 if(t0_census_cap()>=0&&(s->texture_mask&1)&&count>=3){
  extern double xbox_TraceSeconds(void);
  enum { T0_POS=0, T0_TEX0=9 };
  const float*t0=vertices[0][T0_TEX0];
  int flat=1;
  float x0=vertices[0][T0_POS][0],x1=x0,y0=vertices[0][T0_POS][1],y1=y0;
  for(unsigned i=1;i<count;i++){
   const float*p=vertices[i][T0_POS],*t=vertices[i][T0_TEX0];
   if(p[0]<x0)x0=p[0]; if(p[0]>x1)x1=p[0];
   if(p[1]<y0)y0=p[1]; if(p[1]>y1)y1=p[1];
   /* memcmp, not ==: the question is whether the vertex stage produced the
    * same BITS, and a tolerance here would hide a coordinate that varies in
    * the last place -- which still samples one texel but is a different bug
    * from one that never varied at all. */
   if(flat&&memcmp(t,t0,16)) flat=0;
  }
  double area=(double)(x1-x0)*(double)(y1-y0);
  g_t0_draws++;
  if(!flat) g_t0_varying_area+=area;
  else {
   g_t0_flat_draws++; g_t0_flat_area+=area;
   float w=t0[3]!=0.0f?t0[3]:1.0f, u=t0[0]/w, v=t0[1]/w;
   unsigned k;
   for(k=0;k<g_t0_flat_n;k++) if(g_t0_flat[k].tex==s->texture_offset) break;
   if(k==g_t0_flat_n){
    if(g_t0_flat_n<T0_CENSUS_KEYS){
     g_t0_flat[k].tex=s->texture_offset; g_t0_flat[k].w=s->width;
     g_t0_flat[k].h=s->height; g_t0_flat_n++;
    } else { g_t0_flat_overflow++; k=T0_CENSUS_KEYS; }
   }
   if(k<T0_CENSUS_KEYS){
    g_t0_flat[k].draws++; g_t0_flat[k].area+=area;
    g_t0_flat[k].u=u; g_t0_flat[k].v=v;
    if(area>g_t0_flat[k].maxarea){ g_t0_flat[k].maxarea=area;
     g_t0_flat[k].maxverts=count;
     g_t0_flat[k].bx0=x0; g_t0_flat[k].by0=y0;
     g_t0_flat[k].bx1=x1; g_t0_flat[k].by1=y1; }
   }
   {static int shown; static double minarea=-1.0; int cap=t0_census_cap();
    /* RECOMP_T0_CENSUS_MINAREA: the run that printed the first 40 flat draws
     * printed 40 slivers a few pixels across, because those are simply what
     * a frame has most of. The question is about LARGE flat objects. */
    if(minarea<0.0){const char*e=getenv("RECOMP_T0_CENSUS_MINAREA");
                    minarea=(e&&*e)?atof(e):0.0;}
    if(shown<cap&&area>=minarea){ shown++;
     fprintf(stderr,"  [T0] t=%.2f verts=%u prim=%u tex %08X %ux%u%s"
             " screen %.0f,%.0f..%.0f,%.0f (%.0f px) coord=(%g,%g,%g,%g)\n",
             xbox_TraceSeconds(),count,primitive,s->texture_offset,
             s->width,s->height,s->dxt1?" dxt1":s->rgba8?" rgba8":"",
             (double)x0,(double)y0,(double)x1,(double)y1,area,
             (double)t0[0],(double)t0[1],(double)t0[2],(double)t0[3]);}}
  }
 }
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
 /* static: at the raised cap this is 192 KB, and a stack array that size is a
  * crash rather than a slow path. Safe because the submit path is driven by the
  * single pushbuffer executor thread -- the same assumption the ring allocator
  * already makes. 3x the cap because a strip or fan emits 3*(count-2).
  * n stays automatic: only the storage for `indices` needed to move. */

 /* ── RECOMP_GLYPH_DUMP: WHICH ATLAS CELL DID EACH CHARACTER COME FROM? ──
  *
  * G2. Every corrupted letter on record is v, w, x or y -- character codes
  * 0x76-0x79, four consecutive values -- and the replacement is TALLER than
  * its neighbours and shaped like a kanji, on a baseline shared with the
  * Latin glyphs. JSRF's font carries both (the disc ships tex_eng.dat beside
  * tex_jpn.dat). So the standing hypothesis is an INDEX that walks off the
  * end of the Latin range for those four codes and lands in the Japanese
  * block -- arithmetic, not a race, because a race cannot respect character
  * codes.
  *
  * This is the measurement that decides it. Text is drawn as one quad per
  * character, so the quad's TEXCOORD0 rectangle IS the glyph's cell in the
  * atlas. Print, per quad, the screen span and that rectangle: read a line of
  * dialogue left to right and the v/w/x/y cells either sit beside their
  * neighbours or they do not.
  *
  * IT INSTRUMENTS nv2a_metal_draw AND NOT THE FIXED-FUNCTION BATCH WATCHER,
  * deliberately. RECOMP_FF_BATCH_WATCH_TEX has both of its call sites inside
  * the fixed-function branches, and this title draws its text through guest
  * vertex programs -- 8 million VP draws in the 17:39 session. That watcher
  * has been armed in the player's config for days and is blind to the thing
  * it was pointed at. This site is the common draw entry for BOTH paths.
  *
  *   RECOMP_GLYPH_DUMP=<max lines>        off unless set
  *   RECOMP_GLYPH_DUMP_AFTER=<seconds>    start late; text needs gameplay
  *   RECOMP_GLYPH_DUMP_MAXQ=<n>           only draws with <= n quads (default
  *                                        64) -- a line of text is tens of
  *                                        quads and the world is thousands,
  *                                        so this is what separates them
  *   RECOMP_GLYPH_DUMP_LATIN=1            only draws the Latin-page detector
  *                                        below accepts: 256x256, with cells
  *                                        of glyph proportions. The cap counts
  *                                        QUAD LINES and the title screen
  *                                        spends it before gameplay -- 400,000
  *                                        lines were gone by t=32 s of a boot,
  *                                        on menu atlases that are 256x256 and
  *                                        not the font. A tutorial banner
  *                                        arrives minutes into a replay, so
  *                                        without this the dump is all menu.
  *
  * Read-only: it prints and returns. */
 {
  static int glyph_cap=-1; static double glyph_after; static unsigned glyph_maxq;
  static unsigned glyph_lines; static uint32_t glyph_tex; static int glyph_latin;
  extern double xbox_TraceSeconds(void);
  /* Attribute slots, from nv2a_regs.h, spelled out because this file does not
   * include it: 0 = POSITION, 9 = TEXCOORD0. */
  enum { GLYPH_ATTR_POS = 0, GLYPH_ATTR_TEX0 = 9 };
  if(glyph_cap<0){
   const char*e=getenv("RECOMP_GLYPH_DUMP"); glyph_cap=(e&&*e)?atoi(e):0;
   const char*a=getenv("RECOMP_GLYPH_DUMP_AFTER"); glyph_after=(a&&*a)?atof(a):0.0;
   const char*q=getenv("RECOMP_GLYPH_DUMP_MAXQ"); glyph_maxq=(q&&*q)?(unsigned)atoi(q):64u;
   const char*t=getenv("RECOMP_GLYPH_DUMP_TEX"); glyph_tex=(t&&*t)?(uint32_t)strtoul(t,NULL,16):0u;
   glyph_latin=recomp_switch_on("RECOMP_GLYPH_DUMP_LATIN");
   if(glyph_cap>0)
    fprintf(stderr,"  [GLYPH] dumping up to %d quad lines for textured draws of"
            " <= %u quads, after t=%.0fs. One line per character-sized quad:"
            " screen span, then the TEXCOORD0 cell it samples.\n",
            glyph_cap,glyph_maxq,glyph_after);
  }
  /* HOW MANY VERTICES MAKE ONE CHARACTER depends on the primitive, and
   * getting this wrong silently mis-groups the dump: JSRF submits its text as
   * TRIANGLES (prim 5), so a character is SIX vertices -- two triangles --
   * not four. Grouping by four produced quads whose "cell" spanned half of
   * one glyph and half of the next, and the first pass at this instrument
   * reported no text-like draws at all because of it. */
  unsigned vpq = (primitive==8u) ? 4u        /* QUADS */
               : (primitive==5u) ? 6u        /* TRIANGLES: 2 per character */
               : 0u;                         /* strips/fans: not grouped */
  /* THE DETECTOR RUNS ON EVERY DRAW, not only the printed ones. It lived
   * inside the print-capped block first, so capping output to one line
   * silently disabled it and the run reported nothing at all. Counting is
   * not printing. */
  int latin_draw=0;
  if(glyph_cap>0&&(s->texture_mask&1)&&vpq&&count>=vpq&&(count%vpq)==0){
   unsigned quads=count/vpq;
   /* Latin-page geometry is the filter; the argument is below.
    *
    * JSRF's Latin font is TWO 256x256 pages of 84 cells on a 21x34 grid
    * (descriptor at guest 0x1F8B48: 2 pages, 12 cols x 7 rows, base texture
    * id 4). Codes 0x21-0x75 are page 0; 0x76-0x7A -- v w x y z -- are the
    * first five cells of page 1, and EVERY corrupted letter ever recorded is
    * one of those. The guest binds a page's texture only when it differs
    * from the one cached at 0x00251D54, so a page-1 glyph drawn while page 0
    * is still bound samples page 0's cell of the same number: y is page 1
    * row 0 col 3, page 0 row 0 col 3 is '$', and the player's capture reads
    * "$ou" for "you".
    *
    * So the question is not what a frame looks like, it is HOW MANY DISTINCT
    * TEXTURES the text is drawn against. A correct run binds both Latin
    * pages. A run that only ever binds one has the defect by construction,
    * and this counts them without anybody catching a frame.
    *
    * Latin-page geometry is the filter: 256x256, cells about 21 wide and 34
    * tall. Reported once at exit. */
   if(s->width==256&&s->height==256){
    float cw=0,chh=0; unsigned nc=0;
    for(unsigned qi=0;qi<quads;qi++){
     float u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f;
     for(unsigned k=0;k<vpq;k++){
      const float*tt=vertices[qi*vpq+k][GLYPH_ATTR_TEX0];
      if(tt[0]<u0)u0=tt[0]; if(tt[0]>u1)u1=tt[0];
      if(tt[1]<v0)v0=tt[1]; if(tt[1]>v1)v1=tt[1];
     }
     cw+=(u1-u0)*256.0f; chh+=(v1-v0)*256.0f; nc++;
    }
    if(nc){ cw/=nc; chh/=nc;
     if(cw>16.0f&&cw<26.0f&&chh>28.0f&&chh<40.0f){
      latin_draw=1;
      unsigned i; for(i=0;i<g_latin_tex_n;i++) if(g_latin_tex[i]==s->texture_offset) break;
      if(i==g_latin_tex_n&&g_latin_tex_n<16){ g_latin_tex[g_latin_tex_n]=s->texture_offset;
       g_latin_tex_quads[g_latin_tex_n]=0; g_latin_tex_n++; }
      if(i<16) g_latin_tex_quads[i]+=quads;
     }
    }
   }
  }

  if(glyph_cap>0&&(int)glyph_lines<glyph_cap&&(s->texture_mask&1)
     &&vpq&&count>=vpq&&(count%vpq)==0&&(count/vpq)<=glyph_maxq
     &&(!glyph_tex||s->texture_offset==glyph_tex)
     &&(!glyph_latin||latin_draw)
     &&xbox_TraceSeconds()>=glyph_after){
   unsigned quads=count/vpq;
   fprintf(stderr,"  [GLYPH] draw: t=%.1fs %u quads, tex0 %08X %ux%u fmt%s%s,"
           " prim %u, target %08X\n",
           xbox_TraceSeconds(),quads,s->texture_offset,s->width,s->height,
           s->dxt1?" dxt1":"",s->rgba8?" rgba8":"",primitive,s->target_offset);
   /* Pitch and swizzle decide how the dumped bytes are read back, and
    * guessing cost two decodes: a linear atlas whose pitch is not width*bpp
    * shears into stripes, and a swizzled one read linearly looks like noise.
    * Print them beside the dump rather than inferring them later. */
   fprintf(stderr,"  [GLYPH]  atlas layout: pitch %u, %s, levels %u\n",
           s->pitch, s->linear?"LINEAR":"swizzled", s->levels);
   /* IS THE GPU HOLDING A DIFFERENT PICTURE OF THIS TEXTURE THAN GUEST RAM?
    *
    * The atlas the text samples is also something the title RENDERS INTO --
    * these draws sample 01737000 while targeting another surface, and the
    * decoded guest-RAM copy is not text. This tree already carries the
    * hazard in surface_pay_debt_for_range's neighbourhood: a slot cleared
    * residently while another surface is bound leaves the GPU holding pixels
    * guest RAM has not got. If the text is composed on the GPU and we sample
    * the stale guest-RAM copy, the drawn text is whatever that copy last
    * held -- which is exactly the shape of the defect, and "flickers in
    * layers" is what an intermittently stale source looks like.
    *
    * So: find a surface slot overlapping the sampled range, read its colour
    * texture back, and diff it against the bytes we are about to upload.
    * Reports the slot's debt state either way, because "no overlap at all"
    * is itself an answer -- it would mean the atlas is never a render target
    * and the composition happens somewhere else.
    *
    * Costs a drain and a read-back, so it is behind its own switch and off
    * unless asked for. */
   if(recomp_switch_on("RECOMP_GLYPH_GPU_DIFF")){
    int found=0;
    for(unsigned i=0;i<surface_slots_used();i++){
     if(!surf_slot[i].target) continue;
     if(!guest_ranges_overlap(texture,texture_size,
                              surf_slot[i].target,surface_slot_bytes(i))) continue;
     found=1;
     fprintf(stderr,"  [GLYPH]  GPU slot %u overlaps the sampled range:"
             " valid=%d owes_guest_ram=%d %ux%u pitch %u\n",
             i,surf_slot[i].valid,surf_slot[i].owes_guest_ram,
             surf_slot[i].w,surf_slot[i].h,surf_slot[i].pitch);
     if(surf_slot[i].valid&&surf_slot[i].colour){
      size_t stride=(size_t)surf_slot[i].w*(hw_565_on()?2:16);
      size_t need=stride*surf_slot[i].h;
      uint8_t*gpu=(uint8_t*)malloc(need);
      if(gpu){
       id<MTLCommandBuffer> drain=[queue commandBuffer]; [drain commit];
       [drain waitUntilCompleted];
       [surf_slot[i].colour getBytes:gpu bytesPerRow:stride
            fromRegion:MTLRegionMake2D(0,0,surf_slot[i].w,surf_slot[i].h)
            mipmapLevel:0];
       /* Compare only what both sides have, byte for byte, and say WHERE
        * they first part rather than only that they do. */
       size_t n=need<texture_size?need:texture_size, differ=0, first=(size_t)-1;
       for(size_t k=0;k<n;k++) if(gpu[k]!=texture[k]){ if(first==(size_t)-1)first=k; differ++; }
       fprintf(stderr,"  [GLYPH]  GPU vs guest RAM over %zu bytes: %zu differ"
               " (%.1f%%), first at %zu%s\n",
               n,differ,n?100.0*differ/n:0.0,
               first==(size_t)-1?0:first,
               differ?"  <<< THE SAMPLED COPY IS NOT WHAT THE GPU HAS":
                      "  (identical -- guest RAM is current)");
       free(gpu);
      }
     }
    }
    if(!found)
     fprintf(stderr,"  [GLYPH]  no GPU surface slot overlaps this texture:"
             " it is not a render target here, so guest RAM is the only copy"
             " and the composition happens before it reaches us\n");
    fflush(stderr);
   }
   /* RECOMP_GLYPH_DUMP_TEXFILE=<path>: the sampled texture's guest bytes,
    * once, exactly as they are handed to the GPU. Decoded offline rather than
    * here: the atlas is DXT1 and a decoder in the draw path would be a second
    * implementation to keep honest. Seeing the atlas is what says whether the
    * text is composed INTO it (a strip of rendered line) or looked up FROM it
    * (a grid of glyph cells) -- the single-quad draws sampling 1,1..511,111
    * point at the former and that would move the whole defect into the
    * guest's own compositing, not our sampler. */
   {
    static int wrote;
    const char*tf=getenv("RECOMP_GLYPH_DUMP_TEXFILE");
    if(tf&&*tf&&!wrote&&texture&&texture_size){
     FILE*f=fopen(tf,"wb");
     if(f){ fwrite(texture,1,texture_size,f); fclose(f); wrote=1;
      fprintf(stderr,"  [GLYPH] wrote %zu texture bytes to %s (%ux%u%s)\n",
              texture_size,tf,s->width,s->height,s->dxt1?" dxt1":
              s->rgba8?" rgba8":" other"); }
    }
   }
   for(unsigned qi=0;qi<quads&&(int)glyph_lines<glyph_cap;qi++,glyph_lines++){
    /* The character's extent is the BOUNDING BOX over its vertices, in both
     * screen space and texture space. Taking two corners by index assumes a
     * winding order, and a triangle pair does not have one. */
    float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f;
    float u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f;
    for(unsigned k=0;k<vpq;k++){
     const float*pp=vertices[qi*vpq+k][GLYPH_ATTR_POS];
     const float*tt=vertices[qi*vpq+k][GLYPH_ATTR_TEX0];
     if(pp[0]<x0)x0=pp[0]; if(pp[0]>x1)x1=pp[0];
     if(pp[1]<y0)y0=pp[1]; if(pp[1]>y1)y1=pp[1];
     if(tt[0]<u0)u0=tt[0]; if(tt[0]>u1)u1=tt[0];
     if(tt[1]<v0)v0=tt[1]; if(tt[1]>v1)v1=tt[1];
    }
    const float p0[2]={x0,y0}, p2[2]={x1,y1};
    /* Cell in TEXELS as well as normalised: a cell index is what the
     * hypothesis is about, and texels are what you compare against the
     * atlas image. */
    fprintf(stderr,"  [GLYPH]  q%02u screen x %7.1f..%7.1f y %7.1f..%7.1f  "
            "uv %.4f,%.4f..%.4f,%.4f  texel %6.1f,%6.1f..%6.1f,%6.1f\n",
            qi,(double)p0[0],(double)p2[0],(double)p0[1],(double)p2[1],
            (double)u0,(double)v0,(double)u1,(double)v1,
            (double)(u0*s->width),(double)(v0*s->height),
            (double)(u1*s->width),(double)(v1*s->height));
   }
   fflush(stderr);
  }
 }
 static unsigned indices[NV2A_METAL_MAX_VERTICES*3];
 unsigned n=0;
 /* Set BEFORE assembly, because triangle() is what reads it. The same
  * predicate as vsh_gpu_active below, minus the parts that depend on state
  * this function has not reached yet; if either of those later refuses, the
  * draw is rejected rather than drawn with the wrong culling. */
 vsh_gpu_culling = vsh_gpu_on() && vsh_active && hw_state_on() && vsh_constants;
 if(vsh_gpu_culling && s->cull_face==0x408){reject_reason=NULL;return 0;}
 switch(primitive){case 5:for(unsigned i=0;i+2<count;i+=3)triangle(s,vertices,indices,&n,i,i+1,i+2);break;
 case 6:for(unsigned i=0;i+2<count;i++)triangle(s,vertices,indices,&n,i+(i&1),i+1-(i&1),i+2);break;
 case 7:for(unsigned i=1;i+1<count;i++)triangle(s,vertices,indices,&n,0,i,i+1);break;
 case 8:for(unsigned i=0;i+3<count;i+=4){triangle(s,vertices,indices,&n,i,i+1,i+2);triangle(s,vertices,indices,&n,i,i+2,i+3);}break;
 case 9:for(unsigned i=0;i+3<count;i+=2){triangle(s,vertices,indices,&n,i,i+1,i+3);triangle(s,vertices,indices,&n,i,i+3,i+2);}break;default:return reject("primitive");}
 if(mtl_cb_stats()){g_mtl_ns_valid+=mtl_now_ns()-_tf;_tf=mtl_now_ns();}
 if(!n){reject_reason=NULL;return 0;}
 if(clip_audit_on())clip_audit(vertices,indices,n,s->clip_w,s->clip_h);
 if(bench_state==2)bench_capture(s,texture,texture_size,target,target_size,depth,depth_size,vertices,count,primitive);
 @autoreleasepool{if(!initialize())return reject("initialization");
  /* A DRAW WITH NO DEPTH DOES NOT NEED THE RETAINED DEPTH THROWN AWAY.
   *
   * This bound NULL for any draw that neither depth-tests nor stencil-tests,
   * and NULL then failed the retained-surface test below -- which syncs, reads
   * the whole surface back, reallocates every texture and re-uploads it. So
   * every alternation between a depth-using draw and a depth-less one cost a
   * full surface round trip, measured at about 1.7 per frame at gameplay, and
   * each one also ends the open batch.
   *
   * Keeping the binding is not a shortcut, it is what the state already says:
   * with depth_test off the compare is forced to ALWAYS, and since depth
   * writes were gated on the test the write is off too, so an attached depth
   * buffer is inert for such a draw. The software tail agrees -- p.depth_test
   * is 0 so it never compares, and p.depth_write is depth_test && depth_write
   * so it writes dst.a straight back. depth_dirty is likewise only set for
   * depth_test && depth_write, so nothing marks the buffer dirty either.
   *
   * Before anything has been uploaded there is nothing to keep, so the first
   * draw still takes the upload path and hw_depth_upload(NULL, ...) fills a
   * depth texture with 1.0 as it always did. */
  int use_zeta=s->depth_test||s->stencil_test;
  uint8_t*next_depth      =use_zeta?depth        :(depth_valid?depth_target      :NULL);
  uint32_t next_depth_pitch=use_zeta?s->depth_pitch:(depth_valid?depth_pitch      :0);
  size_t next_depth_size  =use_zeta?depth_size   :(depth_valid?depth_target_size :0);
  /* Staging. Anything that fits Metal's 4 KB inline limit still goes through
   * setVertexBytes, which costs no allocation at all; everything larger comes
   * out of the slab ring above in a single contiguous reservation covering
   * both the vertices and, when they are too big to inline too, the indices.
   * The private-allocation path is kept as the fallback for a ring that could
   * not allocate, because a slow correct draw beats a rejected one. */
  /* A program applies only if the executor asked for one for THIS draw and the
   * hardware tail is in play. vsh_active is cleared by the executor for every
   * draw that did not go through nv2a_metal_vsh_ready, so a stale program can
   * never be applied to somebody else's vertices. */
  int vsh_gpu_active = vsh_gpu_on() && vsh_active && hw_state_on()
                       && vsh_constants;
  Vertex small_vertices[36];
  size_t vertex_bytes = vsh_gpu_active
      ? (size_t)count * vsh_active->nattrs * 16
      : count*sizeof(Vertex);
  size_t index_bytes=n*sizeof(indices[0]);
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
  id<MTLTexture>ht[4]={nil,nil,nil,nil};id<MTLSamplerState>hs[4]={nil,nil,nil,nil};unsigned hwmask=0;
  uint8_t tamin[4]={0,0,0,0},tamax[4]={255,255,255,255};
  g_draw_alpha_floor=-1.0f;
  for(unsigned u=0;u<4;u++){
   int active=(s->texture_mask&(1u<<u))!=0;const NV2ATextureCopy*t=u&&active?&s->extra_stages[u-1]:s;
   const uint8_t*data=active?(u?s->extra_texture[u-1]:texture):NULL;size_t bytes=active?nv2a_texture_copy_texture_bytes(t):0;
   /* Render-to-texture, and the one place a deferred write-back could be
    * read straight past. texture_buffer uploads these bytes out of guest
    * RAM; if a slot is still holding them on the GPU it has to hand them
    * over first. Four slots and a pointer compare when nothing is owed. */
   surface_pay_debt_for_range(data,bytes,&g_debt_paid_on_read);
   /* The bound surface itself: see feedback_sync_on. Counted always, paid
    * only when asked. The sync flushes the open batch, so the sample sees
    * every draw before this one. */
   if(data&&surface_valid&&surface_target&&guest_ranges_overlap(data,bytes,surface_target,surface_target_size)){
    ++g_feedback_draws;
    if(feedback_sync_on()&&surface_dirty){nv2a_metal_sync();++g_feedback_syncs;}}
   tb[u]=texture_buffer(data,bytes);if(!tb[u])return reject("buffer-allocation");
   if(active&&bytes&&hw_tex_on()&&(t->rgba8||t->dxt1||t->dxt3)){
    ht[u]=texture_hw(t);
    if(ht[u]){hs[u]=hw_sampler(t);hwmask|=1u<<u;++hw_tex_units;tamin[u]=hw_last_min_a;tamax[u]=hw_last_max_a;}}}
  if(early_z_exact_on()&&s->alpha_test&&s->combiner_count){
   Rng tr[4];
   for(unsigned u=0;u<4;u++)tr[u]=(hwmask&(1u<<u))?rng_c(tamin[u]/255.0f,tamax[u]/255.0f):rng_c(0.0f,1.0f);
   Rng d0=vsh_gpu_active?vsh_out_alpha(vsh_active,0,vsh_constants):rng_c(0.0f,1.0f);
   Rng d1=vsh_gpu_active?vsh_out_alpha(vsh_active,1,vsh_constants):rng_c(0.0f,1.0f);
   g_draw_alpha_floor=comb_alpha_floor(s,d0,d1,tr);}
  /* WHAT `vertices` HOLDS DEPENDS ON WHO IS GOING TO RUN THE PROGRAM.
   *
   * On the CPU path it is the program's OUTPUTS -- position, two colours, four
   * texture coordinates -- and the seven float4 the fixed `vs` wants are
   * copied out of it. With a program active it is the program's INPUTS, and
   * only the attributes the program actually reads are uploaded, packed in
   * ascending attribute order, because uploading all sixteen would more than
   * double this path's bandwidth for slots the shader never names. */
  if(vsh_gpu_active){
   float4v *raw=(float4v*)v;
   for(unsigned i=0;i<count;i++){unsigned slot=0;
    for(unsigned a=0;a<16;a++) if(vsh_active->inputs&(1u<<a))
     memcpy(raw[i*vsh_active->nattrs+slot++].f,vertices[i][a],16);}
  } else
  for(unsigned i=0;i<count;i++){memcpy(v[i].p,vertices[i][0],16);memcpy(v[i].d0,vertices[i][3],16);memcpy(v[i].d1,vertices[i][4],16);for(unsigned u=0;u<4;u++)memcpy(v[i].t[u],vertices[i][9+u],16);}
  if(mtl_cb_stats()){g_mtl_ns_pack+=mtl_now_ns()-_tf;_tf=mtl_now_ns();}
  if(!surface_valid||!depth_valid||surface_target!=target||surface_width!=s->clip_w||surface_height!=s->clip_h||surface_pitch!=s->target_pitch||surface_target_size!=target_size||depth_target!=next_depth||depth_pitch!=next_depth_pitch||depth_target_size!=next_depth_size){
   /* Mark the debt before the sync, so the sync finds nothing to do and takes
    * its already-clean early return. Identity on the texture, not on the guest
    * pointer: two slots can name the same guest address at different sizes,
    * and only the object we are actually holding is the one whose pixels this
    * debt is about. */
   /* THE DEPTH REFUSAL IS NARROWED, AND ONLY BY WHAT MAKES IT POINTLESS.
    *
    * `depth_dirty` refuses the deferral because surface_slot_writeback carries
    * COLOUR only: defer the swap with depth outstanding and the depth is lost,
    * because a cache MISS would re-upload stale depth from guest RAM.
    *
    * That reasoning holds exactly as long as the depth write-back happens at
    * all. With RECOMP_METAL_NO_DEPTH_SYNC on it does not: the write-back is
    * skipped and depth_dirty is cleared without the depth ever reaching guest
    * RAM. Refusing to defer in order to protect a value nobody is going to
    * write is the whole reason A2 was inert -- the refusal fired 5,747-12,656
    * times against ONE successful deferral in a 240 s run.
    *
    * So the condition becomes "depth is dirty AND somebody is going to write
    * it". It is not a relaxation of the safety argument; it is the same
    * argument with its premise checked.
    *
    * Counted separately, because "deferred" and "deferred only because depth
    * is being thrown away" are different facts and a future reader must not
    * have to infer which one a number describes. */
   if(defer_swap_on()&&surface_cache_on()&&surface_valid&&surface&&surface_dirty){
    if(depth_dirty&&!no_depth_sync_on()){++g_swap_defer_depth;}
    else{
     if(depth_dirty)++g_swap_deferred_no_depth;
     int owed=0;
     for(unsigned i=0;i<surface_slots_used();i++)
      if(surf_slot[i].valid&&surf_slot[i].colour==surface){
       surf_slot[i].owes_guest_ram=1;surface_dirty=0;
       owed=1;++g_swap_deferred;break;}
     if(!owed)++g_swap_defer_noslot;}}
   ++surface_uploads;++g_sync_by_swap;g_sync_who=SYNC_WHO_SWAP;
   if(!nv2a_metal_sync())return reject("surface-sync");
   /* A SWAP BACK TO A SURFACE GUEST RAM CANNOT HAVE CHANGED IS A REBIND.
    * The sync above has already written the outgoing surface out, so the
    * incoming slot's textures still hold exactly what was drawn into them --
    * and unlike a re-upload from guest RAM, rebinding cannot lose the early
    * draws that made the round trip. */
   int slot_hit=-1;
   if(surface_cache_on())
    for(unsigned i=0;i<surface_slots_used();i++)
     if(surf_slot[i].valid&&surf_slot[i].target==target&&surf_slot[i].target_size==target_size
        &&surf_slot[i].w==s->clip_w&&surf_slot[i].h==s->clip_h&&surf_slot[i].pitch==s->target_pitch
        &&surf_slot[i].depth==next_depth&&surf_slot[i].depth_pitch==next_depth_pitch
        &&surf_slot[i].depth_size==next_depth_size){slot_hit=(int)i;break;}
   if(slot_hit>=0){
    surface=surf_slot[slot_hit].colour;stencil_surface=surf_slot[slot_hit].stencil;
    hw_depth_tex=surf_slot[slot_hit].hw_depth;hw_stencil_tex=surf_slot[slot_hit].hw_stencil;
    surface_target=target;surface_target_size=target_size;surface_width=s->clip_w;
    surface_height=s->clip_h;surface_pitch=s->target_pitch;
    depth_target=next_depth;depth_target_size=next_depth_size;depth_pitch=next_depth_pitch;
    surface_valid=depth_valid=1;
    /* INHERIT THE SLOT'S DEBT. If this slot was cleared while another surface
     * was bound, its texture holds pixels guest RAM has never seen; becoming
     * the live surface transfers that debt to surface_dirty, which the flip's
     * sync already knows how to pay. Clearing surface_dirty here instead --
     * which is what "a swap back is a rebind" did before there was any way for
     * a slot to be ahead of guest RAM -- would drop the clear on the floor. */
    surface_dirty=surf_slot[slot_hit].owes_guest_ram?1:0;depth_dirty=0;
    surf_slot[slot_hit].owes_guest_ram=0;
    surf_slot[slot_hit].stamp=++surf_clock;++surface_hits;
   }else{
   /* This branch is about to re-upload the surface FROM GUEST RAM, so any
    * slot still owing those bytes must hand them back first or the
    * rebuild starts from pre-render contents. The lookup above is exact
    * on seven fields, so a surface whose depth pointer merely moved lands
    * here with its colour debt outstanding. */
   surface_pay_debt_for_range(target,target_size,&g_debt_paid_on_rebuild);
   MTLTextureDescriptor*td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:hw_565_on()?MTLPixelFormatB5G6R5Unorm:MTLPixelFormatRGBA32Float width:s->clip_w height:s->clip_h mipmapped:NO];td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModeShared;surface=[device newTextureWithDescriptor:td];MTLTextureDescriptor*sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:s->clip_w height:s->clip_h mipmapped:NO];sd.usage=MTLTextureUsageRenderTarget;sd.storageMode=MTLStorageModeShared;stencil_surface=[device newTextureWithDescriptor:sd];if(!surface||!stencil_surface)return reject("surface-allocation");
   surface_target=target;surface_target_size=target_size;surface_width=s->clip_w;surface_height=s->clip_h;surface_pitch=s->target_pitch;size_t pixels=(size_t)surface_width*surface_height;
   /* THE STENCIL BUFFER IS FILLED ONLY IN THE NON-565 BRANCH BELOW, and the
    * replaceRegion that consumes it used to run unconditionally -- so under
    * RECOMP_METAL_565 the R8Uint stencil surface was uploaded from
    * uninitialised heap, 307 KB of it per rebuild. Harmless only because 565
    * implies the hardware tail and stencil_surface is then never attached,
    * which is a reason it was not VISIBLE, not a reason it was not wrong.
    * calloc rather than malloc: the cost is a page-zero the allocator does
    * anyway for a fresh 307 KB, and it makes the uninitialised read impossible
    * rather than merely unreachable. */
   float*rgba=malloc(pixels*(hw_565_on()?2:16));uint8_t*stencil=calloc(pixels,1);if(!rgba||!stencil){free(rgba);free(stencil);return reject("upload-allocation");}
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
   if(hw_state_on()&&!hw_depth_upload(next_depth,surface_width,surface_height,next_depth_pitch)){++g_hw_upload_fail;hw_depth_tex=nil;hw_stencil_tex=nil;}
   /* Remember it, so the next swap back is a rebind. Least-recently-used goes
    * first; a dropped slot only costs the rebuild it would have saved. */
   if(surface_cache_on()){
    unsigned pick=0;
    for(unsigned i=0;i<surface_slots_used();i++){
     if(!surf_slot[i].valid){pick=i;break;}
     if(surf_slot[i].stamp<surf_slot[pick].stamp)pick=i;}
    /* EVICTION HAS TO PAY THE DEBT, AND DID NOT. defer_swap's header says
     * "surface_cache_drop pays on eviction as it always has" -- but this
     * is not surface_cache_drop, it is the LRU pick inside the rebuild,
     * and it overwrote the slot without looking at owes_guest_ram. The
     * texture is then released with the only copy of whatever was
     * rendered or cleared into it while it was unbound. Latent today at
     * one eviction a run, and the first thing deferring every swap would
     * have turned into lost pixels. */
    if(surf_slot[pick].valid&&surf_slot[pick].owes_guest_ram){
     surface_slot_writeback(pick);++g_debt_paid_on_evict;}
    if(surf_slot[pick].valid)++surface_evictions;
    surf_slot[pick].target=target;surf_slot[pick].target_size=target_size;
    surf_slot[pick].w=surface_width;surf_slot[pick].h=surface_height;
    surf_slot[pick].pitch=surface_pitch;
    surf_slot[pick].depth=next_depth;surf_slot[pick].depth_pitch=next_depth_pitch;
    surf_slot[pick].depth_size=next_depth_size;
    surf_slot[pick].colour=surface;surf_slot[pick].stencil=stencil_surface;
    surf_slot[pick].hw_depth=hw_depth_tex;surf_slot[pick].hw_stencil=hw_stencil_tex;
    surf_slot[pick].stamp=++surf_clock;surf_slot[pick].valid=1;}
   }}
  /* The hardware-state path attaches real depth and stencil buffers and drops
   * the second colour attachment the software path used to carry stencil in.
   * Chosen per draw rather than per surface because a state this path cannot
   * translate falls back, and a fallback draw needs the old descriptor. */
  int hw = hw_state_on() && hw_depth_tex && hw_stencil_tex;
  /* A generated program has its own vertex function, so it needs its own
   * pipeline; hw_pipeline_for's cache is keyed on blend state alone and would
   * hand this draw the fixed `vs`. A program that cannot get a pipeline is a
   * CPU draw, not a wrong one -- but the executor has already skipped the
   * interpreter by then, so this refuses the DRAW rather than silently using
   * the wrong vertex stage. */
  /* AND THIS IS THE LINE THAT DID NOT DO THAT.
   *
   * It used to read `if (vsh_gpu_active && !hw) { vsh_gpu_active = 0;
   * vsh_active = NULL; }` -- clear the flag and carry on. But the flag was
   * read a hundred and thirty lines above, to decide the SHAPE of the vertex
   * buffer: with a generated program the batch is packed as the program's
   * inputs, nattrs float4 per vertex in ascending attribute order, and
   * without one it is packed as the fixed `vs`'s 112-byte Vertex. Clearing
   * the flag here does not repack anything. It binds a raw attribute stream
   * to a vertex function that reads it as Vertex, at buffer(0), with the
   * index buffer moved from binding 3 to binding 2 as well -- so the draw
   * takes its positions out of whatever the stride mismatch lands on and
   * reads past the end of its own reservation into the next batch's
   * vertices. That is the same picture as a staging-ring hazard and it is
   * not one; it is a binding the encoder was never told about.
   *
   * The comment directly above already said what the rule is -- refuse the
   * draw rather than silently use the wrong vertex stage -- so this is the
   * comment being implemented rather than a new policy. reject() is the
   * counted route the executor already handles.
   *
   * NOT OBSERVED, and the counter says so rather than an argument: a 150 s
   * gameplay run with RECOMP_METAL_FF=1 reports `vsh draws: 311949 GPU, 0
   * CPU`, and the zero is this branch -- every draw that falls out of the
   * generated-program path increments g_vsh_cpu_draws. The depth and stencil
   * textures existed for all 311,949 of them. The repair is for the case
   * where they do not, which is an allocation failure away. */
  if (vsh_gpu_active && !hw) { vsh_active = NULL; return reject("hw-lost-under-program"); }
  /* The fragment tail is asked for ONCE per draw -- hw_early_z counts as it
   * answers -- and handed to both lookups. The combiner-specialised pipeline
   * is tried first; nil from it (switch off, cache full, compile failed) is
   * the generic interpreter, exactly as before. */
  uint32_t hw_sblend = hw ? (uint32_t)hw_shader_blend(s) : 0u;
  uint32_t hw_early = hw_sblend ? (uint32_t)hw_early_z(s) : 0u;   /* 0, 1 or 2: && would fold 2 into 1 */
  id<MTLRenderPipelineState> hw_pso_use =
      hw ? spec_pipeline_for(s, hwmask, vsh_gpu_active ? vsh_active : NULL, hw_sblend, hw_early) : nil;
  if (!hw_pso_use)
    hw_pso_use = vsh_gpu_active ? vsh_pipeline_for(s, vsh_active, hw_sblend, hw_early)
                                : (hw ? hw_pipeline_for(s, hw_sblend, hw_early) : nil);
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
  if(mtl_cb_stats())g_mtl_ns_state+=mtl_now_ns()-_tf;
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
    if(pass_fence_on()&&g_pass_fence)
     [batch_encoder waitForFence:g_pass_fence beforeStages:MTLRenderStageVertex];
    if(mtl_cb_stats())g_mtl_cbufs++;
   }
   command=batch_command;encoder=batch_encoder;
  }else{
   command=[queue commandBuffer];encoder=command?[command renderCommandEncoderWithDescriptor:pass]:nil;
   if(!command||!encoder)return reject("command-encoder");
   if(pass_fence_on()&&g_pass_fence)
    [encoder waitForFence:g_pass_fence beforeStages:MTLRenderStageVertex];
   if(mtl_cb_stats())g_mtl_cbufs++;
  }
  if(mtl_cb_stats()){g_mtl_ns_create+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
  Params p={0};p.width=s->clip_w;p.height=s->clip_h;p.dither=s->dither;p.untextured=s->untextured;p.combiner_count=s->combiner_count;p.texture_mask=s->texture_mask;p.add_specular=s->add_specular;p.alpha_test=s->alpha_test;p.alpha_ref=s->alpha_ref;
  /* RECOMP_METAL_NO_ALPHA_TEST=1 -- A DIAGNOSTIC ARM, renders incorrectly.
   *
   * A wire fence is alpha-cutout geometry: a mesh texture whose holes are
   * transparent. That matters for a defect we have measured elsewhere -- a
   * triangle whose texture coordinate q is <= 0 after transform collapses to
   * sampling ONE clamped corner texel across its whole area. On opaque
   * geometry that is a flat-coloured object: wrong, but visible, and nobody
   * files a bug. On a cutout texture, if that single texel sits in a hole its
   * alpha is 0, every fragment fails the test, and the object DISAPPEARS.
   * Transparency is what turns "slightly wrong" into "gone", which is why the
   * fences are what the player notices.
   *
   * Forcing the test off distinguishes the two outcomes in one run without
   * touching the MSL string (a subtly wrong shader expression compiles fine
   * and outputs black; that has cost two broken builds here):
   *   fence APPEARS, flat or garbage -> it is being alpha-discarded, and the
   *                                     bug is upstream in the coordinate
   *   fence STILL MISSING            -> the alpha test is not what hides it
   * Pair it with RECOMP_FB_DUMP=<prefix> and look at the frames. */
  if(no_alpha_test_on())p.alpha_test=0;p.frag_force=frag_force_mode();p.modulate=s->modulate;p.blend=s->blend;p.blend_src=s->blend_src;p.blend_dst=s->blend_dst;p.depth_test=s->depth_test;p.depth_write=s->depth_test&&s->depth_write;p.depth_func=s->depth_func;p.z_cull=legacy_zclamp_on()?0u:s->z_cull;p.z_lo=s->z_clip_min;p.z_hi=s->z_clip_max;p.stencil_test=s->stencil_test;p.stencil_write=s->stencil_write;p.stencil_mask=s->stencil_mask;p.stencil_ref=s->stencil_ref;p.stencil_func_mask=s->stencil_func_mask;p.stencil_func=s->stencil_func;p.stencil_fail=s->stencil_fail;p.stencil_zfail=s->stencil_zfail;p.stencil_zpass=s->stencil_zpass;for(unsigned u=0;u<4;u++)if(s->texture_mask&(1u<<u)){const NV2ATextureCopy*t=u?&s->extra_stages[u-1]:s;p.tw[u]=t->width;p.th[u]=t->height;p.pitch[u]=t->pitch;p.linear[u]=t->linear;p.rgba8[u]=t->rgba8;p.dxt1[u]=t->dxt1;p.dxt3[u]=t->dxt3;p.repeat[u]=t->repeat;p.levels[u]=t->levels;p.min_filter[u]=t->min_filter;p.lod_bias[u]=t->lod_bias;}memcpy(p.color_icw,s->color_icw,sizeof(p.color_icw));memcpy(p.alpha_icw,s->alpha_icw,sizeof(p.alpha_icw));memcpy(p.color_ocw,s->color_ocw,sizeof(p.color_ocw));memcpy(p.alpha_ocw,s->alpha_ocw,sizeof(p.alpha_ocw));memcpy(p.const0,s->const0,sizeof(p.const0));memcpy(p.const1,s->const1,sizeof(p.const1));for(unsigned u=0;u<4;u++)p.hw[u]=(hwmask>>u)&1u;
  p.ez_proven=(uint32_t)(early_z_exact_mode()==2&&s->alpha_test&&s->alpha_ref==0&&hw_zcull_cannot_fire(s)
                         &&g_draw_alpha_floor>=1.0f/255.0f);
  if(early_z_exact_mode()==3)p.ez_proven=(uint32_t)(s->alpha_test!=0);   /* the audit's positive control */
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
         [encoder setStencilReferenceValue:(uint32_t)(s->stencil_ref&255)];++g_hw_draws;
         /* WHICH VARIANT THIS DRAW GOT, counted where the draw happens rather
          * than where the pipeline is cached: a cache hit does not re-ask, so
          * counting inside hw_early_z would measure pipeline misses and be
          * read as draws. early=0 with the switch on is not a failure -- it
          * is the alpha-tested and z-culled geometry, which is the number
          * that says how much of the scene the change can reach at all.
          *
          * The test is the pipeline key's test, character for character,
          * including the shader-blend term: fs_hw_early is the variant that
          * reads the colour attachment, so under a shader-blend mode that
          * does not take every draw there is no early variant to select and
          * a counter that said otherwise would be counting an arm that did
          * not run. */
         {int ez=hw_shader_blend(s)?hw_early_z(s):0;
          if(ez)++g_early_z_draws;else++g_late_z_draws;
          if(ez==2)++g_early_nw_draws;
          if(!ez&&alpha_census_on()&&s->alpha_test&&s->depth_test&&s->depth_write)alpha_census_note(s);}}
  else {if(hw_state_on())++g_hw_mixed;[encoder setRenderPipelineState:pipeline];}if(vb)[encoder setVertexBuffer:vb offset:vb_offset atIndex:0];else[encoder setVertexBytes:v length:vertex_bytes atIndex:0];
  /* THE TWO PIPELINES HAVE INCOMPATIBLE VERTEX BINDINGS AND THE ENCODER MUST
   * NOT SET BOTH. The fixed `vs` reads Params at vertex 1 and the indices at
   * 2; vs_gpu reads the guest's 192 float4 constant file at 1, the viewport at
   * 2 and the indices at 3. Getting this wrong does not fail -- the shader
   * reads whatever is bound and draws geometry from it. Params stays at
   * FRAGMENT 1 in both, which is a separate namespace and is untouched. */
  if(vsh_gpu_active){
   /* THE GPU DOES THE CULLING NOW. NV097_SET_CULL_FACE is 0x404 front, 0x405
    * back, 0x408 both; front_cw says which winding the guest calls front.
    * 0x408 has no Metal equivalent -- MTLCullMode has no "cull everything" --
    * and the draw is simply skipped, which is what culling both faces means.
    *
    * THE WINDING SENSE IS NOT DERIVED, IT IS MEASURED. The guest's area is
    * taken in screen space with Y down; the emitted vertex program hands Metal
    * a clip position, and whether the screen-to-clip step flips the sign of a
    * triangle's winding is exactly the kind of thing this project has got
    * wrong from reading before -- the B5G6R5 channel order cost a measurement
    * that way. RECOMP_METAL_VSH_WINDING=1 flips it, so one binary settles it
    * by eye in two runs instead of an argument. */
   {static int flip=-1; if(flip<0) flip=recomp_switch_on("RECOMP_METAL_VSH_WINDING");
    int cw = s->front_cw ? 1 : 0; if(flip) cw = !cw;
    [encoder setFrontFacingWinding:cw?MTLWindingClockwise:MTLWindingCounterClockwise];
    [encoder setCullMode:s->cull_face==0x404?MTLCullModeFront
                        :s->cull_face==0x405?MTLCullModeBack:MTLCullModeNone];}
   [encoder setVertexBytes:vsh_constants length:192*16 atIndex:1];
   {struct { float width, height, depth; } vp =
      { (float)s->clip_w, (float)s->clip_h, 16777215.0f };
    [encoder setVertexBytes:&vp length:sizeof vp atIndex:2];}
   if(ib)[encoder setVertexBuffer:ib offset:ib_offset atIndex:3];
   else  [encoder setVertexBytes:indices length:index_bytes atIndex:3];
   ++g_vsh_gpu_draws; g_vsh_gpu_vertices += count;
  } else {
   [encoder setVertexBytes:&p length:sizeof(p) atIndex:1];
   if(ib)[encoder setVertexBuffer:ib offset:ib_offset atIndex:2];
   else  [encoder setVertexBytes:indices length:index_bytes atIndex:2];
   ++g_vsh_cpu_draws;
  }[encoder setFragmentBuffer:tb[0] offset:0 atIndex:0];[encoder setFragmentBuffer:tb[1] offset:0 atIndex:2];[encoder setFragmentBuffer:tb[2] offset:0 atIndex:3];[encoder setFragmentBuffer:tb[3] offset:0 atIndex:4];[encoder setFragmentBytes:&p length:sizeof(p) atIndex:1];
  hw_bind_defaults();
  if(!g_ez_violation_buf){uint32_t z=0;g_ez_violation_buf=[device newBufferWithBytes:&z length:4 options:MTLResourceStorageModeShared];}
  [encoder setFragmentBuffer:g_ez_violation_buf offset:0 atIndex:5];
  for(unsigned u=0;u<4;u++){[encoder setFragmentTexture:ht[u]?ht[u]:hw_dummy_texture atIndex:u];
   [encoder setFragmentSamplerState:hs[u]?hs[u]:hw_default_sampler atIndex:u];}
  {/* Per draw, because the encoder is shared across the batch and the
    * previous draw's rectangle would otherwise stay in force. */
   MTLScissorRect sc;sc.x=wc_x0;sc.y=wc_y0;
   sc.width=(wc_x1<s->clip_w?wc_x1:s->clip_w-1)-wc_x0+1;
   sc.height=(wc_y1<s->clip_h?wc_y1:s->clip_h-1)-wc_y0+1;
   [encoder setScissorRect:sc];if(sc.width<s->clip_w||sc.height<s->clip_h)++g_scissored_draws;}
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:n];
  if(batch_on()){
   if(pinned_slab>=0)batch_pins|=1u<<(unsigned)pinned_slab;
   ++batch_draws;
   if(mtl_cb_stats()){g_mtl_ns_encode+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
   if(batch_cap()&&batch_draws>=batch_cap())batch_flush();
   if(mtl_cb_stats())g_mtl_ns_commit+=mtl_now_ns()-_t0;
  }else{
   if(pass_fence_on()&&g_pass_fence){
    [encoder updateFence:g_pass_fence afterStages:MTLRenderStageFragment];++g_fence_waits;}
   [encoder endEncoding];if(mtl_cb_stats()){g_mtl_ns_encode+=mtl_now_ns()-_t0;_t0=mtl_now_ns();}
   if(pinned_slab>=0)ring_pin(command,(unsigned)pinned_slab);
   mtl_cb_gpu_watch(command);
   [command commit];if(mtl_cb_stats())g_mtl_ns_commit+=mtl_now_ns()-_t0;last_command=command;
  }
  surface_dirty=1;if((s->depth_test&&s->depth_write)||(s->stencil_test&&s->stencil_write))depth_dirty=1;reject_reason=NULL;return(int)(n/3);}
}
