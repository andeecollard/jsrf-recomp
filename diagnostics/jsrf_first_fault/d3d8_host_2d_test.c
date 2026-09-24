/* G51.1: the host's 2D pipeline against the executor's Metal path, with no game.
 *
 * A synthetic pre-transformed draw is laid out in a fake guest RAM the way
 * D3D would leave it -- an XYZRHW|DIFFUSE|TEX1 vertex buffer, a swizzled
 * A8R8G8B8 texture, a 565 render target -- and described by a
 * D3D8HostDrawCheck exactly as the mirror fills one. Then:
 *
 *   host      d3d8_host_2d_build() + d3d8_host_2d_metal_render(), from the
 *             D3D description alone;
 *   executor  nv2a_metal_draw() from the EQUIVALENT NV2A state -- the
 *             NV2ATextureCopy and post-transform vertices the executor would
 *             hold for the same draw (combiner words from the same
 *             d3d8_ff_combiners() transcription the mirror checks against it).
 *
 * Both start from the same background and must agree within one 565 step
 * per channel. The positive control is RECOMP_D3D8_HOST_2D_CONTROL's own
 * perturbation (geometry +2 px, diffuse red inverted): it MUST differ, or the
 * comparison cannot see a wrong draw.
 *
 * Cases: a textured, alpha-tested, source-alpha blended, dithered strip whose
 * crop does not start at the origin (so the dither lattice must line up); an
 * indexed untextured triangle list with a DST_COLOR multiply blend; and the
 * shadow bookkeeping end to end (pre token, post token, flip) through a fake
 * backend. Plus classification and refusal checks that need no device.
 *
 * Depth (the tutorial's 2D draws all test it): a case whose vertex z crosses
 * a depth surface split into a near and a far field, LEQUAL with writes on.
 * The executor draws it against the guest's D24S8 bytes through its real
 * Depth32Float attachment; the host is seeded with the same values and draws
 * against its own attachment. Colour must agree, and so must the depth each
 * leaves behind -- read from the executor through nv2a_metal_depth_peek, the
 * accessor the shadow uses in the game. Its control seeds the host with a
 * cleared (far) depth instead, which must change the colour.
 *
 * Run with RECOMP_METAL_HW_TEX=1 as well (ctest does both): the player's
 * executor samples through hardware textures, the default through its own
 * software sampler. */
#include "d3d8_host_2d.h"
#include "d3d8_ff_combiner.h"
#include "nv2a_metal.h"
#include "nv2a_texture_copy.h"
#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* d3d8_host.c brings nv2a_pusher.c, whose dispatch names the executor and the
 * D3D11 sink; neither is reached here (as in d3d8_streams_test.c). */
int pgraph_d3d11_method(int subch, uint32_t m, uint32_t p) { (void)subch; (void)m; (void)p; return 1; }
void nv2a_pb_exec_method(uint32_t s, uint32_t m, uint32_t p) { (void)s; (void)m; (void)p; }

#define RAM_SIZE (4u << 20)
static uint8_t *ram;
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { VB = 0x10000, IB = 0x18000, TEX = 0x20000, RT = 0x100000, RTW = 128, RTH = 96, RTPITCH = 256 };
enum { TW = 32 };

static void put32(uint32_t at, uint32_t v) { memcpy(ram + at, &v, 4); }
static void putf(uint32_t at, float v) { memcpy(ram + at, &v, 4); }

static unsigned morton(unsigned x, unsigned y)
{ unsigned i = 0, bit = 0; for (unsigned b = 1; b < TW; b <<= 1) { if (x & b) i |= 1u << bit; ++bit; if (y & b) i |= 1u << bit; ++bit; } return i; }

static void make_texture(void)
{
    for (unsigned y = 0; y < TW; ++y)
        for (unsigned x = 0; x < TW; ++x) {
            uint8_t *c = ram + TEX + 4 * morton(x, y);               /* B, G, R, A */
            c[0] = (uint8_t)(x * 8); c[1] = (uint8_t)(y * 8); c[2] = (uint8_t)((x ^ y) * 8);
            c[3] = (uint8_t)(((x / 4 + y / 4) & 1) ? 255 - x * 4 : 8 + y * 6);   /* some texels fail the alpha test */
        }
}
static void background(uint16_t *t)
{
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x)
            t[y * (RTPITCH / 2) + x] = (uint16_t)(((x * 31 / RTW) << 11) | ((y * 63 / RTH) << 5) | ((x + y) & 31));
}

/* The D3D side of the case, as the mirror would carry it. */
static void base_check(D3D8HostDrawCheck *c)
{
    memset(c, 0, sizeof *c);
    c->serial = 7;
    c->rt = 0x1234; c->rt_data = RT; c->rt_format = 0x0001u | (0x11u << 8);
    c->rt_size = (RTW - 1) | ((RTH - 1) << 12) | ((RTPITCH / 64 - 1) << 24);
    c->vp_x = 0; c->vp_y = 0; c->vp_w = RTW; c->vp_h = RTH; c->vp_minz = 0; c->vp_maxz = 1; c->ss_x = c->ss_y = 1;
    c->ffc_valid = 1; c->ffv_valid = 1;
    for (int s = 0; s < 4; ++s) {
        c->ffc_cur.tss[s][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_DISABLE;
        c->ffc_cur.tss[s][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_DISABLE;
        c->ffc_cur.tss[s][D3D8FF_TSS_RESULTARG] = D3D8FF_TA_CURRENT;
        c->tss[s][0] = c->tss[s][1] = 3; c->tss[s][3] = c->tss[s][4] = 2; c->tss[s][28] = (uint32_t)s;
    }
}
static void set_state(D3D8HostDrawCheck *c, uint32_t method, uint32_t v)
{
    static const uint32_t st[11] = D3D8_HOST_STATE_METHODS, x[8] = D3D8_HOST_2D_EXTRA_METHODS;
    for (unsigned k = 0; k < 11; ++k) if (st[k] == method) { c->st_val[k] = v; c->st_seen |= 1u << k; return; }
    for (unsigned k = 0; k < 8; ++k) if (x[k] == method) { c->x_val[k] = v; c->x_seen |= 1u << k; return; }
    printf("test bug: method %X\n", method); exit(2);
}

/* Case A: textured strip, MODULATE, alpha test GREATER 0x20, SRC_ALPHA blend, dithered. */
static const float A_POS[4][2] = { { 21.25f, 13.5f }, { 101.75f, 17.0f }, { 17.5f, 79.25f }, { 97.0f, 83.75f } };
static const uint32_t A_COL[4] = { 0xC0FF4020u, 0xFF20FF40u, 0x8040A0FFu, 0xFFFFFFFFu };
static const float A_UV[4][2] = { { -0.25f, 0.0f }, { 1.5f, 0.1f }, { 0.0f, 1.25f }, { 1.3f, 1.4f } };
static void case_a(D3D8HostDrawCheck *c)
{
    base_check(c);
    for (int i = 0; i < 4; ++i) {
        uint32_t v = VB + 28u * i;
        putf(v, A_POS[i][0]); putf(v + 4, A_POS[i][1]); putf(v + 8, 0.25f); putf(v + 12, 1.0f);
        put32(v + 16, A_COL[i]); putf(v + 20, A_UV[i][0]); putf(v + 24, A_UV[i][1]);
    }
    make_texture();
    c->vs_handle = 0x144;              /* XYZRHW | DIFFUSE | TEX1 */
    c->draw_kind = 1; c->prim = 6; c->start = 0; c->count = 4;
    c->va_on = (1u << 0) | (1u << 3) | (1u << 9);
    c->va_offset[0] = VB;      c->va_format[0] = (28u << 8) | 0x42u;
    c->va_offset[3] = VB + 16; c->va_format[3] = (28u << 8) | 0x40u;
    c->va_offset[9] = VB + 20; c->va_format[9] = (28u << 8) | 0x22u;
    c->tex[0] = 0x5678; c->data[0] = TEX;
    c->format[0] = 0x1u | 0x20u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24);
    c->tss[0][0] = c->tss[0][1] = 1;   /* wrap */
    c->ffc_cur.texture_bound_mask = 1;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_MODULATE;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_TEXTURE; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG2] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_MODULATE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_TEXTURE; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG2] = D3D8FF_TA_DIFFUSE;
    set_state(c, 0x300, 1); set_state(c, 0x33C, 0x204); set_state(c, 0x340, 0x20);
    set_state(c, 0x304, 1); set_state(c, 0x344, 0x302); set_state(c, 0x348, 0x303); set_state(c, 0x350, 0x8006);
    set_state(c, 0x310, 1);
}

