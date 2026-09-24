/* G51.1: the host's own Metal pipeline for pre-transformed 2D draws.
 *
 * It shares nothing with nv2a_metal.m at run time -- its own queue, library,
 * pipeline and textures -- and writes nothing but the caller's crop, so the
 * executor cannot tell it is running. What it deliberately shares is SOURCE
 * OF TRUTH: textures go through nv2a_texture_decode_rgba8(), the decoder the
 * executor's G27 hardware-sampling path uploads with, and the fragment
 * arithmetic below follows nv2a_metal.m's shade()/comb_stage() and
 * fs_hw_blend() operation for operation (input mappings, output mappings,
 * clamps, R0.a seeded from texture 0, blend then ordered dither), so a
 * disagreement in the differential is a disagreement in the INPUTS -- which
 * is the question G51.1 asks: can D3D state alone describe the draw.
 *
 * Where it departs from the executor it follows the NV2A instead, and each
 * departure can only change a draw the executor would draw differently:
 *   - COMBINER_CONTROL's FACTOR0/FACTOR1 bits (12, 16): SAME_FACTOR_ALL uses
 *     stage 0's constant for every stage; the executor always uses per-stage.
 *     D3D's fixed-function TextureFactor writes all eight the same, so only
 *     a pixel shader can tell them apart.
 *   - alpha test with any comparison (the executor refuses all but GREATER);
 *   - blend factors DST_ALPHA/ONE_MINUS_DST_ALPHA (1 and 0 on a 565 target),
 *     SRC_ALPHA_SATURATE and the constant colour, and every blend equation;
 *   - colour write mask.
 * DEPTH is a real Depth32Float attachment seeded with the executor's own
 * values (guest z / 16777215, the representation nv2a_metal.m keeps), with
 * the executor's compare mapping (nv2a_metal_compare_func) and its rule that
 * depth is written only while the test is on.
 * The MUX rule (AB when R0.a >= 0.5) is the executor's, kept for agreement;
 * that it matches the NV2A is not established here.
 *
 * WHERE THE EXECUTOR ITSELF MAY DEPART FROM THE NV2A, as far as a 2D draw can
 * see it (written down for G51.1; the host follows the executor unless noted):
 *   - MUX: always "R0.a >= 0.5", ignoring COMBINER_CONTROL's MUX_SELECT
 *     (bit 8, LSB vs MSB); and whether it picks AB or CD on that side is
 *     unverified against hardware.
 *   - FACTOR0/FACTOR1: always per stage (the host honours bits 12/16).
 *   - Final combiner: only R0 or R0 + specular (SPECULAR_FOG_CW0 0xC/0xE,
 *     CW1 0x1C80) is modelled; anything else is refused by both.
 *   - Alpha test: only GREATER is accepted; blend: only 0/1/SRC/DST colour and
 *     SRC alpha factors with ADD. Other states are refused by the executor
 *     (not drawn at all), drawn by the host.
 *   - Depth: the rasteriser clamps z (MTLDepthClipModeClamp) and the shader
 *     discards outside SET_CLIP_MIN/MAX, where the NV2A clips.
 *   - Pixel centres: D3D's pass-through adds 0.53125 (the NV2A's centre bias,
 *     1/2 + 1/32) and Metal then samples at +0.5, so every edge decision is
 *     1/32 pixel from where D3D's convention would put it. The host adds the
 *     same 0.53125 and matches the executor, not the hardware.
 *   - Clip w for XYZRHW: the host uses 1/rhw; what the pass-through writes to
 *     oPos.w is not read here. Invisible while rhw is constant over a draw
 *     (the tutorial's logo: 0.653 at all four corners); a draw with varying
 *     rhw would show it as texture swimming. */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "d3d8_host_2d.h"
#include "nv2a_texture_decode.h"
#include "nv2a_metal_state.h"
#include "nv2a_metal.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    float W, H; uint32_t ox, oy;
    uint32_t cc, control, tmask, add_spec;
    uint32_t alpha_test, alpha_func, alpha_ref, blend;
    uint32_t bsrc, bdst, beq, bcolor;
    uint32_t dither, cmask, lin_mask, pad;
    float tw[4], th[4], lod_bias[4];
    uint32_t ci[8], ai[8], co[8], ao[8], k0[8], k1[8];
} H2DUniforms;

#define H2D_FC(name, idx) \
    "constant uint fc_" name "_ [[function_constant(" #idx ")]];" \
    "constant uint FC_" name " = is_function_constant_defined(fc_" name "_) ? fc_" name "_ : 0u;\n"
