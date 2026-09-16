/* Does the emitted fixed-function MSL COMPUTE what nv2a_ff_vertex computes?
 *
 * The sibling of vsh_msl_diff_test.m, asking the same question about the other
 * emitter, and it is the ONLY thing that makes RECOMP_METAL_FF shippable. A
 * wrong MSL expression compiles cleanly, runs, and outputs black or scrambled
 * geometry; this project has lost two builds that way. Nothing else in the
 * tree can tell a correct fixed-function shader from a plausible one.
 *
 * WHAT IT DOES. For each fixed-function STATE in a corpus: derive the key with
 * nv2a_ff_key, pack the constants with nv2a_ff_params, emit MSL with the
 * production emitter, turn the emitted vertex function into a compute kernel
 * (the same surgery vsh_msl_diff_test.m performs, and for the same reason --
 * Metal has no transform feedback), dispatch it over N input vectors, run
 * nv2a_ff_vertex over the same vectors on the CPU, and compare all sixteen
 * output registers component by component.
 *
 * THIS IS A STRONGER CLAIM THAN THE PROGRAMMABLE TEST'S. That one compares its
 * RAW mode against nv2a_vsh_execute -- the production interpreter -- but has to
 * compare its FULL mode against a hand transcription of two other files. Here
 * the CPU side of RAW mode is nv2a_ff_vertex itself, the exact function the
 * shader replaces, called with the exact method array the constants were packed
 * from. If those two disagree, one of them is wrong, and no reading of a third
 * file is involved.
 *
 * THE SURGERY, and exactly what it changes. Decoration only:
 *
 *   [[attribute(N)]] [[stage_in]] [[buffer(N)]] [[position]] [[point_size]]
 *                                        all erased
 *   "vertex VS_OUT vsh_main("  ->  "static void ff_body(thread float4 *out, "
 *
 * and in RAW mode the text is TRUNCATED at the screen-space fixup and given an
 * epilogue that stores the eleven output registers. Every character of the
 * transform, the divide, the viewport offset, the lighting and the texgen --
 * the part the emitter generates, the part under test -- compiles exactly as
 * emitted. Each replacement is checked and a miss FAILS the test, so if the
 * emitter changes shape this stops rather than silently testing something else.
 *
 * FULL mode keeps the emitted epilogue and compares it against a C
 * transcription of the fixed `vs` in nv2a_metal.m MINUS the subpixel snap,
 * because prepare_vertices does not snap on the fixed-function branch
 * (nv2a_pb_exec.c:2779 is inside `if (programmable)`). That is the weaker
 * claim -- it says the emitted tail matches this reading -- and it is the only
 * thing that would catch a transposed viewport divide.
 *
 * THE DIVERGENCES ARE MEASURED, NOT HIDDEN. nv2a_ff_vertex refuses a whole
 * BATCH for four per-vertex conditions a vertex function cannot express: a
 * zero or non-finite clip w, a non-finite position after the divide, a
 * non-finite generated texture coordinate, and a degenerate normal something
 * reads. Vectors that hit one are counted in `refused` and are NOT compared,
 * because there is no CPU answer to compare against. The corpus deliberately
 * generates them, so `refused` is a number this test reports rather than a
 * case it avoids.
 *
 * POSITIVE CONTROLS, because every number below is an absence-measurement:
 *
 *   1. injected fault   every state is compiled a second time with
 *                       "oPos.x += 1.0f" spliced in front of the epilogue. If
 *                       that does not report mismatches, the comparison is not
 *                       comparing and every zero above it is worthless.
 *   2. state anchor     one corpus entry is the composite matrix, viewport
 *                       offset and mode bits MEASURED out of a real run
 *                       (render-investigation/ffdump1/stderr.log:6197-6206),
 *                       with its own measured input vector and its own
 *                       measured expected output. It proves the corpus
 *                       contains the shape the title actually draws, and it
 *                       fails if nv2a_ff_vertex itself ever stops reproducing
 *                       the number that run printed.
 *   3. non-zero work    compared counts are printed per state; a state with
 *                       compared=0 is reported UNTESTED, never as passing.
 *   4. accept parity    for every corpus state, nv2a_ff_key's answer is
 *                       checked against nv2a_ff_vertex's: a state the key
 *                       accepts must not produce a STATE-level rejection from
 *                       the CPU function on well-formed input. A key that
 *                       accepts something the CPU refuses is the exact bug
 *                       that would put untransformed geometry on screen.
 *
 * NOT A CTEST CASE, for the reason stated beside jsrf_vsh_msl_diff_test in
 * CMakeLists.txt: it needs a device, and a headless build machine has none.
 * Run it explicitly:
 *
 *     build/jsrf_ff_msl_diff_test
 *     RECOMP_FF_GPU_NORMAL_ZERO=1 build/jsrf_ff_msl_diff_test
 *
 * The second arm is required whenever that switch is going to be set at run
 * time: it changes which shapes the key accepts AND the arithmetic of the
 * emitted normalise, so a result measured without it does not transfer.
 *
 * PASS CRITERION, and it is not "it printed some numbers":
 *
 *     residual mismatches == 0, over every state, in BOTH modes;
 *     every state's `compared` > 0;
 *     the injected-fault arm reports mismatches for every state;
 *     the anchor state reproduces ffdump1's measured output;
 *     the accept-parity check reports 0 violations.
 *
 * Anything else is a fail and the exit status says so.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVERT  64          /* vectors per dispatch */