/* Case B: indexed triangle list, untextured diffuse, DST_COLOR x ZERO (the fade multiply). */
static void case_b(D3D8HostDrawCheck *c)
{
    static const float P[4][2] = { { 5.5f, 60.25f }, { 70.75f, 4.0f }, { 120.0f, 90.5f }, { 30.0f, 94.0f } };
    static const uint32_t C[4] = { 0xFF808080u, 0xFFFF8000u, 0xFF00FFC0u, 0xFF404040u };
    static const uint16_t I[6] = { 0, 1, 2, 0, 2, 3 };
    base_check(c);
    for (int i = 0; i < 4; ++i) {
        uint32_t v = VB + 20u * i;
        putf(v, P[i][0]); putf(v + 4, P[i][1]); putf(v + 8, 0.0f); putf(v + 12, 1.0f); put32(v + 16, C[i]);
    }
    memcpy(ram + IB, I, sizeof I);
    c->vs_handle = 0x044;              /* XYZRHW | DIFFUSE */
    c->draw_kind = 2; c->prim = 5; c->count = 6; c->idx_ptr = IB; c->nidx = 6;
    for (int k = 0; k < 6; ++k) c->idx[k] = I[k];
    c->va_on = (1u << 0) | (1u << 3);
    c->va_offset[0] = VB;      c->va_format[0] = (20u << 8) | 0x42u;
    c->va_offset[3] = VB + 16; c->va_format[3] = (20u << 8) | 0x40u;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
    set_state(c, 0x304, 1); set_state(c, 0x344, 0x306); set_state(c, 0x348, 0x000); set_state(c, 0x350, 0x8006);
}

/* The executor's view of the same draw: NV2A-level state and post-transform
 * vertices. g_exec_offset is what D3D's pass-through adds to x and y (c1.xy);
 * the logo case's control sets it to 0 to reproduce tutorial run 3. */
static float g_exec_offset = 0.53125f;
static int g_exec_snap = 1;          /* prepare_vertices' 1/16 truncation, as the executor does it */
static float exec_snap(float v) { return g_exec_snap ? truncf(v * 16.0f) / 16.0f : v; }
static int exec_draw(const D3D8HostDrawCheck *c, const D3D8Host2DDraw *d, uint16_t *target, uint8_t *depth)
{
    static float v[16384][16][4];
    static NV2ATextureCopy s;
    unsigned n = c->count;
    memset(&s, 0, sizeof s);
    s.clip_w = RTW; s.clip_h = RTH; s.target_pitch = RTPITCH; s.target_bpp = 2; s.depth_pitch = RTW * 4;
    s.z_clip_min = 0.0f; s.z_clip_max = 16777215.0f; s.z_cull = 1;
    s.combiner_count = d->cc;
    memcpy(s.color_icw, d->ci, sizeof s.color_icw); memcpy(s.alpha_icw, d->ai, sizeof s.alpha_icw);
    memcpy(s.color_ocw, d->co, sizeof s.color_ocw); memcpy(s.alpha_ocw, d->ao, sizeof s.alpha_ocw);
    memcpy(s.const0, d->k0, sizeof s.const0); memcpy(s.const1, d->k1, sizeof s.const1);
    s.add_specular = d->add_specular;
    s.texture_mask = d->tmask; s.untextured = !(d->tmask & 1); s.modulate = 1;
    if (d->tmask & 1) {
        const D3D8H2DTexture *t = &d->tex[0];
        s.width = t->width; s.height = t->height; s.pitch = t->pitch; s.levels = t->levels;
        s.rgba8 = t->fmt == 0x06 || t->fmt == 0x07; s.dxt1 = t->fmt == 0x0C; s.dxt3 = t->fmt == 0x0E;
        s.min_filter = t->min_filter; s.linear = t->mag == 2; s.repeat = t->wrap_u == 1;
    }
    s.alpha_test = d->alpha_test; s.alpha_ref = d->alpha_ref;
    s.blend = d->blend; s.blend_src = d->blend_src; s.blend_dst = d->blend_dst;
    s.dither = d->dither;
    s.depth_test = d->depth_test; s.depth_write = d->depth_write; s.depth_func = d->depth_func;
    memset(v, 0, sizeof(float) * 16 * 4 * (n + 1));
    for (unsigned k = 0; k < n; ++k) {
        uint32_t i = c->draw_kind == 2 ? c->idx[k] : c->start + k;
        float pos[4]; uint32_t col;
        memcpy(pos, ram + c->va_offset[0] + (c->va_format[0] >> 8) * i, 16);
        memcpy(&col, ram + c->va_offset[3] + (c->va_format[3] >> 8) * i, 4);
        /* What D3D's pass-through program hands the executor: xy + c1.xy, z * c0.z. */
        v[k][0][0] = exec_snap(pos[0] + g_exec_offset); v[k][0][1] = exec_snap(pos[1] + g_exec_offset);
        v[k][0][2] = pos[2] * 16777215.0f; v[k][0][3] = 1.0f / pos[3];
        v[k][3][0] = ((col >> 16) & 255) / 255.0f; v[k][3][1] = ((col >> 8) & 255) / 255.0f;
        v[k][3][2] = (col & 255) / 255.0f; v[k][3][3] = (col >> 24) / 255.0f;
        v[k][9][3] = 1.0f;
        if ((c->va_on >> 9) & 1u) memcpy(v[k][9], ram + c->va_offset[9] + (c->va_format[9] >> 8) * i, 8);
    }
    nv2a_metal_invalidate(NULL);
    if (nv2a_metal_draw(&s, (d->tmask & 1) ? ram + d->tex[0].addr : ram + TEX,
                        (d->tmask & 1) ? nv2a_texture_copy_texture_bytes(&s) : TW * TW * 4,
                        (uint8_t *)target, RTPITCH * RTH, depth, RTW * 4 * RTH,
                        (const float (*)[16][4])v, n, c->prim) < 0) {
        printf("executor refused the draw: %s\n", nv2a_metal_last_reject());
        return 0;
    }
    nv2a_metal_sync();
    return 1;
}