static NSString *const k_src =
@"#include <metal_stdlib>\n"
 "using namespace metal;\n"
 "struct V { float4 p, d0, d1, t0, t1, t2, t3; };\n"
 "struct U { float W, H; uint ox, oy; uint cc, control, tmask, add_spec;"
 " uint alpha_test, alpha_func, alpha_ref, blend; uint bsrc, bdst, beq, bcolor;"
 " uint dither, cmask, lin_mask, pad; float tw[4], th[4], lod_bias[4];"
 " uint ci[8], ai[8], co[8], ao[8], k0[8], k1[8]; };\n"
 "struct O { float4 p [[position]]; float4 d0, d1, t0, t1, t2, t3; };\n"
 /* Target-space pixels to clip space over the crop. The same expression as
  * nv2a_metal.m's vs(), with the crop origin taken off first -- an integer,
  * so the subtraction is exact. */
 "vertex O h2d_vs(uint id [[vertex_id]], const device V *v [[buffer(0)]], constant U &u [[buffer(1)]]) {\n"
 " V x = v[id]; O o; float w = x.p.w;\n"
 " o.p = float4(((x.p.x - float(u.ox)) / u.W * 2 - 1) * w, (1 - (x.p.y - float(u.oy)) / u.H * 2) * w, x.p.z * w, w);\n"
 " o.d0 = x.d0; o.d1 = x.d1; o.t0 = x.t0; o.t1 = x.t1; o.t2 = x.t2; o.t3 = x.t3; return o; }\n"
 "float inp(uint code, uint ch, thread float4 *r) { uint s = code & 15; float x = r[s][(code & 16) ? 3 : ch];"
 " switch (code >> 5) { case 0: return max(0.0f, x); case 1: return 1 - min(1.0f, max(0.0f, x));"
 " case 2: return 2 * max(0.0f, x) - 1; case 3: return 1 - 2 * max(0.0f, x);"
 " case 4: return max(0.0f, x) - .5f; case 5: return .5f - max(0.0f, x); case 6: return x; default: return -x; } }\n"
 "float cmap1(uint m, float x) { switch (m) { case 1: return x - 0.5f; case 2: return x * 2.0f;"
 " case 3: return (x - 0.5f) * 2.0f; case 4: return x * 4.0f; case 6: return x * 0.5f; default: return x; } }\n"
 "float3 cmap3(uint m, float3 v) { return float3(cmap1(m, v.x), cmap1(m, v.y), cmap1(m, v.z)); }\n"
 "float4 unpack(uint k) { return float4(float((k >> 16) & 255), float((k >> 8) & 255), float(k & 255), float((k >> 24) & 255)) / 255.0f; }\n"
 /* SPECIALISATION, as the executor's spec_pipeline_for does it: every
  * word that decides the SHAPE of the fragment program -- the combiner
  * words, the stage count, the texture mask, the fixed-function switches --
  * is a function constant, so the stage loop unrolls, every r[] index is a
  * constant and the register file lives in registers instead of on the
  * stack. Measured before this (bench, 24 Sep 2026): the host's interpreter
  * cost 3.4-5.2x the executor's GPU time on the logo. SPEC false is the
  * interpreter, reading the same words from U, with the same arithmetic in
  * the same order: one body, two sources for its words. */
 "constant bool fc_spec_ [[function_constant(0)]];\n"
 "constant bool SPEC = is_function_constant_defined(fc_spec_) && fc_spec_;\n"
 H2D_FC("CC",1) H2D_FC("TMASK",2) H2D_FC("FLAGS",3) H2D_FC("CONTROL",4) H2D_FC("AFUNC",5)
 H2D_FC("BSRC",6) H2D_FC("BDST",7) H2D_FC("BEQ",40) H2D_FC("CMASK",41) H2D_FC("LIN",42)
 H2D_FC("ci0",8) H2D_FC("ci1",9) H2D_FC("ci2",10) H2D_FC("ci3",11) H2D_FC("ci4",12) H2D_FC("ci5",13) H2D_FC("ci6",14) H2D_FC("ci7",15)
 H2D_FC("ai0",16) H2D_FC("ai1",17) H2D_FC("ai2",18) H2D_FC("ai3",19) H2D_FC("ai4",20) H2D_FC("ai5",21) H2D_FC("ai6",22) H2D_FC("ai7",23)
 H2D_FC("co0",24) H2D_FC("co1",25) H2D_FC("co2",26) H2D_FC("co3",27) H2D_FC("co4",28) H2D_FC("co5",29) H2D_FC("co6",30) H2D_FC("co7",31)
 H2D_FC("ao0",32) H2D_FC("ao1",33) H2D_FC("ao2",34) H2D_FC("ao3",35) H2D_FC("ao4",36) H2D_FC("ao5",37) H2D_FC("ao6",38) H2D_FC("ao7",39)
 "#define PICK8(n, st) (st == 0 ? FC_##n##0 : st == 1 ? FC_##n##1 : st == 2 ? FC_##n##2 : st == 3 ? FC_##n##3 : st == 4 ? FC_##n##4 : st == 5 ? FC_##n##5 : st == 6 ? FC_##n##6 : FC_##n##7)\n"
 "uint W_ci(uint st, constant U &u) { return SPEC ? PICK8(ci, st) : u.ci[st]; }\n"
 "uint W_ai(uint st, constant U &u) { return SPEC ? PICK8(ai, st) : u.ai[st]; }\n"
 "uint W_co(uint st, constant U &u) { return SPEC ? PICK8(co, st) : u.co[st]; }\n"
 "uint W_ao(uint st, constant U &u) { return SPEC ? PICK8(ao, st) : u.ao[st]; }\n"
 /* FLAGS: 1 alpha test, 2 blend, 4 dither, 8 add specular. */
 "uint K_cc(constant U &u) { return SPEC ? FC_CC : u.cc; }\n"
 "uint K_tmask(constant U &u) { return SPEC ? FC_TMASK : u.tmask; }\n"
 "uint K_control(constant U &u) { return SPEC ? FC_CONTROL : u.control; }\n"
 "bool K_atest(constant U &u) { return SPEC ? (FC_FLAGS & 1u) != 0 : u.alpha_test != 0; }\n"
 "bool K_blend(constant U &u) { return SPEC ? (FC_FLAGS & 2u) != 0 : u.blend != 0; }\n"
 "bool K_dither(constant U &u) { return SPEC ? (FC_FLAGS & 4u) != 0 : u.dither != 0; }\n"
 "bool K_spec(constant U &u) { return SPEC ? (FC_FLAGS & 8u) != 0 : u.add_spec != 0; }\n"
 "uint K_afunc(constant U &u) { return SPEC ? FC_AFUNC : u.alpha_func; }\n"
 "uint K_bsrc(constant U &u) { return SPEC ? FC_BSRC : u.bsrc; }\n"
 "uint K_bdst(constant U &u) { return SPEC ? FC_BDST : u.bdst; }\n"
 "uint K_beq(constant U &u) { return SPEC ? FC_BEQ : u.beq; }\n"
 "uint K_cmask(constant U &u) { return SPEC ? FC_CMASK : u.cmask; }\n"
 "uint K_lin(constant U &u) { return SPEC ? FC_LIN : u.lin_mask; }\n"
 "void stage(thread float4 *r, uint st, constant U &u) {\n"
 " uint ciw = W_ci(st, u), aiw = W_ai(st, u), cw = W_co(st, u), aw = W_ao(st, u);\n"
 " r[1] = unpack((K_control(u) & 0x1000u) ? u.k0[st] : u.k0[0]); r[2] = unpack((K_control(u) & 0x10000u) ? u.k1[st] : u.k1[0]);\n"
 " float4 ab, cd; for (uint k = 0; k < 4; k++) { uint word = k == 3 ? aiw : ciw; uint ch = k == 3 ? 2 : k;"
 " float a = inp(word >> 24, ch, r), b = inp((word >> 16) & 255, ch, r), c = inp((word >> 8) & 255, ch, r), d = inp(word & 255, ch, r);"
 " ab[k] = a * b; cd[k] = c * d; }\n"
 " float3 abr = ((cw >> 13) & 1) ? float3(ab.r + ab.g + ab.b) : ab.rgb;\n"
 " float3 cdr = ((cw >> 12) & 1) ? float3(cd.r + cd.g + cd.b) : cd.rgb;\n"
 " uint mx = (cw >> 14) & 1, mp = (cw >> 15) & 7;\n"
 " float3 sm = mx ? ((r[12].a >= 0.5f) ? abr : cdr) : (abr + cdr);\n"
 " abr = cmap3(mp, abr); cdr = cmap3(mp, cdr); sm = cmap3(mp, sm);\n"
 " uint dcd = cw & 15, dab = (cw >> 4) & 15, dsm = (cw >> 8) & 15;\n"
 " if (dcd) r[dcd].rgb = clamp(cdr, -1.0f, 1.0f); if (dab) r[dab].rgb = clamp(abr, -1.0f, 1.0f); if (dsm) r[dsm].rgb = clamp(sm, -1.0f, 1.0f);\n"
 " uint amx = (aw >> 14) & 1, amp = (aw >> 15) & 7, acd = aw & 15, aab = (aw >> 4) & 15, asum = (aw >> 8) & 15;\n"
 " if (acd) r[acd].a = clamp(cmap1(amp, cd.a), -1.0f, 1.0f);\n"
 " if (aab) r[aab].a = clamp(cmap1(amp, ab.a), -1.0f, 1.0f);\n"
 " if (asum) r[asum].a = clamp(cmap1(amp, amx ? ((r[12].a >= 0.5f) ? ab.a : cd.a) : (ab.a + cd.a)), -1.0f, 1.0f);\n"
 " if (((cw >> 19) & 1) && dab) r[dab].a = clamp(abr.b, -1.0f, 1.0f);\n"
 " if (((cw >> 18) & 1) && dcd) r[dcd].a = clamp(cdr.b, -1.0f, 1.0f); }\n"
 "float4 samp(texture2d<float> h, sampler q, float4 tc, uint unit, constant U &u) {\n"
 " float2 uv = tc.xy / tc.w; if ((K_lin(u) >> unit) & 1) uv /= float2(u.tw[unit], u.th[unit]);\n"
 " return h.sample(q, uv, bias(u.lod_bias[unit])); }\n"
 "bool cmpf(uint f, uint a, uint b) { switch (f) { case 0x200: return false; case 0x201: return a < b; case 0x202: return a == b;"
 " case 0x203: return a <= b; case 0x204: return a > b; case 0x205: return a != b; case 0x206: return a >= b; default: return true; } }\n"
 "float3 bf(uint f, float4 s, float3 d, float4 k) { switch (f) {\n"
 " case 0x000: return float3(0); case 0x001: return float3(1);\n"
 " case 0x300: return s.rgb; case 0x301: return 1 - s.rgb; case 0x302: return float3(s.a); case 0x303: return float3(1 - s.a);\n"
 " case 0x304: return float3(1); case 0x305: return float3(0);\n"          /* destination alpha: a 565 target has none, so 1 */
 " case 0x306: return d; case 0x307: return 1 - d; case 0x308: return float3(min(s.a, 0.0f));\n"
 " case 0x8001: return k.rgb; case 0x8002: return 1 - k.rgb; case 0x8003: return float3(k.a); case 0x8004: return float3(1 - k.a);\n"
 " default: return float3(0); } }\n"
 "#define H2D_FS_ARGS O i [[stage_in]], float4 dst [[color(0), raster_order_group(0)]], constant U &u [[buffer(0)]],"
 " texture2d<float> h0 [[texture(0)]], texture2d<float> h1 [[texture(1)]], texture2d<float> h2 [[texture(2)]], texture2d<float> h3 [[texture(3)]],"
 " sampler q0 [[sampler(0)]], sampler q1 [[sampler(1)]], sampler q2 [[sampler(2)]], sampler q3 [[sampler(3)]]\n"
 "inline float4 h2d_body(O i, float4 dst, constant U &u, texture2d<float> h0, texture2d<float> h1, texture2d<float> h2, texture2d<float> h3,"
 " sampler q0, sampler q1, sampler q2, sampler q3, bool zcull) {\n"
 " float4 r[14]; for (uint n = 0; n < 14; n++) r[n] = float4(0);\n"
 " uint tm = K_tmask(u); r[4] = i.d0; r[5] = i.d1;\n"
 " if (tm & 1) r[8] = samp(h0, q0, i.t0, 0, u); if (tm & 2) r[9] = samp(h1, q1, i.t1, 1, u);\n"
 " if (tm & 4) r[10] = samp(h2, q2, i.t2, 2, u); if (tm & 8) r[11] = samp(h3, q3, i.t3, 3, u);\n"
 " r[12].a = (tm & 1) ? r[8].a : 1;\n"
 " for (uint st = 0; st < K_cc(u); st++) stage(r, st, u);\n"
 " float4 c = clamp(r[12] + (K_spec(u) ? float4(r[5].rgb, 0) : float4(0)), 0.0f, 1.0f);\n"
 /* The executor's z-range policy for JSRF (CULL over 0..16777215). Under
  * MTLDepthClipModeClamp it cannot fire, which is what lets the early entry
  * drop it, as the executor's fs_hw_early does. */
 " if (zcull && (i.p.z < 0.0f || i.p.z > 1.0f)) { discard_fragment(); return c; }\n"
 " if (K_atest(u) && !cmpf(K_afunc(u), uint(clamp(c.a, 0.0f, 1.0f) * 255 + .5f), u.alpha_ref)) { discard_fragment(); return c; }\n"
 " if (K_blend(u)) { float3 d = dst.rgb; float4 k = unpack(u.bcolor); uint beq = K_beq(u);\n"
 "  float3 sf = c.rgb * bf(K_bsrc(u), c, d, k), df = d * bf(K_bdst(u), c, d, k);\n"
 "  c.rgb = beq == 0x800Au ? sf - df : beq == 0x800Bu ? df - sf : beq == 0x8007u ? min(c.rgb, d) : beq == 0x8008u ? max(c.rgb, d) : sf + df; }\n"
 " if (K_dither(u)) { constexpr uint b[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5}; int2 xy = int2(i.p.xy) + int2(u.ox, u.oy);\n"
 "  float bias = (float(b[(xy.y & 3) * 4 + (xy.x & 3)]) + .5f) / 16 - .5f; c.rgb += bias / float3(31, 63, 31); }\n"
 " uint cm = K_cmask(u); if (!(cm & 0x00FF0000u)) c.r = dst.r; if (!(cm & 0x0000FF00u)) c.g = dst.g; if (!(cm & 0x000000FFu)) c.b = dst.b;\n"
 " return float4(c.rgb, clamp(c.a, 0.0f, 1.0f)); }\n"
 "fragment float4 h2d_fs(H2D_FS_ARGS) { return h2d_body(i, dst, u, h0, h1, h2, h3, q0, q1, q2, q3, true); }\n"
 /* Early depth/stencil: chosen only for a draw that cannot discard (no
  * alpha test), or that may discard but writes neither depth nor stencil --
  * the executor's hw_early_z rule, exact in both cases. */
 "[[early_fragment_tests]] fragment float4 h2d_fs_early(H2D_FS_ARGS) { return h2d_body(i, dst, u, h0, h1, h2, h3, q0, q1, q2, q3, false); }\n";