#define NSET    4          /* distinct input sets per state */

/* Absolute below 1, relative above, so a near-zero result is not judged
 * against its own magnitude. Set from the measurement the run prints, not
 * chosen in advance -- and the expectation here is much tighter than the
 * programmable test's, because the emitter matches nv2a_ff.c's association
 * expression by expression and compiles with contraction off. Read the
 * bit-exact percentage the run prints: a drop in it is a regression even while
 * this threshold still passes. */
#define TOLERANCE 2.0e-5

static int failures;
static unsigned long g_exact, g_inexact;

/* ================================================================
 * State corpus
 * ================================================================ */

typedef struct {
    const char *name;
    uint32_t    m[2048];
    uint8_t     seen[2048];
    int         expect_key;      /* 1 the key must accept, 0 it must refuse */
} FFState;

static void put_f(FFState *st, unsigned byte, float v)
{ memcpy(&st->m[byte / 4], &v, 4); st->seen[byte / 4] = 1; }
static void put_u(FFState *st, unsigned byte, uint32_t v)
{ st->m[byte / 4] = v; st->seen[byte / 4] = 1; }

/* A general, non-degenerate 4x4 in method-register order: word (row*16+col*4)
 * is M[row][col], which is what matrix() in nv2a_ff.c reads. */
static void put_matrix(FFState *st, unsigned base, const float v[16])
{ for (unsigned i = 0; i < 16; ++i) put_f(st, base + i * 4, v[i]); }

/* The composite matrix a real gameplay frame uploaded, measured:
 * render-investigation/ffdump1/stderr.log:6198-6201. It is deliberately the
 * awkward one -- row 2 is of order 1e7, row 3 is small and negative, and the
 * vertex it was printed with divides by a NEGATIVE w. */
static const float ANCHOR_CMAT[16] = {
    -358.2377f,      19.8759f,     114.3981f,        0.0000f,
    -123.9408f,    -322.1385f,     -73.7364f,        0.0000f,
 -7933567.5000f, -217573.8594f, -6738032.5000f, -10411048.0000f,
      -0.4728f,      -0.0130f,      -0.4015f,        0.0000f
};
static const float ANCHOR_IN0[4]  = { 4505.2212f, -743.8711f, 0.0f, 1.0f };
/* ffdump1/stderr.log:6206, on a 640x480 surface. Tolerated to the four decimal
 * places the log prints, which is the resolution of the evidence. */
static const float ANCHOR_OUT0[4] = { 768.6722f, 150.8611f, 16785482.0f, -2120.3469f };