static D3D8H2DVertex verts[D3D8H2D_MAX_VERTS];

/* Host over the draw's bbox, into a copy of the background. */
static int host_draw(const D3D8Host2DDraw *d, uint16_t *target)
{
    unsigned x0 = (unsigned)d->bb_x0, y0 = (unsigned)d->bb_y0, w = (unsigned)(d->bb_x1 - d->bb_x0 + 1), h = (unsigned)(d->bb_y1 - d->bb_y0 + 1);
    if (d3d8_host_2d_metal_render(d, ram, RAM_SIZE, target + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, NULL, x0, y0, w, h) != 0) {
        printf("host render failed: %s\n", d3d8_host_2d_metal_last_error());
        return 0;
    }
    return 1;
}

static void compare(const char *name, const D3D8HostDrawCheck *c, int expect_equal)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static uint8_t depth[RTW * 4 * RTH];
    D3D8Host2DDraw d; D3D8H2DDiff df;
    const char *why;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(c, ram, RAM_SIZE, !expect_equal, &d);
    CHECK(!why, "%s: host builds the draw from D3D state (%s)", name, why ? why : "built");
    if (why) return;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    if (!exec_draw(c, &d, ex, depth) || !host_draw(&d, ho)) { ++fails; return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    printf("  %s: %llu px, executor changed %llu, host %llu, over tolerance %llu, max error r%u g%u b%u, bbox %d,%d..%d,%d\n",
           name, df.pixels, df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2],
           d.bb_x0, d.bb_y0, d.bb_x1, d.bb_y1);
    CHECK(df.exec_changed > 1000, "%s: the executor drew it (%llu px changed)", name, df.exec_changed);
    if (expect_equal) {
        /* The host samples through hardware textures. So does the executor
         * under RECOMP_METAL_HW_TEX=1 (the player's configuration), and there
         * the two must agree everywhere. The executor's software sampler
         * differs from its own hardware path at alpha-test threshold texels
         * (measured: 4869 px changed against 4868 on case A), so that arm may
         * disagree on a handful of pixels and no more. */
        const char *hw = getenv("RECOMP_METAL_HW_TEX");
        unsigned long long allowed = (hw && hw[0] == '1') ? 0 : 4;
        CHECK(df.mismatch <= allowed, "%s: host and executor agree within one 565 step (%llu px over, %llu allowed"
              " with the executor's %s sampler)", name, df.mismatch, allowed, allowed ? "software" : "hardware");
        CHECK(df.host_changed * 10 >= df.exec_changed * 9, "%s: host covered what the executor covered", name);
    } else
        CHECK(df.mismatch > 500, "%s CONTROL: the perturbed host draw differs (%llu px over tolerance)", name, df.mismatch);
}

/* ---- depth ---- */
enum { ZS = 0x200000, ZSPITCH = RTW * 4 };
/* Left of x = 64 the stored depth is near (0.25), right of it far (0.75),
 * with a gradient down the rows so no two rows are alike. Guest D24S8:
 * stencil in the low byte, z in the top 24 bits. */
static uint32_t zq(unsigned x, unsigned y) { return (uint32_t)(((x < 64 ? 0.25 : 0.75) + y * 0.001) * 16777215.0); }
static void make_depth(uint8_t *z)
{
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) { uint32_t q = zq(x, y); memcpy(z + y * ZSPITCH + 4 * x, &(uint32_t){ q << 8 }, 4); }
}
/* Case D: case B's indexed list, flat diffuse, vertex z 0.1 .. 0.9 so the
 * draw passes over part of the near field and fails over part of the far. */
static void case_d(D3D8HostDrawCheck *c)
{
    static const float Z[4] = { 0.1f, 0.9f, 0.5f, 0.6f };
    case_b(c);
    for (int i = 0; i < 4; ++i) putf(VB + 20u * i + 8, Z[i]);
    c->st_seen &= ~(1u << 1);                                /* no blend: the depth decides the picture */
    c->zs = 0x2345; c->zs_data = ZS; c->zs_format = 0x1u | (0x2Eu << 8);
    c->zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
    set_state(c, 0x30C, 1); set_state(c, 0x354, 0x203); set_state(c, 0x35C, 1);
}
static void compare_depth(const char *name, D3D8HostDrawCheck *c, int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH];
    D3D8Host2DDraw d; D3D8H2DDiff df;
    const char *why;
    unsigned steps = 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    make_depth(ram + ZS);
    why = d3d8_host_2d_build(c, ram, RAM_SIZE, 0, &d);
    CHECK(!why, "%s: host builds the depth-tested draw (%s)", name, why ? why : "built");
    if (why) return;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    if (!exec_draw(c, &d, ex, ram + ZS)) { ++fails; return; }
    CHECK(nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec), "%s: the executor's depth attachment is readable", name);
    /* The host's seed: the same guest values, or for the control a cleared buffer. */
    unsigned x0 = (unsigned)d.bb_x0, y0 = (unsigned)d.bb_y0, w = (unsigned)(d.bb_x1 - d.bb_x0 + 1), h = (unsigned)(d.bb_y1 - d.bb_y0 + 1);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) zhost[y * w + x] = control ? 1.0f : (float)zq(x0 + x, y0 + y) / 16777215.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, zhost, x0, y0, w, h) != 0) {
        printf("host render failed: %s\n", d3d8_host_2d_metal_last_error()); ++fails; return;
    }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    {   /* depth over the box, executor crop against host crop */
        static float ec[RTW * RTH];
        for (unsigned y = 0; y < h; ++y) memcpy(ec + y * w, zexec + (y0 + y) * RTW + x0, w * sizeof(float));
        unsigned long long zb = d3d8_host_2d_depth_diff(ec, zhost, (size_t)w * h, 1, &steps);
        printf("  %s: colour %llu px, executor changed %llu, host %llu, over tolerance %llu | depth %llu px over one"
               " 24-bit step, worst %u\n", name, df.pixels, df.exec_changed, df.host_changed, df.mismatch, zb, steps);
        CHECK(df.exec_changed > 500 && df.exec_changed < 12000, "%s: the executor's depth test passed part of it (%llu px)",
              name, df.exec_changed);
        if (!control) {
            CHECK(df.mismatch == 0, "%s: colour agrees with the executor (%llu px over)", name, df.mismatch);
            CHECK(zb == 0, "%s: the depth each side leaves agrees within one 24-bit step (%llu px, worst %u)", name, zb, steps);
        } else
            CHECK(df.mismatch > 500, "%s CONTROL: a host seeded with cleared depth draws where the executor did not (%llu px)",
                  name, df.mismatch);
    }
    /* The proof the shadow reports: vertex z 0.1..0.9 over stored 0.25..0.84 -- it depends. */
    CHECK(d3d8_host_2d_depth_proof(0x203, d.z_min, d.z_max, 0.25f, 0.84f) == 0, "%s: LEQUAL over overlapping ranges is 'depends'", name);
}

/* ---- the tutorial's "Presented by SEGA" logo (run 3's dominant mismatch) ----
 * FVF 0x1C4, an indexed quad (0 1 2 1 3 2), rhw 0.653, z 0.500125 over a
 * cleared depth buffer, a 512x512 DXT1 texture minified about fivefold,
 * alpha test GREATER 0 against its punch-through texels, SRC_ALPHA blend,
 * dither, LEQUAL with writes. The texture is synthetic: concentric rings of
 * two opaque colours and transparent gaps, so edges fall everywhere. */