static id<MTLDevice> s_dev;
static unsigned long long s_ns_texture, s_ns_external;      /* in-process timers for draw mode's report */
static unsigned long long s_vb_chunks;
static id<MTLCommandQueue> s_queue;
static id<MTLRenderPipelineState> s_pso, s_pso_st;
static id<MTLLibrary> s_lib;
static id<MTLFunction> s_vs;
static int s_spec_on = 1;                   /* d3d8_host_2d_metal_set_spec: tests compare the two */
static unsigned long long s_spec_built, s_spec_hits, s_spec_fallback, s_spec_compile_ns;
static id<MTLTexture> s_dummy;
static id<MTLDepthStencilState> s_dss[16];
/* Stencil states, built as hw_depth_state_for builds them, keyed by its fields. */
#define DSS_ST 32
static struct { uint32_t k[9]; id<MTLDepthStencilState> dss; } s_dss_st[DSS_ST];
static unsigned s_dss_st_n;
static id<MTLDepthStencilState> depth_state(const D3D8Host2DDraw *d, int with_stencil)
{
    int cmp = d->depth_test ? nv2a_metal_compare_func(d->depth_func) : NV2A_MTL_CMP_ALWAYS;
    unsigned wr = d->depth_test && d->depth_write, key;
    if (cmp < 0) cmp = NV2A_MTL_CMP_ALWAYS;           /* as the executor: unrecognised is ALWAYS */
    if (with_stencil && d->stencil_test) {
        uint32_t k[9] = { (uint32_t)cmp, wr, d->stencil_func, d->stencil_fail, d->stencil_zfail, d->stencil_zpass,
                          d->stencil_func_mask & 255u, d->stencil_write ? (d->stencil_mask & 255u) : 0u, 1u };
        for (unsigned i = 0; i < s_dss_st_n; ++i) if (!memcmp(s_dss_st[i].k, k, sizeof k)) return s_dss_st[i].dss;
        int sc = nv2a_metal_compare_func(d->stencil_func), f = nv2a_metal_stencil_op(d->stencil_fail),
            zf = nv2a_metal_stencil_op(d->stencil_zfail), zp = nv2a_metal_stencil_op(d->stencil_zpass);
        if (sc < 0 || f < 0 || zf < 0 || zp < 0) return nil;            /* the build refused these already */
        MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = (MTLCompareFunction)cmp;
        ds.depthWriteEnabled = wr ? YES : NO;
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = (MTLCompareFunction)sc;
        sd.stencilFailureOperation = (MTLStencilOperation)f;
        sd.depthFailureOperation = (MTLStencilOperation)zf;
        sd.depthStencilPassOperation = (MTLStencilOperation)zp;
        sd.readMask = k[6]; sd.writeMask = k[7];
        ds.frontFaceStencil = sd; ds.backFaceStencil = sd;
        id<MTLDepthStencilState> dss = [s_dev newDepthStencilStateWithDescriptor:ds];
        if (dss && s_dss_st_n < DSS_ST) { memcpy(s_dss_st[s_dss_st_n].k, k, sizeof k); s_dss_st[s_dss_st_n++].dss = dss; }
        return dss;
    }
    key = ((unsigned)cmp & 7u) | (wr << 3);
    if (!s_dss[key]) {
        MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = (MTLCompareFunction)cmp;
        ds.depthWriteEnabled = wr ? YES : NO;
        s_dss[key] = [s_dev newDepthStencilStateWithDescriptor:ds];
    }
    return s_dss[key];
}
static id<MTLSamplerState> s_samplers[128];
static const char *s_err;
static pthread_mutex_t s_init_mu = PTHREAD_MUTEX_INITIALIZER;
static int s_init_done;