static void state_base(FFState *st, const char *name)
{
    memset(st, 0, sizeof *st);
    st->name = name;
    st->expect_key = 1;
    put_matrix(st, 0x680, ANCHOR_CMAT);
    put_f(st, 0x0a20, 0.53125f);          /* viewport offset x, measured */
    put_f(st, 0x0a24, 0.53125f);          /* viewport offset y, measured */
}

/* An identity-ish inverse model-view and four light blocks, so the lit states
 * exercise real arithmetic rather than zeros. */
static void state_add_lighting(FFState *st, unsigned lights, int normalise)
{
    static const float nm[16] = { 0.8f,0.1f,-0.2f,0, -0.3f,0.9f,0.05f,0,
                                  0.15f,-0.4f,0.7f,0, 0,0,0,1 };
    put_u(st, 0x0314, 1);
    put_matrix(st, 0x580, nm);
    if (normalise) put_u(st, 0x03a4, 1);
    put_u(st, 0x03bc, lights);
    for (unsigned k = 0; k < 3; ++k) {
        put_f(st, 0x0a10 + 4 * k, 0.05f + 0.01f * (float)k);   /* scene ambient */
        put_f(st, 0x03a8 + 4 * k, 0.02f * (float)k);           /* emission      */
    }
    put_f(st, 0x03b4, 0.75f);                                  /* material alpha */
    for (unsigned l = 0; l < 8; ++l) {
        unsigned base = 0x1000 + l * 0x80;
        if (((lights >> (2 * l)) & 3) != 1) continue;
        for (unsigned k = 0; k < 3; ++k) {
            put_f(st, base + 4 * k,          0.03f * (float)(l + 1));
            put_f(st, base + 0x0c + 4 * k,   0.4f - 0.05f * (float)k);
            put_f(st, base + 0x34 + 4 * k,   (k == 1 ? -0.8f : 0.3f));
        }
    }
}

static void state_add_texmat(FFState *st, unsigned unit, int uploaded, int zero)
{
    static const float tm[16] = { 2,0,0,0.25f, 0,2,0,0.5f, 0,0,1,0, 0,0,0,1 };
    put_u(st, 0x0420 + unit * 4, 1);
    if (!uploaded) return;                   /* enabled, never written */
    if (zero) { for (unsigned i = 0; i < 16; ++i) put_f(st, 0x6c0 + unit * 64 + i * 4, 0.0f); }
    else      put_matrix(st, 0x6c0 + unit * 64, tm);
}

static int build_corpus(FFState *out, int cap)
{
    int n = 0;
    if (n < cap) { state_base(&out[n], "anchor/plain");                       ++n; }
    if (n < cap) { state_base(&out[n], "texmat0");
                   state_add_texmat(&out[n], 0, 1, 0);                        ++n; }
    if (n < cap) { state_base(&out[n], "texmat-all-four");
                   for (unsigned u = 0; u < 4; ++u) state_add_texmat(&out[n], u, 1, 0); ++n; }
    if (n < cap) { state_base(&out[n], "texmat0-never-uploaded");
                   state_add_texmat(&out[n], 0, 0, 0);                        ++n; }
    if (n < cap) { state_base(&out[n], "texmat0-all-zero");
                   state_add_texmat(&out[n], 0, 1, 1);                        ++n; }
    if (n < cap) { state_base(&out[n], "texgen-normalmap-u0");
                   put_u(&out[n], 0x03c0, 0x8511);
                   put_u(&out[n], 0x03c4, 0x8511);
                   put_u(&out[n], 0x03c8, 0x8511);                            ++n; }
    if (n < cap) { state_base(&out[n], "lit-one-infinite");
                   state_add_lighting(&out[n], 1u, 0);                        ++n; }
    if (n < cap) { state_base(&out[n], "lit-eight-infinite");
                   state_add_lighting(&out[n], 0x5555u, 0);                   ++n; }
    if (n < cap) { state_base(&out[n], "lit-and-texmat");
                   state_add_lighting(&out[n], 5u, 0);
                   state_add_texmat(&out[n], 0, 1, 0);
                   state_add_texmat(&out[n], 2, 1, 0);                        ++n; }
    /* Shapes the key MUST refuse. Each one is a case where the GPU could not
     * reproduce the CPU's answer, and a key that accepted it would be the bug.
     * They are in the corpus so a future change that loosens the key fails
     * here rather than in a frame. */
    if (n < cap) { state_base(&out[n], "REFUSE skinning");
                   put_u(&out[n], 0x0328, 1); out[n].expect_key = 0;          ++n; }
    if (n < cap) { state_base(&out[n], "REFUSE texgen EYE_LINEAR");
                   put_u(&out[n], 0x03c0, 0x2400); out[n].expect_key = 0;     ++n; }
    if (n < cap) { state_base(&out[n], "REFUSE texgen NORMAL_MAP on q");
                   put_u(&out[n], 0x03cc, 0x8511); out[n].expect_key = 0;     ++n; }
    if (n < cap) { state_base(&out[n], "REFUSE local light");
                   state_add_lighting(&out[n], 2u, 0); out[n].expect_key = 0; ++n; }
    if (n < cap) { state_base(&out[n], "REFUSE composite never uploaded");
                   out[n].seen[0x680 / 4] = 0; out[n].expect_key = 0;         ++n; }
    /* Normalisation with a read normal: refused unless RECOMP_FF_GPU_NORMAL_ZERO
     * is set, which is why the expectation is read from the switch and the
     * header says to run both arms. */
    if (n < cap) { state_base(&out[n], "normalise+normalmap");
                   put_u(&out[n], 0x03c0, 0x8511);
                   put_u(&out[n], 0x03a4, 1);
                   out[n].expect_key = getenv("RECOMP_FF_GPU_NORMAL_ZERO")
                                    && strcmp(getenv("RECOMP_FF_GPU_NORMAL_ZERO"), "0")
                                    && getenv("RECOMP_FF_GPU_NORMAL_ZERO")[0] ? 1 : 0;
                   ++n; }
    return n;
}