enum { TEXD = 0x40000, TD = 512 };
static void make_dxt1(void)
{
    for (unsigned by = 0; by < TD / 4; ++by)
        for (unsigned bx = 0; bx < TD / 4; ++bx) {
            uint8_t *b = ram + TEXD + 8u * (by * (TD / 4) + bx);
            uint16_t c0 = 0x001F, c1 = 0xFFFF;             /* c0 <= c1: index 3 is transparent black */
            uint32_t bits = 0;
            for (unsigned i = 0; i < 16; ++i) {
                int x = (int)(bx * 4 + i % 4) - 256, y = (int)(by * 4 + i / 4) - 256;
                unsigned r = (unsigned)(x * x + y * y) / 700u;
                unsigned idx = (r % 3 == 2) ? 3u : (r & 1u);
                bits |= idx << (2 * i);
            }
            memcpy(b, &c0, 2); memcpy(b + 2, &c1, 2); memcpy(b + 4, &bits, 4);
        }
}
static void case_logo(D3D8HostDrawCheck *c)
{
    static const float P[4][2] = { { 10.0f, 8.0f }, { 118.0f, 8.0f }, { 10.0f, 90.0f }, { 118.0f, 90.0f } };
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    static const uint16_t I[6] = { 0, 1, 2, 1, 3, 2 };
    base_check(c);
    for (int i = 0; i < 4; ++i) {                        /* XYZRHW, DIFFUSE, SPECULAR, TEX1: 36 bytes */
        uint32_t v = VB + 36u * i;
        putf(v, P[i][0]); putf(v + 4, P[i][1]); putf(v + 8, 0.500125f); putf(v + 12, 0.65299f);
        put32(v + 16, 0xFFFFFFFFu); put32(v + 20, 0); putf(v + 24, UV[i][0]); putf(v + 28, UV[i][1]);
    }
    memcpy(ram + IB, I, sizeof I);
    make_dxt1();
    c->vs_handle = 0x1C4;
    c->draw_kind = 2; c->prim = 5; c->count = 6; c->idx_ptr = IB; c->nidx = 6;
    for (int k = 0; k < 6; ++k) c->idx[k] = I[k];
    c->va_on = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 9);
    c->va_offset[0] = VB;      c->va_format[0] = (36u << 8) | 0x42u;
    c->va_offset[3] = VB + 16; c->va_format[3] = (36u << 8) | 0x40u;
    c->va_offset[4] = VB + 20; c->va_format[4] = (36u << 8) | 0x40u;
    c->va_offset[9] = VB + 24; c->va_format[9] = (36u << 8) | 0x22u;
    c->tex[0] = 0x5678; c->data[0] = TEXD;
    c->format[0] = 0x1u | 0x20u | (0x0Cu << 8) | (1u << 16) | (9u << 20) | (9u << 24);
    c->ffc_cur.texture_bound_mask = 1;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_MODULATE;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_TEXTURE; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG2] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_MODULATE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_TEXTURE; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG2] = D3D8FF_TA_DIFFUSE;
    set_state(c, 0x300, 1); set_state(c, 0x33C, 0x204); set_state(c, 0x340, 0);
    set_state(c, 0x304, 1); set_state(c, 0x344, 0x302); set_state(c, 0x348, 0x303); set_state(c, 0x350, 0x8006);
    set_state(c, 0x310, 1);
    c->zs = 0x2345; c->zs_data = ZS; c->zs_format = 0x1u | (0x2Eu << 8);
    c->zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
    set_state(c, 0x30C, 1); set_state(c, 0x354, 0x203); set_state(c, 0x35C, 1);
}
static void compare_logo(int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH], ec[RTW * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df;
    const char *why, *name = control ? "SEGA logo CONTROL (executor without the pass-through offset)" : "SEGA logo";
    unsigned steps = 0;
    case_logo(&c);
    for (unsigned y = 0; y < RTH; ++y)                     /* the cleared depth the logo is drawn over */
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why, "%s: host builds it (%s)", name, why ? why : "built");
    if (why) return;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    g_exec_offset = control ? 0.0f : 0.53125f;
    if (!exec_draw(&c, &d, ex, ram + ZS)) { ++fails; g_exec_offset = 0.53125f; return; }
    g_exec_offset = 0.53125f;
    nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
    unsigned x0 = (unsigned)d.bb_x0, y0 = (unsigned)d.bb_y0, w = (unsigned)(d.bb_x1 - d.bb_x0 + 1), h = (unsigned)(d.bb_y1 - d.bb_y0 + 1);
    for (unsigned k = 0; k < w * h; ++k) zhost[k] = 1.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, zhost, x0, y0, w, h) != 0) {
        printf("host render failed: %s\n", d3d8_host_2d_metal_last_error()); ++fails; return;
    }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    for (unsigned y = 0; y < h; ++y) memcpy(ec + y * w, zexec + (y0 + y) * RTW + x0, w * sizeof(float));
    unsigned long long zb = d3d8_host_2d_depth_diff(ec, zhost, (size_t)w * h, 1, &steps);
    printf("  %s: executor changed %llu, host %llu, over tolerance %llu, max error r%u g%u b%u | depth %llu px, worst %u\n",
           name, df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2], zb, steps);
    CHECK(df.exec_changed > 2000, "%s: the executor drew the rings (%llu px)", name, df.exec_changed);
    if (!control) {
        const char *hw = getenv("RECOMP_METAL_HW_TEX");
        unsigned long long allowed = (hw && hw[0] == '1') ? 0 : 40;   /* the executor's two DXT1 samplers differ at alpha edges */
        CHECK(df.mismatch <= allowed, "%s: colour agrees (%llu px over, %llu allowed)", name, df.mismatch, allowed);
        CHECK(zb <= allowed, "%s: depth agrees (%llu px over one step, %llu allowed)", name, zb, allowed);
    } else
        CHECK(df.mismatch > 200 && zb > 20, "%s: half a pixel is visible in colour (%llu px) and depth (%llu px)",
              name, df.mismatch, zb);
}

/* A full-screen quad at integer coordinates, z 0, over cleared depth: the
 * tutorial's fade/clear quad. Row 0 and column 0 are covered only because the
 * executor snaps 0.53125 down to 0.5. Its control turns the executor's snap
 * off, which reproduces tutorial run 4 (row 0 + column 0 in depth). */