const char *d3d8_host_2d_metal_last_error(void) { return s_err ? s_err : "none"; }
static int fail(const char *why) { s_err = why; return -1; }

static int init(void)
{
    pthread_mutex_lock(&s_init_mu);
    if (s_init_done) { pthread_mutex_unlock(&s_init_mu); return s_pso != nil; }
    s_init_done = 1;
    @autoreleasepool {
        NSError *err = nil;
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { s_err = "host 2d: no Metal device"; goto out; }
        s_queue = [s_dev newCommandQueue];
        MTLCompileOptions *opt = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe; else opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        id<MTLLibrary> lib = [s_dev newLibraryWithSource:k_src options:opt error:&err];
        if (!lib) {
            fprintf(stderr, "[D3D8-HOST-2D] shader compile failed: %s\n", err ? err.localizedDescription.UTF8String : "?");
            s_err = "host 2d: shader compile"; goto out;
        }
        s_lib = lib; s_vs = [lib newFunctionWithName:@"h2d_vs"];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = s_vs;
        {   /* The generic interpreter: a function that reads function
             * constants must be fetched with values, so SPEC is set false. */
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            bool off = false;
            [cv setConstantValue:&off type:MTLDataTypeBool atIndex:0];
            pd.fragmentFunction = [lib newFunctionWithName:@"h2d_fs" constantValues:cv error:&err];
        }
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatB5G6R5Unorm;
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        s_pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        /* Draw mode's: the executor's hardware path attaches Stencil8 beside depth. */
        pd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
        s_pso_st = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_pso || !s_pso_st) {
            fprintf(stderr, "[D3D8-HOST-2D] pipeline failed: %s\n", err ? err.localizedDescription.UTF8String : "?");
            s_err = "host 2d: pipeline"; goto out;
        }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        s_dummy = [s_dev newTextureWithDescriptor:td];
        uint32_t z = 0; [s_dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
    }
out:
    pthread_mutex_unlock(&s_init_mu);
    return s_pso != nil;
}