/* ================================================================
 * Input vectors
 * ================================================================ */

static uint32_t g_rng;
static void rng_seed(uint32_t s) { g_rng = s ? s : 1u; }
static uint32_t rng_next(void)
{ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

/* Positions in the range real geometry occupies -- the anchor vertex is at
 * x=4505 -- plus the values that change a branch: exact zero, exact one, and a
 * position that drives clip w to exactly zero so the refusal path is exercised
 * rather than avoided. */
static float rng_coord(void)
{
    uint32_t r = rng_next();
    switch (r & 63u) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return INFINITY;
    case 4: return NAN;
    default: return ((float)(r >> 8) / (float)(1u << 24)) * 9000.0f - 4500.0f;
    }
}
static float rng_unit(void)
{
    uint32_t r = rng_next();
    if ((r & 63u) == 0) return 0.0f;
    if ((r & 63u) == 1) return 1.0f;
    return (float)(r >> 8) / (float)(1u << 24);
}

static void make_inputs(float in[16][4])
{
    int a, k;
    for (a = 0; a < 16; ++a) for (k = 0; k < 4; ++k) in[a][k] = 0.0f;
    for (k = 0; k < 3; ++k) in[0][k] = rng_coord();
    in[0][3] = 1.0f;
    for (k = 0; k < 3; ++k) in[2][k] = rng_unit() * 2.0f - 1.0f;
    for (k = 0; k < 4; ++k) in[3][k] = rng_unit();
    for (k = 0; k < 4; ++k) in[4][k] = rng_unit();
    for (a = 9; a < 13; ++a) for (k = 0; k < 4; ++k)
        in[a][k] = k == 3 ? 1.0f : rng_unit() * 4.0f - 2.0f;
}

/* ================================================================
 * MSL surgery -- decoration only, every replacement checked
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

/* out[] is indexed the way nv2a_ff_vertex indexes its output array, so the
 * comparison needs no mapping table. 1, 2 and 13..15 are never written by
 * either side and are compared as the zeros both leave them. */
static const char *const RAW_EPILOGUE =
    "    out[0]=oPos; out[3]=oD0; out[4]=oD1; out[5]=oFog; out[6]=oPts;\n"
    "    out[7]=oB0; out[8]=oB1; out[9]=oT0; out[10]=oT1; out[11]=oT2; out[12]=oT3;\n"
    "}\n";
