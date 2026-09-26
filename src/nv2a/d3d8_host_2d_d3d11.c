/* The D3D lift's host renderer on Windows: d3d8_host_2d_metal.m's
 * counterpart, drawing through Direct3D 11.
 *
 * WHAT IS SHARED AND WHAT IS NOT. Everything that decides WHAT a draw is --
 * the mirror, the classes, the vertex fetch, the fixed-function unit, points
 * and lines, Begin/End, stencil, the fog tables, the refusal census -- is
 * d3d8_host.c / d3d8_host_2d.c and runs unchanged on both hosts. This file is
 * only the last step: a D3D8Host2DDraw becomes pixels. It keeps the Metal
 * host's contract function for function (render, external, the stats), so
 * main.c registers one or the other and d3d8_host_2d.c cannot tell which.
 *
 * THE PIXELS FOLLOW THE METAL HOST, THE SURFACES FOLLOW THE D3D11 EXECUTOR.
 * The fragment program below is h2d_body (d3d8_host_2d_metal.m) statement
 * for statement: the same combiner stages in the same order, the same final
 * combiner and fog, the same alpha test and z-range discard, the same MUX and
 * FACTOR rules. It is emitted per state as straight-line HLSL, named
 * registers only, because the bottle's d3dcompiler_47 (Wine's, over
 * vkd3d-shader) crashes on a module-scope array and the Metal host's
 * interpreter is one big array (see nv2a_d3d11.c's note on build_shaders).
 * What cannot follow the Metal host is where the hardware differs:
 *   - The target. The host draws into the D3D11 executor's own retained
 *     surface (nv2a_d3d11_external_draw), which is R8G8B8A8 over D24S8, not
 *     Metal's B5G6R5 over Depth32Float/Stencil8. Guest RAM sees it through
 *     the executor's read-back (truncating to 565), exactly as it sees the
 *     executor's own draws.
 *   - Blending is fixed-function state, as in the executor: a D3D11 shader
 *     cannot read the destination. The factors are the Metal host's bf():
 *     destination alpha reads 1 (a 565 target has none), SRC_ALPHA_SATURATE
 *     0, and the constant colour through the blend factor. So the dither
 *     lands before the blend, as the D3D11 executor's does.
 *   - The z-range cull reads z from its own screen-linear varying rather
 *     than SV_Position.z, whose clamping with depth clip off is not the same
 *     on every D3D11 implementation (D3DMetal, DXVK, Windows).
 * Vertex programs (class 3) run on the CPU with the executor's own
 * interpreter (nv2a_vsh_execute), as the D3D11 executor runs them: there is
 * no D3D11 twin of the Metal executor's VSH->MSL vertex function. The
 * GPU fixed-function unit (RECOMP_D3D8_HOST_FF_GPU) has no D3D11 twin either,
 * so main.c registers none and the fixed-function class keeps its CPU unit.
 *
 * NOT PORTED, REFUSED to the executor with a reason in the census: a
 * BUMPENVMAP unit (RECOMP_D3D8_HOST_BUMP) and a linear 32-bit texture
 * (RECOMP_D3D8_HOST_LIN32), which the Metal host samples from guest bytes
 * with the executor's buffer sampler (nv2a_metal_sample_msl.h). The D3D11
 * executor refuses bump units itself, so those go on to the CPU rasteriser. */
#include "d3d8_internal.h"
#include "../recomp_switch.h"
#include <d3dcompiler.h>
#include "d3d8_host_2d.h"
#include "nv2a_d3d11.h"
#include "nv2a_texture_decode.h"
#include "nv2a_texture_copy.h"
#include "nv2a_vsh.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static const char *s_err;
const char *d3d8_host_2d_d3d11_last_error(void) { return s_err ? s_err : "none"; }
static int fail(const char *why) { s_err = why; return -1; }

static unsigned long long now_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

/* ---- device objects ---------------------------------------------------- */
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_cx;
static ID3D11VertexShader *s_vs;
static ID3D11InputLayout *s_layout;
static ID3D11RasterizerState *s_rs;
static ID3D11Buffer *s_cb, *s_vb;
static ID3D11ShaderResourceView *s_dummy_srv;
static ID3D11SamplerState *s_dummy_smp;
static int s_init_done, s_init_ok;
#define VB_BYTES (16u << 20)
static unsigned s_vb_used;

/* One constant buffer for both stages, 16-byte groups, mirrored in the HLSL. */
typedef struct {
    float vp[4];            /* W, H, ox, oy */
    uint32_t misc[4];       /* alpha_ref, fog_enable, fog_mode, fog_color */
    uint32_t sf[4];         /* final-combiner constants sf0, sf1 */
    float fogp[4];          /* fog_p0, fog_p1 */
    float tsz[4][4];        /* texel sizes of a pitch-linear unit: tw, th */
    uint32_t k0[8], k1[8];  /* combiner constants per stage */
} HCB;

static const char *const k_common =
"cbuffer H : register(b0) { float4 vp; uint4 misc; uint4 sf; float4 fogp; float4 tsz[4]; uint4 k0[2]; uint4 k1[2]; };\n"
"struct VO { float4 p:SV_POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"  float fog:TEXCOORD4; noperspective float zc:TEXCOORD5; };\n";

/* Target-space pixels to clip space over the pass, as h2d_vs. zc is the
 * vertex's own z, interpolated linearly in screen space: what Metal's
 * [[position]].z is for the z-range cull. */
static const char *const k_vs_body =
"struct VI { float4 p:POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; float4 f:TEXCOORD4; };\n"
"VO h2d_vs(VI x) { VO o; float w = x.p.w;\n"
"  o.p = float4(((x.p.x - vp.z) / vp.x * 2 - 1) * w, (1 - (x.p.y - vp.w) / vp.y * 2) * w, x.p.z * w, w);\n"
"  o.d0 = x.d0; o.d1 = x.d1; o.t0 = x.t0; o.t1 = x.t1; o.t2 = x.t2; o.t3 = x.t3; o.fog = x.f.x; o.zc = x.p.z;\n"
"  return o; }\n";

/* The helpers h2d_body calls, operation for operation (d3d8_host_2d_metal.m).
 * The bottle's compiler has no isfinite() (E5005); !(|d| <= FLT_MAX) is the
 * same test, NaN and both infinities failing the comparison. */
static const char *const k_ps_helpers =
"float4 unpack(uint k) { return float4(float((k >> 16) & 255), float((k >> 8) & 255), float(k & 255), float((k >> 24) & 255)) / 255.0; }\n"
"float fog_factor(uint mode, float p0, float p1, float d) { float f;\n"
"  if (mode == 0x804 || mode == 0x802 || mode == 0x803) d = abs(d);\n"
"  if (mode == 0x2601 || mode == 0x804) { if (!(abs(d) <= 3.402823466e38)) d = 0; f = p0 + d * p1 - 1.0; }\n"
"  else if (mode == 0x800 || mode == 0x802) { if (!(abs(d) <= 3.402823466e38)) d = 0; f = p0 + exp2(d * p1 * 16.0) - 1.5; }\n"
"  else if (mode == 0x801 || mode == 0x803) { f = p0 + exp2(-d * d * p1 * p1 * 32.0) - 1.5; }\n"
"  else return 1.0;\n"
"  return f < 0 ? 0 : (f > 1 ? 1 : (f >= 0 ? f : 0)); }\n";

static int compile(const char *src, const char *entry, const char *profile, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "d3d8_host_2d", NULL, NULL, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
    if (FAILED(hr)) {
        static int told;
        if (told++ < 4)
            fprintf(stderr, "[D3D8-HOST-2D] D3D11 %s compile failed: 0x%08X %s\n--- HLSL ---\n%s\n", entry,
                    (unsigned)hr, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "", src);
        RELEASE(errors);
        return 0;
    }
    RELEASE(errors);
    return 1;
}

