/* Does the emitted MSL COMPUTE what nv2a_vsh_execute computes?
 *
 * This is the question vsh_msl_test.c and vsh_msl_compile_test.m both leave
 * open, and they say so: one compares emitted text against expected text, the
 * other compiles that text and builds a pipeline. Neither has ever run the
 * shader. Until this file, the header of nv2a_vsh_msl.c was accurate --
 * "nothing in this file has been run against a real draw" -- and every plan
 * that moves the vertex stage onto the GPU rests on an emitter nobody had
 * checked.
 *
 * WHAT IT DOES. For each program in a corpus: emit MSL with the production
 * emitter, turn the emitted vertex function into a compute kernel (see
 * "THE SURGERY" below), run it on a real device over N input vectors, run
 * nv2a_vsh_execute over the same vectors on the CPU, and compare the sixteen
 * output registers component by component.
 *
 * THE SURGERY, and exactly what it changes. Metal has no transform feedback:
 * a vertex function's outputs cannot be read back without rasterising, and
 * rasterising would compare through interpolation, clipping and a viewport --
 * three more things to be wrong about. So the emitted function is rewritten
 * into a kernel. The rewrite touches only DECORATION:
 *
 *   [[attribute(N)]] [[stage_in]] [[buffer(N)]] [[position]] [[point_size]]
 *                                        all erased
 *   "vertex VS_OUT vsh_main("   ->   "static void vsh_body(thread float4 *out, "
 *
 * and then, in RAW mode, the emitted text is TRUNCATED at the screen-space
 * fixup and given an epilogue that stores the eleven output registers. Every
 * character of the per-instruction body -- the part the emitter generates, the
 * part under test -- is compiled exactly as emitted. Each replacement is
 * checked and a miss fails the test, so if the emitter's shape changes this
 * stops rather than silently testing something else.
 *
 * FULL mode keeps the emitted epilogue (the subpixel snap and the screen ->
 * clip transform) and compares it against a C transcription of what
 * prepare_vertices() and the fixed `vs` in nv2a_metal.m do. That is a weaker
 * claim: it says the emitted tail matches THIS READING of those two, not that
 * the reading is right. It is still worth running -- it is the only thing that
 * would catch a transposed viewport divide.
 *
 * MATH MODE. Safe, because nv2a_metal.m's initialize() sets
 * MTLMathModeSafe / fastMathEnabled=NO for the shader the renderer actually
 * uses. Testing under fast math would measure a compiler this project does not
 * run.
 *
 * NOT A CTEST CASE, for the reason stated beside jsrf_vsh_msl_compile_test in
 * CMakeLists.txt: it needs a device, and a headless build machine has none.
 * (The neighbouring Metal tests are all in that group.) Run it explicitly:
 *
 *     JSRF_GAME_DIR=".../Jet Set Radio Future (US)" \
 *       build/jsrf_vsh_msl_diff_test
 *
 * Without JSRF_GAME_DIR it still runs, on the synthetic corpus and the single
 * captured program, and says so in its first line. It reports and exits 0 with
 * no device, because "no GPU here" is not evidence about the emitter.
 *
 * POSITIVE CONTROLS, because every number below is an absence-measurement and
 * "0 mismatches" has to be distinguishable from a dead instrument:
 *
 *   1. injected fault   one program is compiled a second time with
 *                       "oPos.x += 1" spliced in front of the epilogue. If
 *                       that does not report mismatches, the comparison is not
 *                       comparing and every zero above it is worthless.
 *   2. corpus anchor    the program in vsh_capture.h, captured from a running
 *                       frame by RECOMP_VSH_TRACE long before this scan
 *                       existed, must be found in the XBE by the scan.
 *   3. non-zero work    compared-vector counts are printed per opcode; an
 *                       opcode with compared=0 is reported as UNTESTED, never
 *                       as passing.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "nv2a_vsh.h"
#include "vsh_capture.h"
#include "vsh_encode.h"
#include "vsh_xbe_corpus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vectors per dispatch, and how many distinct constant files each program is
 * run against. The constant file is a `constant float4 *` argument -- one per
 * dispatch, shared by every thread -- so varying it means more dispatches, not
 * more threads. 4 x 16 = 64 vectors per program. */
#define NVERT   16
#define NCONST  4

/* The pass threshold, set from the measurement rather than chosen in advance:
 * see the "worst error" line the run prints. Errors are scored as absolute
 * below 1 and relative above it, so a near-zero result is not judged against
 * its own magnitude. A real emitter defect -- a swapped operand, a dropped
 * negate, a wrong swizzle channel -- lands orders of magnitude above this;
 * anything near it is fused-multiply-add and rsqrt refinement. */
#define TOLERANCE 2.0e-5

static int failures;
static int g_verbose;

/* ================================================================
 * Per-opcode accounting
 * ================================================================ */

static const char *g_mac_names[NV2A_VSH_MAC_COUNT] = {
    "NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4","DST","MIN","MAX","SLT","SGE","ARL"
};
static const char *g_ilu_names[NV2A_VSH_ILU_COUNT] = {
    "NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT"
};

typedef struct {
    unsigned long programs, vectors, compared, refused, mismatch;
    /* The taxonomy of a mismatch, because "73 mismatches" names no cause.
     *   nonfinite  one side inf/NaN and the other finite -- the NV2A
     *              multiply-by-zero rule the emitter deliberately does not
     *              reproduce (see nv2a_vsh_msl.c's MAC_MUL comment);
     *   illcond    one ulp on the operands moves the INTERPRETER's own answer
     *              at least as far as the gap (see ill_conditioned);
     *   residual   neither. This is the only column that is about the
     *              emitter, and the only one allowed to fail a run. */
    unsigned long nonfinite, illcond, residual;
    double max_err, max_residual_err;
} OpStat;

static OpStat g_mac_syn[NV2A_VSH_MAC_COUNT], g_ilu_syn[NV2A_VSH_ILU_COUNT];
static unsigned long g_mac_corpus_insns[NV2A_VSH_MAC_COUNT];
static unsigned long g_ilu_corpus_insns[NV2A_VSH_ILU_COUNT];
static unsigned long g_mac_corpus_bad[NV2A_VSH_MAC_COUNT];
static unsigned long g_ilu_corpus_bad[NV2A_VSH_ILU_COUNT];

static OpStat g_corpus_total, g_syn_total, g_full_total;

static void stat_merge(OpStat *d, const OpStat *s)
{
    d->programs += s->programs; d->vectors += s->vectors;
    d->compared += s->compared; d->refused += s->refused;
    d->mismatch += s->mismatch;
    d->nonfinite += s->nonfinite; d->illcond += s->illcond;
    d->residual += s->residual;
    if (s->max_err > d->max_err) d->max_err = s->max_err;
    if (s->max_residual_err > d->max_residual_err)
        d->max_residual_err = s->max_residual_err;
}