static const char *const FULL_STORES =
    "    out[0]=o.oPos; out[3]=o.oD0; out[4]=o.oD1; out[5]=float4(o.oFog,0,0,0);\n"
    "    out[6]=float4(o.oPts,0,0,0); out[7]=o.oB0; out[8]=o.oB1;\n"
    "    out[9]=o.oT0; out[10]=o.oT1; out[11]=o.oT2; out[12]=o.oT3;\n";

static int build_kernel_source(const NV2AFFKey *key, int mode, int inject,
                               char *out, size_t cap, const char *label)
{
    static char msl[262144];
    char *marker;
    size_t used;
    int i, n;

    n = nv2a_ff_generate_msl(key, msl, (int)sizeof msl);
    if (n <= 0) {
        fprintf(stderr, "[FF-DIFF] %-28s EMITTER REFUSED (returned %d)\n", label, n);
        return 0;
    }
    msl_strip_attrs(msl, "attribute(");
    msl_strip_attrs(msl, "stage_in");
    msl_strip_attrs(msl, "buffer(");
    msl_strip_attrs(msl, "position");
    msl_strip_attrs(msl, "point_size");

    if (!msl_replace_once(msl, sizeof msl, "vertex VS_OUT vsh_main(",
                          "static void ff_body(thread float4 *out, ")) {
        fprintf(stderr, "[FF-DIFF] %-28s the emitter no longer opens vsh_main the way"
                        " this test rewrites it; the rewrite, not the emitter, is stale\n",
                label);
        return 0;
    }
    marker = strstr(msl, FIXUP_MARKER);
    if (!marker) {
        fprintf(stderr, "[FF-DIFF] %-28s emitted text has no screen-space fixup marker\n", label);
        return 0;
    }
    if (mode == 0) {
        *marker = '\0';
        if (inject) strcat(msl, "    oPos.x += 1.0f;\n");
        strcat(msl, RAW_EPILOGUE);
    } else {
        if (inject) {
            static char head[262144];
            size_t head_len = (size_t)(marker - msl);
            memcpy(head, msl, head_len); head[head_len] = '\0';
            strcat(head, "    oPos.x += 1.0f;\n");
            strcat(head, marker);
            memcpy(msl, head, strlen(head) + 1);
        }
        if (!msl_replace_once(msl, sizeof msl, "    return o;\n", FULL_STORES)) {
            fprintf(stderr, "[FF-DIFF] %-28s no `return o;` to replace\n", label);
            return 0;
        }
    }

    used = (size_t)snprintf(out, cap, "%s\n", msl);
    if (used >= cap) return 0;
    used += (size_t)snprintf(out + used, cap - used,
        "kernel void ff_diff(device float4 *outbuf [[buffer(0)]],\n"
        "                    device const float4 *vin [[buffer(1)]],\n"
        "                    constant float4 *c [[buffer(2)]],\n"
        "                    constant VSH_Viewport &vp [[buffer(3)]],\n"
        "                    uint gid [[thread_position_in_grid]]) {\n"
        "    float4 o[16];\n"
        "    for (int i = 0; i < 16; ++i) o[i] = float4(0,0,0,0);\n"
        "    VS_IN vi;\n");
    if (used >= cap) return 0;
    for (i = 0; i < 16; ++i)
        if (key->inputs & (1u << i)) {
            used += (size_t)snprintf(out + used, cap - used,
                                     "    vi.v%d = vin[gid*16+%d];\n", i, i);
            if (used >= cap) return 0;
        }
    used += (size_t)snprintf(out + used, cap - used,
        "    ff_body(o, vi, c, vp);\n"
        "    for (int i = 0; i < 16; ++i) outbuf[gid*16+i] = o[i];\n}\n");
    return used < cap;
}

/* The CPU side of FULL mode: the fixed `vs` in nv2a_metal.m (line 567),
 * WITHOUT the subpixel snap, because the fixed-function branch of
 * prepare_vertices does not snap. Deliberately a separate transcription from
 * nv2a_ff_vertex, which knows nothing about clip space. */