/* The executor's hw_sampler() choices, from D3D's stage state:
 * magnification linear on MAGFILTER 2; minification linear when the min
 * field is even; mips only when a mip filter is set and there are levels. */
static unsigned mip_levels(const D3D8H2DTexture *t) { return (t->min_filter >= 3 && t->levels >= 2) ? t->levels : 1; }
static id<MTLSamplerState> sampler_for(const D3D8H2DTexture *t)
{
    unsigned mipped = mip_levels(t) > 1;
    unsigned key = (t->mag == 2) | ((t->min_filter & 7u) << 1) | (mipped << 4) | ((t->wrap_u & 3u) << 5);
    unsigned k2 = key | ((t->wrap_v & 3u) << 7);
    unsigned slot = (k2 ^ (k2 >> 7)) & 127u;
    static unsigned keys[128];
    if (s_samplers[slot] && keys[slot] == k2 + 1u) return s_samplers[slot];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = t->mag == 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (t->min_filter & 1u) == 0 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = !mipped ? MTLSamplerMipFilterNotMipmapped : t->min_filter >= 5 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    MTLSamplerAddressMode m[4] = { MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                   MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToEdge };
    d.sAddressMode = m[t->wrap_u & 3u]; d.tAddressMode = m[t->wrap_v & 3u];
    d.normalizedCoordinates = YES;
    keys[slot] = k2 + 1u;
    return s_samplers[slot] = [s_dev newSamplerStateWithDescriptor:d];
}

/* DECODED TEXTURES, keyed by what D3D names (Data, Format, Size) and
 * validated by a hash of the bytes -- a render target used as a texture
 * changes under the same address.
 *
 * SIZED FOR A 3D FRAME. The 2D shadow needed a handful; fixed-function draw
 * mode touches every texture of the scene each frame, and a 32-entry LRU
 * under that load re-decoded and re-uploaded textures on nearly every draw.
 * So: up to TEX_CACHE entries within TEX_BUDGET bytes, and each entry's bytes
 * are hashed at most ONCE PER FLIP (a word-wise hash, 8 bytes a step), not
 * once per draw. A texture rewritten mid-frame after its first use that
 * frame is the one case this misses, and the shadow comparison is where it
 * would show. */