static int init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,   0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    char src[4096];
    ID3DBlob *vs = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    if (s_init_done) return s_init_ok;
    s_init_done = 1;
    if (sizeof(D3D8H2DVertex) != 128) { s_err = "host d3d11: vertex layout"; return 0; }
    if (!nv2a_d3d11_ready()) { s_err = "host d3d11: the executor's D3D11 pipeline is not up (RECOMP_D3D11)"; return 0; }
    s_dev = d3d8_GetD3D11Device(); s_cx = d3d8_GetD3D11Context();
    if (!s_dev || !s_cx) { s_err = "host d3d11: no device"; return 0; }
    snprintf(src, sizeof src, "%s%s", k_common, k_vs_body);
    if (!compile(src, "h2d_vs", "vs_4_0", &vs)) { s_err = "host d3d11: vertex shader compile"; return 0; }
    if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &s_vs))
        || FAILED(ID3D11Device_CreateInputLayout(s_dev, el, 8, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), &s_layout))) {
        RELEASE(vs); s_err = "host d3d11: vertex shader objects"; return 0;
    }
    RELEASE(vs);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (sizeof(HCB) + 15u) & ~15u; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb))) { s_err = "host d3d11: constant buffer"; return 0; }
    bd.ByteWidth = VB_BYTES; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) { s_err = "host d3d11: vertex buffer"; return 0; }
    /* The Metal host's encoder: no culling (the build culled, as the
     * executor does), depth clamped rather than clipped, the scissor on. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE; rd.ScissorEnable = TRUE;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs))) { s_err = "host d3d11: rasterizer state"; return 0; }
    {   /* Unit slots the program does not declare still get a view and a sampler. */
        D3D11_TEXTURE2D_DESC td; D3D11_SUBRESOURCE_DATA sd; D3D11_SAMPLER_DESC smd;
        ID3D11Texture2D *t = NULL; uint32_t z = 0;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = &z; sd.SysMemPitch = 4; sd.SysMemSlicePitch = 4;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t))
            || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &s_dummy_srv))) {
            RELEASE(t); s_err = "host d3d11: dummy texture"; return 0;
        }
        RELEASE(t);
        memset(&smd, 0, sizeof smd);
        smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smd.MaxAnisotropy = 1; smd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &smd, &s_dummy_smp))) { s_err = "host d3d11: dummy sampler"; return 0; }
    }
    fprintf(stderr, "[D3D8-HOST-2D] D3D11 host renderer ready (d3d8_host_2d_d3d11.c): draws into the D3D11 executor's"
                    " retained surfaces; vertex programs on the CPU interpreter; bump and linear 32-bit units refused\n");
    s_init_ok = 1;
    return 1;
}

/* ---- the fragment program, emitted per state ---------------------------- */
typedef struct {
    uint32_t cc, tmask, flags, control, afunc, lin;
    uint32_t ci[8], ai[8], co[8], ao[8];
    uint32_t fcw0, fcw1;
} PKey;     /* flags: 1 alpha test, 4 dither, 8 add specular, 16 final combiner */

typedef struct { char *at, *end; int overflow; } Emit;
static void emit(Emit *e, const char *fmt, ...)
{
    va_list ap; int n;
    if (e->overflow) return;
    va_start(ap, fmt);
    n = vsnprintf(e->at, (size_t)(e->end - e->at), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)(e->end - e->at)) { e->overflow = 1; return; }
    e->at += n;
}
/* inp(): one combiner input, channel ch (0..2 r g b; the alpha half passes 2,
 * and reads blue unless the replicate bit asks for alpha -- NV2A behaviour). */