static void compare_fullscreen(int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH];
    static const float P[4][2] = { { 0, 0 }, { RTW, 0 }, { 0, RTH }, { RTW, RTH } };
    const char *name = control ? "full-screen quad CONTROL (executor unsnapped)" : "full-screen quad";
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df;
    unsigned steps = 0;
    case_logo(&c);
    for (int i = 0; i < 4; ++i) { putf(VB + 36u * i, P[i][0]); putf(VB + 36u * i + 4, P[i][1]); putf(VB + 36u * i + 8, 0.0f); putf(VB + 36u * i + 12, 1.0f); }
    c.tex[0] = 0; c.ffc_cur.texture_bound_mask = 0; c.va_on &= ~(1u << 9);
    c.ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_DIFFUSE; c.ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
    c.st_seen &= ~1u;                                              /* no alpha test */
    for (int i = 0; i < 4; ++i) put32(VB + 36u * i + 16, 0xC0204080u);
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) { ++fails; return; }
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    g_exec_snap = !control;
    int ok = exec_draw(&c, &d, ex, ram + ZS);
    g_exec_snap = 1;
    if (!ok) { ++fails; return; }
    nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
    for (unsigned k = 0; k < RTW * RTH; ++k) zhost[k] = 1.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho, RTPITCH / 2, zhost, 0, 0, RTW, RTH) != 0) { ++fails; return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    unsigned long long zb = d3d8_host_2d_depth_diff(zexec, zhost, RTW * RTH, 1, &steps);
    printf("  %s: executor changed %llu, host %llu, over tolerance %llu | depth %llu px, worst %u\n",
           name, df.exec_changed, df.host_changed, df.mismatch, zb, steps);
    if (!control) {
        CHECK(df.mismatch == 0 && zb == 0 && zhost[0] == 0.0f && zhost[RTW - 1] == 0.0f && zhost[(RTH - 1) * RTW] == 0.0f,
              "%s: colour and depth agree everywhere, and row 0 / column 0 were written (z 0)", name);
    } else
        CHECK(zb == RTW + RTH - 1, "%s: row 0 + column 0 differ in depth (%llu px, want %u)", name, zb, RTW + RTH - 1);
}

/* The shadow bookkeeping end to end, through a fake backend whose "executor"
 * result is placed in RAM between the two tokens. Case B, which both of the
 * executor's samplers draw identically. */
static uint16_t *g_exec_result;
static int g_sync_calls;
static int fake_sync(uint8_t *p, size_t bytes)
{
    (void)bytes;
    /* The second sync is the post token's: the executor has drawn by then. */
    if (++g_sync_calls == 2 && g_exec_result) memcpy(p, g_exec_result, RTPITCH * RTH);
    return 0;
}
/* The pre token's depth read happens before the executor has drawn, so the
 * fake declines it and the shadow falls back to the guest's D24S8 bytes --
 * the path a surface the executor holds no attachment for takes. The post
 * token's read is the executor's real attachment, through the real accessor. */
static int g_peeks;
static int fake_peek(const uint8_t *z, unsigned w, unsigned h, float *out)
{
    if (++g_peeks == 1) return 0;
    return nv2a_metal_depth_peek(z, w, h, out);
}
/* What the mirror does at the draw call: indices into the ring, and the
 * vertex hash, while pIndexData still holds this draw's indices. */
static void snapshot_at_call(D3D8HostDrawCheck *c)
{
    uint32_t imin = 0xFFFFFFFFu, imax = 0;
    if (c->draw_kind == 2) {
        uint64_t pos; uint16_t *dst = d3d8_host_2d_idx_reserve(c->count, &pos);
        memcpy(dst, ram + c->idx_ptr, 2u * c->count);
        for (uint32_t k = 0; k < c->count; ++k) { if (dst[k] < imin) imin = dst[k]; if (dst[k] > imax) imax = dst[k]; }
        d3d8_host_2d_idx_publish(pos, c->count);
        c->idx_snap_pos = pos; c->idx_snap_n = c->count;
    } else { imin = c->start; imax = c->start + c->count - 1u; }
    c->vtx_hash = d3d8_host_2d_vertex_hash(ram, RAM_SIZE, c, imin, imax); c->vtx_hash_ok = 1;
}
/* `stale`: after the call, overwrite pIndexData with the next draw's indices
 * -- the tutorial's dynamic index buffer -- so only the snapshot is right. */
static int g_stale;
static void shadow_flow(D3D8HostDrawCheck *c, int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH];
    static uint8_t depth[RTW * 4 * RTH];
    D3D8Host2DDraw d; D3D8H2DStats before, after;
    D3D8Host2DBackend be;
    uint8_t *z = c->zs ? ram + ZS : depth;
    memset(&d, 0, sizeof d); d.verts = verts;
    if (c->zs) make_depth(ram + ZS);
    if (d3d8_host_2d_build(c, ram, RAM_SIZE, 0, &d)) { ++fails; return; }
    background(bg); memcpy(ex, bg, sizeof bg);
    if (!exec_draw(c, &d, ex, z)) { ++fails; return; }
    memset(&be, 0, sizeof be);
    be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
    be.last_error = d3d8_host_2d_metal_last_error; be.depth_peek = fake_peek; g_peeks = 0;
    d3d8_host_2d_set_backend(&be);
    memcpy(ram + RT, bg, sizeof bg);
    g_exec_result = ex; g_sync_calls = 0;
    snapshot_at_call(c);
    if (g_stale && c->draw_kind == 2) { static const uint16_t next[6] = { 3, 3, 3, 1, 1, 1 }; memcpy(ram + c->idx_ptr, next, sizeof next); }
    d3d8_host_2d_get_stats(&before);
    d3d8_host_2d_pre(c->serial, c->rt_data, c->rt_format, c->rt_size, c->zs_data, c->zs_size);
    d3d8_host_2d_post(c, NULL);
    d3d8_host_2d_flip();
    d3d8_host_2d_get_stats(&after);
    CHECK(after.compared == before.compared + 1, "shadow%s%s: one draw compared at the flip", c->zs ? " depth" : "",
          control ? " CONTROL" : "");
    if (c->zs && !control) {
        CHECK(after.depth_draws == before.depth_draws + 1 && after.z_from_ram == before.z_from_ram + 1,
              "shadow depth: seeded from guest RAM, depth compared");
        CHECK(after.depth_px_mismatch == before.depth_px_mismatch,
              "shadow depth: the executor's attachment after the draw matches the host's (%llu px over)",
              after.depth_px_mismatch - before.depth_px_mismatch);
        CHECK(after.proof_depends == before.proof_depends + 1, "shadow depth: the proof says it depends per pixel");
    }
    if (control)
        CHECK(after.mismatching == before.mismatching + 1, "shadow CONTROL: the flip reports it MISMATCHING");
    else
        CHECK(after.mismatching == before.mismatching, "shadow: the flip reports no mismatch (exact %llu, within %llu)",
              after.exact - before.exact, after.within - before.within);
}

/* ---- G51.3: a fixed-function 3D draw ----
 * FVF 0x142 (XYZ, DIFFUSE, TEX1), WORLD/VIEW/PROJECTION with a real
 * perspective, lighting off, a textured MODULATE, LEQUAL with writes over
 * cleared depth. The host builds the register file from D3D state
 * (d3d8_host_ff_registers) and evaluates each vertex with nv2a_ff_vertex; the
 * executor side here is nv2a_metal_draw fed the SAME unit's outputs from the
 * same registers, so this checks the host's plumbing -- attribute fetch,
 * z and w handling, primitive assembly, the pipeline -- not the transcription,
 * which G42/G42b/G39 check against the executor in the game. */