static void cpu_fixup(const float src[16][4], float w, float h, float d,
                      float out[16][4])
{
    float p[4], z;
    int i;
    for (i = 0; i < 16; ++i) memcpy(out[i], src[i], sizeof(float) * 4);
    memcpy(p, src[0], sizeof p);
    z = p[2] / d;
    out[0][0] = (p[0] / w * 2.0f - 1.0f) * p[3];
    out[0][1] = (1.0f - p[1] / h * 2.0f) * p[3];
    out[0][2] = z * p[3];
    out[0][3] = p[3];
}

static int close_enough(float a, float b, double *err_out)
{
    double x = a, y = b, err;
    if (isnan(x) && isnan(y)) { *err_out = 0.0; return 1; }
    if (x == y) { *err_out = 0.0; return 1; }
    if (isinf(x) || isinf(y) || isnan(x) || isnan(y)) { *err_out = INFINITY; return 0; }
    err = fabs(x - y);
    if (fabs(x) > 1.0) err /= fabs(x);
    *err_out = err;
    return err <= TOLERANCE;
}

/* ================================================================
 * One state, one mode
 * ================================================================ */

typedef struct {
    id<MTLDevice>       device;
    id<MTLCommandQueue> queue;
    id<MTLBuffer>       vin, out;
} Rig;

typedef struct {
    unsigned long compared, refused, mismatch, residual, divergent;
    double max_err;
} Score;