static void emit_inp(Emit *e, uint32_t code, unsigned ch)
{
    char x[16];
    snprintf(x, sizeof x, "r%u.%c", code & 15u, (code & 16u) ? 'a' : "rgb"[ch]);
    switch ((code >> 5) & 7u) {
    case 0: emit(e, "max(0.0,%s)", x); break;
    case 1: emit(e, "(1.0-min(1.0,max(0.0,%s)))", x); break;
    case 2: emit(e, "(2.0*max(0.0,%s)-1.0)", x); break;
    case 3: emit(e, "(1.0-2.0*max(0.0,%s))", x); break;
    case 4: emit(e, "(max(0.0,%s)-0.5)", x); break;
    case 5: emit(e, "(0.5-max(0.0,%s))", x); break;
    case 6: emit(e, "(%s)", x); break;
    default: emit(e, "(-%s)", x); break;
    }
}
/* cmap1()/cmap3(): the output mapping, applied to an expression. */
static void emit_map(Emit *e, unsigned m, const char *x)
{
    switch (m) {
    case 1: emit(e, "(%s-0.5)", x); break;
    case 2: emit(e, "(%s*2.0)", x); break;
    case 3: emit(e, "((%s-0.5)*2.0)", x); break;
    case 4: emit(e, "(%s*4.0)", x); break;
    case 6: emit(e, "(%s*0.5)", x); break;
    default: emit(e, "(%s)", x); break;
    }
}
/* stage(): one general combiner stage, in the Metal host's statement order. */
static void emit_stage(Emit *e, unsigned st, uint32_t ciw, uint32_t aiw, uint32_t cw, uint32_t aw, uint32_t control)
{
    unsigned k0s = (control & 0x1000u) ? st : 0u, k1s = (control & 0x10000u) ? st : 0u, k;
    unsigned mx = (cw >> 14) & 1u, mp = (cw >> 15) & 7u, dcd = cw & 15u, dab = (cw >> 4) & 15u, dsm = (cw >> 8) & 15u;
    unsigned amx = (aw >> 14) & 1u, amp = (aw >> 15) & 7u, acd = aw & 15u, aab = (aw >> 4) & 15u, asum = (aw >> 8) & 15u;
    emit(e, "  { float4 ab, cd;\n    r1 = unpack(k0[%u].%c); r2 = unpack(k1[%u].%c);\n",
         k0s >> 2, "xyzw"[k0s & 3u], k1s >> 2, "xyzw"[k1s & 3u]);
    for (k = 0; k < 4; ++k) {
        uint32_t w = k == 3 ? aiw : ciw;
        unsigned ch = k == 3 ? 2u : k;
        emit(e, "    ab.%c = ", "xyzw"[k]); emit_inp(e, w >> 24, ch); emit(e, " * "); emit_inp(e, (w >> 16) & 255u, ch);
        emit(e, "; cd.%c = ", "xyzw"[k]); emit_inp(e, (w >> 8) & 255u, ch); emit(e, " * "); emit_inp(e, w & 255u, ch);
        emit(e, ";\n");
    }
    emit(e, "    float3 abr = %s; float3 cdr = %s;\n",
         ((cw >> 13) & 1u) ? "(ab.x + ab.y + ab.z).xxx" : "ab.xyz", ((cw >> 12) & 1u) ? "(cd.x + cd.y + cd.z).xxx" : "cd.xyz");
    emit(e, "    float3 sm = %s;\n", mx ? "(r12.a >= 0.5) ? abr : cdr" : "abr + cdr");
    if (mp == 1 || mp == 2 || mp == 3 || mp == 4 || mp == 6) {
        emit(e, "    abr = "); emit_map(e, mp, "abr"); emit(e, "; cdr = "); emit_map(e, mp, "cdr");
        emit(e, "; sm = "); emit_map(e, mp, "sm"); emit(e, ";\n");
    }
    if (dcd) emit(e, "    r%u.rgb = clamp(cdr, -1.0, 1.0);\n", dcd);
    if (dab) emit(e, "    r%u.rgb = clamp(abr, -1.0, 1.0);\n", dab);
    if (dsm) emit(e, "    r%u.rgb = clamp(sm, -1.0, 1.0);\n", dsm);
    if (acd) { emit(e, "    r%u.a = clamp(", acd); emit_map(e, amp, "cd.w"); emit(e, ", -1.0, 1.0);\n"); }
    if (aab) { emit(e, "    r%u.a = clamp(", aab); emit_map(e, amp, "ab.w"); emit(e, ", -1.0, 1.0);\n"); }
    if (asum) {
        emit(e, "    r%u.a = clamp(", asum);
        emit_map(e, amp, amx ? "((r12.a >= 0.5) ? ab.w : cd.w)" : "(ab.w + cd.w)");
        emit(e, ", -1.0, 1.0);\n");
    }
    if (((cw >> 19) & 1u) && dab) emit(e, "    r%u.a = clamp(abr.z, -1.0, 1.0);\n", dab);
    if (((cw >> 18) & 1u) && dcd) emit(e, "    r%u.a = clamp(cdr.z, -1.0, 1.0);\n", dcd);
    emit(e, "  }\n");
}
/* final_comb(), with fog. */
static void emit_final(Emit *e, uint32_t w0, uint32_t w1)
{
    unsigned k;
    emit(e, "  { float fogf = misc.y != 0 ? fog_factor(misc.z, fogp.x, fogp.y, i.fog) : 1.0;\n");
    emit(e, "    r1 = unpack(sf.x); r2 = unpack(sf.y);\n");
    emit(e, "    r3 = float4(float(misc.w & 255), float((misc.w >> 8) & 255), float((misc.w >> 16) & 255), 0) / 255.0; r3.a = fogf;\n");
    for (k = 0; k < 3; ++k) {
        char c = "rgb"[k];
        emit(e, "    { float v1 = r5.%c, q0 = r12.%c;", c, c);
        if (w1 & 0x40u) emit(e, " v1 = 1.0 - min(1.0, max(0.0, v1));");
        if (w1 & 0x20u) emit(e, " q0 = 1.0 - min(1.0, max(0.0, q0));");
        emit(e, " float s = v1 + q0;%s r14.%c = s; }\n", (w1 & 0x80u) ? " s = min(1.0, max(0.0, s));" : "", c);
    }
    emit(e, "    r14.a = 0;\n");
    for (k = 0; k < 3; ++k) {
        emit(e, "    r15.%c = ", "rgb"[k]); emit_inp(e, w1 >> 24, k); emit(e, " * "); emit_inp(e, (w1 >> 16) & 255u, k); emit(e, ";\n");
    }
    emit(e, "    r15.a = 0;\n");
    for (k = 0; k < 3; ++k) {
        emit(e, "    { float a = "); emit_inp(e, w0 >> 24, k);
        emit(e, ", b = "); emit_inp(e, (w0 >> 16) & 255u, k);
        emit(e, ", c = "); emit_inp(e, (w0 >> 8) & 255u, k);
        emit(e, ", d = "); emit_inp(e, w0 & 255u, k);
        emit(e, "; o.%c = min(1.0, max(0.0, d + a * b + (1.0 - a) * c)); }\n", "rgb"[k]);
    }
    emit(e, "    o.a = min(1.0, max(0.0, "); emit_inp(e, (w1 >> 8) & 255u, 2); emit(e, "));\n  }\n");
}
static int emit_ps(const PKey *k, char *buf, size_t n)
{
    Emit e = { buf, buf + n, 0 };
    unsigned u, st;
    emit(&e, "%s%s", k_common, k_ps_helpers);
    for (u = 0; u < 4; ++u)
        if (k->tmask & (1u << u)) emit(&e, "Texture2D tex%u : register(t%u); SamplerState smp%u : register(s%u);\n", u, u, u, u);
    emit(&e, "float4 h2d_ps(VO i) : SV_TARGET {\n");
    emit(&e, "  float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = i.d0, r5 = i.d1, r6 = 0, r7 = 0;\n");
    emit(&e, "  float4 r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;\n");
    for (u = 0; u < 4; ++u) {
        if (!(k->tmask & (1u << u))) continue;
        emit(&e, "  { float2 uv = i.t%u.xy / i.t%u.w;%s r%u = tex%u.Sample(smp%u, uv); }\n", u, u,
             ((k->lin >> u) & 1u) ? (u == 0 ? " uv /= tsz[0].xy;" : u == 1 ? " uv /= tsz[1].xy;" : u == 2 ? " uv /= tsz[2].xy;" : " uv /= tsz[3].xy;") : "",
             8u + u, u, u);
    }
    emit(&e, "  r12.a = %s;\n", (k->tmask & 1u) ? "r8.a" : "1.0");
    for (st = 0; st < k->cc && st < 8; ++st) emit_stage(&e, st, k->ci[st], k->ai[st], k->co[st], k->ao[st], k->control);
    emit(&e, "  float4 o;\n");
    if (k->flags & 16u) emit_final(&e, k->fcw0, k->fcw1);
    else emit(&e, "  o = clamp(r12 + %s, 0.0, 1.0);\n", (k->flags & 8u) ? "float4(r5.rgb, 0)" : "float4(0, 0, 0, 0)");
    /* The executor's z-range policy (CULL over 0..16777215). */
    emit(&e, "  if (i.zc < 0.0 || i.zc > 1.0) discard;\n");
    if (k->flags & 1u) {
        const char *op = NULL;
        switch (k->afunc) {
        case 0x200: op = "never"; break;
        case 0x201: op = "<"; break; case 0x202: op = "=="; break; case 0x203: op = "<="; break;
        case 0x204: op = ">"; break; case 0x205: op = "!="; break; case 0x206: op = ">="; break;
        default: op = NULL; break;
        }
        if (op && !strcmp(op, "never")) emit(&e, "  discard;\n");
        else if (op) emit(&e, "  if (!((uint)(clamp(o.a, 0.0, 1.0) * 255.0 + 0.5) %s misc.x)) discard;\n", op);
    }
    if (k->flags & 4u) {
        /* The ordered dither, before the fixed-function blend (see the header):
         * {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5} by bits, no array. */
        emit(&e, "  { uint2 q = (uint2(int2(i.p.xy) + int2(vp.zw))) & 3; uint x = q.x ^ q.y;\n"
                 "    uint b = ((x & 1) << 3) | ((q.y & 1) << 2) | (x & 2) | ((q.y >> 1) & 1);\n"
                 "    float bias = (float(b) + 0.5) / 16.0 - 0.5; o.rgb += bias / float3(31.0, 63.0, 31.0); }\n");
    }
    emit(&e, "  return float4(o.rgb, clamp(o.a, 0.0, 1.0));\n}\n");
    return !e.overflow;
}