/* ================================================================
 * Deterministic operand data
 * ================================================================ */

static uint32_t rng_state;
static void rng_seed(uint32_t s) { rng_state = s ? s : 1u; }
static uint32_t rng_next(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;  return rng_state;
}
/* Operands in [-1.5, 1.5] with the odd exact 0 and 1.
 *
 * NOT arbitrary: the range is chosen so ARL of any component lands in
 * {-2,-1,0,1}, which keeps a0-relative constant reads inside the 192-entry
 * bank for most base indices. The interpreter REFUSES a draw whose relative
 * index leaves the bank (read_source returns 0) where the emitted MSL clamps,
 * so a vector that leaves the bank produces no comparison at all -- it is
 * counted as refused and reported, not silently dropped. Exact 0 and 1 are
 * seeded deliberately: they are where multiply()'s zero-suppression, step()'s
 * tie and LIT's `src.x > 0` branch all change behaviour. */
static float rng_float(void)
{
    uint32_t r = rng_next();
    switch (r & 31u) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    default: return ((float)(r >> 8) / (float)(1u << 24)) * 3.0f - 1.5f;
    }
}

/* ================================================================
 * MSL surgery
 * ================================================================ */

static int msl_strip_attrs(char *s, const char *prefix)
{
    char needle[64];
    int n = 0;
    snprintf(needle, sizeof needle, " [[%s", prefix);
    for (;;) {
        char *at = strstr(s, needle), *end;
        if (!at) break;
        end = strstr(at, "]]");
        if (!end) break;
        memmove(at, end + 2, strlen(end + 2) + 1);
        ++n;
    }
    return n;
}

static int msl_replace_once(char *s, size_t cap, const char *from, const char *to)
{
    char *at = strstr(s, from);
    size_t flen = strlen(from), tlen = strlen(to), tail;
    if (!at) return 0;
    tail = strlen(at + flen);
    if (strlen(s) - flen + tlen + 1 > cap) return 0;
    memmove(at + tlen, at + flen, tail + 1);
    memcpy(at, to, tlen);
    return 1;
}

static const char *const FIXUP_MARKER = "    /* Screen-space fixup";

/* out[] is indexed by NV2AVshOutputReg, so it lines up with
 * NV2AVshResult::output and the comparison needs no mapping table. 1, 2 and
 * 13..15 have no output register and stay zero on both sides. */
static const char *const RAW_EPILOGUE =
    "    out[0]=oPos; out[3]=oD0; out[4]=oD1; out[5]=oFog; out[6]=oPts;\n"
    "    out[7]=oB0; out[8]=oB1; out[9]=oT0; out[10]=oT1; out[11]=oT2; out[12]=oT3;\n"
    "}\n";

/* The same eleven registers, but read back out of VS_OUT after the emitted
 * epilogue has run. oFog and oPts are scalars in VS_OUT; their other
 * components are not compared in this mode. */
static const char *const FULL_STORES =
    "    out[0]=o.oPos; out[3]=o.oD0; out[4]=o.oD1; out[5]=float4(o.oFog,0,0,0);\n"
    "    out[6]=float4(o.oPts,0,0,0); out[7]=o.oB0; out[8]=o.oB1;\n"
    "    out[9]=o.oT0; out[10]=o.oT1; out[11]=o.oT2; out[12]=o.oT3;\n";

/* Build the compute source. `mode` 0 = RAW (truncate at the fixup), 1 = FULL
 * (keep it). `inject` splices a known-wrong statement in front of the epilogue
 * -- the positive control. Returns 0 and explains if any required replacement
 * was not found, which is how this notices the emitter changing shape. */
static int build_kernel_source(const NV2AVshProgram *program, int mode,
                               int inject, char *out, size_t cap,
                               const char *label)
{
    static char msl[262144];
    int n = nv2a_vsh_generate_msl(program, msl, sizeof msl);
    char *marker;
    size_t used;
    int i;

    if (n <= 0) {
        fprintf(stderr, "[VSH-DIFF] %-28s EMITTER REFUSED (returned %d)\n", label, n);
        return 0;
    }

    msl_strip_attrs(msl, "attribute(");
    msl_strip_attrs(msl, "stage_in");
    msl_strip_attrs(msl, "buffer(");
    msl_strip_attrs(msl, "position");
    msl_strip_attrs(msl, "point_size");

    if (!msl_replace_once(msl, sizeof msl, "vertex VS_OUT vsh_main(",
                          "static void vsh_body(thread float4 *out, ")) {
        fprintf(stderr, "[VSH-DIFF] %-28s the emitter no longer opens vsh_main the way "
                        "this test rewrites it; the rewrite, not the emitter, is stale\n", label);
        return 0;
    }

    marker = strstr(msl, FIXUP_MARKER);
    if (!marker) {
        fprintf(stderr, "[VSH-DIFF] %-28s emitted text has no screen-space fixup marker\n", label);
        return 0;
    }
    if (mode == 0) {
        *marker = '\0';
        if (inject) strcat(msl, "    oPos.x += 1.0f;\n");
        strcat(msl, RAW_EPILOGUE);
    } else {
        if (inject) {
            /* In FULL mode the injected fault has to land before the fixup
             * reads oPos, or the fixup would hide it. */
            char head[262144];
            size_t head_len = (size_t)(marker - msl);
            memcpy(head, msl, head_len); head[head_len] = '\0';
            strcat(head, "    oPos.x += 1.0f;\n");
            strcat(head, marker);
            memcpy(msl, head, strlen(head) + 1);
        }
        if (!msl_replace_once(msl, sizeof msl, "    return o;\n", FULL_STORES)) {
            fprintf(stderr, "[VSH-DIFF] %-28s no `return o;` to replace\n", label);
            return 0;
        }
    }

    used = (size_t)snprintf(out, cap, "%s\n", msl);
    if (used >= cap) return 0;
    used += (size_t)snprintf(out + used, cap - used,
        "kernel void vsh_diff(device float4 *outbuf [[buffer(0)]],\n"
        "                     device const float4 *vin [[buffer(1)]],\n"
        "                     constant float4 *c [[buffer(2)]],\n"
        "                     constant VSH_Viewport &vp [[buffer(3)]],\n"
        "                     uint gid [[thread_position_in_grid]]) {\n"
        "    float4 o[16];\n"
        "    for (int i = 0; i < 16; ++i) o[i] = float4(0,0,0,0);\n");
    if (used >= cap) return 0;
    if (program->inputs_read) {
        used += (size_t)snprintf(out + used, cap - used, "    VS_IN vi;\n");
        for (i = 0; i < NV2A_VS_MAX_INPUTS; ++i)
            if (program->inputs_read & (1u << i)) {
                used += (size_t)snprintf(out + used, cap - used,
                                         "    vi.v%d = vin[gid*16+%d];\n", i, i);
                if (used >= cap) return 0;
            }
        used += (size_t)snprintf(out + used, cap - used, "    vsh_body(o, vi, c, vp);\n");
    } else {
        used += (size_t)snprintf(out + used, cap - used, "    vsh_body(o, c, vp);\n");
    }
    if (used >= cap) return 0;
    used += (size_t)snprintf(out + used, cap - used,
        "    for (int i = 0; i < 16; ++i) outbuf[gid*16+i] = o[i];\n}\n");
    return used < cap;
}