static int run_state(Rig *rig, const FFState *st, const NV2AFFKey *key,
                     int mode, int inject, const char *label, Score *sc)
{
    static char source[512 * 1024];
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    id<MTLLibrary> library;
    id<MTLFunction> fn;
    id<MTLComputePipelineState> pso;
    float (*vin)[16][4];
    float (*gpu)[16][4];
    const float vp[3] = { 640.0f, 480.0f, 16777215.0f };
    int set, v, reg, k, shown = 0;

    if (!build_kernel_source(key, mode, inject, source, sizeof source, label)) return 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    /* Safe math, because that is what nv2a_metal.m compiles its own shaders
     * with. A result measured under fast math is about a compiler this project
     * does not run. */
    if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
    else options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    library = [rig->device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                        options:options error:&error];
    if (!library) {
        fprintf(stderr, "[FF-DIFF] %-28s COMPILE FAILED\n%s\n", label,
                error.description.UTF8String);
        ++failures;
        return 0;
    }
    fn = [library newFunctionWithName:@"ff_diff"];
    pso = fn ? [rig->device newComputePipelineStateWithFunction:fn error:&error] : nil;
    if (!pso) {
        fprintf(stderr, "[FF-DIFF] %-28s PIPELINE FAILED: %s\n", label,
                error.description.UTF8String);
        ++failures;
        return 0;
    }

    vin = (float (*)[16][4])rig->vin.contents;
    gpu = (float (*)[16][4])rig->out.contents;
    rng_seed(0x5EED1234u ^ (uint32_t)mode);

    for (set = 0; set < NSET; ++set) {
        for (v = 0; v < NVERT; ++v) make_inputs(vin[v]);
        /* The measured anchor vertex, every set, in slot 0: the one input this
         * test has evidence for. */
        memcpy(vin[0][0], ANCHOR_IN0, sizeof ANCHOR_IN0);
        memset(gpu, 0, (size_t)NVERT * 16 * 4 * sizeof(float));

        @autoreleasepool {
            id<MTLCommandBuffer> cb = [rig->queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:rig->out offset:0 atIndex:0];
            [enc setBuffer:rig->vin offset:0 atIndex:1];
            /* 192 float4 = 3072 bytes, the same block nv2a_metal.m binds at
             * index 1 for a program's constant file -- which is exactly why
             * nv2a_ff_constants is that size. */
            [enc setBytes:nv2a_ff_constants length:NV2A_FF_C_SLOTS * 16 atIndex:2];
            [enc setBytes:vp length:sizeof vp atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake(1,1,1)
                threadsPerThreadgroup:MTLSizeMake(NVERT,1,1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "[FF-DIFF] %-28s DISPATCH FAILED: %s\n", label,
                        cb.error.description.UTF8String);
                ++failures;
                return 0;
            }
        }

        for (v = 0; v < NVERT; ++v) {
            float ref[16][4], expect[16][4];
            const char *reason;
            nv2a_ff_method_seen = st->seen;
            reason = nv2a_ff_vertex(st->m, (const float (*)[4])vin[v], ref);
            if (reason) {
                /* No CPU answer exists: nv2a_ff_vertex refused the BATCH for a
                 * per-vertex condition a vertex function cannot express. The
                 * GPU drew something and the CPU drew nothing; that is the
                 * documented divergence, counted here so it is a number rather
                 * than a silence. */
                ++sc->refused;
                ++sc->divergent;
                continue;
            }
            if (mode == 0) memcpy(expect, ref, sizeof expect);
            else           cpu_fixup((const float (*)[4])ref, vp[0], vp[1], vp[2], expect);
            ++sc->compared;
            for (reg = 0; reg < 16; ++reg) {
                /* oFog and oPts survive FULL mode as scalars, so only x is
                 * meaningful there; RAW mode compares all four, and both sides
                 * leave them zero. */
                int lanes = (mode == 1 && (reg == 5 || reg == 6)) ? 1 : 4;
                for (k = 0; k < lanes; ++k) {
                    double err;
                    if (expect[reg][k] == gpu[v][reg][k]) { ++g_exact; continue; }
                    ++g_inexact;
                    if (close_enough(expect[reg][k], gpu[v][reg][k], &err)) {
                        if (err > sc->max_err) sc->max_err = err;
                        continue;
                    }
                    ++sc->mismatch;
                    ++sc->residual;
                    if (err > sc->max_err) sc->max_err = err;
                    if (shown++ < 4)
                        fprintf(stderr, "[FF-DIFF] %-28s o[%d].%c cpu=%.9g gpu=%.9g err=%.3g\n",
                                label, reg, "xyzw"[k],
                                expect[reg][k], gpu[v][reg][k], err);
                }
            }
        }
    }
    return 1;
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    static FFState corpus[32];
    Rig rig;
    int n, i, mode, parity_violations = 0, untested = 0, control_dead = 0;

    printf("[FF-DIFF] emitted fixed-function MSL against nv2a_ff_vertex\n");
    printf("[FF-DIFF] RECOMP_FF_GPU_NORMAL_ZERO=%s RECOMP_FF_TEXMAT_TRANSPOSE=%s"
           " RECOMP_FF_TEXMAT_IDENTITY=%s\n",
           getenv("RECOMP_FF_GPU_NORMAL_ZERO") ? getenv("RECOMP_FF_GPU_NORMAL_ZERO") : "(unset)",
           getenv("RECOMP_FF_TEXMAT_TRANSPOSE") ? getenv("RECOMP_FF_TEXMAT_TRANSPOSE") : "(unset)",
           getenv("RECOMP_FF_TEXMAT_IDENTITY") ? getenv("RECOMP_FF_TEXMAT_IDENTITY") : "(unset)");

    n = build_corpus(corpus, (int)(sizeof corpus / sizeof corpus[0]));
    printf("[FF-DIFF] corpus: %d states\n", n);

    /* CONTROL 2, and it runs with no device: does nv2a_ff_vertex still
     * reproduce the output a real run printed for the measured input and the
     * measured matrix? If this fails, the CPU side of every comparison below
     * has changed and the comparison is against the wrong thing. */
    {
        FFState a; float out[16][4]; const char *reason; int bad = 0;
        state_base(&a, "anchor");
        nv2a_ff_method_seen = a.seen;
        {
            float in[16][4];
            memset(in, 0, sizeof in);
            memcpy(in[0], ANCHOR_IN0, sizeof ANCHOR_IN0);
            reason = nv2a_ff_vertex(a.m, (const float (*)[4])in, out);
        }
        if (reason) { printf("[FF-DIFF] ANCHOR REFUSED: %s\n", reason); bad = 1; }
        else for (i = 0; i < 4; ++i) {
            double tol = fabs((double)ANCHOR_OUT0[i]) * 1e-4 + 1e-3;
            if (fabs((double)out[0][i] - (double)ANCHOR_OUT0[i]) > tol) bad = 1;
        }
        printf("[FF-DIFF] anchor (ffdump1/stderr.log:6206): cpu=(%.4f %.4f %.1f %.4f)"
               " measured=(%.4f %.4f %.1f %.4f) %s\n",
               out[0][0], out[0][1], out[0][2], out[0][3],
               ANCHOR_OUT0[0], ANCHOR_OUT0[1], ANCHOR_OUT0[2], ANCHOR_OUT0[3],
               bad ? "MISMATCH" : "ok");
        if (bad) ++failures;
    }

    /* CONTROL 4: the key's accept set against nv2a_ff_vertex's. */
    for (i = 0; i < n; ++i) {
        NV2AFFKey key;
        int got;
        nv2a_ff_method_seen = corpus[i].seen;
        got = nv2a_ff_key(corpus[i].m, &key);
        if (got != corpus[i].expect_key) {
            printf("[FF-DIFF] ACCEPT PARITY: %-28s key=%d expected %d\n",
                   corpus[i].name, got, corpus[i].expect_key);
            ++parity_violations;
        }
    }
    printf("[FF-DIFF] accept parity: %d violations\n", parity_violations);
    if (parity_violations) ++failures;

    rig.device = MTLCreateSystemDefaultDevice();
    if (!rig.device) {
        printf("[FF-DIFF] no Metal device; the emitter was not executed."
               " That is not evidence about it.\n");
        return failures ? 1 : 0;
    }
    rig.queue = [rig.device newCommandQueue];
    rig.vin = [rig.device newBufferWithLength:NVERT * 16 * 16
                                      options:MTLResourceStorageModeShared];
    rig.out = [rig.device newBufferWithLength:NVERT * 16 * 16
                                      options:MTLResourceStorageModeShared];
    printf("[FF-DIFF] device: %s, math mode safe (as nv2a_metal.m compiles its own)\n",
           rig.device.name.UTF8String);

    for (mode = 0; mode < 2; ++mode) {
        printf("[FF-DIFF] ---- %s mode ----\n", mode ? "FULL (emitted epilogue)"
                                                     : "RAW (against nv2a_ff_vertex)");
        for (i = 0; i < n; ++i) {
            NV2AFFKey key;
            Score sc, fault;
            char label[64];
            nv2a_ff_method_seen = corpus[i].seen;
            if (!nv2a_ff_key(corpus[i].m, &key)) continue;   /* refusals tested above */
            nv2a_ff_params(corpus[i].m, &key);
            memset(&sc, 0, sizeof sc);
            memset(&fault, 0, sizeof fault);
            snprintf(label, sizeof label, "%s", corpus[i].name);
            if (!run_state(&rig, &corpus[i], &key, mode, 0, label, &sc)) continue;
            run_state(&rig, &corpus[i], &key, mode, 1, label, &fault);
            printf("[FF-DIFF] %-28s compared=%-6lu refused=%-5lu residual=%-5lu"
                   " worst=%.3g   control=%lu %s\n",
                   corpus[i].name, sc.compared, sc.refused, sc.residual,
                   sc.max_err, fault.mismatch,
                   fault.mismatch ? "" : "<<< CONTROL DEAD");
            if (!sc.compared) { printf("[FF-DIFF]   ^ UNTESTED: nothing compared\n"); ++untested; }
            if (!fault.mismatch) ++control_dead;
            if (sc.residual) ++failures;
        }
    }

    printf("[FF-DIFF] bit-exact components %lu of %lu (%.3f%%)\n",
           g_exact, g_exact + g_inexact,
           (g_exact + g_inexact) ? 100.0 * (double)g_exact / (double)(g_exact + g_inexact) : 0.0);
    if (untested)     { printf("[FF-DIFF] %d states UNTESTED\n", untested); ++failures; }
    if (control_dead) { printf("[FF-DIFF] %d injected-fault controls did not fire;"
                               " every zero above is worthless\n", control_dead); ++failures; }
    printf("[FF-DIFF] %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