#define PS_CACHE 1024
static struct { PKey k; uint64_t h; ID3D11PixelShader *ps; int failed; } s_ps[PS_CACHE];
static unsigned s_ps_n;
static unsigned long long s_ps_built, s_ps_hits, s_ps_failed, s_ps_compile_ns, s_ps_compile_max_ns;
static ID3D11PixelShader *ps_for(const D3D8Host2DDraw *d, uint32_t lin)
{
    PKey k;
    uint64_t h = 1469598103934665603ull;
    unsigned i;
    char *src;
    ID3DBlob *code = NULL;
    ID3D11PixelShader *ps = NULL;
    unsigned long long t0;
    memset(&k, 0, sizeof k);
    k.cc = d->cc > 8 ? 8 : d->cc; k.tmask = d->tmask & 15u; k.control = d->control; k.lin = lin;
    k.flags = (d->alpha_test ? 1u : 0u) | (d->dither ? 4u : 0u) | (d->add_specular ? 8u : 0u) | (d->final_general ? 16u : 0u);
    if (d->final_general) { k.fcw0 = d->final_cw0; k.fcw1 = d->final_cw1; }
    k.afunc = d->alpha_test ? d->alpha_func : 0;
    for (i = 0; i < k.cc; ++i) { k.ci[i] = d->ci[i]; k.ai[i] = d->ai[i]; k.co[i] = d->co[i]; k.ao[i] = d->ao[i]; }
    { const uint8_t *b = (const uint8_t *)&k; for (i = 0; i < sizeof k; ++i) { h ^= b[i]; h *= 1099511628211ull; } }
    for (i = 0; i < s_ps_n; ++i)
        if (s_ps[i].h == h && !memcmp(&s_ps[i].k, &k, sizeof k)) {
            if (s_ps[i].failed) return NULL;
            ++s_ps_hits; return s_ps[i].ps;
        }
    if (s_ps_n >= PS_CACHE) { s_err = "host d3d11: fragment program cache full"; return NULL; }
    t0 = now_ns();
    src = malloc(64u << 10);
    if (!src) { s_err = "host d3d11: out of memory"; return NULL; }
    if (emit_ps(&k, src, 64u << 10) && compile(src, "h2d_ps", "ps_4_0", &code)
        && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &ps)))
        ++s_ps_built;
    else { ps = NULL; ++s_ps_failed; }
    RELEASE(code);
    free(src);
    s_ps[s_ps_n].k = k; s_ps[s_ps_n].h = h; s_ps[s_ps_n].ps = ps; s_ps[s_ps_n].failed = ps == NULL; ++s_ps_n;
    {   unsigned long long w = now_ns() - t0; s_ps_compile_ns += w; if (w > s_ps_compile_max_ns) s_ps_compile_max_ns = w; }
    if (!ps) s_err = "host d3d11: fragment program compile";
    return ps;
}

/* ---- fixed-function state ---------------------------------------------- */
/* bf() of the Metal host, as D3D11 factors. CONSTANT_ALPHA has no D3D11
 * factor of its own: the blend factor is set to the constant's alpha
 * splatted, which a draw that also names the constant COLOUR cannot share. */
static int blend_factor(uint32_t f, D3D11_BLEND *out, int *uses_colour, int *uses_alpha)
{
    switch (f) {
    case 0x000: *out = D3D11_BLEND_ZERO; return 1;
    case 0x001: *out = D3D11_BLEND_ONE; return 1;
    case 0x300: *out = D3D11_BLEND_SRC_COLOR; return 1;
    case 0x301: *out = D3D11_BLEND_INV_SRC_COLOR; return 1;
    case 0x302: *out = D3D11_BLEND_SRC_ALPHA; return 1;
    case 0x303: *out = D3D11_BLEND_INV_SRC_ALPHA; return 1;
    case 0x304: *out = D3D11_BLEND_ONE; return 1;          /* destination alpha: a 565 target has none, so 1 */
    case 0x305: *out = D3D11_BLEND_ZERO; return 1;
    case 0x306: *out = D3D11_BLEND_DEST_COLOR; return 1;
    case 0x307: *out = D3D11_BLEND_INV_DEST_COLOR; return 1;
    case 0x308: *out = D3D11_BLEND_ZERO; return 1;         /* min(As, 1 - 1) */
    case 0x8001: *out = D3D11_BLEND_BLEND_FACTOR; *uses_colour = 1; return 1;
    case 0x8002: *out = D3D11_BLEND_INV_BLEND_FACTOR; *uses_colour = 1; return 1;
    case 0x8003: *out = D3D11_BLEND_BLEND_FACTOR; *uses_alpha = 1; return 1;
    case 0x8004: *out = D3D11_BLEND_INV_BLEND_FACTOR; *uses_alpha = 1; return 1;
    default: *out = D3D11_BLEND_ZERO; return 1;             /* bf()'s default */
    }
}
#define STATE_CACHE 64
static struct { uint32_t k[5]; ID3D11BlendState *st; } s_bs[STATE_CACHE];
static unsigned s_bs_n;
static ID3D11BlendState *blend_state(const D3D8Host2DDraw *d, float factor[4])
{
    uint32_t k[5] = { d->blend != 0, d->blend ? d->blend_src : 0, d->blend ? d->blend_dst : 0, d->blend ? d->blend_eq : 0, d->color_mask };
    D3D11_BLEND_DESC bd;
    D3D11_BLEND src, dst;
    int uc = 0, ua = 0;
    unsigned i;
    ID3D11BlendState *st = NULL;
    uint32_t c = d->blend_color;
    factor[0] = factor[1] = factor[2] = factor[3] = 1.0f;
    if (d->blend) {
        blend_factor(d->blend_src, &src, &uc, &ua); blend_factor(d->blend_dst, &dst, &uc, &ua);
        if (uc && ua) { s_err = "host d3d11: constant colour and constant alpha in one blend"; return NULL; }
        if (uc) { factor[0] = ((c >> 16) & 255u) / 255.0f; factor[1] = ((c >> 8) & 255u) / 255.0f; factor[2] = (c & 255u) / 255.0f; factor[3] = (c >> 24) / 255.0f; }
        if (ua) factor[0] = factor[1] = factor[2] = factor[3] = (c >> 24) / 255.0f;
    } else src = D3D11_BLEND_ONE, dst = D3D11_BLEND_ZERO;
    for (i = 0; i < s_bs_n; ++i) if (!memcmp(s_bs[i].k, k, sizeof k)) return s_bs[i].st;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = d->blend ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = src; bd.RenderTarget[0].DestBlend = dst;
    switch (d->blend_eq) {
    case 0x800A: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
    case 0x800B: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case 0x8007: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN; break;
    case 0x8008: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX; break;
    default:     bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; break;
    }
    /* Alpha: never read back (a 565 surface) and never read as a factor. */
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(((d->color_mask & 0x00FF0000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
        | ((d->color_mask & 0x0000FF00u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
        | ((d->color_mask & 0x000000FFu) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | D3D11_COLOR_WRITE_ENABLE_ALPHA);
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &st))) { s_err = "host d3d11: blend state"; return NULL; }
    if (s_bs_n < STATE_CACHE) { memcpy(s_bs[s_bs_n].k, k, sizeof k); s_bs[s_bs_n++].st = st; }
    return st;
}
static int cmp_func(uint32_t f)          /* nv2a_metal_compare_func's table; -1 unknown */
{
    return f >= 0x200u && f <= 0x207u ? (int)(f - 0x200u + 1u) : -1;
}
static int stencil_op(uint32_t op)       /* nv2a_metal_stencil_op's table; -1 unknown */
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1e00: return D3D11_STENCIL_OP_KEEP;
    case 0x1e01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1e02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1e03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150a: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return -1;
    }
}
static struct { uint32_t k[10]; ID3D11DepthStencilState *st; } s_ds[STATE_CACHE];
static unsigned s_ds_n;
static ID3D11DepthStencilState *depth_state(const D3D8Host2DDraw *d, int with_stencil)
{
    int cmp = d->depth_test ? cmp_func(d->depth_func) : (int)D3D11_COMPARISON_ALWAYS;
    int st = with_stencil && d->stencil_test;
    uint32_t k[10];
    D3D11_DEPTH_STENCIL_DESC dd;
    ID3D11DepthStencilState *s = NULL;
    unsigned i;
    if (cmp < 0) cmp = D3D11_COMPARISON_ALWAYS;              /* as the executor: unrecognised is ALWAYS */
    memset(k, 0, sizeof k);
    k[0] = d->depth_test != 0; k[1] = (uint32_t)cmp; k[2] = d->depth_test && d->depth_write; k[3] = (uint32_t)st;
    if (st) {
        k[4] = d->stencil_func; k[5] = d->stencil_fail; k[6] = d->stencil_zfail; k[7] = d->stencil_zpass;
        k[8] = d->stencil_func_mask & 255u; k[9] = d->stencil_write ? (d->stencil_mask & 255u) : 0u;
    }
    for (i = 0; i < s_ds_n; ++i) if (!memcmp(s_ds[i].k, k, sizeof k)) return s_ds[i].st;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = d->depth_test ? TRUE : FALSE;
    dd.DepthWriteMask = k[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)cmp;
    if (st) {
        int sc = cmp_func(d->stencil_func), f = stencil_op(d->stencil_fail), zf = stencil_op(d->stencil_zfail), zp = stencil_op(d->stencil_zpass);
        if (sc < 0 || f < 0 || zf < 0 || zp < 0) { s_err = "host d3d11: stencil state"; return NULL; }
        dd.StencilEnable = TRUE; dd.StencilReadMask = (UINT8)k[8]; dd.StencilWriteMask = (UINT8)k[9];
        dd.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)sc; dd.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)f;
        dd.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)zf; dd.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)zp;
        dd.BackFace = dd.FrontFace;
    }
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s))) { s_err = "host d3d11: depth-stencil state"; return NULL; }
    if (s_ds_n < STATE_CACHE) { memcpy(s_ds[s_ds_n].k, k, sizeof k); s_ds[s_ds_n++].st = s; }
    return s;
}
/* The Metal host's sampler_for(): magnification linear on MAGFILTER 2,
 * minification linear when the min field is even, mips only with a mip
 * filter and levels, linear between levels from 5; LOD bias in the sampler. */