/* ================================================================
 * The CPU side of FULL mode
 * ================================================================ */

/* A transcription, by reading, of the two pieces of fixed work the emitted
 * epilogue absorbed:
 *   prepare_vertices() in src/kernel/nv2a_pb_exec.c -- the subpixel snap;
 *   the fixed `vs` in src/nv2a/nv2a_metal.m         -- screen to clip.
 * Deliberately separate from nv2a_vsh_execute, which knows nothing about
 * either. See the file header for why agreement here is the weaker claim. */
static void cpu_fixup(const NV2AVshResult *r, float vp_w, float vp_h, float vp_d,
                      float out[16][4])
{
    float p[4], k;
    int i;
    for (i = 0; i < 16; ++i) memcpy(out[i], r->output[i], sizeof(float) * 4);
    memcpy(p, r->output[0], sizeof p);
    for (k = 0, i = 0; i < 2; ++i)
        if (fabsf(p[i]) < 0x1p20f) p[i] = truncf(p[i] * 16.0f) / 16.0f;
    (void)k;
    out[0][0] = (p[0] / vp_w * 2.0f - 1.0f) * p[3];
    out[0][1] = (1.0f - p[1] / vp_h * 2.0f) * p[3];
    out[0][2] = (p[2] / vp_d) * p[3];
    out[0][3] = p[3];
    for (i = 0; i < 4; ++i) {
        out[NV2A_VSH_OUT_D0][i] = fminf(fmaxf(out[NV2A_VSH_OUT_D0][i], 0.0f), 1.0f);
        out[NV2A_VSH_OUT_D1][i] = fminf(fmaxf(out[NV2A_VSH_OUT_D1][i], 0.0f), 1.0f);
        out[NV2A_VSH_OUT_B0][i] = fminf(fmaxf(out[NV2A_VSH_OUT_B0][i], 0.0f), 1.0f);
        out[NV2A_VSH_OUT_B1][i] = fminf(fmaxf(out[NV2A_VSH_OUT_B1][i], 0.0f), 1.0f);
    }
    /* VS_OUT carries fog and point size as scalars. */
    out[NV2A_VSH_OUT_FOG][1] = out[NV2A_VSH_OUT_FOG][2] = out[NV2A_VSH_OUT_FOG][3] = 0.0f;
    out[NV2A_VSH_OUT_PTS][1] = out[NV2A_VSH_OUT_PTS][2] = out[NV2A_VSH_OUT_PTS][3] = 0.0f;
}

/* ================================================================
 * Comparison
 * ================================================================ */

/* IS THIS VECTOR ABLE TO DECIDE ANYTHING?
 *
 * A disagreement between two float implementations of the same program is
 * evidence about the implementations only where the program's answer is
 * determined by its inputs to better than the size of the disagreement. These
 * programs are frequently not: they are skinning and lighting shaders, and
 * they divide. NV2A's RCC saturates the reciprocal at 2^64, so an intermediate
 * that one side computes as exactly 0 and the other as 1e-20 -- a difference
 * of one denormal, entirely within float rounding -- comes back out of the
 * multiply that follows as a difference of 0.2 in clip space.
 *
 * This measures that directly instead of arguing about it. Perturb every
 * input and every constant by ONE ULP, run the INTERPRETER again, and see how
 * far the interpreter's own answer moves. If its own answer moves as far as
 * the CPU/GPU gap, the gap is the program's conditioning at that vector and
 * says nothing about the emitter. If it does not move, the gap is real and
 * the emitter is wrong.
 *
 * It is deliberately one-sided: it can excuse a mismatch, never manufacture
 * one. The injected-fault control is unaffected by it -- an added constant
 * survives any perturbation -- which is the check that this has not been
 * turned into a way of explaining everything away. */
static int ill_conditioned(const NV2AVshProgram *p, const float in[16][4],
                           const float c[192][4], int reg, int k,
                           float base, double gap)
{
    static float in2[16][4], c2[NV2A_VS_MAX_CONSTANTS][4];
    NV2AVshResult r;
    int dir, a, j;
    for (dir = 0; dir < 2; ++dir) {
        float to = dir ? -INFINITY : INFINITY;
        for (a = 0; a < 16; ++a)
            for (j = 0; j < 4; ++j) in2[a][j] = nextafterf(in[a][j], to);
        for (a = 0; a < NV2A_VS_MAX_CONSTANTS; ++a)
            for (j = 0; j < 4; ++j) c2[a][j] = nextafterf(c[a][j], to);
        if (!nv2a_vsh_execute(p, (const float (*)[4])in2,
                              (const float (*)[4])c2, &r))
            return 1;   /* one ulp turns it into a refusal: not decidable */
        {
            float v = r.output[reg][k];
            double move;
            if (isnan(v) != isnan(base) || isinf(v) != isinf(base)) return 1;
            if (!isfinite(v) || !isfinite(base)) continue;
            move = fabs((double)v - (double)base);
            if (move >= gap * 0.5) return 1;
        }
    }
    return 0;
}

static int close_enough(float a, float b, double *err)
{
    double d, m;
    *err = 0.0;
    if (isnan(a) && isnan(b)) return 1;
    if (a == b) return 1;
    if (isnan(a) || isnan(b) || isinf(a) || isinf(b)) { *err = INFINITY; return 0; }
    d = fabs((double)a - (double)b);
    m = fmax(fabs((double)a), fabs((double)b));
    *err = m > 1.0 ? d / m : d;
    return *err <= TOLERANCE;
}

/* ================================================================
 * Metal plumbing
 * ================================================================ */

typedef struct {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLBuffer> vin, out;
} Rig;

/* Which output-register slots both sides define. 1, 2 and 13..15 are not
 * output registers on the NV2A; nothing writes them and comparing them would
 * be comparing two zeroes. */