static void identity(float m[16]) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void case_ff(D3D8HostDrawCheck *c)
{
    static const float P[4][3] = { { -0.7f, -0.6f, 2.0f }, { 0.8f, -0.5f, 3.0f }, { -0.6f, 0.7f, 2.5f }, { 0.75f, 0.8f, 4.0f } };
    static const uint32_t C[4] = { 0xFFFF8040u, 0xFF40FF80u, 0xFF8040FFu, 0xFFFFFFFFu };
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    base_check(c);
    for (int i = 0; i < 4; ++i) {                        /* 12 + 4 + 8 = 24 bytes */
        uint32_t v = VB + 24u * i;
        putf(v, P[i][0]); putf(v + 4, P[i][1]); putf(v + 8, P[i][2]); put32(v + 12, C[i]);
        putf(v + 16, UV[i][0]); putf(v + 20, UV[i][1]);
    }
    make_texture();
    c->vs_handle = 0x142;
    c->draw_kind = 1; c->prim = 6; c->start = 0; c->count = 4;
    c->va_on = (1u << 0) | (1u << 3) | (1u << 9);
    c->va_offset[0] = VB;      c->va_format[0] = (24u << 8) | 0x32u;
    c->va_offset[3] = VB + 12; c->va_format[3] = (24u << 8) | 0x40u;
    c->va_offset[9] = VB + 16; c->va_format[9] = (24u << 8) | 0x22u;
    c->tex[0] = 0x5678; c->data[0] = TEX;
    c->format[0] = 0x1u | 0x20u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24);
    c->ffc_cur.texture_bound_mask = 1;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_MODULATE;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_TEXTURE; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG2] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
    /* Transforms: world and view identity, a perspective projection (z/w in 0..1). */
    identity(c->xf_world); identity(c->xf_view); identity(c->xf_proj);
    c->xf_proj[10] = 1.2f; c->xf_proj[11] = 1.0f; c->xf_proj[14] = -1.2f; c->xf_proj[15] = 0.0f;
    c->xf_seen = 7;
    memcpy(c->imv_cur.world, c->xf_world, 64); memcpy(c->imv_cur.view, c->xf_view, 64);
    c->zs = 0x2345; c->zs_data = ZS; c->zs_format = 0x1u | (0x2Eu << 8);
    c->zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
    set_state(c, 0x30C, 1); set_state(c, 0x354, 0x203); set_state(c, 0x35C, 1);
}
/* The executor's side for a fixed-function draw: its own vertex unit on the
 * same register file, outputs straight to nv2a_metal_draw (no snap). */
static int exec_draw_ff(const D3D8HostDrawCheck *c, const D3D8Host2DDraw *d, const uint32_t *ffm, uint16_t *target, uint8_t *depth)
{
    static float v[64][16][4];
    static NV2ATextureCopy s;
    memset(&s, 0, sizeof s);
    s.clip_w = RTW; s.clip_h = RTH; s.target_pitch = RTPITCH; s.target_bpp = 2; s.depth_pitch = RTW * 4;
    s.z_clip_min = 0.0f; s.z_clip_max = 16777215.0f; s.z_cull = 1;
    s.combiner_count = d->cc;
    memcpy(s.color_icw, d->ci, sizeof s.color_icw); memcpy(s.alpha_icw, d->ai, sizeof s.alpha_icw);
    memcpy(s.color_ocw, d->co, sizeof s.color_ocw); memcpy(s.alpha_ocw, d->ao, sizeof s.alpha_ocw);
    memcpy(s.const0, d->k0, sizeof s.const0); memcpy(s.const1, d->k1, sizeof s.const1);
    s.texture_mask = d->tmask; s.untextured = !(d->tmask & 1); s.modulate = 1;
    s.width = s.height = TW; s.pitch = TW * 4; s.levels = 1; s.rgba8 = 1; s.min_filter = 2; s.linear = 1;
    s.depth_test = d->depth_test; s.depth_write = d->depth_write; s.depth_func = d->depth_func;
    for (unsigned k = 0; k < c->count; ++k) {
        float in[16][4];
        for (unsigned a = 0; a < 16; ++a) { in[a][0] = in[a][1] = in[a][2] = 0; in[a][3] = 1; }
        memcpy(in[0], ram + VB + 24u * k, 12);
        { uint32_t col; memcpy(&col, ram + VB + 24u * k + 12, 4);
          in[3][0] = ((col >> 16) & 255) / 255.0f; in[3][1] = ((col >> 8) & 255) / 255.0f; in[3][2] = (col & 255) / 255.0f; in[3][3] = (col >> 24) / 255.0f; }
        memcpy(in[9], ram + VB + 24u * k + 16, 8);
        if (nv2a_ff_vertex(ffm, (const float (*)[4])in, v[k])) return 0;
    }
    nv2a_metal_invalidate(NULL);
    if (nv2a_metal_draw(&s, ram + TEX, TW * TW * 4, (uint8_t *)target, RTPITCH * RTH, depth, RTW * 4 * RTH,
                        (const float (*)[16][4])v, c->count, c->prim) < 0) {
        printf("executor refused the FF draw: %s\n", nv2a_metal_last_reject()); return 0;
    }
    nv2a_metal_sync();
    return 1;
}
static void compare_ff(int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH], ec[RTW * RTH];
    static uint32_t ffm[2048];
    const char *name = control ? "fixed-function quad CONTROL" : "fixed-function quad", *why;
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df; unsigned steps = 0;
    case_ff(&c);
    CHECK(d3d8_host_2d_class(&c) == 2, "%s: FVF 0x142 is the fixed-function class", name);
    why = d3d8_host_ff_registers(&c, ffm);
    CHECK(!why, "%s: register file from D3D state (%s)", name, why ? why : "built");
    if (why) return;
    { float x; memcpy(&x, &ffm[0x680 / 4], 4); CHECK(x != 0.0f, "%s: COMPOSITE written", name); }
    for (unsigned y = 0; y < RTH; ++y)                     /* cleared depth */
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, control, NULL, ffm, nv2a_ff_vertex, &d);
    CHECK(!why && d.nverts == 6, "%s: host builds it through nv2a_ff_vertex (%s)", name, why ? why : "built");
    if (why) return;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) { ++fails; return; }
    nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
    unsigned x0 = (unsigned)d.bb_x0, y0 = (unsigned)d.bb_y0, w = (unsigned)(d.bb_x1 - d.bb_x0 + 1), h = (unsigned)(d.bb_y1 - d.bb_y0 + 1);
    for (unsigned k = 0; k < w * h; ++k) zhost[k] = 1.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, zhost, x0, y0, w, h) != 0) { ++fails; return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    for (unsigned y = 0; y < h; ++y) memcpy(ec + y * w, zexec + (y0 + y) * RTW + x0, w * sizeof(float));
    unsigned long long zb = d3d8_host_2d_depth_diff(ec, zhost, (size_t)w * h, 1, &steps);
    printf("  %s: executor changed %llu, host %llu, over tolerance %llu, max r%u g%u b%u | depth %llu px, worst %u\n",
           name, df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2], zb, steps);
    CHECK(df.exec_changed > 500, "%s: the executor drew it (%llu px; perspective shrinks the quad)", name, df.exec_changed);
    if (!control) {
        const char *hw = getenv("RECOMP_METAL_HW_TEX");
        unsigned long long allowed = (hw && hw[0] == '1') ? 0 : 8;
        CHECK(df.mismatch <= allowed && zb == 0, "%s: host and executor agree, colour (%llu px over, %llu allowed) and depth (%llu)",
              name, df.mismatch, allowed, zb);
    } else
        CHECK(df.mismatch > 200, "%s: the perturbed host draw differs (%llu px)", name, df.mismatch);
}