static unsigned mip_levels(const D3D8H2DTexture *t) { return (t->min_filter >= 3 && t->levels >= 2) ? t->levels : 1; }
static struct { uint32_t k[4]; float bias; ID3D11SamplerState *st; } s_sm[STATE_CACHE];
static unsigned s_sm_n;
static ID3D11SamplerState *sampler_for(const D3D8H2DTexture *t)
{
    unsigned mips = mip_levels(t), filter = 0;
    uint32_t k[4] = { t->mag == 2, t->min_filter & 7u, (t->wrap_u & 3u) | (t->wrap_v & 3u) << 2, mips };
    D3D11_SAMPLER_DESC sd;
    ID3D11SamplerState *s = NULL;
    static const D3D11_TEXTURE_ADDRESS_MODE m[4] = { D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
                                                     D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP };
    unsigned i;
    for (i = 0; i < s_sm_n; ++i) if (!memcmp(s_sm[i].k, k, sizeof k) && s_sm[i].bias == t->lod_bias) return s_sm[i].st;
    if ((t->min_filter & 1u) == 0) filter |= 0x10;            /* minification linear */
    if (t->mag == 2) filter |= 0x04;                          /* magnification linear */
    if (mips > 1 && t->min_filter >= 5) filter |= 0x01;       /* linear between levels */
    memset(&sd, 0, sizeof sd);
    sd.Filter = (D3D11_FILTER)filter;
    sd.AddressU = m[t->wrap_u & 3u]; sd.AddressV = m[t->wrap_v & 3u]; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = t->lod_bias; sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f; sd.MaxLOD = mips > 1 ? (float)(mips - 1) : 0.0f;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &sd, &s))) { s_err = "host d3d11: sampler"; return NULL; }
    if (s_sm_n < STATE_CACHE) { memcpy(s_sm[s_sm_n].k, k, sizeof k); s_sm[s_sm_n].bias = t->lod_bias; s_sm[s_sm_n++].st = s; }
    else { static ID3D11SamplerState *spill; RELEASE(spill); spill = s; }
    return s;
}

/* ---- textures: the Metal host's cache, decoded by the same decoder ------- */
#define TEX_CACHE 512
#define TEX_BUDGET ((size_t)256u << 20)
#define TEX_INDEX 2048u
static struct { uint32_t addr, fmt, size; uint64_t hash; unsigned long long used, checked; size_t bytes; ID3D11ShaderResourceView *srv; } s_tc[TEX_CACHE];
static unsigned long long s_tc_clock, s_tc_hits, s_tc_builds, s_tc_hashes, s_ns_texture, s_ns_external;
static size_t s_tc_bytes;
static uint16_t s_tix[TEX_INDEX];      /* slot + 1: the index is always on here (RECOMP_D3D8_HOST_TEX_INDEX's scan is Metal's) */
static uint64_t hash64(const uint8_t *p, size_t n)
{
    uint64_t h0 = 0xCBF29CE484222325ull, h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull, w0, w1, w2, w3;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8); memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * 0x100000001B3ull; h1 = (h1 ^ w1) * 0x100000001B3ull;
        h2 = (h2 ^ w2) * 0x100000001B3ull; h3 = (h3 ^ w3) * 0x100000001B3ull;
        h0 ^= h0 >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull) ^ (uint64_t)n;
    for (; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
static unsigned tix_home(uint32_t a, uint32_t f, uint32_t s)
{
    uint64_t k = ((uint64_t)a * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)f * 0xC2B2AE3D27D4EB4Full) ^ ((uint64_t)s * 0x165667B19E3779F9ull);
    return (unsigned)(k >> 40) & (TEX_INDEX - 1u);
}
static int tix_find(uint32_t a, uint32_t f, uint32_t s)
{
    unsigned i, h;
    for (i = 0, h = tix_home(a, f, s); i < TEX_INDEX; ++i, h = (h + 1u) & (TEX_INDEX - 1u)) {
        unsigned slot;
        if (!s_tix[h]) return -1;
        slot = s_tix[h] - 1u;
        if (s_tc[slot].srv && s_tc[slot].addr == a && s_tc[slot].fmt == f && s_tc[slot].size == s) return (int)slot;
    }
    return -1;
}
static void tix_rebuild(void)
{
    unsigned i, k, h;
    memset(s_tix, 0, sizeof s_tix);
    for (i = 0; i < TEX_CACHE; ++i)
        if (s_tc[i].srv)
            for (k = 0, h = tix_home(s_tc[i].addr, s_tc[i].fmt, s_tc[i].size); k < TEX_INDEX; ++k, h = (h + 1u) & (TEX_INDEX - 1u))
                if (!s_tix[h]) { s_tix[h] = (uint16_t)(i + 1u); break; }
}
static ID3D11ShaderResourceView *texture_for(const D3D8H2DTexture *t, const uint8_t *ram, size_t ram_size)
{
    unsigned mips = mip_levels(t), w = t->width, h = t->height, pitch = t->pitch, l;
    size_t total = 0, off = 0, bytes = 0, need;
    unsigned long long flip = d3d8_host_2d_flip_count() + 1u;
    const uint8_t *src;
    int slot;
    uint64_t hash;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[16];
    uint8_t *rgba[16];
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    int ok = 1;
    if (mips > 16) mips = 16;
    for (l = 0; l < mips; ++l) {
        total += nv2a_texture_level_bytes((int)t->fmt, w, h, pitch);
        pitch = nv2a_texture_next_pitch((int)t->fmt, w, pitch); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    if (!total || (uint64_t)t->addr + total > ram_size) { s_err = "host d3d11: texture bounds"; return NULL; }
    if (t->width > 4096 || t->height > 4096) { s_err = "host d3d11: texture size"; return NULL; }
    src = ram + t->addr;
    slot = tix_find(t->addr, t->d3d_format, t->d3d_size);
    if (slot >= 0) {
        if (s_tc[slot].checked == flip && !(d3d8_host_2d_bisect() & 16u)) { s_tc[slot].used = ++s_tc_clock; ++s_tc_hits; return s_tc[slot].srv; }
        hash = hash64(src, total); ++s_tc_hashes;
        if (s_tc[slot].hash == hash) { s_tc[slot].checked = flip; s_tc[slot].used = ++s_tc_clock; ++s_tc_hits; return s_tc[slot].srv; }
        s_tc_bytes -= s_tc[slot].bytes; RELEASE(s_tc[slot].srv);          /* rewritten: rebuild in place */
    } else {
        hash = hash64(src, total); ++s_tc_hashes;
        need = (size_t)t->width * t->height * 4u * (mips > 1 ? 2u : 1u);
        for (;;) {
            int freei = -1, lru = -1;
            unsigned i;
            for (i = 0; i < TEX_CACHE; ++i) {
                if (!s_tc[i].srv) { if (freei < 0) freei = (int)i; continue; }
                if (lru < 0 || s_tc[i].used < s_tc[lru].used) lru = (int)i;
            }
            if (freei >= 0 && s_tc_bytes + need <= TEX_BUDGET) { slot = freei; break; }
            if (lru < 0) { slot = freei >= 0 ? freei : 0; break; }
            s_tc_bytes -= s_tc[lru].bytes; RELEASE(s_tc[lru].srv);
        }
    }
    memset(rgba, 0, sizeof rgba);
    w = t->width; h = t->height; pitch = t->pitch;
    for (l = 0; ok && l < mips; ++l) {
        size_t level = nv2a_texture_level_bytes((int)t->fmt, w, h, pitch);
        if (!(rgba[l] = malloc((size_t)w * h * 4u)) ||
            !nv2a_texture_decode_rgba8(src + off, total - off, w, h, pitch, (int)t->fmt, rgba[l])) { ok = 0; break; }
        sd[l].pSysMem = rgba[l]; sd[l].SysMemPitch = w * 4u; sd[l].SysMemSlicePitch = w * h * 4u;
        bytes += (size_t)w * h * 4u;
        off += level; pitch = nv2a_texture_next_pitch((int)t->fmt, w, pitch); w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    if (ok) {
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height; td.MipLevels = mips; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &tex))
          && SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &srv));
        RELEASE(tex);
    }
    for (l = 0; l < 16; ++l) free(rgba[l]);
    if (!ok) { RELEASE(srv); s_err = "host d3d11: texture decode"; return NULL; }
    s_tc[slot].addr = t->addr; s_tc[slot].fmt = t->d3d_format; s_tc[slot].size = t->d3d_size; s_tc[slot].bytes = bytes;
    s_tc[slot].hash = hash; s_tc[slot].checked = flip; s_tc[slot].used = ++s_tc_clock; s_tc[slot].srv = srv; ++s_tc_builds;
    s_tc_bytes += bytes;
    tix_rebuild();
    return srv;
}