static const int g_out_slots[] = {0,3,4,5,6,7,8,9,10,11,12};
#define N_OUT_SLOTS ((int)(sizeof g_out_slots / sizeof g_out_slots[0]))

/* Run one program both ways. Returns 0 if the program could not be run at all
 * (emitter refusal, compile failure) -- distinct from running and disagreeing,
 * which is counted in `st`. */
static int compare_program(Rig *rig, const char *label,
                           const NV2AVshProgram *program,
                           int mode, int inject, uint32_t seed, OpStat *st)
{
    static char source[512 * 1024];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    id<MTLLibrary> library;
    id<MTLFunction> fn;
    id<MTLComputePipelineState> pso;
    float (*vin)[16][4];
    float (*gpu)[16][4];
    float constants[NCONST][NV2A_VS_MAX_CONSTANTS][4];
    const float vp[3] = { 640.0f, 480.0f, 16777215.0f };
    int set, v, s, k, reported = 0, reported_residual = 0;

    if (!build_kernel_source(program, mode, inject, source, sizeof source, label))
        return 0;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    /* Match nv2a_metal.m exactly: it compiles its shader with safe math, so a
     * result measured under fast math would not be about the renderer. */
    if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
    else options.fastMathEnabled = NO;
#pragma clang diagnostic pop

    library = [rig->device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                        options:options error:&error];
    if (!library) {
        fprintf(stderr, "[VSH-DIFF] %-28s COMPILE FAILED\n%s\n", label,
                error.description.UTF8String);
        if (g_verbose) fprintf(stderr, "----- source -----\n%s\n", source);
        ++failures;
        return 0;
    }
    fn = [library newFunctionWithName:@"vsh_diff"];
    pso = fn ? [rig->device newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (!pso) {
        fprintf(stderr, "[VSH-DIFF] %-28s PIPELINE FAILED: %s\n", label,
                error.description.UTF8String);
        ++failures;
        return 0;
    }

    vin = (float (*)[16][4])rig->vin.contents;
    gpu = (float (*)[16][4])rig->out.contents;
    rng_seed(seed);
    ++st->programs;

    for (set = 0; set < NCONST; ++set) {
        for (k = 0; k < NV2A_VS_MAX_CONSTANTS; ++k)
            for (s = 0; s < 4; ++s) constants[set][k][s] = rng_float();
        for (v = 0; v < NVERT; ++v)
            for (k = 0; k < 16; ++k)
                for (s = 0; s < 4; ++s) vin[v][k][s] = rng_float();
        memset(gpu, 0, (size_t)NVERT * 16 * 4 * sizeof(float));

        @autoreleasepool {
            id<MTLCommandBuffer> cb = [rig->queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:rig->out offset:0 atIndex:0];
            [enc setBuffer:rig->vin offset:0 atIndex:1];
            /* 192 float4 is 3072 bytes, inside Metal's 4 KB setBytes limit --
             * the same limit the emitter's file comment sizes the constant
             * file against, and the reason it is 192 and not 256. */
            [enc setBytes:constants[set] length:sizeof constants[set] atIndex:2];
            [enc setBytes:vp length:sizeof vp atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake(1,1,1)
                threadsPerThreadgroup:MTLSizeMake(NVERT,1,1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "[VSH-DIFF] %-28s DISPATCH FAILED: %s\n", label,
                        cb.error.description.UTF8String);
                ++failures;
                return 0;
            }
        }

        for (v = 0; v < NVERT; ++v) {
            NV2AVshResult ref;
            float expect[16][4];
            ++st->vectors;
            if (!nv2a_vsh_execute(program, (const float (*)[4])vin[v],
                                  (const float (*)[4])constants[set], &ref)) {
                /* The interpreter refused this vector. On the real path the
                 * whole DRAW is refused here; a vertex function cannot, and
                 * the emitted MSL clamps instead. There is nothing to compare,
                 * and pretending otherwise would score the clamp against a
                 * value that does not exist. */
                ++st->refused;
                continue;
            }
            if (mode == 0)
                memcpy(expect, ref.output, sizeof expect);
            else
                cpu_fixup(&ref, vp[0], vp[1], vp[2], expect);
            ++st->compared;
            for (s = 0; s < N_OUT_SLOTS; ++s) {
                int reg = g_out_slots[s];
                for (k = 0; k < 4; ++k) {
                    double err;
                    if (close_enough(expect[reg][k], gpu[v][reg][k], &err)) {
                        if (err > st->max_err) st->max_err = err;
                        continue;
                    }
                    ++st->mismatch;
                    if (err > st->max_err) st->max_err = err;
                    {
                        float a = expect[reg][k], b = gpu[v][reg][k];
                        const char *why;
                        if (isfinite(a) != isfinite(b)) { ++st->nonfinite; why = "nonfinite"; }
                        else if (mode == 0
                                 && ill_conditioned(program,
                                        (const float (*)[4])vin[v],
                                        (const float (*)[4])constants[set],
                                        reg, k, a, fabs((double)a - (double)b)))
                                { ++st->illcond; why = "ill-conditioned"; }
                        else { ++st->residual; why = "RESIDUAL";
                               if (err > st->max_residual_err) st->max_residual_err = err; }
                        /* Cap both classes. The injected-fault control
                         * mismatches on every vector, and 64 identical lines
                         * bury the corpus results underneath them. */
                        if (reported++ < 4
                            || (strcmp(why, "RESIDUAL") == 0 && reported_residual++ < 8))
                            fprintf(stderr, "[VSH-DIFF] %-28s set=%d v=%d o%d.%c "
                                            "cpu=%.9g gpu=%.9g err=%.3g  %s\n",
                                    label, set, v, reg, "xyzw"[k],
                                    (double)a, (double)b, err, why);
                    }
                    break; /* one report per register is enough */
                }
            }
        }
    }
    return 1;
}

/* ================================================================
 * Synthetic per-opcode corpus
 * ================================================================ */

/* One slot's worth of operand shape. The encoding shares `const_index` and
 * `input_index` across all three sources of a slot -- that is a hardware
 * constraint, not a simplification -- so a variant names one of each. */
typedef struct {
    unsigned a_mux, a_temp, a_swz, a_neg;
    unsigned b_mux, b_temp, b_swz, b_neg;
    unsigned c_mux, c_temp, c_swz, c_neg;
    unsigned input_index, const_index;
    unsigned mac_temp, mac_mask;
    unsigned out_reg, out_mask;
} Variant;

/* Eleven variants: every source bank in every position, identity/reversed/
 * splat swizzles, negation on each source in turn, partial write masks, and
 * one output register each so that all eleven are observable at once. The
 * temps R0-R4 are preloaded by the five MOV slots build_mac_program emits
 * first, so a program for opcode X still depends on MAC MOV being right --
 * which MOV's own program, whose variants read no temps until slot 5, does
 * not. */
static const Variant g_variants[] = {
 {2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 2,0,SWZ_ID,0, 0,1, 5,15, NV2A_VSH_OUT_D0, 15},
 {2,0,SWZ_ID,1, 3,0,SWZ_ID,0, 3,0,SWZ_ID,1, 2,2, 6,15, NV2A_VSH_OUT_D1, 15},
 {2,0,SWZ(3,2,1,0),0, 3,0,SWZ(1,1,3,3),0, 2,0,SWZ(2,2,2,2),0, 1,3, 7,15, NV2A_VSH_OUT_T0, 15},
 {3,0,SWZ_ID,0, 2,0,SWZ_ID,0, 3,0,SWZ(0,0,1,1),0, 4,4, 8,12, NV2A_VSH_OUT_T1, 12},
 {1,0,SWZ_ID,0, 1,1,SWZ_ID,0, 1,2,SWZ_ID,0, 0,0, 9,3, NV2A_VSH_OUT_T2, 3},
 {2,0,SWZ(0,0,0,0),0, 3,0,SWZ(3,3,3,3),1, 2,0,SWZ_ID,0, 5,6, 10,15, NV2A_VSH_OUT_T3, 15},
 {2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 1,3,SWZ_ID,0, 7,7, 11,15, NV2A_VSH_OUT_B0, 15},
 {1,4,SWZ(1,0,3,2),0, 2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 9,8, 0,10, NV2A_VSH_OUT_B1, 10},
 {2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 2,0,SWZ(3,3,3,3),0, 11,9, 1,15, NV2A_VSH_OUT_FOG, 4},
 {2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 3,0,SWZ_ID,0, 13,10, 2,15, NV2A_VSH_OUT_PTS, 8},
 {2,0,SWZ_ID,0, 3,0,SWZ_ID,0, 3,0,SWZ_ID,0, 15,11, 3,15, NV2A_VSH_OUT_POS, 15},
};
#define N_VARIANTS ((int)(sizeof g_variants / sizeof g_variants[0]))

static void apply_variant(VshIns *ins, const Variant *var)
{
    ins->a.mux = var->a_mux; ins->a.temp = var->a_temp;
    ins->a.swz = var->a_swz; ins->a.neg = var->a_neg;
    ins->b.mux = var->b_mux; ins->b.temp = var->b_temp;
    ins->b.swz = var->b_swz; ins->b.neg = var->b_neg;
    ins->c.mux = var->c_mux; ins->c.temp = var->c_temp;
    ins->c.swz = var->c_swz; ins->c.neg = var->c_neg;
    ins->input_index = var->input_index;
    ins->const_index = var->const_index;
    ins->mac_temp = var->mac_temp;
    ins->out_reg = var->out_reg;
    ins->out_mask = var->out_mask;
}

static int build_mac_program(NV2AVshMacOp op, VshIns *ins)
{
    int n = 0, i;
    /* Preload R0-R4 so the temp-source variants read something. */
    for (i = 0; i < 5; ++i) {
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = NV2A_VSH_MAC_MOV;
        ins[n].a.mux = (i & 1) ? 3u : 2u;
        ins[n].a.swz = SWZ_ID;
        ins[n].input_index = (unsigned)i;
        ins[n].const_index = (unsigned)(20 + i);
        ins[n].mac_temp = (unsigned)i;
        ins[n].mac_mask = 15;
        ++n;
    }
    for (i = 0; i < N_VARIANTS; ++i) {
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = (unsigned)op;
        apply_variant(&ins[n], &g_variants[i]);
        ins[n].mac_mask = g_variants[i].mac_mask;
        ++n;
    }
    ins[n-1].final = 1;
    return n;
}

static int build_ilu_program(NV2AVshIluOp op, VshIns *ins)
{
    int n = 0, i;
    for (i = 0; i < 5; ++i) {
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = NV2A_VSH_MAC_MOV;
        ins[n].a.mux = (i & 1) ? 3u : 2u;
        ins[n].a.swz = SWZ_ID;
        ins[n].input_index = (unsigned)i;
        ins[n].const_index = (unsigned)(20 + i);
        ins[n].mac_temp = (unsigned)i;
        ins[n].mac_mask = 15;
        ++n;
    }
    /* The ILU reads source C only, so the variants collapse to C's bank,
     * swizzle and sign. The swizzle matters more here than anywhere else: the
     * decoder scalarises C for RCP..LOG and does not for MOV and LIT, and the
     * emitter has to follow that split exactly. */
    for (i = 0; i < N_VARIANTS; ++i) {
        const Variant *var = &g_variants[i];
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].ilu = (unsigned)op;
        ins[n].c.mux = var->c_mux; ins[n].c.temp = var->c_temp;
        ins[n].c.swz = var->c_swz; ins[n].c.neg = var->c_neg;
        ins[n].input_index = var->input_index;
        ins[n].const_index = var->const_index;
        ins[n].mac_temp = var->mac_temp;   /* shared temp destination */
        ins[n].ilu_mask = var->mac_mask;
        ins[n].out_reg = var->out_reg;
        ins[n].out_mask = var->out_mask;
        ins[n].out_from_ilu = 1;           /* or the output write is attributed
                                            * to the NOP MAC and never happens */
        ++n;
    }
    ins[n-1].final = 1;
    return n;
}