/* ---- rhw 0: a zeroed vertex in a partly filled dynamic buffer ----
 * Tutorial run 5's one mismatch. Case B's list with vertex 3 zeroed: its
 * triangle has clip w = 1/0, so the executor drops it and the host must too. */
static void compare_rhw0(void)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    case_b(&c);
    putf(VB + 60, 0.0f); putf(VB + 64, 0.0f); putf(VB + 68, 0.0f); putf(VB + 72, 0.0f); put32(VB + 76, 0);
    memset(&d, 0, sizeof d); d.verts = verts;
    CHECK(!d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) && d.tris_dropped_w == 1 && d.nverts == 3,
          "rhw 0: the triangle reaching the zeroed vertex is dropped (%u dropped, %u vertices kept)", d.tris_dropped_w, d.nverts);
    compare("indexed list with a zeroed vertex (rhw 0)", &c, 1);
}

/* ---- draw mode ----
 * The host draws into the surface the EXECUTOR has bound, through the same
 * nv2a_metal_external_draw the game uses; the reference is the executor
 * drawing the same draw itself. A "binder" draw first makes the executor
 * bind the target and its depth (depth test on, ALWAYS, no write, so it
 * changes no depth), and is drawn identically in both arms. */
static void draw_binder(uint16_t *rt)
{
    D3D8HostDrawCheck b; D3D8Host2DDraw bd;
    case_b(&b);
    b.zs = 0x2345; b.zs_data = ZS; b.zs_format = 0x1u | (0x2Eu << 8);
    b.zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
    set_state(&b, 0x30C, 1); set_state(&b, 0x354, 0x207); set_state(&b, 0x35C, 0);
    memset(&bd, 0, sizeof bd); bd.verts = verts;
    if (d3d8_host_2d_build(&b, ram, RAM_SIZE, 0, &bd) || !exec_draw(&b, &bd, rt, ram + ZS)) { ++fails; printf("binder failed\n"); }
}
static void logo_depth(void)
{
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
}
/* Arm A: the executor draws the logo. Arm B: the host draws it into the
 * executor's bound surface. Returns colour and depth over the whole target. */
static int draw_arm(int host, int control, uint16_t *out, float *zout)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    uint16_t *rt = (uint16_t *)(ram + RT);
    background(rt); logo_depth();
    draw_binder(rt);
    case_logo(&c);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, control, &d)) return 0;
    if (host) {
        if (!d3d8_host_2d_metal_external(&d, ram, RAM_SIZE)) { printf("external: %s\n", d3d8_host_2d_metal_last_error()); return 0; }
        nv2a_metal_sync();
    } else if (!exec_draw(&c, &d, rt, ram + ZS)) return 0;
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout);
}
static unsigned long long g_fake_skipped; static int g_fake_skip;
static void fake_skip(int on) { g_fake_skip = on; }
static unsigned long long fake_skipped(void) { return g_fake_skipped; }
static void draw_mode_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad;
    background(bg);
    CHECK(draw_arm(0, 0, a, za) && draw_arm(1, 0, b, zb), "draw mode: both arms drew");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
    zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
    printf("  draw mode, logo: executor changed %llu, host (in the executor's surface) %llu, differing %llu, max r%u g%u b%u"
           " | depth %llu px, worst %u\n", df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1],
           df.max_err[2], zbad, steps);
    CHECK(df.exec_changed > 2000 && df.host_changed == df.exec_changed, "draw mode: the host drew into the executor's surface");
    CHECK(df.mismatch == 0 && df.max_err[0] + df.max_err[1] + df.max_err[2] == 0 && zbad == 0,
          "draw mode: identical to the executor drawing it, colour and depth");
    CHECK(draw_arm(1, 1, b, zb), "draw mode CONTROL: drew");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 1, &df);
    CHECK(df.mismatch > 200, "draw mode CONTROL: the perturbed host draw is what the surface holds (%llu px differ)", df.mismatch);

    /* The controller: replace() draws and turns the skip on, after() turns it
     * off and checks the executor skipped something; a refused draw and an
     * unbound target are left to the executor with the skip never on. */
    {
        D3D8Host2DBackend be; D3D8H2DStats s0, s1, s2, s3;
        D3D8HostDrawCheck c;
        memset(&be, 0, sizeof be);
        be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
        be.last_error = d3d8_host_2d_metal_last_error; be.external_draw = d3d8_host_2d_metal_external;
        be.exec_skip = fake_skip; be.exec_skipped = fake_skipped;
        d3d8_host_2d_set_backend(&be);
        background((uint16_t *)(ram + RT)); logo_depth(); draw_binder((uint16_t *)(ram + RT));
        case_logo(&c); snapshot_at_call(&c);
        d3d8_host_2d_get_stats(&s0);
        d3d8_host_2d_replace(&c);
        CHECK(g_fake_skip == 1, "draw mode: replace() drew and turned the executor's skip on");
        if (g_fake_skip) ++g_fake_skipped;                       /* the executor's batch, skipped */
        d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s1);
        CHECK(g_fake_skip == 0 && s1.replaced == s0.replaced + 1 && s1.replaced_without_skip == s0.replaced_without_skip,
              "draw mode: after() turned it off; replaced once, and the executor skipped");
        case_logo(&c); snapshot_at_call(&c); set_state(&c, 0x32C, 1);
        d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s2);
        CHECK(g_fake_skip == 0 && s2.replaced == s1.replaced && s2.replace_refused == s1.replace_refused + 1,
              "draw mode: a draw the host refuses (stencil) is left to the executor");
        case_logo(&c); c.rt_data = RT + 0x40000; snapshot_at_call(&c);
        d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s3);
        CHECK(g_fake_skip == 0 && s3.replaced == s2.replaced && s3.replace_unbound == s2.replace_unbound + 1,
              "draw mode: a target the executor has not bound is left to the executor");
        /* And the control on the skip counter: a replaced draw whose batches were NOT skipped is counted. */
        case_logo(&c); snapshot_at_call(&c);
        d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s0);
        CHECK(s0.replaced_without_skip == s3.replaced_without_skip + 1, "draw mode CONTROL: a replaced draw the executor did not skip is caught");
        d3d8_host_2d_report("test");
    }
}