/* ---- vertex programs on the CPU (class 3) -------------------------------
 * The D3D11 executor's own path: nv2a_vsh_execute per vertex, oPos x and y
 * truncated to 1/16 pixel, z over 16777215, then its triangle rule --
 * vertex_valid (finite position and colours; each textured unit finite with
 * q > 0), a zero or non-finite area dropped, facing and culling by
 * nv2a_texture_copy_front_facing/_culled. The same refusals as its batch:
 * an unexecutable program, oPos.zw unwritten, a non-finite oPos. */
#define VS_CACHE 32
static struct { const uint32_t *words; uint32_t len; uint64_t h; NV2AVshProgram prog; int ok; } s_vp[VS_CACHE];
static unsigned s_vp_next;
static unsigned long long s_vs_cpu_draws, s_vs_cpu_verts;
static const NV2AVshProgram *vs_program(const D3D8Host2DDraw *d)
{
    uint64_t h = hash64((const uint8_t *)d->vs_words, (size_t)d->vs_len * 16u);
    unsigned i;
    for (i = 0; i < VS_CACHE; ++i)
        if (s_vp[i].len == d->vs_len && s_vp[i].h == h) return s_vp[i].ok ? &s_vp[i].prog : NULL;
    i = s_vp_next++ % VS_CACHE;
    memset(&s_vp[i], 0, sizeof s_vp[i]);
    s_vp[i].words = d->vs_words; s_vp[i].len = d->vs_len; s_vp[i].h = h;
    s_vp[i].ok = nv2a_vsh_parse(d->vs_words, (int)d->vs_len, &s_vp[i].prog) && s_vp[i].prog.valid && s_vp[i].prog.has_final;
    return s_vp[i].ok ? &s_vp[i].prog : NULL;
}
static int vs_valid(const D3D8H2DVertex *v, uint32_t tmask)
{
    unsigned k, u;
    for (k = 0; k < 4; ++k) if (!isfinite(v->p[k]) || !isfinite(v->d0[k]) || !isfinite(v->d1[k])) return 0;
    for (u = 0; u < 4; ++u) if (tmask & (1u << u)) {
        for (k = 0; k < 4; ++k) if (!isfinite(v->t[u][k])) return 0;
        if (v->t[u][3] <= 0) return 0;
    }
    return 1;
}
static const char *vs_cpu(const D3D8Host2DDraw *d, D3D8H2DVertex **out, unsigned *nout)
{
    static D3D8H2DVertex *xv, *tv;
    static uint8_t *ok;
    static unsigned xcap, tcap;
    const NV2AVshProgram *prog = vs_program(d);
    NV2ATextureCopy fc;
    unsigned v, a, n = 0, t;
    uint32_t nattrs = d->vs_nattrs ? d->vs_nattrs : 1u;
    if (!prog) return "host d3d11: vertex program not executable";
    if (d->vs_nin > xcap) {
        D3D8H2DVertex *g = realloc(xv, d->vs_nin * sizeof *g); uint8_t *o = g ? realloc(ok, d->vs_nin) : NULL;
        if (g) xv = g;
        if (!o) return "host d3d11: out of memory";
        ok = o; xcap = d->vs_nin;
    }
    if (d->vs_nidx > tcap) {
        D3D8H2DVertex *g = realloc(tv, d->vs_nidx * sizeof *g);
        if (!g) return "host d3d11: out of memory";
        tv = g; tcap = d->vs_nidx;
    }
    for (v = 0; v < d->vs_nin; ++v) {
        float in[16][4];
        NV2AVshResult r;
        unsigned slot = 0, k;
        D3D8H2DVertex *o = &xv[v];
        memset(in, 0, sizeof in);
        for (a = 0; a < 16; ++a) {
            in[a][3] = 1.0f;
            if (d->vs_inputs & (1u << a)) { memcpy(in[a], d->vs_in[v * nattrs + slot], 16); ++slot; }
        }
        if (!nv2a_vsh_execute(prog, (const float (*)[4])in, (const float (*)[4])d->vs_c, &r)) return "host d3d11: vertex program execution failed";
        if ((r.written[0] & 12) != 12) return "host d3d11: program left oPos.zw unwritten";
        for (k = 0; k < 2; ++k) {
            float p = r.output[0][k];
            if (!isfinite(p)) return "host d3d11: oPos is not finite";
            if (fabsf(p) < 0x1p20f) p = truncf(p * 16.0f) / 16.0f;
            o->p[k] = p;
        }
        o->p[2] = r.output[0][2] / 16777215.0f; o->p[3] = r.output[0][3];
        memcpy(o->d0, r.output[3], 16); memcpy(o->d1, r.output[4], 16);
        for (k = 0; k < 4; ++k) memcpy(o->t[k], r.output[9 + k], 16);
        memcpy(o->f, r.output[5], 16);
        ok[v] = (uint8_t)vs_valid(o, d->tmask);
    }
    memset(&fc, 0, sizeof fc);
    fc.front_cw = d->front_cw; fc.cull_face = d->cull_face;
    for (t = 0; t + 2 < d->vs_nidx; t += 3) {
        uint32_t i0 = d->vs_idx[t], i1 = d->vs_idx[t + 1], i2 = d->vs_idx[t + 2];
        const float *pa, *pb, *pc;
        float ar;
        if (i0 >= d->vs_nin || i1 >= d->vs_nin || i2 >= d->vs_nin) return "host d3d11: triangle index out of range";
        if (!ok[i0] || !ok[i1] || !ok[i2]) continue;
        pa = xv[i0].p; pb = xv[i1].p; pc = xv[i2].p;
        ar = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
        if (!isfinite(ar) || ar == 0) continue;
        if (nv2a_texture_copy_culled(&fc, nv2a_texture_copy_front_facing(&fc, ar, pa[3], pb[3], pc[3]))) continue;
        tv[n++] = xv[i0]; tv[n++] = xv[i1]; tv[n++] = xv[i2];
    }
    ++s_vs_cpu_draws; s_vs_cpu_verts += d->vs_nin;
    *out = tv; *nout = n;
    return NULL;
}