/* ARL is not a value-producing opcode, so it cannot be tested by the variant
 * table: its whole effect is the address register, and the only way to observe
 * that is an a0-relative constant read. Base 64 with a0 in {-2..1} stays
 * inside the 192-entry bank for every input this test generates, so the
 * interpreter does not refuse and the comparison is real. */
static int build_arl_program(VshIns *ins)
{
    int n = 0, i;
    static const unsigned bases[] = { 64, 65, 0, 100, 191 };
    memset(&ins[n], 0, sizeof ins[n]);
    ins[n].mac = NV2A_VSH_MAC_MOV;
    ins[n].a.mux = 2; ins[n].a.swz = SWZ_ID; ins[n].input_index = 0;
    ins[n].mac_temp = 0; ins[n].mac_mask = 15; ++n;
    for (i = 0; i < (int)(sizeof bases / sizeof bases[0]); ++i) {
        /* a0 = floor(v1.<component i>) -- a different component each time, so
         * the scalar swizzle path is exercised, not just .x. */
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = NV2A_VSH_MAC_ARL;
        ins[n].a.mux = 2; ins[n].a.swz = SWZ(i & 3, i & 3, i & 3, i & 3);
        ins[n].input_index = 1;
        ++n;
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = NV2A_VSH_MAC_MOV;
        ins[n].a.mux = 3; ins[n].a.swz = SWZ_ID;
        ins[n].const_index = bases[i];
        ins[n].rel = 1;
        ins[n].mac_temp = (unsigned)(1 + i); ins[n].mac_mask = 15;
        ins[n].out_reg = (unsigned)(NV2A_VSH_OUT_T0 + (i % 4));
        ins[n].out_mask = 15;
        ++n;
    }
    memset(&ins[n], 0, sizeof ins[n]);
    ins[n].mac = NV2A_VSH_MAC_MOV;
    ins[n].a.mux = 1; ins[n].a.temp = 1; ins[n].a.swz = SWZ_ID;
    ins[n].mac_temp = 12; ins[n].mac_mask = 15;
    ins[n].out_reg = NV2A_VSH_OUT_POS; ins[n].out_mask = 15;
    ins[n].final = 1; ++n;
    return n;
}