int main(int argc, char **argv)
{
    D3D8HostDrawCheck c;
    D3D8Host2DDraw d;
    /* `control`: arm the shadow with RECOMP_D3D8_HOST_2D_CONTROL=1 and a dump
     * directory, so the flip must report the draw MISMATCHING and write it. */
    int control = argc > 1 && strcmp(argv[1], "control") == 0;
    char dir[512];
    ram = calloc(1, RAM_SIZE);
    if (!ram) return 2;
    if (argc > 1 && strcmp(argv[1], "draw") == 0) {         /* the draw-mode arm, a process of its own */
        setenv("RECOMP_D3D8_HOST_2D", "draw", 1);
        CHECK(d3d8_host_2d_mode() == 2, "RECOMP_D3D8_HOST_2D=draw arms draw mode");
        draw_mode_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }

    /* Classification: XYZRHW FVFs only. */
    CHECK(d3d8_host_2d_is_fvf_xyzrhw(0x1C4) && d3d8_host_2d_is_fvf_xyzrhw(0x044), "XYZRHW FVFs 0x1C4, 0x044 are 2D");
    CHECK(!d3d8_host_2d_is_fvf_xyzrhw(0x142) && !d3d8_host_2d_is_fvf_xyzrhw(0x112) && !d3d8_host_2d_is_fvf_xyzrhw(0x1C2),
          "XYZ FVFs 0x142, 0x112, 0x1C2 are not");
    CHECK(!d3d8_host_2d_is_fvf_xyzrhw(0x00A3B5C5u), "a programmable shader handle (odd) is not");

    /* Refusals the host must make rather than draw wrongly. */
    memset(&d, 0, sizeof d); d.verts = verts;
    case_a(&c); set_state(&c, 0x30C, 1); set_state(&c, 0x354, 0x201);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) != NULL, "a depth-tested 2D draw with no depth surface is refused");
    case_a(&c); set_state(&c, 0x32C, 1);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) != NULL, "a stencil-tested 2D draw is refused");
    case_a(&c); set_state(&c, 0x30C, 1); set_state(&c, 0x354, 0x207);
    c.zs = 0x2345; c.zs_data = 0x200000; c.zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((RTW * 4 / 64 - 1) << 24);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) == NULL && d.depth_test, "a depth-tested draw with a depth surface is drawn");
    case_a(&c); c.rt_format = 0x1u | (0x12u << 8);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) != NULL, "a 32-bit target is refused");
    case_a(&c); c.format[0] = (c.format[0] & ~0xFF00u) | (0x0Fu << 8);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) != NULL, "an undecodable texture format (DXT5) is refused");
    case_a(&c);
    CHECK(d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d) == NULL && d.nverts == 6 && d.cc >= 1 && d.tmask == 1,
          "case A builds: 2 triangles, combiners, texture 0");

    /* Pixels. */
    case_a(&c); compare("textured alpha-tested blended dithered strip", &c, 1);
    case_a(&c); compare("textured alpha-tested blended dithered strip", &c, 0);
    case_b(&c); compare("indexed DST_COLOR multiply list", &c, 1);
    case_b(&c); compare("indexed DST_COLOR multiply list", &c, 0);
    case_d(&c); compare_depth("depth-tested LEQUAL list with writes", &c, 0);
    case_d(&c); compare_depth("depth-tested LEQUAL list with writes", &c, 1);
    compare_logo(0);
    compare_logo(1);
    compare_fullscreen(0);
    compare_fullscreen(1);
    compare_rhw0();
    compare_ff(0);
    compare_ff(1);
    CHECK(d3d8_host_2d_snap(0.53125f) == 0.5f && d3d8_host_2d_snap(160.53125f) == 160.5f && d3d8_host_2d_snap(-0.53125f) == -0.5f,
          "snap: 0.53125 -> 0.5, 160.53125 -> 160.5, toward zero for negatives");
    /* The proof on its own. */
    CHECK(d3d8_host_2d_depth_proof(0x203, 0.0f, 0.0f, 0.0f, 1.0f) == 1, "proof: LEQUAL at z 0 passes over any stored depth");
    CHECK(d3d8_host_2d_depth_proof(0x201, 0.0f, 0.0f, 0.0f, 1.0f) == 0, "proof: LESS at z 0 depends (stored 0 rejects)");
    CHECK(d3d8_host_2d_depth_proof(0x204, 0.0f, 0.1f, 0.5f, 1.0f) == -1, "proof: GREATER under stored depth fails");
    CHECK(d3d8_host_2d_depth_proof(0x207, 2.0f, 3.0f, 0.0f, 1.0f) == 1, "proof: ALWAYS passes");

    /* The bookkeeping, with the switch armed as the game would arm it. */
    setenv("RECOMP_D3D8_HOST_2D", "shadow", 1);
    if (control) {
        const char *t = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/jsrf-host-2d-test.%d", t && *t ? t : "/tmp", (int)getpid());
        setenv("RECOMP_D3D8_HOST_2D_CONTROL", "1", 1);
        setenv("RECOMP_D3D8_HOST_2D_DUMP", dir, 1);
    }
    CHECK(d3d8_host_2d_mode() == 1, "RECOMP_D3D8_HOST_2D=shadow arms the shadow");
    case_b(&c); shadow_flow(&c, control);
    if (!control) {
        D3D8H2DStats a, b;
        case_d(&c); shadow_flow(&c, 0);
        /* The tutorial's failure: pIndexData rewritten between the call and
         * the token. The snapshot must carry the draw through it... */
        d3d8_host_2d_get_stats(&a);
        g_stale = 1; case_b(&c); shadow_flow(&c, 0); g_stale = 0;
        d3d8_host_2d_get_stats(&b);
        CHECK(b.idx_changed == a.idx_changed + 1 && b.idx_from_snapshot == a.idx_from_snapshot + 1,
              "stale index buffer: seen as changed, drawn from the snapshot");
        /* ...and without one the draw must be refused, not drawn from the stale buffer. */
        case_b(&c); c.idx_snap_n = 0;
        d3d8_host_2d_pre(c.serial, c.rt_data, c.rt_format, c.rt_size, 0, 0);
        d3d8_host_2d_post(&c, NULL);
        d3d8_host_2d_get_stats(&a);
        CHECK(a.compared == b.compared, "no index snapshot: the draw is refused, not guessed");
        d3d8_host_2d_flip();
    }
    /* The ring on its own: round trip, and a producer that has come round is caught. */
    {
        uint64_t pos, p2; uint16_t *w = d3d8_host_2d_idx_reserve(4, &pos), out[4];
        w[0] = 9; w[1] = 8; w[2] = 7; w[3] = 6; d3d8_host_2d_idx_publish(pos, 4);
        CHECK(d3d8_host_2d_idx_copy(pos, 4, out) && out[0] == 9 && out[3] == 6, "index ring: round trip");
        for (unsigned k = 0; k < 2u * D3D8H2D_IDX_RING / D3D8H2D_IDX_PER_DRAW; ++k) {
            d3d8_host_2d_idx_reserve(D3D8H2D_IDX_PER_DRAW, &p2); d3d8_host_2d_idx_publish(p2, D3D8H2D_IDX_PER_DRAW);
        }
        CHECK(!d3d8_host_2d_idx_copy(pos, 4, out), "index ring CONTROL: overwritten entries are refused");
    }
    d3d8_host_2d_report("test");
    if (control) {
        char path[640], head[32] = { 0 };
        D3D8H2DStats st;
        FILE *f;
        d3d8_host_2d_get_stats(&st);
        CHECK(st.dumped == 1, "shadow CONTROL: the mismatching draw was dumped (%llu)", st.dumped);
        snprintf(path, sizeof path, "%s/h2d_f000001_d%08u.ppm", dir, c.serial);
        f = fopen(path, "rb");
        CHECK(f && fread(head, 1, 2, f) == 2 && head[0] == 'P' && head[1] == '6', "shadow CONTROL: %s is a PPM", path);
        if (f) fclose(f);
        remove(path);
        snprintf(path, sizeof path, "%s/h2d_index.txt", dir);
        f = fopen(path, "r");
        CHECK(f != NULL, "shadow CONTROL: h2d_index.txt describes it");
        if (f) { char line[512]; if (fgets(line, sizeof line, f)) printf("  index: %s", line); fclose(f); }
        remove(path); rmdir(dir);
    }

    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