/* ---- encode one draw ---------------------------------------------------- */
static int encode_draw(ID3D11DeviceContext *cx, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int with_stencil,
                       const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size, unsigned W, unsigned H, unsigned ox, unsigned oy)
{
    int32_t sx0 = d->sc_x0 > (int32_t)ox ? d->sc_x0 : (int32_t)ox, sy0 = d->sc_y0 > (int32_t)oy ? d->sc_y0 : (int32_t)oy;
    int32_t sx1 = d->sc_x1 < (int32_t)(ox + W - 1) ? d->sc_x1 : (int32_t)(ox + W - 1);
    int32_t sy1 = d->sc_y1 < (int32_t)(oy + H - 1) ? d->sc_y1 : (int32_t)(oy + H - 1);
    const D3D8H2DVertex *verts = d->verts;
    unsigned nverts = d->nverts, s;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    ID3D11PixelShader *ps;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *ds;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    float factor[4];
    uint32_t lin = 0;
    HCB cb;
    unsigned long long t0;
    size_t vbytes;
    UINT stride = sizeof(D3D8H2DVertex), offset;
    if (d->cls == 3 || d->ff_gpu) {
        const char *why;
        if (d->ff_gpu) { s_err = "host d3d11: a GPU-unit fixed-function draw (no D3D11 unit)"; return 0; }
        if (ox || oy) { s_err = "host vs: a programmable draw renders the whole target"; return 0; }
        if ((why = vs_cpu(d, (D3D8H2DVertex **)&verts, &nverts))) { s_err = why; return 0; }
    }
    if (!nverts || sx1 < sx0 || sy1 < sy0) return 1;
    memset(&cb, 0, sizeof cb);
    cb.vp[0] = (float)W; cb.vp[1] = (float)H; cb.vp[2] = (float)ox; cb.vp[3] = (float)oy;
    cb.misc[0] = d->alpha_ref; cb.misc[1] = d->fog_enable; cb.misc[2] = d->fog_mode; cb.misc[3] = d->fog_color;
    cb.sf[0] = d->sf0; cb.sf[1] = d->sf1; cb.fogp[0] = d->fog_p0; cb.fogp[1] = d->fog_p1;
    memcpy(cb.k0, d->k0, sizeof cb.k0); memcpy(cb.k1, d->k1, sizeof cb.k1);
    t0 = now_ns();
    for (s = 0; s < 4; ++s) {
        const D3D8H2DTexture *t = &d->tex[s];
        srv[s] = s_dummy_srv; smp[s] = s_dummy_smp;
        if (!(d->tmask & (1u << s))) continue;
        if (d->bump[s]) { s_err = "host d3d11: BUMPENVMAP unit (not ported: RECOMP_D3D8_HOST_BUMP)"; return 0; }
        if (t->fmt == 0x12u || t->fmt == 0x1Eu) { s_err = "host d3d11: linear 32-bit texture (not ported: RECOMP_D3D8_HOST_LIN32)"; return 0; }
        if (!(srv[s] = texture_for(t, ram, ram_size))) return 0;
        if (!(smp[s] = sampler_for(t))) return 0;
        cb.tsz[s][0] = (float)t->width; cb.tsz[s][1] = (float)t->height;
        if (t->linear) lin |= 1u << s;
    }
    s_ns_texture += now_ns() - t0;
    if (!(ps = ps_for(d, lin))) return 0;
    if (!(bs = blend_state(d, factor))) return 0;
    if (!(ds = depth_state(d, with_stencil))) return 0;
    vbytes = (size_t)nverts * sizeof(D3D8H2DVertex);
    if (vbytes > VB_BYTES) { s_err = "host d3d11: vertex count"; return 0; }
    if (s_vb_used + vbytes > VB_BYTES) s_vb_used = 0;
    if (FAILED(ID3D11DeviceContext_Map(cx, (ID3D11Resource *)s_vb, 0, s_vb_used ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        s_err = "host d3d11: vertex buffer map"; return 0;
    }
    memcpy((uint8_t *)m.pData + s_vb_used, verts, vbytes);
    ID3D11DeviceContext_Unmap(cx, (ID3D11Resource *)s_vb, 0);
    offset = s_vb_used; s_vb_used += (unsigned)((vbytes + 255u) & ~(size_t)255u);
    if (FAILED(ID3D11DeviceContext_Map(cx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        s_err = "host d3d11: constant buffer map"; return 0;
    }
    memcpy(m.pData, &cb, sizeof cb);
    ID3D11DeviceContext_Unmap(cx, (ID3D11Resource *)s_cb, 0);
    ID3D11DeviceContext_IASetInputLayout(cx, s_layout);
    ID3D11DeviceContext_IASetVertexBuffers(cx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(cx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(cx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(cx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(cx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(cx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(cx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(cx, 0, 4, smp);
    ID3D11DeviceContext_RSSetState(cx, s_rs);
    memset(&vp, 0, sizeof vp);
    vp.Width = (float)W; vp.Height = (float)H; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(cx, 1, &vp);
    sc.left = sx0 - (int32_t)ox; sc.top = sy0 - (int32_t)oy; sc.right = sx1 - (int32_t)ox + 1; sc.bottom = sy1 - (int32_t)oy + 1;
    ID3D11DeviceContext_RSSetScissorRects(cx, 1, &sc);
    ID3D11DeviceContext_OMSetBlendState(cx, bs, factor, 0xffffffffu);
    ID3D11DeviceContext_OMSetDepthStencilState(cx, ds, with_stencil && d->stencil_test ? (d->stencil_ref & 255u) : 0u);
    ID3D11DeviceContext_OMSetRenderTargets(cx, 1, &rtv, dsv);
    ID3D11DeviceContext_Draw(cx, nverts, 0);
    return 1;
}

/* ---- shadow mode and VERIFY: a private crop ------------------------------
 * The crop starts as the 565 pixels expanded to 8 bits a channel and the
 * depth as D24 (guest z / 16777215 back to its 24-bit integer), both exactly
 * as the executor uploads a surface; the result comes back as the executor's
 * read-back writes guest RAM (8-bit channels truncated to 565). */
int d3d8_host_2d_d3d11_render(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size,
                              uint16_t *pixels, unsigned pitch_px, float *depth,
                              unsigned x0, unsigned y0, unsigned w, unsigned h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *ct = NULL, *cs = NULL, *zt = NULL, *zs = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    uint8_t *rgba = NULL;
    uint32_t *z24 = NULL;
    unsigned x, y;
    int rc = -1;
    if (!d || !pixels || !w || !h || pitch_px < w) return fail("host 2d: bad arguments");
    if (d->depth_test && !depth) return fail("host 2d: depth test without the depth it starts from");
    nv2a_d3d11_lock();
    if (!init()) { nv2a_d3d11_unlock(); return -1; }
    rgba = malloc((size_t)w * h * 4u); z24 = malloc((size_t)w * h * 4u);
    if (!rgba || !z24) { s_err = "host d3d11: out of memory"; goto out; }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            unsigned c = pixels[(size_t)y * pitch_px + x], r5 = c >> 11, g6 = (c >> 5) & 63u, b5 = c & 31u;
            uint8_t *o = rgba + ((size_t)y * w + x) * 4u;
            o[0] = (uint8_t)(r5 << 3 | r5 >> 2); o[1] = (uint8_t)(g6 << 2 | g6 >> 4); o[2] = (uint8_t)(b5 << 3 | b5 >> 2); o[3] = 255;
            z24[(size_t)y * w + x] = depth ? (uint32_t)lrint((double)depth[(size_t)y * w + x] * 16777215.0) & 0xFFFFFFu : 0xFFFFFFu;
        }
    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    sd.pSysMem = rgba; sd.SysMemPitch = w * 4u; sd.SysMemSlicePitch = w * h * 4u;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &ct))) { s_err = "host d3d11: crop allocation"; goto out; }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    sd.pSysMem = z24;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &zt))) { s_err = "host d3d11: depth allocation"; goto out; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &cs))) { s_err = "host d3d11: staging allocation"; goto out; }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (depth && FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &zs))) { s_err = "host d3d11: staging allocation"; goto out; }
    if (FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)ct, NULL, &rtv))
        || FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)zt, NULL, &dsv))) { s_err = "host d3d11: views"; goto out; }
    if (!encode_draw(s_cx, rtv, dsv, 0, d, ram, ram_size, w, h, x0, y0)) goto out;
    ID3D11DeviceContext_CopyResource(s_cx, (ID3D11Resource *)cs, (ID3D11Resource *)ct);
    if (zs) ID3D11DeviceContext_CopyResource(s_cx, (ID3D11Resource *)zs, (ID3D11Resource *)zt);
    if (FAILED(ID3D11DeviceContext_Map(s_cx, (ID3D11Resource *)cs, 0, D3D11_MAP_READ, 0, &m))) { s_err = "host d3d11: read-back"; goto out; }
    for (y = 0; y < h; ++y) {
        const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        for (x = 0; x < w; ++x)
            pixels[(size_t)y * pitch_px + x] = (uint16_t)((unsigned)(row[x * 4] >> 3) << 11 | (unsigned)(row[x * 4 + 1] >> 2) << 5 | (unsigned)(row[x * 4 + 2] >> 3));
    }
    ID3D11DeviceContext_Unmap(s_cx, (ID3D11Resource *)cs, 0);
    if (zs) {
        if (FAILED(ID3D11DeviceContext_Map(s_cx, (ID3D11Resource *)zs, 0, D3D11_MAP_READ, 0, &m))) { s_err = "host d3d11: depth read-back"; goto out; }
        for (y = 0; y < h; ++y) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
            for (x = 0; x < w; ++x) depth[(size_t)y * w + x] = (float)(row[x] & 0xFFFFFFu) / 16777215.0f;
        }
        ID3D11DeviceContext_Unmap(s_cx, (ID3D11Resource *)zs, 0);
    }
    rc = 0;