/* One slot carrying a MAC and an ILU at once. The decoder forces the ILU's
 * temporary destination to R1 when a slot is paired, and suppresses the MAC's
 * temporary write if the MAC was also aiming at R1; both units read the
 * pre-write register state. Three rules, none of them obvious from the
 * emitted text, all of them only visible if a paired slot is executed. */
static int build_paired_program(VshIns *ins)
{
    int n = 0, i;
    static const unsigned macs[] = { NV2A_VSH_MAC_MOV, NV2A_VSH_MAC_MUL,
                                     NV2A_VSH_MAC_MAD, NV2A_VSH_MAC_DP4 };
    static const unsigned ilus[] = { NV2A_VSH_ILU_MOV, NV2A_VSH_ILU_RCP,
                                     NV2A_VSH_ILU_RSQ, NV2A_VSH_ILU_RCC };
    for (i = 0; i < 4; ++i) {
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = NV2A_VSH_MAC_MOV;
        ins[n].a.mux = 2; ins[n].a.swz = SWZ_ID; ins[n].input_index = (unsigned)i;
        ins[n].mac_temp = (unsigned)i; ins[n].mac_mask = 15;
        ++n;
    }
    for (i = 0; i < 4; ++i) {
        memset(&ins[n], 0, sizeof ins[n]);
        ins[n].mac = macs[i];
        ins[n].ilu = ilus[i];
        ins[n].a.mux = 2; ins[n].a.swz = SWZ_ID;
        ins[n].b.mux = 3; ins[n].b.swz = SWZ_ID;
        ins[n].c.mux = 1; ins[n].c.temp = 1; ins[n].c.swz = SWZ(i&3,i&3,i&3,i&3);
        ins[n].input_index = (unsigned)(4 + i);
        ins[n].const_index = (unsigned)(30 + i);
        ins[n].mac_temp = (i == 0) ? 1u : (unsigned)(5 + i); /* i==0 aims the MAC at R1 too */
        ins[n].mac_mask = 15;
        ins[n].ilu_mask = 15;
        ins[n].out_reg = (unsigned)(NV2A_VSH_OUT_T0 + i);
        ins[n].out_mask = 15;
        ins[n].out_from_ilu = (unsigned)(i & 1);
        ++n;
    }
    memset(&ins[n], 0, sizeof ins[n]);
    ins[n].mac = NV2A_VSH_MAC_MOV;
    ins[n].a.mux = 1; ins[n].a.temp = 1; ins[n].a.swz = SWZ_ID;
    ins[n].mac_temp = 12; ins[n].mac_mask = 15;
    ins[n].out_reg = NV2A_VSH_OUT_POS; ins[n].out_mask = 15;
    ins[n].final = 1; ++n;
    return n;
}

static int encode_and_parse(const VshIns *ins, int count, NV2AVshProgram *p,
                            const char *label)
{
    static uint32_t words[NV2A_VS_MAX_INSTRUCTIONS * 4];
    int i;
    for (i = 0; i < count; ++i) vsh_encode(words + i * 4, &ins[i]);
    if (!nv2a_vsh_parse(words, count, p) || !p->has_final) {
        fprintf(stderr, "[VSH-DIFF] %-28s FIXTURE DECODE FAILED (the encoder or "
                        "the fixture is wrong, not the emitter)\n", label);
        ++failures;
        return 0;
    }
    return 1;
}

/* ================================================================
 * Instruction-level localisation
 * ================================================================ */

/* WHICH INSTRUCTION FIRST DISAGREES.
 *
 * A mismatch on a thirty-instruction program names the program and nothing
 * else, and the opcode census below is presence, not attribution. This runs
 * the same program truncated after instruction k, for every k, and reports the
 * first k whose outputs disagree. Truncation is exact -- both the interpreter
 * and the emitter walk `length` instructions and neither looks at is_final for
 * anything but has_final -- so the prefix is a real program with the same
 * semantics up to that point.
 *
 * Temporary registers are not directly observable, so a defect in an
 * instruction that writes only a temp surfaces at the first later instruction
 * that carries it to an output. The report says so rather than pretending to
 * an instruction number it has not earned.
 *
 *     RECOMP_VSH_DIFF_BISECT=1F02BC build/jsrf_vsh_msl_diff_test
 */
static void bisect_program(Rig *rig, const NV2AVshProgram *program,
                           unsigned long offset, uint32_t seed)
{
    int k;
    printf("\n[VSH-DIFF] BISECT xbe@%08lX, %d instructions\n", offset, program->length);
    for (k = 1; k <= program->length; ++k) {
        NV2AVshProgram prefix = *program;
        OpStat st;
        char label[64];
        prefix.length = k;
        prefix.insns[k-1].is_final = 1;
        prefix.has_final = 1;
        memset(&st, 0, sizeof st);
        snprintf(label, sizeof label, "bisect k=%d", k);
        if (!compare_program(rig, label, &prefix, 0, 0, seed, &st)) continue;
        printf("  k=%-3d compared=%lu mismatch=%lu worst=%.4g%s\n",
               k, st.compared, st.mismatch, st.max_err,
               st.mismatch ? "   <== first disagreement is at or before "
                             "this instruction" : "");
        if (st.mismatch) break;
    }
}

/* ================================================================
 * main
 * ================================================================ */