#define TEX_CACHE 512
#define TEX_BUDGET ((size_t)256u << 20)
static struct { uint32_t addr, fmt, size; uint64_t hash; unsigned long long used, checked; size_t bytes; id<MTLTexture> tex; } s_tc[TEX_CACHE];
static unsigned long long s_tc_clock, s_tc_hits, s_tc_builds, s_tc_hashes;
static size_t s_tc_bytes;
static uint64_t hash64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull, w;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
unsigned long long d3d8_host_2d_flip_count(void);
static id<MTLTexture> texture_for(const D3D8H2DTexture *t, const uint8_t *ram, size_t ram_size)
{
    unsigned mips = mip_levels(t), w = t->width, h = t->height, pitch = t->pitch;
    size_t total = 0;
    unsigned long long flip = d3d8_host_2d_flip_count() + 1u;       /* 0 never matches a fresh entry */
    for (unsigned l = 0; l < mips; ++l) {
        total += nv2a_texture_level_bytes((int)t->fmt, w, h, pitch);
        pitch = nv2a_texture_next_pitch((int)t->fmt, w, pitch); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    if (!total || (uint64_t)t->addr + total > ram_size) { s_err = "host 2d: texture bounds"; return nil; }
    const uint8_t *src = ram + t->addr;
    int slot = -1;
    uint64_t hash = 0;
    for (unsigned i = 0; i < TEX_CACHE; ++i)
        if (s_tc[i].tex && s_tc[i].addr == t->addr && s_tc[i].fmt == t->d3d_format && s_tc[i].size == t->d3d_size) { slot = (int)i; break; }
    if (slot >= 0) {
        if (s_tc[slot].checked == flip) { s_tc[slot].used = ++s_tc_clock; ++s_tc_hits; return s_tc[slot].tex; }
        hash = hash64(src, total); ++s_tc_hashes;
        if (s_tc[slot].hash == hash) { s_tc[slot].checked = flip; s_tc[slot].used = ++s_tc_clock; ++s_tc_hits; return s_tc[slot].tex; }
        s_tc_bytes -= s_tc[slot].bytes; s_tc[slot].tex = nil;            /* rewritten: rebuild in place */
    } else {
        hash = hash64(src, total); ++s_tc_hashes;
        size_t need = (size_t)t->width * t->height * 4u * (mips > 1 ? 2u : 1u);
        for (;;) {                                                         /* a free entry within the budget */
            int freei = -1, lru = -1;
            for (unsigned i = 0; i < TEX_CACHE; ++i) {
                if (!s_tc[i].tex) { if (freei < 0) freei = (int)i; continue; }
                if (lru < 0 || s_tc[i].used < s_tc[lru].used) lru = (int)i;
            }
            if (freei >= 0 && s_tc_bytes + need <= TEX_BUDGET) { slot = freei; break; }
            if (lru < 0) { slot = freei >= 0 ? freei : 0; break; }
            s_tc_bytes -= s_tc[lru].bytes; s_tc[lru].tex = nil;
        }
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:t->width height:t->height mipmapped:NO];
    d.mipmapLevelCount = mips; d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [s_dev newTextureWithDescriptor:d];
    uint8_t *rgba = malloc((size_t)t->width * t->height * 4u);
    size_t off = 0, bytes = 0;
    w = t->width; h = t->height; pitch = t->pitch;
    int ok = tex && rgba;
    for (unsigned l = 0; ok && l < mips; ++l) {
        size_t level = nv2a_texture_level_bytes((int)t->fmt, w, h, pitch);
        if (!nv2a_texture_decode_rgba8(src + off, total - off, w, h, pitch, (int)t->fmt, rgba)) { ok = 0; break; }
        [tex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l withBytes:rgba bytesPerRow:(NSUInteger)w * 4u];
        bytes += (size_t)w * h * 4u;
        off += level; pitch = nv2a_texture_next_pitch((int)t->fmt, w, pitch); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    free(rgba);
    if (!ok) { s_err = "host 2d: texture decode"; return nil; }
    s_tc[slot].addr = t->addr; s_tc[slot].fmt = t->d3d_format; s_tc[slot].size = t->d3d_size; s_tc[slot].bytes = bytes;
    s_tc[slot].hash = hash; s_tc[slot].checked = flip; s_tc[slot].used = ++s_tc_clock; s_tc[slot].tex = tex; ++s_tc_builds;
    s_tc_bytes += bytes;
    return tex;
}

/* The pipeline for a draw: specialised on its fragment shape (see SPECIALISATION
 * in the shader), with or without the Stencil8 attachment, early or late
 * tests. Cached, never evicted; a full cache or a failed compile falls back to
 * the generic interpreter, counted. */
typedef struct {
    uint32_t cc, tmask, flags, control, afunc, bsrc, bdst, beq, cmask, lin;
    uint32_t ci[8], ai[8], co[8], ao[8];
    uint32_t stencil, early;
} H2DSpecKey;
#define SPEC_CACHE 1024
static struct { H2DSpecKey k; uint64_t h; id<MTLRenderPipelineState> pso; } s_spec[SPEC_CACHE];
static unsigned s_spec_n;
void d3d8_host_2d_metal_set_spec(int on) { s_spec_on = on; }
void d3d8_host_2d_metal_spec_stats(unsigned long long *built, unsigned long long *hits, unsigned long long *fallback,
                                   unsigned long long *compile_ns)
{
    if (built) *built = s_spec_built;
    if (hits) *hits = s_spec_hits;
    if (fallback) *fallback = s_spec_fallback;
    if (compile_ns) *compile_ns = s_spec_compile_ns;
}
static int draw_early(const D3D8Host2DDraw *d)
{
    int writes = (d->depth_test && d->depth_write) || (d->stencil_test && d->stencil_write && (d->stencil_mask & 255u));
    return !d->alpha_test || !writes;
}
static id<MTLRenderPipelineState> pipeline_for(const D3D8Host2DDraw *d, int with_stencil, uint32_t lin_mask)
{
    H2DSpecKey k;
    uint64_t h = 1469598103934665603ull;
    if (!s_spec_on) return with_stencil ? s_pso_st : s_pso;
    memset(&k, 0, sizeof k);
    k.cc = d->cc; k.tmask = d->tmask; k.control = d->control;
    k.flags = (d->alpha_test ? 1u : 0u) | (d->blend ? 2u : 0u) | (d->dither ? 4u : 0u) | (d->add_specular ? 8u : 0u);
    k.afunc = d->alpha_test ? d->alpha_func : 0; k.lin = lin_mask;
    if (d->blend) { k.bsrc = d->blend_src; k.bdst = d->blend_dst; k.beq = d->blend_eq; }
    k.cmask = d->color_mask;
    for (unsigned i = 0; i < 8 && i < d->cc; ++i) { k.ci[i] = d->ci[i]; k.ai[i] = d->ai[i]; k.co[i] = d->co[i]; k.ao[i] = d->ao[i]; }
    k.stencil = with_stencil != 0; k.early = (uint32_t)draw_early(d);
    { const uint8_t *b = (const uint8_t *)&k; for (size_t i = 0; i < sizeof k; ++i) { h ^= b[i]; h *= 1099511628211ull; } }
    for (unsigned i = 0; i < s_spec_n; ++i)
        if (s_spec[i].h == h && !memcmp(&s_spec[i].k, &k, sizeof k)) {
            if (!s_spec[i].pso) break;
            ++s_spec_hits; return s_spec[i].pso;
        }
    if (s_spec_n >= SPEC_CACHE) { ++s_spec_fallback; return with_stencil ? s_pso_st : s_pso; }
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    id<MTLRenderPipelineState> pso = nil;
    @autoreleasepool {
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        bool on = true;
        [cv setConstantValue:&on type:MTLDataTypeBool atIndex:0];
        [cv setConstantValue:&k.cc type:MTLDataTypeUInt atIndex:1];
        [cv setConstantValue:&k.tmask type:MTLDataTypeUInt atIndex:2];
        [cv setConstantValue:&k.flags type:MTLDataTypeUInt atIndex:3];
        [cv setConstantValue:&k.control type:MTLDataTypeUInt atIndex:4];
        [cv setConstantValue:&k.afunc type:MTLDataTypeUInt atIndex:5];
        [cv setConstantValue:&k.bsrc type:MTLDataTypeUInt atIndex:6];
        [cv setConstantValue:&k.bdst type:MTLDataTypeUInt atIndex:7];
        [cv setConstantValues:k.ci type:MTLDataTypeUInt withRange:NSMakeRange(8, 8)];
        [cv setConstantValues:k.ai type:MTLDataTypeUInt withRange:NSMakeRange(16, 8)];
        [cv setConstantValues:k.co type:MTLDataTypeUInt withRange:NSMakeRange(24, 8)];
        [cv setConstantValues:k.ao type:MTLDataTypeUInt withRange:NSMakeRange(32, 8)];
        [cv setConstantValue:&k.beq type:MTLDataTypeUInt atIndex:40];
        [cv setConstantValue:&k.cmask type:MTLDataTypeUInt atIndex:41];
        [cv setConstantValue:&k.lin type:MTLDataTypeUInt atIndex:42];
        NSError *err = nil;
        id<MTLFunction> fn = [s_lib newFunctionWithName:k.early ? @"h2d_fs_early" : @"h2d_fs" constantValues:cv error:&err];
        if (fn) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = s_vs; pd.fragmentFunction = fn;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatB5G6R5Unorm;
            pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            if (with_stencil) pd.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
            pso = [s_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        }
        if (!pso) {
            static int told;
            if (!told++) fprintf(stderr, "[D3D8-HOST-2D] specialised pipeline failed: %s\n", err ? err.localizedDescription.UTF8String : "?");
        }
    }
    s_spec_compile_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    s_spec[s_spec_n].k = k; s_spec[s_spec_n].h = h; s_spec[s_spec_n].pso = pso; ++s_spec_n;
    if (!pso) { ++s_spec_fallback; return with_stencil ? s_pso_st : s_pso; }
    ++s_spec_built;
    return pso;
}

/* Encode `d` into a pass whose attachments are W x H and whose top-left is
 * (ox, oy) in target space: the shadow's crop, or (0, 0) and the whole
 * surface in draw mode. Returns 1 if encoded (nothing inside the scissor is
 * encoded as nothing), 0 on failure with s_err set. */
static int encode_draw(id<MTLRenderCommandEncoder> enc, int with_stencil, const D3D8Host2DDraw *d,
                       const uint8_t *ram, size_t ram_size, unsigned W, unsigned H, unsigned ox, unsigned oy)
{
    int32_t sx0 = d->sc_x0 > (int32_t)ox ? d->sc_x0 : (int32_t)ox, sy0 = d->sc_y0 > (int32_t)oy ? d->sc_y0 : (int32_t)oy;
    int32_t sx1 = d->sc_x1 < (int32_t)(ox + W - 1) ? d->sc_x1 : (int32_t)(ox + W - 1);
    int32_t sy1 = d->sc_y1 < (int32_t)(oy + H - 1) ? d->sc_y1 : (int32_t)(oy + H - 1);
    H2DUniforms u;
    id<MTLTexture> tex[4] = { nil, nil, nil, nil };
    id<MTLSamplerState> smp[4] = { nil, nil, nil, nil };
    if (!d->nverts || sx1 < sx0 || sy1 < sy0) return 1;
    memset(&u, 0, sizeof u);
    u.W = (float)W; u.H = (float)H; u.ox = ox; u.oy = oy;
    u.cc = d->cc; u.control = d->control; u.tmask = d->tmask; u.add_spec = d->add_specular;
    u.alpha_test = d->alpha_test; u.alpha_func = d->alpha_func; u.alpha_ref = d->alpha_ref;
    u.blend = d->blend; u.bsrc = d->blend_src; u.bdst = d->blend_dst; u.beq = d->blend_eq; u.bcolor = d->blend_color;
    u.dither = d->dither; u.cmask = d->color_mask;
    memcpy(u.ci, d->ci, sizeof u.ci); memcpy(u.ai, d->ai, sizeof u.ai);
    memcpy(u.co, d->co, sizeof u.co); memcpy(u.ao, d->ao, sizeof u.ao);
    memcpy(u.k0, d->k0, sizeof u.k0); memcpy(u.k1, d->k1, sizeof u.k1);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (unsigned s = 0; s < 4; ++s) {
        if (!(d->tmask & (1u << s))) continue;
        const D3D8H2DTexture *t = &d->tex[s];
        if (!(tex[s] = texture_for(t, ram, ram_size))) return 0;
        smp[s] = sampler_for(t);
        u.tw[s] = (float)t->width; u.th[s] = (float)t->height; u.lod_bias[s] = t->lod_bias;
        if (t->linear) u.lin_mask |= 1u << s;
    }
    s_ns_texture += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    size_t vbytes = (size_t)d->nverts * sizeof(D3D8H2DVertex), voff = 0;
    id<MTLBuffer> vb = nil;
    if (vbytes > 4096) {                     /* Metal's inline limit; below it, setVertexBytes */
        /* A BUMP ALLOCATOR, not a buffer per draw. An FF draw carries ~90 KB
         * of vertices, and allocating shared memory for each was most of the
         * ~40 us a replaced draw spent outside texture lookup (G51.3 run,
         * 24 Sep 2026). Each draw takes the next slice of an 8 MB chunk; a
         * full chunk is dropped for a fresh one, and the encoders that
         * reference the old one keep it alive until their GPU work is done,
         * so no slice is ever rewritten while in flight. */
        enum { CHUNK = 8u << 20 };
        static id<MTLBuffer> chunk;
        static size_t used;
        size_t need = (vbytes + 255u) & ~(size_t)255u;
        if (need > CHUNK) {
            vb = [s_dev newBufferWithBytes:d->verts length:(NSUInteger)vbytes options:MTLResourceStorageModeShared];
        } else {
            if (!chunk || used + need > CHUNK) {
                chunk = [s_dev newBufferWithLength:CHUNK options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
                used = 0; ++s_vb_chunks;
            }
            if (chunk) { memcpy((uint8_t *)chunk.contents + used, d->verts, vbytes); vb = chunk; voff = used; used += need; }
        }
        if (!vb) { s_err = "host 2d: vertex buffer"; return 0; }
    }
    [enc setRenderPipelineState:pipeline_for(d, with_stencil, u.lin_mask)];
    {   /* The stencil unit only where the pass has one (draw mode); the
         * shadow's colour comparison does not depend on it under ALWAYS. */
        id<MTLDepthStencilState> dss = depth_state(d, with_stencil);
        if (!dss) { s_err = "host 2d: depth-stencil state"; return 0; }
        [enc setDepthStencilState:dss];
        if (with_stencil && d->stencil_test) [enc setStencilReferenceValue:d->stencil_ref & 255u];
    }
    [enc setDepthClipMode:MTLDepthClipModeClamp];
    [enc setCullMode:MTLCullModeNone];
    MTLScissorRect sc = { (NSUInteger)(sx0 - (int32_t)ox), (NSUInteger)(sy0 - (int32_t)oy),
                          (NSUInteger)(sx1 - sx0 + 1), (NSUInteger)(sy1 - sy0 + 1) };
    [enc setScissorRect:sc];
    if (vb) [enc setVertexBuffer:vb offset:voff atIndex:0];
    else [enc setVertexBytes:d->verts length:(NSUInteger)vbytes atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    for (unsigned s = 0; s < 4; ++s) {
        [enc setFragmentTexture:tex[s] ? tex[s] : s_dummy atIndex:s];
        [enc setFragmentSamplerState:smp[s] ? smp[s] : sampler_for(&(D3D8H2DTexture){ .mag = 1, .min_filter = 1, .wrap_u = 3, .wrap_v = 3 }) atIndex:s];
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:d->nverts];
    return 1;
}

int d3d8_host_2d_metal_render(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size,
                              uint16_t *pixels, unsigned pitch_px, float *depth,
                              unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    if (!d || !pixels || !w || !h || pitch_px < w) return fail("host 2d: bad arguments");
    if (d->depth_test && !depth) return fail("host 2d: depth test without the depth it starts from");
    if (!init()) return -1;
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatB5G6R5Unorm
                                                                                       width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [s_dev newTextureWithDescriptor:td];
        if (!target) return fail("host 2d: target allocation");
        [target replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)pitch_px * 2u];
        MTLTextureDescriptor *zd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                       width:w height:h mipmapped:NO];
        zd.usage = MTLTextureUsageRenderTarget; zd.storageMode = MTLStorageModeShared;
        id<MTLTexture> ztex = [s_dev newTextureWithDescriptor:zd];
        if (!ztex) return fail("host 2d: depth allocation");
        if (depth) [ztex replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:depth bytesPerRow:(NSUInteger)w * 4u];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = ztex;
        pass.depthAttachment.loadAction = depth ? MTLLoadActionLoad : MTLLoadActionClear;
        pass.depthAttachment.clearDepth = 1.0;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [s_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        if (!cb || !enc) return fail("host 2d: command encoder");
        int ok = encode_draw(enc, 0, d, ram, ram_size, w, h, x0, y0);
        [enc endEncoding];
        if (!ok) return -1;
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) return fail("host 2d: GPU error");
        [target getBytes:pixels bytesPerRow:(NSUInteger)pitch_px * 2u fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        if (depth) [ztex getBytes:depth bytesPerRow:(NSUInteger)w * 4u fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    }
    return 0;
}

/* ---- draw mode: into the executor's bound surface ---- */
void d3d8_host_2d_metal_stats(unsigned long long *tex_hits, unsigned long long *tex_builds, unsigned long long *tex_hashes,
                              unsigned long long *ns_texture, unsigned long long *ns_external)
{
    if (tex_hits) *tex_hits = s_tc_hits;
    if (tex_builds) *tex_builds = s_tc_builds;
    if (tex_hashes) *tex_hashes = s_tc_hashes;
    if (ns_texture) *ns_texture = s_ns_texture;
    if (ns_external) *ns_external = s_ns_external;
}
static unsigned long long s_binds;
unsigned long long d3d8_host_2d_metal_binds(void) { return s_binds; }
typedef struct { const D3D8Host2DDraw *d; const uint8_t *ram; size_t ram_size; int ok; } ExtCtx;
static int external_encode(void *encoder, unsigned w, unsigned h, void *ctx)
{
    ExtCtx *x = ctx;
    id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)encoder;
    if (w != x->d->rt_w || h != x->d->rt_h) { s_err = "executor surface is not the render target's size"; return 0; }
    return x->ok = encode_draw(enc, 1, x->d, x->ram, x->ram_size, w, h, 0, 0);
}
int d3d8_host_2d_metal_external(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size)
{
    ExtCtx x = { d, ram, ram_size, 0 };
    int drawn;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!init()) return 0;
    @autoreleasepool {
        int uses_zs = d->depth_test || d->stencil_test;
        int writes_zs = (d->depth_test && d->depth_write) || (d->stencil_test && d->stencil_write && (d->stencil_mask & 255u));
        uint8_t *zs = uses_zs ? (uint8_t *)ram + d->zs_addr : NULL;
        drawn = nv2a_metal_external_draw(ram + d->rt_addr, zs, writes_zs, external_encode, &x);
        /* Not bound: bind it the way the executor's own draw into it would
         * (nv2a_metal_bind is that code, shared), then draw. The executor's
         * next draw into this target finds it bound, as after its own swap. */
        if (drawn == -2 || drawn == -3 || drawn == -4) {
            size_t tsz = (size_t)d->rt_pitch * d->rt_h, zsz = (size_t)d->zs_pitch * d->rt_h;
            if (nv2a_metal_bind((uint8_t *)ram + d->rt_addr, tsz, d->rt_w, d->rt_h, d->rt_pitch, zs, d->zs_pitch, zsz,
                                uses_zs) == 0) {
                ++s_binds;
                drawn = nv2a_metal_external_draw(ram + d->rt_addr, zs, writes_zs, external_encode, &x);
            }
        }
    }
    switch (drawn) {
    case 1:  break;
    case -1: s_err = "executor not on the hardware 565 path"; break;
    case -2: s_err = "target not bound: no valid executor surface"; break;
    case -3: s_err = "target not bound: executor holds another colour target"; break;
    case -4: s_err = "target not bound: executor holds another depth surface"; break;
    default: if (x.ok) s_err = "host encode declined"; break;
    }
    s_ns_external += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    return drawn == 1 && x.ok;
}