out:
    RELEASE(rtv); RELEASE(dsv); RELEASE(ct); RELEASE(cs); RELEASE(zt); RELEASE(zs);
    free(rgba); free(z24);
    nv2a_d3d11_unlock();
    return rc;
}

/* ---- draw mode: into the executor's retained surface --------------------- */
typedef struct { const D3D8Host2DDraw *d; const uint8_t *ram; size_t ram_size; int ok; } ExtCtx;
static unsigned long long s_ext_ok, s_ext_fail;
static int external_encode(void *ctx, void *context, void *rtv, void *dsv, unsigned w, unsigned h)
{
    ExtCtx *x = ctx;
    if (w != x->d->rt_w || h != x->d->rt_h) { s_err = "executor surface is not the render target's size"; return 0; }
    return x->ok = encode_draw((ID3D11DeviceContext *)context, (ID3D11RenderTargetView *)rtv, (ID3D11DepthStencilView *)dsv,
                               dsv != NULL, x->d, x->ram, x->ram_size, w, h, 0, 0);
}
int d3d8_host_2d_d3d11_external(const D3D8Host2DDraw *d, const uint8_t *ram, size_t ram_size)
{
    ExtCtx x = { d, ram, ram_size, 0 };
    unsigned long long t0 = now_ns();
    int uses_zs = d->depth_test || d->stencil_test;
    int writes_zs = (d->depth_test && d->depth_write) || (d->stencil_test && d->stencil_write && (d->stencil_mask & 255u));
    int drawn;
    nv2a_d3d11_lock();
    drawn = init() ? 0 : -1;
    if (!drawn) {
        uint8_t *zs = uses_zs ? (uint8_t *)ram + d->zs_addr : NULL;
        drawn = nv2a_d3d11_external_draw((uint8_t *)ram + d->rt_addr, (size_t)d->rt_pitch * d->rt_h, d->rt_w, d->rt_h, d->rt_pitch,
                                         zs, uses_zs ? (size_t)d->zs_pitch * d->rt_h : 0, uses_zs ? d->zs_pitch : 0, writes_zs,
                                         external_encode, &x);
        if (drawn == -1) s_err = "executor not on the D3D11 path";
        else if (drawn == -2) s_err = "target not bound: the executor's bind failed";
        else if (!drawn && !x.ok && !s_err) s_err = "host encode declined";
    }
    nv2a_d3d11_unlock();
    if (drawn == 1 && (d3d8_host_2d_bisect() & 64u)) nv2a_d3d11_sync();
    s_ns_external += now_ns() - t0;
    if (drawn == 1 && x.ok) { ++s_ext_ok; return 1; }
    ++s_ext_fail;
    return 0;
}

/* ---- the report's lines ------------------------------------------------- */
void d3d8_host_2d_d3d11_stats(unsigned long long *tex_hits, unsigned long long *tex_builds, unsigned long long *tex_hashes,
                              unsigned long long *ns_texture, unsigned long long *ns_external)
{
    if (tex_hits) *tex_hits = s_tc_hits;
    if (tex_builds) *tex_builds = s_tc_builds;
    if (tex_hashes) *tex_hashes = s_tc_hashes;
    if (ns_texture) *ns_texture = s_ns_texture;
    if (ns_external) *ns_external = s_ns_external;
}
unsigned long long d3d8_host_2d_d3d11_binds(void)
{
    unsigned long long d = 0;
    nv2a_d3d11_external_stats(&d, NULL, NULL, NULL);
    return d;
}
void d3d8_host_2d_d3d11_spec_stats(unsigned long long *built, unsigned long long *hits, unsigned long long *fallback,
                                   unsigned long long *compile_ns)
{
    if (built) *built = s_ps_built;
    if (hits) *hits = s_ps_hits;
    if (fallback) *fallback = s_ps_failed;
    if (compile_ns) *compile_ns = s_ps_compile_ns;
}
void d3d8_host_2d_d3d11_pipe_stats(char *buf, size_t n)
{
    unsigned long long draws = 0, adopted = 0, d3dg = 0, declined = 0;
    nv2a_d3d11_external_stats(&draws, &adopted, &d3dg, &declined);
    snprintf(buf, n, "D3D11: fragment programs %llu compiled in line (%.1f ms, worst %.1f ms), %llu failed | into the executor's"
             " surfaces %llu (its geometry %llu, D3D's %llu, declined %llu) | vertex programs on the CPU: %llu draws, %llu vertices",
             s_ps_built, s_ps_compile_ns / 1e6, s_ps_compile_max_ns / 1e6, s_ps_failed, draws, adopted, d3dg, declined,
             s_vs_cpu_draws, s_vs_cpu_verts);
}