static void report_opstats(const char *title, const char *const *names, int n,
                           const OpStat *st, const unsigned long *corpus_insns)
{
    int i;
    printf("\n%s\n", title);
    printf("  opcode  programs  vectors  compared  refused  mismatch   worst err  nonfin illcnd RESIDL   corpus insns\n");
    for (i = 0; i < n; ++i) {
        char err[32];
        if (st[i].compared == 0) snprintf(err, sizeof err, "  UNTESTED");
        else snprintf(err, sizeof err, "%10.3g", st[i].max_err);
        printf("  %-6s  %8lu  %7lu  %8lu  %7lu  %8lu  %s  %6lu %6lu %6lu   %10lu%s\n",
               names[i], st[i].programs, st[i].vectors, st[i].compared,
               st[i].refused, st[i].mismatch, err,
               st[i].nonfinite, st[i].illcond, st[i].residual,
               corpus_insns ? corpus_insns[i] : 0ul,
               corpus_insns && corpus_insns[i] == 0 ? "  (absent from corpus)" : "");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    g_verbose = getenv("RECOMP_VSH_DIFF_VERBOSE") != NULL;
    @autoreleasepool {
        Rig rig;
        char pathbuf[1024];
        const char *xbe;
        static VshXbeProgram corpus[VSH_XBE_MAX_PROGRAMS];
        VshXbeScan scan;
        int ncorpus = 0, i, anchor_found = 0;
        OpStat control_clean, control_injected;
        char label[64];

        rig.device = MTLCreateSystemDefaultDevice();
        if (!rig.device) {
            puts("[VSH-DIFF] no Metal device: nothing executed, nothing proved");
            return 0;
        }
        rig.queue = [rig.device newCommandQueue];
        rig.vin = [rig.device newBufferWithLength:NVERT * 16 * 4 * sizeof(float)
                                          options:MTLResourceStorageModeShared];
        rig.out = [rig.device newBufferWithLength:NVERT * 16 * 4 * sizeof(float)
                                          options:MTLResourceStorageModeShared];
        printf("[VSH-DIFF] device: %s\n", rig.device.name.UTF8String);
        printf("[VSH-DIFF] math mode: safe (as nv2a_metal.m compiles its own shader)\n");
        printf("[VSH-DIFF] tolerance: %.1e, scored absolute below 1 and relative above\n",
               TOLERANCE);
        printf("[VSH-DIFF] %d vectors per program (%d constant files x %d vertices)\n",
               NVERT * NCONST, NCONST, NVERT);

        /* ---- corpus 1: the title's own programs ---- */
        xbe = vsh_xbe_default_path(pathbuf, sizeof pathbuf);
        ncorpus = xbe ? vsh_xbe_scan(xbe, corpus, VSH_XBE_MAX_PROGRAMS, &scan) : -1;
        if (ncorpus <= 0) {
            printf("[VSH-DIFF] NO TITLE CORPUS: set JSRF_GAME_DIR or "
                   "JSRF_VSH_CORPUS_XBE. Everything below is synthetic, which is "
                   "the weak form of this test.\n");
        } else {
            printf("[VSH-DIFF] corpus: %d distinct programs (%d runs) from %s\n",
                   scan.count, scan.runs, scan.path);
            printf("[VSH-DIFF] corpus cross-check: %d of %d carry a matching "
                   "(slots<<16)|0x2078 length header; %d such headers in the file\n",
                   scan.header_agreements, scan.count, scan.headers_in_file);
            /* Positive control on the scan itself. */
            for (i = 0; i < ncorpus; ++i)
                if (corpus[i].length == 12
                    && !memcmp(corpus[i].words, jsrf_vsh_words, sizeof jsrf_vsh_words))
                    anchor_found = 1;
            if (anchor_found)
                printf("[VSH-DIFF] scan positive control: the program captured from a "
                       "running frame (vsh_capture.h) is in the scan output\n");
            else {
                fprintf(stderr, "[VSH-DIFF] scan positive control FAILED: the captured "
                                "program is not in the scan output, so the corpus is "
                                "not the title's programs\n");
                ++failures;
            }
        }

        /* ---- one program, instruction by instruction ---- */
        if (getenv("RECOMP_VSH_DIFF_BISECT")) {
            unsigned long want = strtoul(getenv("RECOMP_VSH_DIFF_BISECT"), NULL, 16);
            for (i = 0; i < ncorpus; ++i) {
                NV2AVshProgram p;
                if (corpus[i].offset != want) continue;
                if (!nv2a_vsh_parse(corpus[i].words, corpus[i].length, &p)) break;
                bisect_program(&rig, &p, corpus[i].offset, 0x5000u + (uint32_t)i);
                return failures ? 1 : 0;
            }
            fprintf(stderr, "[VSH-DIFF] no corpus program at offset %lX\n", want);
            return 1;
        }

        /* ---- the injected-fault control, before anything is believed ---- */
        memset(&control_clean, 0, sizeof control_clean);
        memset(&control_injected, 0, sizeof control_injected);
        {
            NV2AVshProgram captured;
            if (!nv2a_vsh_parse(jsrf_vsh_words, 12, &captured) || !captured.has_final) {
                fprintf(stderr, "[VSH-DIFF] captured program failed to decode\n");
                return 1;
            }
            compare_program(&rig, "control: captured clean", &captured, 0, 0, 0xC0FFEEu,
                            &control_clean);
            compare_program(&rig, "control: captured +1 oPos.x", &captured, 0, 1, 0xC0FFEEu,
                            &control_injected);
            printf("\n[VSH-DIFF] POSITIVE CONTROL\n");
            printf("  clean      compared=%lu mismatch=%lu worst=%.3g\n",
                   control_clean.compared, control_clean.mismatch, control_clean.max_err);
            printf("  +1 oPos.x  compared=%lu mismatch=%lu worst=%.3g\n",
                   control_injected.compared, control_injected.mismatch,
                   control_injected.max_err);
            if (control_injected.mismatch == 0 || control_injected.compared == 0) {
                fprintf(stderr, "[VSH-DIFF] POSITIVE CONTROL FAILED: a shader known to "
                                "be wrong compared equal. Every zero below is a dead "
                                "instrument, not a result.\n");
                ++failures;
            }
        }

        /* ---- corpus 2: one program per opcode ---- */
        {
            static VshIns ins[NV2A_VS_MAX_INSTRUCTIONS];
            NV2AVshProgram p;
            int n;
            for (i = 1; i < NV2A_VSH_MAC_COUNT; ++i) {
                snprintf(label, sizeof label, "MAC %s", g_mac_names[i]);
                n = (i == NV2A_VSH_MAC_ARL) ? build_arl_program(ins)
                                            : build_mac_program((NV2AVshMacOp)i, ins);
                if (!encode_and_parse(ins, n, &p, label)) continue;
                compare_program(&rig, label, &p, 0, 0, 0x1000u + (uint32_t)i,
                                &g_mac_syn[i]);
            }
            for (i = 1; i < NV2A_VSH_ILU_COUNT; ++i) {
                snprintf(label, sizeof label, "ILU %s", g_ilu_names[i]);
                n = build_ilu_program((NV2AVshIluOp)i, ins);
                if (!encode_and_parse(ins, n, &p, label)) continue;
                compare_program(&rig, label, &p, 0, 0, 0x2000u + (uint32_t)i,
                                &g_ilu_syn[i]);
            }
            n = build_paired_program(ins);
            if (encode_and_parse(ins, n, &p, "paired MAC+ILU slots")) {
                OpStat paired; memset(&paired, 0, sizeof paired);
                compare_program(&rig, "paired MAC+ILU slots", &p, 0, 0, 0x3000u, &paired);
                printf("\n[VSH-DIFF] paired MAC+ILU slots: compared=%lu mismatch=%lu "
                       "worst=%.3g\n", paired.compared, paired.mismatch, paired.max_err);
                stat_merge(&g_syn_total, &paired);
            }
        }
        for (i = 0; i < NV2A_VSH_MAC_COUNT; ++i) stat_merge(&g_syn_total, &g_mac_syn[i]);
        for (i = 0; i < NV2A_VSH_ILU_COUNT; ++i) stat_merge(&g_syn_total, &g_ilu_syn[i]);

        /* ---- the title's programs, RAW then FULL ---- */
        for (i = 0; i < ncorpus; ++i) {
            NV2AVshProgram p;
            OpStat st;
            int k, bad;
            if (!nv2a_vsh_parse(corpus[i].words, corpus[i].length, &p)) continue;
            for (k = 0; k < p.length; ++k) {
                g_mac_corpus_insns[p.insns[k].mac_op]++;
                g_ilu_corpus_insns[p.insns[k].ilu_op]++;
            }
            memset(&st, 0, sizeof st);
            snprintf(label, sizeof label, "xbe@%08lX (%d)", corpus[i].offset, corpus[i].length);
            if (!compare_program(&rig, label, &p, 0, 0, 0x5000u + (uint32_t)i, &st))
                continue;
            bad = st.residual != 0;
            if (bad) {
                unsigned long seen_mac = 0, seen_ilu = 0;
                for (k = 0; k < p.length; ++k) {
                    if (!(seen_mac & (1ul << p.insns[k].mac_op)))
                        { seen_mac |= 1ul << p.insns[k].mac_op;
                          g_mac_corpus_bad[p.insns[k].mac_op]++; }
                    if (!(seen_ilu & (1ul << p.insns[k].ilu_op)))
                        { seen_ilu |= 1ul << p.insns[k].ilu_op;
                          g_ilu_corpus_bad[p.insns[k].ilu_op]++; }
                }
            }
            stat_merge(&g_corpus_total, &st);
            memset(&st, 0, sizeof st);
            compare_program(&rig, label, &p, 1, 0, 0x5000u + (uint32_t)i, &st);
            stat_merge(&g_full_total, &st);
        }

        report_opstats("[VSH-DIFF] MAC opcodes, one synthetic program each",
                       g_mac_names, NV2A_VSH_MAC_COUNT, g_mac_syn, g_mac_corpus_insns);
        report_opstats("[VSH-DIFF] ILU opcodes, one synthetic program each",
                       g_ilu_names, NV2A_VSH_ILU_COUNT, g_ilu_syn, g_ilu_corpus_insns);

        printf("\n[VSH-DIFF] TOTALS   (a component is counted once; RESIDUAL is the "
               "only column about the emitter)\n");
        {
            const OpStat *t[3] = { &g_syn_total, &g_corpus_total, &g_full_total };
            static const char *tn[3] = { "synthetic ", "title RAW ", "title FULL" };
            for (i = 0; i < 3; ++i)
                printf("  %s programs=%lu vectors=%lu compared=%lu refused=%lu "
                       "mismatch=%lu (nonfinite=%lu ill-conditioned=%lu RESIDUAL=%lu) "
                       "worst=%.3g worst-residual=%.3g\n",
                       tn[i], t[i]->programs, t[i]->vectors, t[i]->compared,
                       t[i]->refused, t[i]->mismatch, t[i]->nonfinite,
                       t[i]->illcond, t[i]->residual, t[i]->max_err,
                       t[i]->max_residual_err);
        }

        /* Gated on RESIDUAL, not on mismatch: a program whose only mismatches
         * were ill-conditioned vectors has not implicated any opcode, and
         * listing its opcodes here would read as if it had. */
        if (g_corpus_total.residual) {
            printf("\n[VSH-DIFF] opcodes present in every title program with a RESIDUAL "
                   "mismatch\n");
            for (i = 0; i < NV2A_VSH_MAC_COUNT; ++i)
                if (g_mac_corpus_bad[i])
                    printf("  MAC %-4s in %lu mismatching programs\n",
                           g_mac_names[i], g_mac_corpus_bad[i]);
            for (i = 0; i < NV2A_VSH_ILU_COUNT; ++i)
                if (g_ilu_corpus_bad[i])
                    printf("  ILU %-4s in %lu mismatching programs\n",
                           g_ilu_names[i], g_ilu_corpus_bad[i]);
            printf("  (presence, not attribution: a mismatching program contains many "
                   "opcodes. The synthetic table above is where attribution lives.)\n");
        }

        if (g_syn_total.compared == 0 || (ncorpus > 0 && g_corpus_total.compared == 0)) {
            fprintf(stderr, "[VSH-DIFF] nothing was compared; this is not a pass\n");
            ++failures;
        }
        /* A nonfinite split is a KNOWN, DOCUMENTED divergence and an
         * ill-conditioned vector is not a measurement, so neither fails the
         * run -- both are printed, counted and named in the report instead.
         * Only RESIDUAL, which is the emitter disagreeing where the program's
         * own answer is determined, fails. FULL mode has no conditioning probe
         * (the fixup is not in the interpreter), so it is reported and never
         * fails on its own. */
        if (g_syn_total.residual || g_corpus_total.residual) ++failures;
        if (g_syn_total.nonfinite || g_corpus_total.nonfinite)
            printf("\n[VSH-DIFF] NOTE %lu components differ by finiteness alone. That is "
                   "the NV2A rule that a*0 and 0*b are +0 even when the other operand is "
                   "infinite or NaN -- nv2a_vsh.c's multiply(). The emitter does not "
                   "reproduce it and says so; the HLSL emitter does not either.\n",
                   g_syn_total.nonfinite + g_corpus_total.nonfinite);

        if (failures) {
            fprintf(stderr, "\n[VSH-DIFF] FAILED (%d)\n", failures);
            return 1;
        }
        puts("\n[VSH-DIFF] no RESIDUAL disagreement: wherever the program's own answer "
             "is determined to better than the gap, the emitted MSL agrees with "
             "nv2a_vsh_execute");
        return 0;
    }
}
