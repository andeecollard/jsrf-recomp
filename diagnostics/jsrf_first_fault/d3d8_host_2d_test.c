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
#include <time.h>
#include "d3d8_host_2d.h"
#include "d3d8_ff_combiner.h"
#include "nv2a_metal.h"
#include "nv2a_host_read.h"
#include "nv2a_vsh.h"
#include "vsh_capture.h"
#include "vsh_encode.h"
#include "d3d8_ff_vertex_state.h"
#include <pthread.h>
#include "nv2a_texture_copy.h"
#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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
    static const uint32_t st[11] = D3D8_HOST_STATE_METHODS, x[D3D8_HOST_2D_EXTRA_N] = D3D8_HOST_2D_EXTRA_METHODS;
    for (unsigned k = 0; k < 11; ++k) if (st[k] == method) { c->st_val[k] = v; c->st_seen |= 1u << k; return; }
    for (unsigned k = 0; k < D3D8_HOST_2D_EXTRA_N; ++k) if (x[k] == method) { c->x_val[k] = v; c->x_seen |= 1u << k; return; }
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
static int g_exec_keep_surfaces;     /* 1: do not invalidate first, so binding state carries across draws */
/* G53: exec_draw_ff draws with fog -- D3D's fog final combiner, and the fog
 * mode, params and enable from the register file it is handed -- when set. */
static int g_exec_fog; static uint32_t g_exec_fog_color;
static int g_exec_no_sync;
/* G74: screen positions forced onto the executor's first vertices (x, y), so
 * a point can be put exactly where pixel centres fall on its quad's edges. */
static int g_exec_pos_n; static float g_exec_pos[8][2];
static int g_exec_zw; static float g_exec_z, g_exec_w;   /* force z (guest units) and w on those vertices */
static size_t g_exec_size_extra;     /* the executor's target size beyond D3D's: pitch * (clip_y + clip_h) vs pitch * height */           /* 1: exec_draw / exec_draw_ff leave the batch open, as the executor's own next draw finds it */
static int g_exec_snap = 1;          /* prepare_vertices' 1/16 truncation, as the executor does it */
static float exec_snap(float v) { return g_exec_snap ? truncf(v * 16.0f) / 16.0f : v; }
static int exec_draw(const D3D8HostDrawCheck *c, const D3D8Host2DDraw *d, uint16_t *target, uint8_t *depth)
{
    static float v[16384][16][4];
    static NV2ATextureCopy s;
    unsigned n = c->count;
    static uint8_t upcopy[4096];
    const uint8_t *vb = ram;
    if (c->draw_kind == 3 && c->up_bytes <= sizeof upcopy && d3d8_host_2d_up_copy(c->up_pos, c->up_bytes, upcopy)) vb = upcopy;   /* G75 */
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
        s.lin32 = t->fmt == 0x12 ? 1u : t->fmt == 0x1E ? 2u : 0u;       /* G75 */
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
        memcpy(pos, vb + c->va_offset[0] + (c->va_format[0] >> 8) * i, 16);
        memcpy(&col, vb + c->va_offset[3] + (c->va_format[3] >> 8) * i, 4);
        /* What D3D's pass-through program hands the executor: xy + c1.xy, z * c0.z. */
        v[k][0][0] = exec_snap(pos[0] + g_exec_offset); v[k][0][1] = exec_snap(pos[1] + g_exec_offset);
        v[k][0][2] = pos[2] * 16777215.0f; v[k][0][3] = 1.0f / pos[3];
        v[k][3][0] = ((col >> 16) & 255) / 255.0f; v[k][3][1] = ((col >> 8) & 255) / 255.0f;
        v[k][3][2] = (col & 255) / 255.0f; v[k][3][3] = (col >> 24) / 255.0f;
        v[k][9][3] = 1.0f;
        if ((c->va_on >> 9) & 1u) memcpy(v[k][9], vb + c->va_offset[9] + (c->va_format[9] >> 8) * i, 8);
    }
    if (!g_exec_keep_surfaces) nv2a_metal_invalidate(NULL);
    if (nv2a_metal_draw(&s, (d->tmask & 1) ? ram + d->tex[0].addr : ram + TEX,
                        (d->tmask & 1) ? nv2a_texture_copy_texture_bytes(&s) : TW * TW * 4,
                        (uint8_t *)target, RTPITCH * RTH + g_exec_size_extra, depth, RTW * 4 * RTH,
                        (const float (*)[16][4])v, n, c->prim) < 0) {
        printf("executor refused the draw: %s\n", nv2a_metal_last_reject());
        return 0;
    }
    if (!g_exec_no_sync) nv2a_metal_sync();
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
    d3d8_host_2d_pre(c->serial, c->vs_handle, c->rt_data, c->rt_format, c->rt_size, c->zs_data, c->zs_size);
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
    /* G74: the point state D3D's updater writes, as the executor reads it
     * (SET_POINT_SIZE / 8, POINT_SMOOTH_ENABLE). */
    s.point_size = (float)(d->point_reg & 0x1FFu) / 8.0f; s.point_sprite = d->point_sprite != 0;
    if (d->stencil_test) {
        s.stencil_test = 1; s.stencil_write = d->stencil_write; s.stencil_mask = d->stencil_mask; s.stencil_func = d->stencil_func;
        s.stencil_ref = d->stencil_ref; s.stencil_func_mask = d->stencil_func_mask;
        s.stencil_fail = d->stencil_fail; s.stencil_zfail = d->stencil_zfail; s.stencil_zpass = d->stencil_zpass;
    }
    if (g_exec_fog) {
        uint32_t w0, w1;
        d3d8_ff_final_combiner(1, 0, 0, 0, &w0, &w1);
        s.final_general = 1; s.final_cw0 = w0; s.final_cw1 = w1;
        s.fog_enable = ffm[0x2A4u / 4u]; s.fog_mode = ffm[0x29Cu / 4u];
        memcpy(&s.fog_p0, &ffm[0x9C0u / 4u], 4); memcpy(&s.fog_p1, &ffm[0x9C4u / 4u], 4);
        s.fog_color = d3d8_ff_fog_color(g_exec_fog_color);
    }
    for (unsigned k = 0; k < c->count; ++k) {
        float in[16][4];
        for (unsigned a = 0; a < 16; ++a) { in[a][0] = in[a][1] = in[a][2] = 0; in[a][3] = 1; }
        uint32_t vb = c->va_offset[0];
        memcpy(in[0], ram + vb + 24u * k, 12);
        { uint32_t col; memcpy(&col, ram + vb + 24u * k + 12, 4);
          in[3][0] = ((col >> 16) & 255) / 255.0f; in[3][1] = ((col >> 8) & 255) / 255.0f; in[3][2] = (col & 255) / 255.0f; in[3][3] = (col >> 24) / 255.0f; }
        memcpy(in[9], ram + vb + 24u * k + 16, 8);
        if (nv2a_ff_vertex(ffm, (const float (*)[4])in, v[k])) return 0;
        if (k < (unsigned)g_exec_pos_n) { v[k][0][0] = g_exec_pos[k][0]; v[k][0][1] = g_exec_pos[k][1];
                                          if (g_exec_zw) { v[k][0][2] = g_exec_z; v[k][0][3] = g_exec_w; } }
    }
    if (!g_exec_keep_surfaces) nv2a_metal_invalidate(NULL);
    if (nv2a_metal_draw(&s, ram + TEX, TW * TW * 4, (uint8_t *)target, RTPITCH * RTH, depth, RTW * 4 * RTH,
                        (const float (*)[16][4])v, c->count, c->prim) < 0) {
        printf("executor refused the FF draw: %s\n", nv2a_metal_last_reject()); return 0;
    }
    if (!g_exec_no_sync) nv2a_metal_sync();
    return 1;
}
/* THE PER-INDEX VERTEX CACHE: an indexed FF list naming 4 vertices 6 times
 * is evaluated 4 times, and each emitted vertex is exactly nv2a_ff_vertex of
 * its index. Then a second draw re-evaluates (the cache is per draw). */
static void ff_cache_tests(void)
{
    static const uint16_t I[6] = { 0, 1, 2, 2, 1, 3 };
    static uint32_t ffm[2048];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    const char *why;
    case_ff(&c);
    c.draw_kind = 2; c.prim = 5; c.count = 6; c.idx_ptr = IB; c.nidx = 6;
    memcpy(ram + IB, I, sizeof I); for (int k = 0; k < 6; ++k) c.idx[k] = I[k];
    set_state(&c, 0x30C, 0);
    why = d3d8_host_ff_registers(&c, ffm);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (!why) why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, I, ffm, nv2a_ff_vertex, &d);
    CHECK(!why && d.nverts == 6 && d.ff_evals == 4, "vertex cache: 6 indices over 4 vertices evaluated 4 times (%s, %u vertices, %u evaluations)",
          why ? why : "built", d.nverts, d.ff_evals);
    int same = 1;
    for (unsigned k = 0; k < d.nverts && k < 6; ++k) {
        float in[16][4], out[16][4];
        uint32_t i = I[k];
        for (unsigned a = 0; a < 16; ++a) { in[a][0] = in[a][1] = in[a][2] = 0; in[a][3] = 1; }
        memcpy(in[0], ram + VB + 24u * i, 12);
        { uint32_t col; memcpy(&col, ram + VB + 24u * i + 12, 4);
          in[3][0] = ((col >> 16) & 255) / 255.0f; in[3][1] = ((col >> 8) & 255) / 255.0f; in[3][2] = (col & 255) / 255.0f; in[3][3] = (col >> 24) / 255.0f; }
        memcpy(in[9], ram + VB + 24u * i + 16, 8);
        nv2a_ff_vertex(ffm, (const float (*)[4])in, out);
        if (d.verts[k].p[0] != out[0][0] || d.verts[k].p[1] != out[0][1] || d.verts[k].p[3] != out[0][3] ||
            memcmp(d.verts[k].d0, out[3], 16) || memcmp(d.verts[k].t[0], out[9], 16)) same = 0;
    }
    CHECK(same, "vertex cache: every emitted vertex is nv2a_ff_vertex of its own index");
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, I, ffm, nv2a_ff_vertex, &d);
    CHECK(!why && d.ff_evals == 4, "vertex cache: the next draw evaluates afresh (%u)", d.ff_evals);
}
/* THE TEXTURE CACHE: the same texture drawn twice in a flip is decoded once
 * and hashed once; rewritten and drawn after a flip it is re-hashed and
 * re-decoded. */
static void tex_cache_tests(void)
{
    static uint32_t ffm[2048];
    static uint16_t px[RTPITCH / 2 * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    unsigned long long h0, b0, s0, h1, b1, s1, h2, b2, s2;
    case_ff(&c); set_state(&c, 0x30C, 0);
    d3d8_host_ff_registers(&c, ffm);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d)) { CHECK(0, "texture cache: build"); return; }
    background(px);
    d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, px, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_metal_stats(&h0, &b0, &s0, NULL, NULL);
    d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, px, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_metal_stats(&h1, &b1, &s1, NULL, NULL);
    CHECK(h1 == h0 + 1 && b1 == b0 && s1 == s0, "texture cache: a second draw in the flip is a hit, no decode, no hash");
    ram[TEX] ^= 0xFF;                                   /* rewritten */
    d3d8_host_2d_flip();                                /* the "ff" arm has the FF shadow armed */
    d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, px, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_metal_stats(&h2, &b2, &s2, NULL, NULL);
    CHECK(d3d8_host_2d_flip_count() > 0 && s2 == s1 + 1 && b2 == b1 + 1,
          "texture cache: after a flip a rewritten texture is re-hashed and re-decoded (flips %llu, hashes +%llu, decodes +%llu)",
          d3d8_host_2d_flip_count(), s2 - s1, b2 - b1);
    ram[TEX] ^= 0xFF;
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

/* G74: POINTS AND LINES ON THE HOST (RECOMP_D3D8_HOST_POINTS=1). The
 * fixed-function vertices of case_ff drawn as a point list (sprites, or not),
 * a line list, a line loop and a line strip, by the host and by the
 * executor's own draw_points_lines from the same vertex unit's outputs; they
 * must agree. The control shifts the host's quads and must differ. */
static float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void set_points(D3D8HostDrawCheck *c, float size, float scale, int sprite)
{
    c->pt_valid = 1;
    c->pt_rs[0] = u_of(size); c->pt_rs[1] = u_of(0.0f); c->pt_rs[2] = sprite ? 1u : 0u; c->pt_rs[3] = 0;
    c->pt_rs[4] = u_of(1.0f); c->pt_rs[5] = u_of(0.0f); c->pt_rs[6] = u_of(0.0f); c->pt_rs[7] = u_of(64.0f);
    c->pt_dev_scale = u_of(scale);
}
static void compare_points(unsigned prim, unsigned count, float size, int sprite, int control, unsigned long long min_px)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH], ec[RTW * RTH];
    static uint32_t ffm[2048];
    char name[160];
    const char *why;
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df; unsigned steps = 0;
    snprintf(name, sizeof name, "prim %u count %u%s%s%s", prim, count, prim == 1 ? (sprite ? " sprites" : " points") : " lines",
             prim == 1 ? "" : "", control ? " CONTROL" : "");
    case_ff(&c); c.prim = prim; c.count = count; set_points(&c, size, 1.0f, sprite);
    c.rs_valid = 1; c.rs_cull = 0x900; c.rs_front = 0x900;          /* culling on: points and lines ignore it */
    why = d3d8_host_ff_registers(&c, ffm);
    if (why) { CHECK(0, "%s: register file (%s)", name, why); return; }
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, control, NULL, ffm, nv2a_ff_vertex, &d);
    CHECK(!why && d.cull_face == 0 && !d.ff_gpu, "%s: host builds it, uncull, on the CPU unit (%s)", name, why ? why : "built");
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
    printf("  %s: %u quads, %u vertices, reg %u | executor changed %llu, host %llu, over tolerance %llu, max r%u g%u b%u |"
           " depth %llu px, worst %u\n", name, d.pl_segs, d.nverts, d.point_reg, df.exec_changed, df.host_changed, df.mismatch,
           df.max_err[0], df.max_err[1], df.max_err[2], zb, steps);
    CHECK(df.exec_changed >= min_px, "%s: the executor drew it (%llu px)", name, df.exec_changed);
    if (!control)
        CHECK(df.mismatch == 0 && zb == 0, "%s: host and executor agree, colour (%llu px over) and depth (%llu)", name, df.mismatch, zb);
    else
        CHECK(df.mismatch > 8, "%s: the perturbed host draw differs (%llu px)", name, df.mismatch);
}
/* Points placed exactly on the pixel grid: a 1-pixel quad whose edges pass
 * through pixel centres. Both renderers must pick the same pixel (or none). */
static void point_edge_case(float X, float Y, int depth)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zhost[RTW * RTH];
    static uint32_t ffm[2048];
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df;
    const char *why;
    case_ff(&c); c.prim = 1; c.count = 1; set_points(&c, 1.0f, 1.0f, 0);
    if (!depth) set_state(&c, 0x30C, 0);
    d3d8_host_ff_registers(&c, ffm);
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
    if (why || d.nverts != 6) { CHECK(0, "edge point %g,%g: build (%s)", X, Y, why ? why : "count"); return; }
    {   static const unsigned tri[6] = { 0, 1, 2, 0, 2, 3 };
        static const float sx[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, sy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
        for (unsigned m = 0; m < 6; ++m) { d.verts[m].p[0] = X + sx[tri[m]]; d.verts[m].p[1] = Y + sy[tri[m]]; }
        d.bb_x0 = (int32_t)floorf(X - 0.5f) - 2; d.bb_x1 = (int32_t)ceilf(X + 0.5f) + 2;
        d.bb_y0 = (int32_t)floorf(Y - 0.5f) - 2; d.bb_y1 = (int32_t)ceilf(Y + 0.5f) + 2; }
    g_exec_pos_n = 1; g_exec_pos[0][0] = X; g_exec_pos[0][1] = Y;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) { ++fails; g_exec_pos_n = 0; return; }
    g_exec_pos_n = 0;
    unsigned x0 = (unsigned)d.bb_x0, y0 = (unsigned)d.bb_y0, w = (unsigned)(d.bb_x1 - d.bb_x0 + 1), h = (unsigned)(d.bb_y1 - d.bb_y0 + 1);
    for (unsigned k = 0; k < w * h; ++k) zhost[k] = 1.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, zhost, x0, y0, w, h) != 0) { ++fails; return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    printf("  edge point at %g,%g%s: executor changed %llu, host %llu, over tolerance %llu\n", X, Y, depth ? " (depth)" : "",
           df.exec_changed, df.host_changed, df.mismatch);
    CHECK(df.mismatch == 0 && df.exec_changed == df.host_changed, "edge point at %g,%g: same pixel on both sides", X, Y);
}
/* THE DEPTH TIE. A point drawn again at its own depth (LEQUAL): the
 * executor passes its second draw against the depth its first wrote. The
 * host, seeded with that depth, must pass it too -- its z must be the bits
 * the executor's vertex stage produces, not a neighbour. */
static void point_depth_tie(float X, float Y)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH];
    static uint32_t ffm[2048];
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df;
    const char *why;
    case_ff(&c); c.prim = 1; c.count = 1; set_points(&c, 1.0f, 1.0f, 0);
    d3d8_host_ff_registers(&c, ffm);
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
    if (why || d.nverts != 6) { CHECK(0, "depth tie: build (%s)", why ? why : "count"); return; }
    {   static const unsigned tri[6] = { 0, 1, 2, 0, 2, 3 };
        static const float sx[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, sy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
        for (unsigned m = 0; m < 6; ++m) { d.verts[m].p[0] = X + sx[tri[m]]; d.verts[m].p[1] = Y + sy[tri[m]]; }
        d.bb_x0 = (int32_t)floorf(X - 0.5f) - 2; d.bb_x1 = (int32_t)ceilf(X + 0.5f) + 2;
        d.bb_y0 = (int32_t)floorf(Y - 0.5f) - 2; d.bb_y1 = (int32_t)ceilf(Y + 0.5f) + 2; }
    g_exec_pos_n = 1; g_exec_pos[0][0] = X; g_exec_pos[0][1] = Y;
    background(bg); memcpy(ex, bg, sizeof bg);
    if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) { ++fails; g_exec_pos_n = 0; return; }   /* first: writes its depth */
    nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
    memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    g_exec_keep_surfaces = 1;
    if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) { ++fails; g_exec_pos_n = 0; g_exec_keep_surfaces = 0; return; }
    g_exec_keep_surfaces = 0; g_exec_pos_n = 0;
    unsigned x0 = (unsigned)d.bb_x0, y0 = (unsigned)d.bb_y0, w = (unsigned)(d.bb_x1 - d.bb_x0 + 1), h = (unsigned)(d.bb_y1 - d.bb_y0 + 1);
    for (unsigned y = 0; y < h; ++y) memcpy(zhost + y * w, zexec + (y0 + y) * RTW + x0, w * sizeof(float));
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + y0 * (RTPITCH / 2) + x0, RTPITCH / 2, zhost, x0, y0, w, h) != 0) { ++fails; return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    printf("  depth tie at %g,%g: executor changed %llu, host %llu, over tolerance %llu\n", X, Y, df.exec_changed, df.host_changed, df.mismatch);
    CHECK(df.exec_changed == 1 && df.host_changed == 1 && df.mismatch == 0, "depth tie at %g,%g: both draw it again", X, Y);
}
/* SWEEP: host point depth vs executor point depth, bit for bit, over random
 * z and w. The host divides z by 16777215 on the CPU, the executor in its
 * vertex shader; the GPU's divide is correctly rounded, so they agree (a
 * reciprocal multiply on the CPU was measured to differ in 109 of 300). */
static unsigned point_depth_sweep(unsigned n)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH];
    static uint32_t ffm[2048];
    unsigned bad = 0, seed = 12345;
    for (unsigned it = 0; it < n; ++it) {
        D3D8HostDrawCheck c; D3D8Host2DDraw d;
        float X = 40.5f, Y = 50.5f, z, w;
        seed = seed * 1103515245u + 12345u; z = (float)(seed >> 8);           /* 0 .. 2^24 */
        if (z > 16777215.0f) z = 16777215.0f;
        seed = seed * 1103515245u + 12345u; w = 0.05f + (float)(seed >> 8) / 16777216.0f * 200.0f;
        case_ff(&c); c.prim = 1; c.count = 1; set_points(&c, 1.0f, 1.0f, 0);
        set_state(&c, 0x354, 0x207);                                           /* ALWAYS, writes on */
        d3d8_host_ff_registers(&c, ffm);
        memset(&d, 0, sizeof d); d.verts = verts;
        if (d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d) || d.nverts != 6) { ++bad; continue; }
        {   static const unsigned tri[6] = { 0, 1, 2, 0, 2, 3 };
            static const float sx[4] = { -0.5f, 0.5f, 0.5f, -0.5f }, sy[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
            for (unsigned m = 0; m < 6; ++m) {
                d.verts[m].p[0] = X + sx[tri[m]]; d.verts[m].p[1] = Y + sy[tri[m]];
                d.verts[m].p[2] = z / 16777215.0f; d.verts[m].p[3] = w;
            }
            d.bb_x0 = 38; d.bb_x1 = 43; d.bb_y0 = 48; d.bb_y1 = 53; d.z_min = d.z_max = d.verts[0].p[2]; d.w_min = w; }
        for (unsigned y = 0; y < RTH; ++y)
            for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
        g_exec_pos_n = 1; g_exec_pos[0][0] = X; g_exec_pos[0][1] = Y; g_exec_zw = 1; g_exec_z = z; g_exec_w = w;
        background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
        if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) { ++bad; g_exec_pos_n = 0; g_exec_zw = 0; continue; }
        g_exec_pos_n = 0; g_exec_zw = 0;
        nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
        for (unsigned k = 0; k < 36; ++k) zhost[k] = 1.0f;
        if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho + 48 * (RTPITCH / 2) + 38, RTPITCH / 2, zhost, 38, 48, 6, 6) != 0) { ++bad; continue; }
        {   float a = zexec[50 * RTW + 40], b = zhost[2 * 6 + 2];
            if (memcmp(&a, &b, 4)) { if (bad < 6) printf("    z %.1f w %g: executor %.9g host %.9g\n", z, w, a, b); ++bad; } }
    }
    return bad;
}
/* G75: BUMPENVMAP ON THE HOST (RECOMP_D3D8_HOST_BUMP=1). A pre-transformed
 * strip whose unit 0 is a du/dv map and unit 1 an environment map, with
 * D3D's BUMPENVMAP (0x19) or BUMPENVMAPLUMINANCE (0x1A) colour op on stage
 * 0: the stage program gives unit 1 mode 6 or 7. The matrix sits in D3D's
 * texture-stage state 22..27 of stage 0, the stage a fixed-function draw's
 * SetTextureState_BumpEnv pushes to unit 1. The executor draws the same
 * strip from the NV2A state the XDK would have pushed -- the matrix given
 * here as M00 M01 M10 M11, not read from the host's description -- and the
 * two must agree. The negative control is the executor drawing it WITHOUT
 * the displacement, which must not. */
static void case_ff_stencil(D3D8HostDrawCheck *c, uint32_t zpass, uint32_t ref);
enum { TEX2 = 0x30000, BVB = 0x14000 };
static void bump_case(D3D8HostDrawCheck *c, unsigned op, const float mat[4], float ls, float lo)
{
    static const float UV[4][2] = { { -0.2f, 0.05f }, { 1.3f, 0.0f }, { 0.1f, 1.2f }, { 1.4f, 1.35f } };
    base_check(c);
    for (int i = 0; i < 4; ++i) {
        uint32_t v = BVB + 36u * i;
        putf(v, A_POS[i][0]); putf(v + 4, A_POS[i][1]); putf(v + 8, 0.25f); putf(v + 12, 1.0f);
        put32(v + 16, 0xFFFFFFFFu);
        putf(v + 20, UV[i][0] * 0.5f); putf(v + 24, UV[i][1] * 0.5f);     /* du/dv: 2x magnified */
        putf(v + 28, UV[i][0]); putf(v + 32, UV[i][1]);
    }
    for (unsigned y = 0; y < TW; ++y)
        for (unsigned x = 0; x < TW; ++x) {
            uint8_t *p = ram + TEX + 4 * morton(x, y), *e = ram + TEX2 + 4 * morton(x, y);
            p[0] = (uint8_t)((x * 37 + y * 11) & 0xFF); p[1] = (uint8_t)((y * 29 + x * 5 + 0x80) & 0xFF);   /* du, dv */
            p[2] = (uint8_t)(x * 8); p[3] = 0xFF;                                                          /* L */
            e[0] = (uint8_t)(x * 8); e[1] = (uint8_t)(y * 8); e[2] = (uint8_t)((x ^ y) * 8); e[3] = 0xFF;
        }
    c->vs_handle = 0x244;                                      /* XYZRHW | DIFFUSE | TEX2 */
    c->draw_kind = 1; c->prim = 6; c->start = 0; c->count = 4;
    c->va_on = (1u << 0) | (1u << 3) | (1u << 9) | (1u << 10);
    c->va_offset[0] = BVB;      c->va_format[0] = (36u << 8) | 0x42u;
    c->va_offset[3] = BVB + 16; c->va_format[3] = (36u << 8) | 0x40u;
    c->va_offset[9] = BVB + 20; c->va_format[9] = (36u << 8) | 0x22u;
    c->va_offset[10] = BVB + 28; c->va_format[10] = (36u << 8) | 0x22u;
    for (unsigned s = 0; s < 2; ++s) {
        c->tex[s] = 0x5678 + s; c->data[s] = s ? TEX2 : TEX;
        c->format[s] = 0x1u | 0x20u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24);
        c->tss[s][0] = c->tss[s][1] = c->tss[s][2] = 1;       /* wrap */
        c->tss[s][3] = s ? 2u : 1u; c->tss[s][4] = s ? 2u : 1u;   /* du/dv point, environment linear */
    }
    c->tss[0][12] = op;
    memcpy(&c->tss[0][22], &mat[0], 4); memcpy(&c->tss[0][23], &mat[1], 4);   /* M00, M01 */
    memcpy(&c->tss[0][25], &mat[2], 4); memcpy(&c->tss[0][24], &mat[3], 4);   /* M10 is word 25, M11 word 24 */
    memcpy(&c->tss[0][26], &ls, 4); memcpy(&c->tss[0][27], &lo, 4);
    /* Stage 1: SELECTARG1 TEXTURE -- the output is unit 1's bumped texel. */
    c->ffc_cur.texture_bound_mask = 3;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[1][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[1][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_TEXTURE;
    c->ffc_cur.tss[1][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[1][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
}
static int exec_draw_bump(const D3D8HostDrawCheck *c, const D3D8Host2DDraw *d, unsigned mode, const float mat[4],
                          float ls, float lo, uint16_t *target)
{
    static float v[4][16][4];
    static NV2ATextureCopy s, extra[3];
    memset(&s, 0, sizeof s); memset(extra, 0, sizeof extra); memset(v, 0, sizeof v);
    s.clip_w = RTW; s.clip_h = RTH; s.target_pitch = RTPITCH; s.target_bpp = 2; s.depth_pitch = RTW * 4;
    s.z_clip_min = 0.0f; s.z_clip_max = 16777215.0f; s.z_cull = 1;
    s.combiner_count = d->cc;
    memcpy(s.color_icw, d->ci, sizeof s.color_icw); memcpy(s.alpha_icw, d->ai, sizeof s.alpha_icw);
    memcpy(s.color_ocw, d->co, sizeof s.color_ocw); memcpy(s.alpha_ocw, d->ao, sizeof s.alpha_ocw);
    memcpy(s.const0, d->k0, sizeof s.const0); memcpy(s.const1, d->k1, sizeof s.const1);
    s.texture_mask = 3; s.untextured = 0; s.modulate = 1;
    s.width = s.height = TW; s.pitch = TW * 4; s.levels = 1; s.rgba8 = 1; s.min_filter = 1; s.linear = 0; s.repeat = 1;
    extra[0].width = extra[0].height = TW; extra[0].pitch = TW * 4; extra[0].levels = 1; extra[0].rgba8 = 1;
    extra[0].min_filter = 2; extra[0].linear = 1; extra[0].repeat = 1;
    s.extra_stages = extra; s.extra_texture[0] = ram + TEX2; s.extra_size[0] = TW * TW * 4;
    if (mode) {
        s.bump[1] = mode; s.bump_input[1] = 0;
        memcpy(s.bump_mat[1], mat, 16); s.bump_scale[1] = ls; s.bump_offset[1] = lo;
    }
    for (unsigned k = 0; k < 4; ++k) {
        float pos[4];
        memcpy(pos, ram + BVB + 36u * k, 16);
        v[k][0][0] = exec_snap(pos[0] + g_exec_offset); v[k][0][1] = exec_snap(pos[1] + g_exec_offset);
        v[k][0][2] = pos[2] * 16777215.0f; v[k][0][3] = 1.0f / pos[3];
        v[k][3][0] = v[k][3][1] = v[k][3][2] = v[k][3][3] = 1.0f;
        v[k][9][3] = v[k][10][3] = 1.0f;
        memcpy(v[k][9], ram + BVB + 36u * k + 20, 8); memcpy(v[k][10], ram + BVB + 36u * k + 28, 8);
    }
    nv2a_metal_invalidate(NULL);
    if (nv2a_metal_draw(&s, ram + TEX, TW * TW * 4, (uint8_t *)target, RTPITCH * RTH, NULL, 0,
                        (const float (*)[16][4])v, 4, c->prim) < 0) {
        printf("executor refused the bump draw: %s\n", nv2a_metal_last_reject()); return 0;
    }
    nv2a_metal_sync();
    return 1;
}
static void bump_tests(void)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH], fl[RTPITCH / 2 * RTH];
    const float mat[4] = { 0.09f, -0.03f, 0.05f, 0.11f };          /* M00 M01 M10 M11: asymmetric */
    const float ls = 0.75f, lo = 0.2f;
    for (unsigned mode = 6; mode <= 7; ++mode) {
        D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df, dfl;
        const char *why;
        char name[64];
        snprintf(name, sizeof name, "BUMPENVMAP%s (mode %u)", mode == 7 ? "LUMINANCE" : "", mode);
        bump_case(&c, mode == 6 ? 0x19u : 0x1Au, mat, ls, lo);
        d3d8_host_2d_set_bump(0);
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        CHECK(why && strstr(why, "texture shader mode (fixed function)") && strstr(why, mode == 6 ? " 6 " : " 7 "),
              "%s, switch off: left to the executor (%s)", name, why ? why : "built");
        d3d8_host_2d_set_bump(1);
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        CHECK(!why && d.tmask == 3u && d.bump[1] == mode && d.bump_in[1] == 0u, "%s: the host builds it (%s)", name, why ? why : "built");
        if (why) continue;
        CHECK(d.bump_mat[1][0] == mat[0] && d.bump_mat[1][1] == mat[1] && d.bump_mat[1][2] == mat[2] && d.bump_mat[1][3] == mat[3]
              && d.bump_scale[1] == ls && d.bump_offset[1] == lo, "%s: matrix M00 M01 M10 M11 and luminance from stage 0's words", name);
        background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg); memcpy(fl, bg, sizeof bg);
        if (!exec_draw_bump(&c, &d, mode, mat, ls, lo, ex) || !exec_draw_bump(&c, &d, 0, mat, ls, lo, fl) || !host_draw(&d, ho)) {
            ++fails; continue;
        }
        d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
        d3d8_host_2d_diff(bg, ex, fl, RTPITCH / 2, RTH, 1, &dfl);
        printf("  %s: executor changed %llu, host %llu, over tolerance %llu, max error r%u g%u b%u | executor without the"
               " displacement: %llu px over\n", name, df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1],
               df.max_err[2], dfl.mismatch);
        CHECK(df.exec_changed > 3000, "%s: the executor drew it (%llu px)", name, df.exec_changed);
        CHECK(df.mismatch == 0, "%s: host and executor agree within one 565 step (%llu px over)", name, df.mismatch);
        CHECK(dfl.mismatch > 1000, "%s CONTROL: the undisplaced draw differs (%llu px)", name, dfl.mismatch);
        /* Mirror addressing on the bump unit is not the executor's: refused. */
        c.tss[1][0] = c.tss[1][1] = 2;
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        CHECK(why && strstr(why, "bump env unit's address mode"), "%s, mirrored bump unit: refused (%s)", name, why ? why : "built");
    }
    d3d8_host_2d_set_bump(0);
}
/* G75: LINEAR 32-BIT (0x12 A8R8G8B8, 0x1E X8R8G8B8) ON THE HOST
 * (RECOMP_D3D8_HOST_LIN32=1): case A's strip over a 32x32 image rectangle,
 * coordinates in texels, host against the executor's lin32 path. Off, the
 * format is refused by name. The control is case A's perturbation. */
static void lin32_tests(void)
{
    for (unsigned k = 0; k < 2; ++k) {
        uint32_t fb = k ? 0x1Eu : 0x12u;
        D3D8HostDrawCheck c;
        D3D8Host2DDraw d;
        const char *why;
        char name[64];
        case_a(&c);
        for (unsigned y = 0; y < TW; ++y)
            for (unsigned x = 0; x < TW; ++x) {
                uint8_t *p = ram + TEX + y * 256u + 4u * x;               /* pitch 256: wider than the row */
                p[0] = (uint8_t)(x * 8); p[1] = (uint8_t)(y * 8); p[2] = (uint8_t)((x ^ y) * 8);
                p[3] = (uint8_t)(((x / 4 + y / 4) & 1) ? 255 - x * 4 : 8 + y * 6);
            }
        for (int i = 0; i < 4; ++i) { putf(VB + 28u * i + 20, A_UV[i][0] * 24.0f); putf(VB + 28u * i + 24, A_UV[i][1] * 24.0f); }
        c.format[0] = 0x1u | 0x20u | (fb << 8) | (1u << 16);
        c.size[0] = (TW - 1u) | ((TW - 1u) << 12) | ((256u / 64u - 1u) << 24);
        snprintf(name, sizeof name, "linear 32-bit 0x%02X", fb);
        d3d8_host_2d_set_lin32(0);
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        CHECK(why && strstr(why, k ? "texture format 0x1E" : "texture format 0x12"), "%s, switch off: refused by name (%s)",
              name, why ? why : "built");
        d3d8_host_2d_set_lin32(1);
        compare(name, &c, 1);
        compare(name, &c, 0);
        d3d8_host_2d_set_lin32(0);
    }
}
/* G75: DRAWVERTICESUP. Case A's strip as a DrawVerticesUP draw: its
 * vertices in the UP ring (as the mirror copies them at the call), its arrays
 * offsets into that copy. Then the guest's vertex buffer is overwritten, so a
 * host that read guest RAM instead of the copy would draw garbage. */
static void up_tests(void)
{
    D3D8HostDrawCheck c;
    uint64_t pos;
    uint8_t *dst;
    case_a(&c);
    d3d8_host_2d_set_inline(1);
    d3d8_host_2d_set_bisect(16u);    /* the bump tests left other bytes under case A's texture key: hash every draw */
    dst = d3d8_host_2d_up_reserve(4u * 28u, &pos);
    CHECK(dst != NULL, "UP ring: reserve");
    if (!dst) return;
    memcpy(dst, ram + VB, 4u * 28u);
    d3d8_host_2d_up_publish(pos, 4u * 28u);
    c.draw_kind = 3; c.start = 0; c.up_pos = pos; c.up_bytes = 4u * 28u; c.up_stride = 28;
    c.va_offset[0] = 0; c.va_offset[3] = 16; c.va_offset[9] = 20;
    compare("DrawVerticesUP strip", &c, 1);
    {   D3D8Host2DDraw d; const char *why;
        c.up_over = 1;
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        CHECK(why && strstr(why, "DrawVerticesUP"), "DrawVerticesUP whose copy did not fit: refused (%s)", why ? why : "built");
        c.up_over = 0; }
    /* Begin/End (kind 4): the same strip as the mirror assembles it from
     * SetVertexData4f -- 16 float4 a vertex, diffuse as floats -- must build
     * the same vertices as the strip from its vertex buffer. */
    {   static float iv[4][16][4];
        D3D8HostDrawCheck c4, c1; D3D8Host2DDraw d4, d1; static D3D8H2DVertex v1[64];
        const char *why4, *why1;
        unsigned same = 1;
        case_a(&c1);
        memset(iv, 0, sizeof iv);
        for (unsigned k = 0; k < 4; ++k) {
            uint32_t col; memcpy(iv[k][0], ram + VB + 28u * k, 16); memcpy(&col, ram + VB + 28u * k + 16, 4);
            iv[k][3][0] = ((col >> 16) & 255) / 255.0f; iv[k][3][1] = ((col >> 8) & 255) / 255.0f;
            iv[k][3][2] = (col & 255) / 255.0f; iv[k][3][3] = (col >> 24) / 255.0f;
            memcpy(iv[k][9], ram + VB + 28u * k + 20, 8); iv[k][9][3] = 1.0f;
        }
        c4 = c1; c4.draw_kind = 4; c4.start = 0;
        dst = d3d8_host_2d_up_reserve(sizeof iv, &pos);
        memcpy(dst, iv, sizeof iv); d3d8_host_2d_up_publish(pos, sizeof iv);
        c4.up_pos = pos; c4.up_bytes = sizeof iv; c4.up_stride = 256;
        for (unsigned i = 0; i < 16; ++i) { c4.va_format[i] = (256u << 8) | 0x42u; c4.va_offset[i] = 16u * i; }
        memset(&d1, 0, sizeof d1); d1.verts = v1; why1 = d3d8_host_2d_build(&c1, ram, RAM_SIZE, 0, &d1);
        memset(&d4, 0, sizeof d4); d4.verts = verts; why4 = d3d8_host_2d_build(&c4, ram, RAM_SIZE, 0, &d4);
        if (!why1 && !why4 && d1.nverts == d4.nverts)
            for (unsigned k = 0; k < d1.nverts; ++k)
                for (unsigned j = 0; j < 4; ++j)
                    if (fabsf(v1[k].p[j] - verts[k].p[j]) > 0 || fabsf(v1[k].d0[j] - verts[k].d0[j]) > 1e-6f ||
                        fabsf(v1[k].t[0][j] - verts[k].t[0][j]) > 0) same = 0;
        CHECK(!why1 && !why4 && d1.nverts == d4.nverts && same, "Begin/End strip: the same vertices as the strip from"
              " its buffer (%s / %s, %u / %u)", why1 ? why1 : "built", why4 ? why4 : "built", d1.nverts, d4.nverts);
    }
    d3d8_host_2d_set_bisect(0);
    /* G75: a LEQUAL stencil test (the HUD's shadow quads) outside draw mode
     * -- the shadow's crop has no stencil -- is refused with or without
     * RECOMP_D3D8_HOST_STENCIL. */
    {   static uint32_t ffm[2048];
        D3D8HostDrawCheck cs; D3D8Host2DDraw ds; const char *why;
        case_ff_stencil(&cs, 0x1E00, 1); set_state(&cs, 0x364, 0x203);
        d3d8_host_ff_registers(&cs, ffm);
        for (int on = 0; on < 2; ++on) {
            d3d8_host_2d_set_stencil(on);
            memset(&ds, 0, sizeof ds); ds.verts = verts;
            why = d3d8_host_draw_build(&cs, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &ds);
            CHECK(why && !strcmp(why, "stencil func not ALWAYS"), "stencil LEQUAL in the shadow, switch %s: refused (%s)",
                  on ? "on" : "off", why ? why : "built");
        }
        d3d8_host_2d_set_stencil(0);
    }
}
static void points_tests(void)
{
    {   unsigned n = getenv("H2D_ZSWEEP") ? (unsigned)atoi(getenv("H2D_ZSWEEP")) : 64u, bad = point_depth_sweep(n);
        CHECK(bad == 0, "point depth: host and executor write the same bits (%u of %u differ)", bad, n); }
    uint32_t rs[8] = { 0 };
    D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
    const char *why;
    /* SET_POINT_SIZE from D3D's render states, 0x195140's arithmetic. */
    rs[0] = u_of(4.0f); rs[1] = u_of(0.0f); rs[7] = u_of(64.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 32u, "point size: 4 px -> 32 (1/8 px, +1/2 truncated)");
    CHECK(d3d8_host_point_size_reg(rs, u_of(2.0f)) == 64u, "point size: the device scale multiplies it (8 px -> 64)");
    rs[0] = u_of(100.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 0x1FFu, "point size: cut to 64 px, and 512 to 0x1FF");
    rs[0] = u_of(0.5f); rs[1] = u_of(1.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 8u, "point size: raised to POINTSIZE_MIN (1 px -> 8)");
    rs[0] = u_of(10.0f); rs[1] = u_of(0.0f); rs[7] = u_of(3.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 24u, "point size: cut to POINTSIZE_MAX (3 px -> 24)");
    rs[0] = u_of(-1.0f); rs[1] = u_of(-5.0f); rs[7] = u_of(64.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 0x1FFu, "point size: negative compares unsigned -> 0x1FF");
    rs[0] = u_of(1.3f); rs[1] = u_of(0.0f);
    CHECK(d3d8_host_point_size_reg(rs, u_of(1.0f)) == 10u && f_of(rs[0]) == 1.3f, "point size: 1.3 px -> 10.9 -> 10");

    /* Pixels against the executor's draw_points_lines. */
    compare_points(1, 4, 16.0f, 1, 0, 400);         /* sprites: texture 0..1 over each quad */
    compare_points(1, 4, 16.0f, 1, 1, 400);
    compare_points(1, 4, 6.0f, 0, 0, 60);           /* plain points: the vertex's own coordinates */
    compare_points(1, 4, 0.0f, 0, 0, 2);            /* size 0: one pixel */
    compare_points(2, 4, 1.0f, 0, 0, 50);           /* two segments */
    compare_points(2, 4, 1.0f, 0, 1, 50);
    compare_points(3, 4, 1.0f, 0, 0, 100);          /* loop: four */
    compare_points(3, 2, 1.0f, 0, 0, 25);           /* two-vertex loop: one */
    compare_points(4, 4, 1.0f, 0, 0, 70);           /* strip: three */

    {   static const float P[] = { 40.0f, 40.5f, 41.0f, 40.25f, 40.75f, 40.03125f, 40.96875f, 40.5078125f };
        for (unsigned i = 0; i < sizeof P / sizeof P[0]; ++i)
            for (unsigned j = 0; j < sizeof P / sizeof P[0]; j += 3) point_edge_case(P[i], P[j] + 10.0f, 0);
        point_edge_case(40.0f, 51.0f, 1); point_edge_case(40.5f, 50.5f, 1);
        point_depth_tie(40.5f, 50.5f); point_depth_tie(60.25f, 70.75f); point_depth_tie(33.0f, 21.0f);
    }
    /* What stays with the executor. */
    case_ff(&c); c.prim = 1; c.count = 4; set_points(&c, 4.0f, 1.0f, 1); c.pt_rs[3] = 1;
    d3d8_host_ff_registers(&c, ffm); memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
    CHECK(why && strstr(why, "POINTSCALEENABLE"), "attenuated points: left to the executor (%s)", why ? why : "built");
    case_ff(&c); c.prim = 2; c.count = 4; c.pt_valid = 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
    CHECK(why && strstr(why, "no point state"), "no point state from the mirror: left to the executor (%s)", why ? why : "built");
    case_a(&c); c.prim = 1; c.count = 4; set_points(&c, 4.0f, 1.0f, 1);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, NULL, NULL, &d);
    CHECK(why && strstr(why, "pre-transformed"), "pre-transformed points: left to the executor (%s)", why ? why : "built");
}

/* G52: RECOMP_D3D8_HOST_FF_GPU. The same fixed-function quad, transformed by
 * the executor's GPU unit (nv2a_ff_key + its RECOMP_METAL_FF vertex function)
 * instead of nv2a_ff_vertex per vertex on the CPU, against the executor's
 * draw. The control drops every other triangle and must differ. */
static void compare_ff_gpu(int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], ho[RTPITCH / 2 * RTH];
    static float zexec[RTW * RTH], zhost[RTW * RTH];
    static uint32_t ffm[2048];
    const char *name = control ? "FF on the GPU CONTROL" : "FF on the GPU", *why;
    D3D8HostDrawCheck c; D3D8Host2DDraw d, dc; D3D8H2DDiff df; unsigned steps = 0;
    case_ff(&c);
    why = d3d8_host_ff_registers(&c, ffm);
    CHECK(!why, "%s: register file (%s)", name, why ? why : "built");
    if (why) return;
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    d3d8_host_2d_set_ff_gpu(NULL);
    memset(&dc, 0, sizeof dc); dc.verts = verts;           /* the CPU build, for the executor's draw */
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &dc);
    CHECK(!why && !dc.ff_gpu, "%s: without the hook the draw is built on the CPU (%s)", name, why ? why : "built");
    if (why) return;
    background(bg); memcpy(ex, bg, sizeof bg); memcpy(ho, bg, sizeof bg);
    if (!exec_draw_ff(&c, &dc, ffm, ex, ram + ZS)) { ++fails; return; }
    nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zexec);
    d3d8_host_2d_set_ff_gpu(d3d8_host_2d_metal_ff_gpu);
    static D3D8H2DVertex gv[D3D8H2D_MAX_VERTS];
    memset(&d, 0, sizeof d); d.verts = gv;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, control, NULL, ffm, nv2a_ff_vertex, &d);
    if (!why && !d.ff_gpu) {
        /* The first ask starts the compile in the background (the hook never
         * waits: that draw is evaluated on the CPU, as the executor's batch
         * is); once it has finished the same draw goes to the GPU. */
        CHECK(d.nverts == 6 || (control && d.nverts == 3), "%s: while the unit compiles, built on the CPU (%u vertices)", name, d.nverts);
        nv2a_metal_pipelines_settle();
        memset(&d, 0, sizeof d); d.verts = gv;
        why = d3d8_host_draw_build(&c, ram, RAM_SIZE, control, NULL, ffm, nv2a_ff_vertex, &d);
    }
    CHECK(!why && d.ff_gpu && d.vs_fn && d.vs_nidx == (control ? 3u : 6u),
          "%s: built for the GPU unit (%s, ff_gpu %u, %u indices)", name, why ? why : "built", d.ff_gpu, d.vs_nidx);
    if (why || !d.ff_gpu) {
        unsigned long long ok, nokey, nofn; extern void d3d8_host_2d_metal_ff_gpu_stats(unsigned long long *, unsigned long long *, unsigned long long *);
        d3d8_host_2d_metal_ff_gpu_stats(&ok, &nokey, &nofn);
        printf("  hook: accepted %llu, no key %llu, no function %llu\n", ok, nokey, nofn);
        return;
    }
    for (unsigned k = 0; k < RTW * RTH; ++k) zhost[k] = 1.0f;
    if (d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, ho, RTPITCH / 2, zhost, 0, 0, RTW, RTH) != 0) {
        CHECK(0, "%s: render (%s)", name, d3d8_host_2d_metal_last_error()); return; }
    d3d8_host_2d_diff(bg, ex, ho, RTPITCH / 2, RTH, 1, &df);
    unsigned long long zb = d3d8_host_2d_depth_diff(zexec, zhost, (size_t)RTW * RTH, 1, &steps);
    printf("  %s: executor changed %llu, host %llu, over tolerance %llu, max r%u g%u b%u | depth %llu px, worst %u\n",
           name, df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2], zb, steps);
    if (!control)
        CHECK(df.exec_changed > 500 && df.mismatch <= 8 && steps <= 4,
              "%s: host and executor agree (%llu px over, depth worst %u steps)", name, df.mismatch, steps);
    else
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
static unsigned long long g_fake_skipped, g_fake_seen; static int g_fake_skip;
static unsigned long long fake_seen(void) { return g_fake_seen; }
static void fake_skip(int on) { g_fake_skip = on; }
static unsigned long long fake_skipped(void) { return g_fake_skipped; }
/* THE SAME BINDING SEQUENCE. Executor alone: draw into A, into B, into A.
 * With the host: draw into A, the host binds B (nv2a_metal_bind), draw into
 * B, into A. The swap B needs happens in the bind instead of in the draw, so
 * the counters -- surfaces uploaded, rebinds, evictions -- and every pixel of
 * both targets must come out the same. */
static int bind_sequence(int host_binds, unsigned long long cnt[3], uint16_t *outA, uint16_t *outB)
{
    enum { RT2 = RT + 0x40000 };
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    unsigned long long u0, h0, e0, u1, h1, e1;
    uint16_t *a = (uint16_t *)(ram + RT), *b = (uint16_t *)(ram + RT2);
    static uint8_t depth[RTW * 4 * RTH];
    background(a); background(b);
    nv2a_metal_invalidate(NULL); nv2a_metal_sync();
    g_exec_keep_surfaces = 1;
    nv2a_metal_bind_counters(&u0, &h0, &e0);
    case_b(&c); memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) return 0;
    if (!exec_draw(&c, &d, a, depth)) return 0;
    if (host_binds && nv2a_metal_bind((uint8_t *)b, RTPITCH * RTH, RTW, RTH, RTPITCH, NULL, 0, 0, 0) != 0) return 0;
    if (!exec_draw(&c, &d, b, depth)) return 0;
    if (!exec_draw(&c, &d, a, depth)) return 0;
    nv2a_metal_sync();
    g_exec_keep_surfaces = 0;
    nv2a_metal_bind_counters(&u1, &h1, &e1);
    cnt[0] = u1 - u0; cnt[1] = h1 - h0; cnt[2] = e1 - e0;
    memcpy(outA, a, RTPITCH * RTH); memcpy(outB, b, RTPITCH * RTH);
    return 1;
}
static void bind_tests(void)
{
    static uint16_t a1[RTPITCH / 2 * RTH], b1[RTPITCH / 2 * RTH], a2[RTPITCH / 2 * RTH], b2[RTPITCH / 2 * RTH];
    unsigned long long c1[3], c2[3];
    CHECK(bind_sequence(0, c1, a1, b1) && bind_sequence(1, c2, a2, b2), "bind: both sequences ran");
    printf("  bind: executor alone uploads %llu rebinds %llu evictions %llu; with the host binding B: %llu %llu %llu\n",
           c1[0], c1[1], c1[2], c2[0], c2[1], c2[2]);
    CHECK(c1[0] == c2[0] && c1[1] == c2[1] && c1[2] == c2[2] && c1[0] >= 2,
          "bind: the host's bind is the executor's swap -- same uploads, rebinds and evictions");
    CHECK(!memcmp(a1, a2, sizeof a1) && !memcmp(b1, b2, sizeof b1), "bind: both targets identical, pixel for pixel");
}

/* FF draw mode: the fixed-function quad drawn by the host into the
 * executor's bound surface, against the executor drawing it itself. */
static int ff_draw_arm(int host, int control, uint16_t *out, float *zout)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
    uint16_t *rt = (uint16_t *)(ram + RT);
    background(rt); logo_depth();
    draw_binder(rt);
    case_ff(&c);
    if (d3d8_host_ff_registers(&c, ffm)) return 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_draw_build(&c, ram, RAM_SIZE, control, NULL, ffm, nv2a_ff_vertex, &d)) return 0;
    if (host) {
        if (!d3d8_host_2d_metal_external(&d, ram, RAM_SIZE)) { printf("external: %s\n", d3d8_host_2d_metal_last_error()); return 0; }
        nv2a_metal_sync();
    } else {
        g_exec_keep_surfaces = 1;
        int ok = exec_draw_ff(&c, &d, ffm, rt, ram + ZS);
        g_exec_keep_surfaces = 0;
        if (!ok) return 0;
    }
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout);
}
static void ff_draw_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad;
    background(bg);
    CHECK(ff_draw_arm(0, 0, a, za) && ff_draw_arm(1, 0, b, zb), "FF draw mode: both arms drew");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
    zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
    printf("  FF draw mode: executor changed %llu, host (in the executor's surface) %llu, differing %llu | depth %llu, worst %u\n",
           df.exec_changed, df.host_changed, df.mismatch, zbad, steps);
    CHECK(df.exec_changed > 500 && df.mismatch == 0 && zbad == 0, "FF draw mode: identical to the executor drawing it, colour and depth");
    CHECK(ff_draw_arm(1, 1, b, zb), "FF draw mode CONTROL: drew");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 1, &df);
    CHECK(df.mismatch > 200, "FF draw mode CONTROL: the perturbed host draw is what the surface holds (%llu px)", df.mismatch);
}

/* THE BATCH JOIN. With RECOMP_METAL_BATCH on (the default) a host draw is
 * encoded into the executor's open encoder, so the executor's NEXT draw in
 * that encoder inherits whatever state the host left. Arm A: the executor
 * draws the FF quad and then the indexed list, both in one open batch. Arm B:
 * the host draws the quad and the executor draws the list into the same,
 * unflushed encoder. The two must match to the pixel -- the executor re-sets
 * everything it uses.
 *
 * BOTH ARMS KEEP THE TWO DRAWS IN ONE ENCODER, and that matters: with a sync
 * between them the list's DST_COLOR multiply differs by one 565 step on 268
 * pixels, with or without the host (measured 24 Sep 2026). Within a pass the
 * blend reads the destination at the tile's precision; across passes it reads
 * the stored 565. The executor's own batches have always done the former, so
 * a host draw that joins the batch reproduces the executor's picture more
 * closely than one in a pass of its own. */
static int join_arm(int host, uint16_t *out, float *zout)
{
    D3D8HostDrawCheck c, b; D3D8Host2DDraw d, bd; static uint32_t ffm[2048];
    static D3D8H2DVertex bverts[64];
    uint16_t *rt = (uint16_t *)(ram + RT);
    background(rt); logo_depth();
    draw_binder(rt);
    case_ff(&c);
    if (d3d8_host_ff_registers(&c, ffm)) return 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d)) return 0;
    g_exec_keep_surfaces = 1;
    g_exec_no_sync = 1;
    int ok = host ? d3d8_host_2d_metal_external(&d, ram, RAM_SIZE) : exec_draw_ff(&c, &d, ffm, rt, ram + ZS);
    g_exec_no_sync = 0;
    if (ok) {                                            /* no sync between: the list joins the host's encoder */
        case_b(&b);
        b.zs = 0x2345; b.zs_data = ZS; b.zs_format = 0x1u | (0x2Eu << 8);
        b.zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
        set_state(&b, 0x30C, 1); set_state(&b, 0x354, 0x203); set_state(&b, 0x35C, 1);
        memset(&bd, 0, sizeof bd); bd.verts = bverts;
        ok = !d3d8_host_2d_build(&b, ram, RAM_SIZE, 0, &bd) && exec_draw(&b, &bd, rt, ram + ZS);
    }
    g_exec_keep_surfaces = 0;
    if (!ok) return 0;
    nv2a_metal_sync();
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout);
}
static void join_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad;
    const char *bt = getenv("RECOMP_METAL_BATCH");
    background(bg);
    CHECK(join_arm(0, a, za) && join_arm(1, b, zb), "batch join (RECOMP_METAL_BATCH=%s): both arms drew", bt ? bt : "default on");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
    zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
    printf("  batch join: executor-only changed %llu, host-then-executor %llu, differing %llu | depth %llu, worst %u\n",
           df.exec_changed, df.host_changed, df.mismatch, zbad, steps);
    CHECK(df.exec_changed > 2000 && df.mismatch == 0 && zbad == 0,
          "batch join: an executor draw after a host draw in the same encoder is unchanged, colour and depth");
}

/* STENCIL, the game's class: func ALWAYS, zpass ZERO, colour on, over a
 * D24S8 surface whose stencil starts at 0x5A. The host must write exactly the
 * stencil the executor writes, as well as the colour and depth. */
static void case_ff_stencil(D3D8HostDrawCheck *c, uint32_t zpass, uint32_t ref)
{
    case_ff(c);
    set_state(c, 0x32C, 1);
    set_state(c, 0x364, 0x207); set_state(c, 0x378, zpass); set_state(c, 0x374, 0x1E00); set_state(c, 0x370, 0x1E00);
    set_state(c, 0x360, 0xFF); set_state(c, 0x36C, 0xFF); set_state(c, 0x368, ref);
}
static int stencil_arm(int host, uint32_t zpass, uint16_t *out, float *zout, uint8_t *sout)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
    uint16_t *rt = (uint16_t *)(ram + RT);
    const char *why;
    background(rt);
    for (unsigned y = 0; y < RTH; ++y)                   /* cleared depth, stencil 0x5A */
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF5Au }, 4);
    nv2a_metal_invalidate(NULL); nv2a_metal_sync();
    draw_binder(rt);
    case_ff_stencil(&c, zpass, 0x33);
    if (d3d8_host_ff_registers(&c, ffm)) return 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    if ((why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d))) { printf("stencil build: %s\n", why); return 0; }
    if (!d.stencil_test || !d.stencil_write) return 0;
    if (host) {
        if (!d3d8_host_2d_metal_external(&d, ram, RAM_SIZE)) { printf("external: %s\n", d3d8_host_2d_metal_last_error()); return 0; }
    } else {
        g_exec_keep_surfaces = 1;
        int ok = exec_draw_ff(&c, &d, ffm, rt, ram + ZS);
        g_exec_keep_surfaces = 0;
        if (!ok) return 0;
    }
    nv2a_metal_sync();
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout) && nv2a_metal_stencil_peek(ram + ZS, RTW, RTH, sout);
}
static void stencil_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    static uint8_t sa[RTW * RTH], sb[RTW * RTH];
    static const uint32_t ops[2] = { 0x0000u, 0x1E01u };            /* ZERO (the game's), REPLACE (ref 0x33) */
    background(bg);
    for (unsigned o = 0; o < 2; ++o) {
        D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad, sdiff = 0, swritten = 0;
        CHECK(stencil_arm(0, ops[o], a, za, sa) && stencil_arm(1, ops[o], b, zb, sb), "stencil zpass %X: both arms drew", ops[o]);
        d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
        zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
        for (unsigned k = 0; k < RTW * RTH; ++k) { sdiff += sa[k] != sb[k]; swritten += sa[k] != 0x5A; }
        printf("  stencil zpass %X: executor changed %llu px, differing %llu | depth %llu | stencil written %llu, differing %llu\n",
               ops[o], df.exec_changed, df.mismatch, zbad, swritten, sdiff);
        CHECK(df.exec_changed > 500 && df.mismatch == 0 && zbad == 0 && swritten > 500 && sdiff == 0,
              "stencil zpass %X: host writes the executor's colour, depth and stencil", ops[o]);
    }
    {   /* Refusals: a func other than ALWAYS, and state D3D never pushed. */
        D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
        case_ff_stencil(&c, 0, 0); set_state(&c, 0x364, 0x202);
        d3d8_host_ff_registers(&c, ffm); memset(&d, 0, sizeof d); d.verts = verts;
        const char *why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(why && !strcmp(why, "stencil func not ALWAYS"), "stencil: func EQUAL is refused (%s)", why ? why : "built");
        case_ff(&c); set_state(&c, 0x32C, 1); set_state(&c, 0x364, 0x207);
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(why && !strcmp(why, "stencil state not pushed"), "stencil: ops never pushed are refused, not defaulted (%s)", why ? why : "built");
    }
}

/* GPU COST, host against executor, in process: the same draw N times into
 * the executor's bound surface, unsynced, then one sync. Wall time over N is
 * GPU-bound once N is large (the CPU side of both is microseconds). Not a
 * pass/fail test -- `jsrf_d3d8_host_2d_test bench` prints it. */
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void bench_one(const char *name, void (*mk)(D3D8HostDrawCheck *), int ff, unsigned n)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
    uint16_t *rt = (uint16_t *)(ram + RT);
    double t[2];
    for (int host = 0; host < 2; ++host) {
        background(rt); logo_depth(); draw_binder(rt);
        mk(&c);
        memset(&d, 0, sizeof d); d.verts = verts;
        if (ff) { if (d3d8_host_ff_registers(&c, ffm) || d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d)) return; }
        else if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) return;
        g_exec_keep_surfaces = 1; g_exec_no_sync = 1;
        for (unsigned k = 0; k < 20; ++k) host ? d3d8_host_2d_metal_external(&d, ram, RAM_SIZE) : ff ? exec_draw_ff(&c, &d, ffm, rt, ram + ZS) : exec_draw(&c, &d, rt, ram + ZS);
        nv2a_metal_sync();
        unsigned long long ne0 = 0, ne1 = 0;
        d3d8_host_2d_metal_stats(NULL, NULL, NULL, NULL, &ne0);
        double t0 = now_ms(), tl;
        for (unsigned k = 0; k < n; ++k) host ? d3d8_host_2d_metal_external(&d, ram, RAM_SIZE) : ff ? exec_draw_ff(&c, &d, ffm, rt, ram + ZS) : exec_draw(&c, &d, rt, ram + ZS);
        tl = now_ms() - t0;
        nv2a_metal_sync();
        t[host] = now_ms() - t0;
        d3d8_host_2d_metal_stats(NULL, NULL, NULL, NULL, &ne1);
        printf("    %s: encode loop %.3f ms (host encode %.2f us/draw), then GPU drain %.3f ms\n", host ? "host" : "executor",
               tl, host ? (ne1 - ne0) / 1e3 / n : 0.0, t[host] - tl);
        g_exec_keep_surfaces = 0; g_exec_no_sync = 0;
    }
    printf("  bench %-28s x%u: executor %.3f ms (%.2f us/draw), host %.3f ms (%.2f us/draw), host/executor %.2f\n",
           name, n, t[0], t[0] * 1e3 / n, t[1], t[1] * 1e3 / n, t[1] / t[0]);
}
static void mk_logo(D3D8HostDrawCheck *c) { case_logo(c); }
static void mk_logo_noat(D3D8HostDrawCheck *c) { case_logo(c); set_state(c, 0x300, 0); }
static void mk_ff(D3D8HostDrawCheck *c) { case_ff(c); }
/* A SEQUENCE, NOT A DRAW. The in-game defect of 24 Sep 2026 (blocky debris
 * over the FF scene, every frame, in draw mode) did not show in any
 * single-draw comparison. This interleaves N draws in one unflushed batch --
 * the FF quad and the logo (host or executor) with the indexed list (always
 * the executor) -- and compares the surface and depth after the lot, for
 * each RECOMP_D3D8_HOST_BISECT arm, against the executor drawing everything. */
static void relocate(D3D8HostDrawCheck *c, uint32_t to)
{
    memcpy(ram + to, ram + VB, 0x400);
    for (unsigned a = 0; a < 16; ++a) if ((c->va_on >> a) & 1u) c->va_offset[a] = c->va_offset[a] - VB + to;
    if (c->idx_ptr) { memcpy(ram + to + 0x400, ram + c->idx_ptr, 2u * c->count); c->idx_ptr = to + 0x400; }
}
static int seq_arm(int host, unsigned mask, unsigned n, uint16_t *out, float *zout)
{
    D3D8HostDrawCheck c[3]; D3D8Host2DDraw d[3]; static uint32_t ffm[2048];
    static D3D8H2DVertex vv[3][64];
    uint16_t *rt = (uint16_t *)(ram + RT);
    d3d8_host_2d_set_bisect(mask);
    background(rt); logo_depth(); draw_binder(rt);
    /* The cases share VB and IB, so each is moved to its own copy before the next is made. */
    case_ff(&c[0]); relocate(&c[0], 0x12000);
    case_logo(&c[1]); set_state(&c[1], 0x300, 0); relocate(&c[1], 0x13000);
    case_b(&c[2]); relocate(&c[2], 0x14000);
    c[2].zs = 0x2345; c[2].zs_data = ZS; c[2].zs_format = 0x1u | (0x2Eu << 8);
    c[2].zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
    set_state(&c[2], 0x30C, 1); set_state(&c[2], 0x354, 0x203); set_state(&c[2], 0x35C, 1);
    for (int k = 0; k < 3; ++k) { memset(&d[k], 0, sizeof d[k]); d[k].verts = vv[k]; }
    if (d3d8_host_ff_registers(&c[0], ffm) || d3d8_host_draw_build(&c[0], ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d[0])) return 0;
    if (d3d8_host_2d_build(&c[1], ram, RAM_SIZE, 0, &d[1]) || d3d8_host_2d_build(&c[2], ram, RAM_SIZE, 0, &d[2])) { printf("seq: build\n"); return 0; }
    g_exec_keep_surfaces = 1; g_exec_no_sync = 1;
    int ok = 1;
    for (unsigned k = 0; k < n && ok; ++k) {
        unsigned w = k % 3;
        if (w == 2) ok = exec_draw(&c[2], &d[2], rt, ram + ZS);
        else if (host) ok = d3d8_host_2d_metal_external(&d[w], ram, RAM_SIZE);
        else ok = w == 0 ? exec_draw_ff(&c[0], &d[0], ffm, rt, ram + ZS) : exec_draw(&c[1], &d[1], rt, ram + ZS);
    }
    g_exec_keep_surfaces = 0; g_exec_no_sync = 0;
    d3d8_host_2d_set_bisect(0);
    if (!ok) { printf("seq: a draw failed: %s\n", d3d8_host_2d_metal_last_error()); return 0; }
    nv2a_metal_sync();
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout);
}
static void seq_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    /* Bits 1 (own pass) and 64 (wait every draw) are left out: splitting the
     * batch changes blended pixels by one 565 step through tile precision
     * (measured in join_tests), so they cannot be held to the joined arm. */
    static const unsigned masks[] = { 0, 2, 4, 8, 16, 32, 2 | 4 | 8 | 16 | 32 };
    const char *e = getenv("H2D_SEQ_N");
    unsigned n = e ? (unsigned)atoi(e) : 3000;
    background(bg);
    CHECK(seq_arm(0, 0, n, a, za), "sequence: the executor drew %u draws", n);
    for (unsigned m = 0; m < sizeof masks / sizeof masks[0]; ++m) {
        D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad;
        CHECK(seq_arm(1, masks[m], n, b, zb), "sequence, bisect 0x%X: the host arm drew", masks[m]);
        d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
        zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
        printf("  sequence of %u, bisect 0x%02X: executor changed %llu, host arm %llu, differing %llu | depth %llu, worst %u\n",
               n, masks[m], df.exec_changed, df.host_changed, df.mismatch, zbad, steps);
        CHECK(df.mismatch == 0 && zbad == 0, "sequence of %u draws in one batch, bisect 0x%X: identical to the executor", n, masks[m]);
    }
}

static void bench_tests(void)
{
    const char *e = getenv("H2D_BENCH_N");
    unsigned n = e ? (unsigned)atoi(e) : 4000;
    bench_one("logo (alpha test, blend, dither)", mk_logo, 0, n);
    bench_one("logo without alpha test", mk_logo_noat, 0, n);
    bench_one("FF quad", mk_ff, 1, n);
}

/* SPECIALISED AGAINST GENERIC: the host's specialised fragment programs
 * (function constants) must write exactly what its interpreter writes, on
 * the logo (alpha test, blend, dither, DXT1) and the FF quad. The executor
 * comparisons above already run specialised; this holds the interpreter to
 * it, and proves pipelines were actually specialised (built > 0). */
static void spec_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    unsigned long long built = 0, hits = 0, fb = 0, ns = 0, fb0 = 0;
    int ok;
    d3d8_host_2d_metal_spec_stats(NULL, NULL, &fb0, NULL);
    d3d8_host_2d_metal_set_spec(1);
    ok = draw_arm(1, 0, a, za); d3d8_host_2d_metal_set_spec(0); ok = ok && draw_arm(1, 0, b, zb); d3d8_host_2d_metal_set_spec(1);
    CHECK(ok && !memcmp(a, b, sizeof a) && !memcmp(za, zb, sizeof za), "specialised logo == generic logo, colour and depth");
    ok = ff_draw_arm(1, 0, a, za); d3d8_host_2d_metal_set_spec(0); ok = ok && ff_draw_arm(1, 0, b, zb); d3d8_host_2d_metal_set_spec(1);
    CHECK(ok && !memcmp(a, b, sizeof a) && !memcmp(za, zb, sizeof za), "specialised FF quad == generic FF quad, colour and depth");
    d3d8_host_2d_metal_spec_stats(&built, &hits, &fb, &ns);
    printf("  specialised pipelines built %llu, hits %llu, fallbacks %llu, compile %.1f ms\n", built, hits, fb, ns / 1e6);
    CHECK(built > 0 && fb == fb0, "specialised pipelines were built and none fell back");
}

/* THE z-RANGE CULL UNDER EARLY TESTS -- the draw-mode debris of 24 Sep 2026.
 * The executor discards every fragment whose z falls outside [0, 1] (JSRF's
 * CULL policy), in the shader, with late tests. MTLDepthClipModeClamp clamps
 * the depth that is TESTED, not the z the shader reads, so a fragment beyond
 * the far plane (or in front of the near one) is still discarded by the
 * executor. An early-tests entry with that discard removed drew them --
 * geometry crossing the near plane painted over the scene. The logo without
 * its alpha test (an early candidate) is tilted here from z 0.6 to 1.4:
 * the host must discard exactly what the executor discards. */
static int zrange_arm(int host, uint16_t *out, float *zout)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    uint16_t *rt = (uint16_t *)(ram + RT);
    background(rt); logo_depth(); draw_binder(rt);
    case_logo(&c); set_state(&c, 0x300, 0);
    putf(VB + 8, 0.6f); putf(VB + 36 + 8, 1.4f); putf(VB + 72 + 8, 0.6f); putf(VB + 108 + 8, 1.4f);
    set_state(&c, 0x354, 0x207);                         /* ALWAYS: only the z-range cull decides */
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) return 0;
    if (host) {
        if (!d3d8_host_2d_metal_external(&d, ram, RAM_SIZE)) return 0;
        nv2a_metal_sync();
    } else if (!exec_draw(&c, &d, rt, ram + ZS)) return 0;
    memcpy(out, rt, RTPITCH * RTH);
    return nv2a_metal_depth_peek(ram + ZS, RTW, RTH, zout);
}
static void zrange_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static float za[RTW * RTH], zb[RTW * RTH];
    D3D8H2DDiff df; unsigned steps = 0; unsigned long long zbad;
    background(bg);
    CHECK(zrange_arm(0, a, za) && zrange_arm(1, b, zb), "z range: both arms drew");
    d3d8_host_2d_diff(bg, a, b, RTPITCH / 2, RTH, 0, &df);
    zbad = d3d8_host_2d_depth_diff(za, zb, RTW * RTH, 1, &steps);
    printf("  z range 0.6..1.4: executor changed %llu, host %llu, differing %llu | depth %llu\n",
           df.exec_changed, df.host_changed, df.mismatch, zbad);
    CHECK(df.exec_changed > 1000 && df.exec_changed < 8000, "z range: the executor culled part of the draw (%llu px kept)", df.exec_changed);
    CHECK(df.mismatch == 0 && zbad == 0, "z range: the host culls exactly the fragments the executor culls");
}

/* SPECIALISED AGAINST GENERIC, FUZZED: random combiner programs (1-8
 * stages, random input/output words, SAME_FACTOR bits, alpha test, blend,
 * dither, specular) on the logo's geometry and texture, rendered through
 * the shadow path with specialisation on and off. The two must agree to the
 * byte: the game's FF draws run four stages over two textures, which the
 * fixed cases above never did. */
static uint32_t fz_state = 0x12345678u;
static unsigned s_fz_spec_vs_exec, s_fz_gen_vs_exec, s_fz_printed;
static uint32_t fz(void) { fz_state ^= fz_state << 13; fz_state ^= fz_state >> 17; fz_state ^= fz_state << 5; return fz_state; }
static unsigned long long g_sg_a, g_sg_b;
static unsigned long long spec_gen_diff(const D3D8Host2DDraw *e)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH];
    unsigned long long diff = 0;
    background(a); background(b);
    d3d8_host_2d_metal_set_spec(1);
    d3d8_host_2d_metal_render(e, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_metal_set_spec(0);
    d3d8_host_2d_metal_render(e, ram, RAM_SIZE, b, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_metal_set_spec(1);
    {   static uint16_t bg[RTPITCH / 2 * RTH]; background(bg); g_sg_a = g_sg_b = 0;
        for (size_t k = 0; k < sizeof a / 2; ++k) { diff += a[k] != b[k]; g_sg_a += a[k] != bg[k]; g_sg_b += b[k] != bg[k]; } }
    return diff;
}
/* Shrink a specialised-vs-generic disagreement: drop stages, zero words and
 * fields one at a time while the disagreement survives; print what is left. */
static void spec_minimize(D3D8Host2DDraw e)
{
    int changed = 1;
    while (changed) {
        changed = 0;
        if (e.cc > 1) { D3D8Host2DDraw f = e; f.cc--; f.control = (f.control & ~0xFu) | f.cc; if (spec_gen_diff(&f)) { e = f; changed = 1; continue; } }
        uint32_t *w[4] = { e.ci, e.ai, e.co, e.ao };
        for (unsigned a = 0; a < 4 && !changed; ++a)
            for (unsigned i = 0; i < e.cc && !changed; ++i)
                for (unsigned byte = 0; byte < 4 && !changed; ++byte) {
                    uint32_t m = 0xFFu << (8 * byte);
                    if (!(w[a][i] & m)) continue;
                    D3D8Host2DDraw f = e; uint32_t *fw[4] = { f.ci, f.ai, f.co, f.ao };
                    fw[a][i] &= ~m;
                    if (spec_gen_diff(&f)) { e = f; changed = 1; }
                }
        if (!changed && e.alpha_test) { D3D8Host2DDraw f = e; f.alpha_test = 0; if (spec_gen_diff(&f)) { e = f; changed = 1; } }
        if (!changed && e.blend) { D3D8Host2DDraw f = e; f.blend = 0; if (spec_gen_diff(&f)) { e = f; changed = 1; } }
        if (!changed && e.dither) { D3D8Host2DDraw f = e; f.dither = 0; if (spec_gen_diff(&f)) { e = f; changed = 1; } }
        if (!changed && e.add_specular) { D3D8Host2DDraw f = e; f.add_specular = 0; if (spec_gen_diff(&f)) { e = f; changed = 1; } }
    }
    printf("  MINIMAL: cc %u control %X atest %u blend %u dither %u spec %u | %llu px\n", e.cc, e.control, e.alpha_test, e.blend,
           e.dither, e.add_specular, spec_gen_diff(&e));
    printf("    specialised drew %llu px, generic %llu px\n", g_sg_a, g_sg_b);
    for (unsigned i = 0; i < e.cc; ++i)
        printf("    stage %u: ci %08X ai %08X co %08X ao %08X k0 %08X k1 %08X\n", i, e.ci[i], e.ai[i], e.co[i], e.ao[i], e.k0[i], e.k1[i]);
}
/* THE MINIMISED CASE, fixed so it cannot drift with the fuzz's generator:
 * four stages, COMBINER_CONTROL 4 (SAME_FACTOR_ALL, as D3D's fixed function
 * sets it), stage 2 writing R0.a = 0, stage 3 reading registers and writing
 * nothing, alpha test GREATER 215. Every fragment must be discarded. The
 * looped specialisation drew 4,663 of them. */
static void spec_min_case(void)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw e;
    case_logo(&c); set_state(&c, 0x30C, 0);
    memset(&e, 0, sizeof e); e.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &e)) { CHECK(0, "minimal case: build"); return; }
    e.cc = 4; e.control = 4;
    for (unsigned i = 0; i < 8; ++i) { e.ci[i] = e.ai[i] = e.co[i] = e.ao[i] = 0; e.k0[i] = 0x80C04020u; e.k1[i] = 0x40206080u; }
    e.ao[0] = 0x00001100u; e.ao[2] = 0x000000DCu; e.ai[3] = 0xE300E900u;
    e.alpha_test = 1; e.alpha_func = 0x204u; e.alpha_ref = 215; e.blend = 0; e.dither = 0; e.add_specular = 0;
    unsigned long long diff = spec_gen_diff(&e);
    printf("  minimal case: specialised drew %llu px, generic %llu px\n", g_sg_a, g_sg_b);
    CHECK(diff == 0 && g_sg_a == 0 && g_sg_b == 0, "minimal case: both discard every fragment (R0.a = 0 fails GREATER 215)");
}
static void spec_fuzz_tests(void)
{
    spec_min_case();
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    unsigned bad = 0, n = 300;
    case_logo(&c); set_state(&c, 0x30C, 0);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) { CHECK(0, "fuzz: build"); return; }
    for (unsigned it = 0; it < n; ++it) {
        D3D8Host2DDraw e = d;
        e.cc = 1u + fz() % 8u; if (getenv("H2D_FUZZ_CC")) e.cc = (unsigned)atoi(getenv("H2D_FUZZ_CC"));
        for (unsigned i = 0; i < 8; ++i) {
            /* inputs: register 0-13 (codes 14/15 unused), channel bit, mapping 0-7 */
            uint32_t w = 0, x = 0;
            for (unsigned k = 0; k < 4; ++k) { w = (w << 8) | ((fz() % 14u) | (fz() & 0x10u) | ((fz() % 8u) << 5)); x = (x << 8) | ((fz() % 14u) | (fz() & 0x10u) | ((fz() % 8u) << 5)); }
            e.ci[i] = w; e.ai[i] = x;
            e.co[i] = (fz() % 14u) | ((fz() % 14u) << 4) | ((fz() % 14u) << 8) | (fz() & 0x000FF000u);
            e.ao[i] = (fz() % 14u) | ((fz() % 14u) << 4) | ((fz() % 14u) << 8) | (fz() & 0x0000F000u);
            e.k0[i] = fz(); e.k1[i] = fz();
        }
        e.control = (e.cc) | (fz() & 0x11000u);
        e.alpha_test = fz() & 1u; e.alpha_func = 0x200u + fz() % 8u; e.alpha_ref = fz() % 256u;
        /* The executor's model, so it can be the referee: per-stage factors, GREATER. */
        /* Every COMBINER_CONTROL factor mode -- D3D's fixed function clears
         * both bits, which is how the looped specialisation's defect hid from
         * a fuzz that always set them. With SAME_FACTOR_ALL the per-stage
         * factors are made equal, as D3D writes them, so the executor (always
         * per stage) stays a valid referee. It is consulted only for GREATER,
         * the one alpha test it models; every draw is held spec == generic. */
        if (!(e.control & 0x1000u)) for (unsigned i = 1; i < 8; ++i) e.k0[i] = e.k0[0];
        if (!(e.control & 0x10000u)) for (unsigned i = 1; i < 8; ++i) e.k1[i] = e.k1[0];
        if (fz() & 1u) e.alpha_func = 0x204u;
        e.blend = fz() & 1u; e.blend_src = 0x302; e.blend_dst = 0x303; e.blend_eq = 0x8006;
        e.dither = fz() & 1u; e.add_specular = fz() & 1u;
        if (getenv("H2D_FUZZ_NOSPEC")) e.add_specular = 0;
        if (getenv("H2D_FUZZ_NOAT")) e.alpha_test = 0;
        if (getenv("H2D_FUZZ_NOBLEND")) e.blend = 0;
        if (getenv("H2D_FUZZ_NODITHER")) e.dither = 0;
        background(a); background(b);
        d3d8_host_2d_metal_set_spec(1);
        int ok = d3d8_host_2d_metal_render(&e, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0;
        d3d8_host_2d_metal_set_spec(0);
        ok = ok && d3d8_host_2d_metal_render(&e, ram, RAM_SIZE, b, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0;
        d3d8_host_2d_metal_set_spec(1);
        {   /* the executor, the referee */
            static uint16_t x[RTPITCH / 2 * RTH]; static uint8_t zd[RTW * 4 * RTH];
            unsigned long long dsx = 0, dgx = 0, dsg = 0;
            background(x);
            if (e.alpha_func == 0x204u && exec_draw(&c, &e, x, zd)) {
                for (size_t k = 0; k < sizeof a / 2; ++k) { dsx += a[k] != x[k]; dgx += b[k] != x[k]; dsg += a[k] != b[k]; }
                if (dsx) ++s_fz_spec_vs_exec; if (dgx) ++s_fz_gen_vs_exec;
                if ((dsx || dgx) && s_fz_printed++ < 6)
                    printf("  fuzz %u cc %u: specialised vs executor %llu px, generic vs executor %llu px, specialised vs generic %llu\n",
                           it, e.cc, dsx, dgx, dsg);
            }
        }
        if (!ok || memcmp(a, b, sizeof a)) {
            unsigned long long diff = 0;
            for (size_t k = 0; k < sizeof a / 2; ++k) diff += a[k] != b[k];
            if (bad == 0 && getenv("H2D_FUZZ_MIN")) spec_minimize(e);
            if (bad++ < 5) printf("  fuzz %u: cc %u control %X atest %u func %X ref %u blend %u dither %u spec %u: %llu px differ%s\n",
                                  it, e.cc, e.control, e.alpha_test, e.alpha_func, e.alpha_ref, e.blend, e.dither, e.add_specular,
                                  diff, ok ? "" : " (render failed)");
        }
    }
    printf("  fuzz: differ from the executor -- specialised %u, generic %u of %u\n", s_fz_spec_vs_exec, s_fz_gen_vs_exec, n);
    CHECK(s_fz_spec_vs_exec == 0, "fuzz: specialised host == executor on every random combiner program (%u differ)", s_fz_spec_vs_exec);
    CHECK(bad == 0, "fuzz: %u random combiner programs, specialised == generic on %u", n, n - bad);
}

/* THE SAME SURFACE HELD TWICE. The executor keys a slot on its registers'
 * geometry; D3D's description of the same target can differ (here the size:
 * pitch * (clip_y + clip_h) against pitch * height). A host bind with D3D's
 * geometry then makes a second slot for the same guest surface -- its own
 * colour and depth -- instead of rebinding the executor's. Executor draws into
 * A, then B; the host then draws into A. With the executor's geometry the
 * host's bind is a rebind of A's slot (no upload); with D3D's (bisect 1024)
 * it rebuilds a second copy from guest RAM. */
static int geom_arm(unsigned mask, unsigned long long *uploads, unsigned long long *copies)
{
    enum { RT2 = RT + 0x40000 };
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    unsigned long long u0, h0, e0, u1, h1, e1;
    uint16_t *a = (uint16_t *)(ram + RT), *b = (uint16_t *)(ram + RT2);
    static uint8_t depth[RTW * 4 * RTH];
    background(a); background(b);
    nv2a_metal_invalidate(NULL); nv2a_metal_sync();
    d3d8_host_2d_set_bisect(mask);
    g_exec_keep_surfaces = 1; g_exec_size_extra = RTPITCH;
    case_b(&c); memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) return 0;
    if (!exec_draw(&c, &d, a, depth) || !exec_draw(&c, &d, b, depth)) return 0;
    nv2a_metal_bind_counters(&u0, &h0, &e0);
    int ok = d3d8_host_2d_metal_external(&d, ram, RAM_SIZE);
    nv2a_metal_sync();
    nv2a_metal_bind_counters(&u1, &h1, &e1);
    *uploads = (u1 - u0) - (h1 - h0);             /* rebuilds from guest RAM: swaps that were not rebinds */
    *copies = (unsigned long long)nv2a_metal_slot_geometry((uint8_t *)a, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    g_exec_keep_surfaces = 0; g_exec_size_extra = 0;
    d3d8_host_2d_set_bisect(0);
    return ok;
}
static void geom_tests(void)
{
    unsigned long long up = 0, cp = 0, up2 = 0, cp2 = 0;
    CHECK(geom_arm(0, &up, &cp), "bind geometry: the host drew into A");
    printf("  bind geometry: executor's geometry -> %llu rebuilds, A held %llu time(s); D3D's (bisect 1024) ->", up, cp);
    CHECK(geom_arm(1024, &up2, &cp2), "bind geometry CONTROL: the host drew into A");
    printf(" %llu rebuilds, A held %llu time(s)\n", up2, cp2);
    CHECK(up == 0 && cp == 1, "bind geometry: the host's bind rebinds the executor's slot for A (no upload, one copy)");
    CHECK(up2 >= 1 && cp2 >= 2, "bind geometry CONTROL: with D3D's geometry A is held twice");
}

/* THE GAME'S STATE, replayed: the combiner setups the mirror logged for the
 * building draws that VERIFY found the specialised pipelines dropping pixels
 * on (key run g51key, 24 Sep 2026: fvf 202, two DXT1 textures, four stages,
 * COMBINER_CONTROL 4, alpha test GREATER 0, dither, depth write).
 * Specialised and generic must agree on each. On this test's texture they
 * agreed even under the looped specialisation -- the defect needed texel
 * alpha this texture does not have -- so spec_min_case, minimised from the
 * fuzz, is the case that fails without the fix; this one keeps the game's
 * words under test. */
static const uint32_t k_game[][9] = {
    /* ci0, ai0, ci1, ai1, co, ao, final cw0, tmask, add_spec */
    { 0x08040000u, 0x00002014u, 0x0C200000u, 0x1C200000u, 0x00000C00u, 0x00000C00u, 0xEu, 3u, 1u },
    { 0x08040000u, 0x18140000u, 0x090C0000u, 0x0000201Cu, 0x00000C00u, 0x00000C00u, 0xCu, 3u, 0u },
    { 0x08040000u, 0x18140000u, 0x0919390Cu, 0x0000201Cu, 0x00000C00u, 0x00000C00u, 0xCu, 3u, 0u },
    { 0x08040000u, 0x18140000u, 0x09200000u, 0x19200000u, 0x00000C00u, 0x00000C00u, 0xCu, 3u, 0u },
    { 0x08040000u, 0x18140000u, 0x090C0000u, 0x191C0000u, 0x00000C00u, 0x00000C00u, 0xEu, 3u, 1u },
};
static void game_state_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    case_logo(&c); set_state(&c, 0x30C, 0);
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) { CHECK(0, "game state: build"); return; }
    for (unsigned g = 0; g < sizeof k_game / sizeof k_game[0]; ++g) {
        D3D8Host2DDraw e = d;
        e.cc = 4; e.control = 4;
        for (unsigned i = 0; i < 8; ++i) { e.ci[i] = e.ai[i] = e.co[i] = e.ao[i] = 0; e.k0[i] = e.k1[i] = 0xFFFFFFFFu; }
        e.ci[0] = k_game[g][0]; e.ai[0] = k_game[g][1];
        for (unsigned i = 1; i < 4; ++i) { e.ci[i] = i == 1 ? k_game[g][2] : 0x0C200000u; e.ai[i] = i == 1 ? k_game[g][3] : 0x1C200000u; }
        for (unsigned i = 0; i < 4; ++i) { e.co[i] = k_game[g][4]; e.ao[i] = k_game[g][5]; }
        e.final_cw0 = k_game[g][6]; e.add_specular = k_game[g][8];
        e.tmask = k_game[g][7]; e.tex[1] = e.tex[0];
        e.alpha_test = 1; e.alpha_func = 0x204; e.alpha_ref = 0; e.dither = 1; e.blend = 0;
        background(a); background(b);
        d3d8_host_2d_metal_set_spec(1);
        int ok = d3d8_host_2d_metal_render(&e, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0;
        d3d8_host_2d_metal_set_spec(0);
        ok = ok && d3d8_host_2d_metal_render(&e, ram, RAM_SIZE, b, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0;
        d3d8_host_2d_metal_set_spec(1);
        unsigned long long diff = 0, ca = 0, cb = 0;
        static uint16_t bg[RTPITCH / 2 * RTH]; background(bg);
        for (size_t k = 0; k < sizeof a / 2; ++k) { diff += a[k] != b[k]; ca += a[k] != bg[k]; cb += b[k] != bg[k]; }
        printf("  game state %u: specialised drew %llu px, generic %llu px, differing %llu\n", g, ca, cb, diff);
        CHECK(ok && diff == 0, "game state %u: specialised == generic", g);
    }
}

/* ASYNCHRONOUS COMPILE: the first draw of a new key returns promptly on the
 * generic interpreter; once the compile lands, the same key is served by the
 * specialised pipeline -- and the pixels are the same either way. */
static void async_spec_test(void)
{
    static uint16_t first[RTPITCH / 2 * RTH], later[RTPITCH / 2 * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    unsigned long long b0 = 0, h0 = 0, f0 = 0, b1 = 0, h1 = 0, f1 = 0, n = 0;
    case_logo(&c); set_state(&c, 0x30C, 0); set_state(&c, 0x340, 7);   /* a key no other test builds */
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d)) { CHECK(0, "async: build"); return; }
    d.cc = 3; d.ci[2] = 0x0C200000u; d.ai[2] = 0x1C200000u; d.co[2] = 0x00000C00u; d.ao[2] = 0x00000C00u;
    d3d8_host_2d_metal_set_spec_sync(0);
    d3d8_host_2d_metal_spec_stats(&b0, &h0, &f0, NULL);
    background(first);
    CHECK(d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, first, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0, "async: first draw rendered");
    d3d8_host_2d_metal_spec_stats(&b1, &h1, &f1, NULL);
    CHECK(f1 == f0 + 1 && h1 == h0, "async: the first draw of a new key used the generic interpreter (stand-ins +%llu)", f1 - f0);
    do { d3d8_host_2d_metal_spec_stats(&b1, NULL, NULL, NULL); if (b1 > b0) break; struct timespec ts = { 0, 5000000 }; nanosleep(&ts, NULL); } while (++n < 400);
    CHECK(b1 == b0 + 1, "async: the compile landed (%llu ms waited)", n * 5);
    background(later);
    CHECK(d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, later, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0, "async: later draw rendered");
    d3d8_host_2d_metal_spec_stats(NULL, &h1, NULL, NULL);
    CHECK(h1 == h0 + 1, "async: the same key is now served by the specialised pipeline");
    CHECK(!memcmp(first, later, sizeof first), "async: stand-in and specialised pipeline drew the same pixels");
}

static void draw_mode_tests(void)
{
    bind_tests();
    game_state_tests();
    geom_tests();
    spec_fuzz_tests();
    zrange_tests();
    spec_tests();
    ff_draw_tests();
    join_tests();
    stencil_tests();
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
        be.exec_skip = fake_skip; be.exec_skipped = fake_skipped; be.exec_seen = fake_seen; be.external_binds = d3d8_host_2d_metal_binds; be.ff_vertex = nv2a_ff_vertex;
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
        /* A target the executor has not bound: the host binds it (nv2a_metal_bind) and draws. */
        {   unsigned long long b0 = d3d8_host_2d_metal_binds();
            case_logo(&c); c.rt_data = RT + 0x40000; snapshot_at_call(&c);
            d3d8_host_2d_replace(&c); if (g_fake_skip) ++g_fake_skipped; d3d8_host_2d_after(&c);
            d3d8_host_2d_get_stats(&s3);
            CHECK(s3.replaced == s2.replaced + 1 && s3.replace_unbound == s2.replace_unbound && d3d8_host_2d_metal_binds() == b0 + 1,
                  "draw mode: a target the executor has not bound is bound by the host and drawn"); }
        /* A fixed-function draw goes through replace() the same way. */
        {   D3D8H2DStats f0, f1;
            d3d8_host_2d_get_stats(&f0);
            background((uint16_t *)(ram + RT)); logo_depth(); draw_binder((uint16_t *)(ram + RT));
            case_ff(&c); snapshot_at_call(&c);
            d3d8_host_2d_replace(&c); if (g_fake_skip) ++g_fake_skipped; d3d8_host_2d_after(&c);
            d3d8_host_2d_get_stats(&f1);
            CHECK(f1.replaced_ff == f0.replaced_ff + 1 && g_fake_skip == 0, "FF draw mode: replace() drew a fixed-function draw and skipped the executor's");
            d3d8_host_2d_get_stats(&s3); }
        /* And the control on the skip counter: a replaced draw whose batches were NOT skipped is counted. */
        case_logo(&c); snapshot_at_call(&c);
        d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s0);
        CHECK(s0.replaced_without_skip == s3.replaced_without_skip + 1 &&
              s0.replaced_exec_stopped_host_empty == s3.replaced_exec_stopped_host_empty &&
              s0.replaced_exec_stopped_host_drew == s3.replaced_exec_stopped_host_drew,
              "draw mode CONTROL: a replaced draw with no batch between its tokens is caught as the double-draw case");
        /* G54: the executor now draws points and lines, so a point draw is no
         * longer replaced with nothing -- it is left to the executor. */
        {   D3D8H2DStats q0, q1;
            d3d8_host_2d_get_stats(&q0);
            case_ff(&c); c.prim = 1; c.count = 1; snapshot_at_call(&c);
            d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
            d3d8_host_2d_get_stats(&q1);
            CHECK(g_fake_skip == 0 && q1.replaced == q0.replaced && q1.replace_refused == q0.replace_refused + 1,
                  "point draw: left to the executor, which draws points now (not replaced, no skip)");
        }
        /* The game's 684 (24 Sep 2026) had this shape as a point list; with
         * points drawn, the draw neither side draws is a one-index POLYGON. The
         * host replaces it with nothing; the executor sees the batch and stops
         * it before the rasteriser, so nothing is skipped -- and nothing is
         * drawn by either. Counted, not called a double draw. */
        {   D3D8H2DStats p0, p1;
            d3d8_host_2d_get_stats(&p0);
            case_ff(&c); c.prim = 10; c.count = 1; snapshot_at_call(&c);
            d3d8_host_2d_replace(&c);
            CHECK(g_fake_skip == 1, "polygon of one index: replaced, skip on");
            if (g_fake_skip) ++g_fake_seen;                  /* executor: batch arrives, idx_count < 3, returns */
            d3d8_host_2d_after(&c);
            d3d8_host_2d_get_stats(&p1);
            CHECK(p1.replaced == p0.replaced + 1 && p1.replaced_without_skip == p0.replaced_without_skip + 1 &&
                  p1.replaced_exec_stopped_host_empty == p0.replaced_exec_stopped_host_empty + 1 &&
                  p1.replaced_exec_stopped_host_drew == p0.replaced_exec_stopped_host_drew,
                  "polygon of one index: seen and stopped by the executor, drawn by neither -- not a double draw");
            /* A triangle draw the executor stops but the host drew: a divergence, counted apart. */
            background((uint16_t *)(ram + RT)); logo_depth(); draw_binder((uint16_t *)(ram + RT));
            case_logo(&c); snapshot_at_call(&c);
            d3d8_host_2d_replace(&c); if (g_fake_skip) ++g_fake_seen; d3d8_host_2d_after(&c);
            d3d8_host_2d_get_stats(&p0);
            CHECK(p0.replaced_exec_stopped_host_drew == p1.replaced_exec_stopped_host_drew + 1,
                  "a draw the host drew and the executor stopped before its rasteriser is counted apart");
        }
        d3d8_host_2d_report("test");
    }
}

/* The FF shadow end to end with ONLY RECOMP_D3D8_HOST_FF armed, as the game
 * run armed it: pre token, post token, flip. With the control on, the FF
 * draw must come out MISMATCHING -- the arm that caught the shared knobs
 * never being read when the 2D shadow was off. */
static int ff_arm(int control)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH];
    static uint32_t ffm[2048];
    D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DStats a, b; D3D8Host2DBackend be;
    setenv("RECOMP_D3D8_HOST_FF", "shadow", 1);
    setenv("RECOMP_D3D8_HOST_FF_STRIDE", "1", 1);
    if (control) setenv("RECOMP_D3D8_HOST_2D_CONTROL", "1", 1);
    CHECK(d3d8_host_ff_mode() == 1 && d3d8_host_2d_mode() == 0, "FF shadow armed alone");
    case_ff(&c); c.serial = 11;
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    if (d3d8_host_ff_registers(&c, ffm)) return 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    if (d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d)) return 0;
    background(bg); memcpy(ex, bg, sizeof bg);
    if (!exec_draw_ff(&c, &d, ffm, ex, ram + ZS)) return 0;
    memset(&be, 0, sizeof be);
    be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
    be.last_error = d3d8_host_2d_metal_last_error; be.depth_peek = fake_peek; be.ff_vertex = nv2a_ff_vertex;
    d3d8_host_2d_set_backend(&be);
    /* The executor's depth after the draw is its attachment; before it, the guest's cleared bytes. */
    memcpy(ram + RT, bg, sizeof bg);
    g_exec_result = ex; g_sync_calls = 0; g_peeks = 0;
    snapshot_at_call(&c);
    d3d8_host_2d_get_stats(&a);
    d3d8_host_2d_pre(c.serial, c.vs_handle, c.rt_data, c.rt_format, c.rt_size, c.zs_data, c.zs_size);
    d3d8_host_2d_post(&c, NULL);
    d3d8_host_2d_flip();
    d3d8_host_2d_get_stats(&b);
    CHECK(b.ff_compared == a.ff_compared + 1, "FF shadow%s: the draw was compared at the flip", control ? " CONTROL" : "");
    if (control)
        CHECK(b.ff_mismatching == a.ff_mismatching + 1, "FF shadow CONTROL: the perturbed FF draw is MISMATCHING");
    else
        CHECK(b.ff_mismatching == a.ff_mismatching, "FF shadow: no mismatch (exact %llu, within %llu)",
              b.ff_exact - a.ff_exact, b.ff_within - a.ff_within);
    d3d8_host_2d_report("test");
    return 1;
}

/* G53: HOST FOG, AGAINST THE EXECUTOR'S FOG.
 *
 * D3D's fog state as the mirror carries it (FOGENABLE, FOGTABLEMODE,
 * start/end/density, RANGEFOGENABLE, FOGCOLOR) and the fog final combiner the
 * fog updater writes. The host draws it through its own transcription
 * (d3d8_host_ff_registers: FOG_ENABLE, GEN_MODE, MODE, PARAMS, the
 * model-view and D3D's (0,0,1,0) FOG_PLANE) and its own shader; the executor
 * draws the same register file with nv2a_ff_vertex's fog coordinate and its
 * own final combiner. Within one 565 step; the fog must be VISIBLE (the
 * executor's unfogged draw differs); and the host with the fog colour moved
 * must differ from the executor (the control). Then the whole shadow flow,
 * pre token to flip, for one fogged draw: compared, not refused. */
static void set_fog(D3D8HostDrawCheck *c, unsigned table, unsigned range, uint32_t color)
{
    float f;
    memset(&c->fg_cur, 0, sizeof c->fg_cur);
    c->fg_cur.enable = 1; c->fg_cur.table_mode = table; c->fg_cur.range_enable = range;
    f = 1.8f; memcpy(&c->fg_cur.start, &f, 4); f = 4.2f; memcpy(&c->fg_cur.end, &f, 4);
    f = 0.45f; memcpy(&c->fg_cur.density, &f, 4); f = 8192.0f; memcpy(&c->fg_cur.equal_scale, &f, 4);
    c->ffv_fog_color = color;
    c->fog_cur[0] = 1; c->fog_cur[1] = 0; c->fog_cur[2] = 0; c->fog_cur[3] = 0;
}
/* One fogged FF draw through the whole shadow flow -- pre token, post, flip
 * -- against an executor image; returns the flip's verdict (1 compared and
 * matching, 0 compared and MISMATCHING, -1 not compared). */
static int ff_fog_flow(D3D8HostDrawCheck *c, const uint16_t *bg, uint16_t *exec_img)
{
    D3D8H2DStats a, b; D3D8Host2DBackend be;
    memset(&be, 0, sizeof be);
    be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
    be.last_error = d3d8_host_2d_metal_last_error; be.depth_peek = fake_peek; be.ff_vertex = nv2a_ff_vertex;
    d3d8_host_2d_set_backend(&be);
    for (unsigned y = 0; y < RTH; ++y)
        for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
    memcpy(ram + RT, bg, RTPITCH * RTH);
    g_exec_result = exec_img; g_sync_calls = 0; g_peeks = 0;
    snapshot_at_call(c);
    d3d8_host_2d_get_stats(&a);
    d3d8_host_2d_pre(c->serial, c->vs_handle, c->rt_data, c->rt_format, c->rt_size, c->zs_data, c->zs_size);
    d3d8_host_2d_post(c, NULL);
    d3d8_host_2d_flip();
    d3d8_host_2d_get_stats(&b);
    if (b.ff_compared != a.ff_compared + 1) return -1;
    return b.ff_mismatching == a.ff_mismatching;
}
static void ff_fog_tests(void)
{
    static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH], nf[RTPITCH / 2 * RTH];
    static uint16_t exc[RTPITCH / 2 * RTH], nfc[RTPITCH / 2 * RTH];   /* copies: the executor's surfaces move on */
    static uint32_t ffm[2048];
    static const struct { const char *name; unsigned table, range; } fc[] = {
        { "LINEAR planar", 3, 0 }, { "EXP radial", 1, 1 }, { "EXP2 planar", 2, 0 } };
    for (unsigned i = 0; i < sizeof fc / sizeof fc[0]; ++i) {
        D3D8HostDrawCheck c; D3D8Host2DDraw d; D3D8H2DDiff df;
        const char *why; int v;
        case_ff(&c); c.serial = 30 + i;
        set_fog(&c, fc[i].table, fc[i].range, 0xFF3060A0u);
        for (unsigned y = 0; y < RTH; ++y)
            for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
        why = d3d8_host_ff_registers(&c, ffm);
        memset(&d, 0, sizeof d); d.verts = verts;
        if (!why) why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(!why && d.fog_enable && d.final_general, "FF fog %s: the host builds it (%s)", fc[i].name, why ? why : "built");
        if (why) continue;
        background(bg); memcpy(ex, bg, sizeof bg); memcpy(nf, bg, sizeof bg);
        g_exec_fog = 1; g_exec_fog_color = c.ffv_fog_color;
        CHECK(exec_draw_ff(&c, &d, ffm, ex, ram + ZS), "FF fog %s: the executor drew it fogged", fc[i].name);
        memcpy(exc, ex, sizeof ex);
        g_exec_fog = 0;
        for (unsigned y = 0; y < RTH; ++y)
            for (unsigned x = 0; x < RTW; ++x) memcpy(ram + ZS + y * ZSPITCH + 4 * x, &(uint32_t){ 0xFFFFFF00u }, 4);
        CHECK(exec_draw_ff(&c, &d, ffm, nf, ram + ZS), "FF fog %s: the executor drew it unfogged", fc[i].name);
        memcpy(nfc, nf, sizeof nf);
        d3d8_host_2d_diff(bg, exc, nfc, RTPITCH / 2, RTH, 1, &df);
        CHECK(df.exec_changed > 500 && df.mismatch > 500, "FF fog %s: the fog is visible in the executor's image (%llu of %llu px)",
              fc[i].name, df.mismatch, df.exec_changed);
        v = ff_fog_flow(&c, bg, exc);
        CHECK(v == 1, "FF fog %s: the shadow compares the fogged draw at the flip and it MATCHES the executor's (%d)", fc[i].name, v);
        v = ff_fog_flow(&c, bg, nfc);
        CHECK(v == 0, "FF fog %s CONTROL: against the executor's UNFOGGED image the shadow says MISMATCHING (%d)", fc[i].name, v);
    }
}

/* G53: THE 2D CLASS'S FOG. D3D's vertex-fog pass-through (FOGTABLEMODE NONE)
 * takes the coordinate from the specular alpha; a fog table on
 * pre-transformed vertices runs a program the host does not model and is
 * refused by name. */
static void fog_2d_tests(void)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d; const char *why;
    static const float SPA[4] = { 0.0f, 0.25f, 0.75f, 1.0f };
    case_b(&c);
    for (int i = 0; i < 4; ++i) put32(VB + 0x400 + 4u * i, (uint32_t)(SPA[i] * 255.0f + 0.5f) << 24 | 0x00102030u);
    c.va_on |= 1u << 4; c.va_offset[4] = VB + 0x400; c.va_format[4] = (4u << 8) | 0x40u;
    c.ffv_valid = 1; set_fog(&c, 0, 0, 0xFF808080u);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    {   int ok = !why && d.fog_enable && d.final_general && d.nverts == 6;
        for (unsigned k = 0; ok && k < d.nverts; ++k) if (d.verts[k].f[0] != d.verts[k].d1[3]) ok = 0;
        CHECK(ok, "2D fog, FOGTABLEMODE NONE: built, the coordinate is each vertex's specular alpha (%s)", why ? why : "built"); }
    set_fog(&c, 3, 0, 0xFF808080u);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why && !strcmp(why, "2D fog from a fog table"), "2D fog from a fog table: refused by name (%s)", why ? why : "built");
    /* G75: RECOMP_D3D8_HOST_FOGTABLE -- D3D's two fog-table programs. Device
     * +8 bit 1 set: Z fog, oFog = v0.z; clear: W fog, oFog = 1/v0.w. */
    for (int i = 0; i < 4; ++i) { putf(VB + 20u * i + 8, 0.1f + 0.2f * (float)i); putf(VB + 20u * i + 12, 0.5f + (float)i); }
    d3d8_host_2d_set_fogtable(1);
    for (int zf = 0; zf < 2; ++zf) {
        int ok;
        c.dev_flags_valid = 1; c.dev_flags = zf ? 0x2u : 0x0u;
        memset(&d, 0, sizeof d); d.verts = verts;
        why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
        ok = !why && d.fog_enable && d.nverts == 6;
        for (unsigned k = 0; ok && k < d.nverts; ++k)
            if (d.verts[k].f[0] != (zf ? d.verts[k].p[2] : d.verts[k].p[3])) ok = 0;
        CHECK(ok, "2D fog from a fog table, %s fog: the coordinate is %s (%s)", zf ? "Z" : "W", zf ? "v0.z" : "1/v0.w",
              why ? why : "built");
    }
    c.dev_flags_valid = 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why && strstr(why, "device flags"), "2D fog from a fog table, no device flags: refused (%s)", why ? why : "built");
    d3d8_host_2d_set_fogtable(0);
}

/* G51.2: A DRAW THROUGH THE TITLE'S OWN VERTEX PROGRAM. The first complete
 * JSRF program captured (vsh_capture.h: position and texcoord in, oPos scaled
 * by c0 and offset by c1, oD0 from the current diffuse), its constants as
 * SetVertexShaderConstant would leave them, a quad of two triangles. The host
 * draws it through the executor's own translation (nv2a_metal_vsh_function);
 * the reference runs the same program on the CPU interpreter
 * (nv2a_vsh_execute) and draws its outputs as pre-transformed vertices. They
 * must agree; the control drops every other triangle and must not. */
static void case_vs(D3D8HostDrawCheck *c)
{
    static const float P[4][2] = { { 12.0f, 10.0f }, { 110.0f, 14.0f }, { 16.0f, 84.0f }, { 104.0f, 88.0f } };
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    static const uint16_t I[6] = { 0, 1, 2, 2, 1, 3 };
    base_check(c);
    for (int i = 0; i < 4; ++i) {
        uint32_t v = VB + 16u * i;
        putf(v, P[i][0]); putf(v + 4, P[i][1]); putf(v + 8, UV[i][0]); putf(v + 12, UV[i][1]);
    }
    memcpy(ram + IB, I, sizeof I);
    c->vs_handle = 0x00ABCDE1u; c->vs_kind = 1; c->vs_nwords = (uint32_t)(sizeof jsrf_vsh_words / 4u);
    memcpy(c->vs_words, jsrf_vsh_words, sizeof jsrf_vsh_words);
    c->draw_kind = 2; c->prim = 5; c->count = 6; c->idx_ptr = IB; c->nidx = 6;
    for (int k = 0; k < 6; ++k) c->idx[k] = I[k];
    c->va_on = (1u << 0) | (1u << 9);
    c->va_offset[0] = VB;     c->va_format[0] = (16u << 8) | 0x22u;
    c->va_offset[9] = VB + 8; c->va_format[9] = (16u << 8) | 0x22u;
    memset(c->vc, 0, sizeof c->vc); memset(c->vc_written, 0, sizeof c->vc_written);
    c->vc[0][0] = 1; c->vc[0][1] = 1; c->vc[0][2] = 16777215.0f; c->vc[0][3] = 1;
    c->vc[1][0] = 0.53125f; c->vc[1][1] = 0.53125f;
    c->vc_written[0] = 3u;
    c->ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_DIFFUSE;
    c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c->ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
}
/* The CPU interpreter's outputs for a built programmable draw, as a
 * pre-transformed draw (class 1) with the hardware's 1/16 snap and colour
 * saturation: the reference both the shadow and the draw-mode tests use. */
static void vs_reference(const D3D8Host2DDraw *d, D3D8Host2DDraw *r, D3D8H2DVertex *rv)
{
    NV2AVshProgram prog;
    memset(&prog, 0, sizeof prog);
    nv2a_vsh_parse(jsrf_vsh_words, (int)(sizeof jsrf_vsh_words / 16u), &prog);
    *r = *d; r->cls = 1; r->verts = rv; r->nverts = d->vs_nidx;
    for (unsigned k = 0; k < d->vs_nidx; ++k) {
        float in[16][4]; NV2AVshResult o; unsigned slot = 0;
        for (unsigned q = 0; q < 16; ++q) { in[q][0] = in[q][1] = in[q][2] = 0; in[q][3] = 1; }
        in[3][0] = in[3][1] = in[3][2] = 1;
        for (unsigned q = 0; q < 16; ++q) if (d->vs_inputs & (1u << q)) memcpy(in[q], d->vs_in[d->vs_idx[k] * d->vs_nattrs + slot++], 16);
        memset(&o, 0, sizeof o);
        nv2a_vsh_execute(&prog, (const float (*)[4])in, (const float (*)[4])d->vs_c, &o);
        memset(&rv[k], 0, sizeof rv[k]);
        rv[k].p[0] = fabsf(o.output[0][0]) < 1048576.0f ? truncf(o.output[0][0] * 16.0f) / 16.0f : o.output[0][0];
        rv[k].p[1] = fabsf(o.output[0][1]) < 1048576.0f ? truncf(o.output[0][1] * 16.0f) / 16.0f : o.output[0][1];
        rv[k].p[2] = o.output[0][2] / 16777215.0f; rv[k].p[3] = o.output[0][3];
        for (unsigned ch = 0; ch < 4; ++ch) {
            rv[k].d0[ch] = fminf(1.0f, fmaxf(0.0f, o.output[3][ch]));
            rv[k].d1[ch] = fminf(1.0f, fmaxf(0.0f, o.output[4][ch]));
        }
        for (unsigned u = 0; u < 4; ++u) memcpy(rv[k].t[u], o.output[9 + u], 16);
    }
}
/* G51.2 DRAW MODE: the programmable draw into the executor's own bound
 * surface (d3d8_host_2d_metal_external), against the CPU interpreter's
 * outputs drawn the same way; then the controller: replace() draws it and
 * turns the executor's skip on, after() turns it off. */
static void vs_draw_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static D3D8H2DVertex rv[64];
    uint16_t *rt = (uint16_t *)(ram + RT);
    D3D8HostDrawCheck c; D3D8Host2DDraw d, r;
    D3D8H2DDiff df;
    const char *why;
    CHECK(d3d8_host_vs_mode() == 2 && d3d8_host_any_draw_mode(), "RECOMP_D3D8_HOST_VS=draw arms draw mode");
    case_vs(&c);
    CHECK(d3d8_host_replaces_handle(c.vs_handle), "draw mode replaces an odd (programmable) handle");
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, (const uint16_t *)(ram + IB), NULL, NULL, &d);
    CHECK(!why, "built (%s)", why ? why : "ok");
    if (why) return;
    vs_reference(&d, &r, rv);
    /* The binder's first draw into a fresh surface differs from its later
     * ones (by up to 15 steps of green over ~860 px, the dither/rounding of a
     * first upload); take the starting pixels after a warm-up, from the
     * executor's surface itself, for each arm. */
    background(rt); logo_depth(); draw_binder(rt); nv2a_metal_sync();
    {   static uint16_t bg0[RTPITCH / 2 * RTH], base2[RTPITCH / 2 * RTH];
        background(bg0);
        memcpy(rt, bg0, sizeof bg0); logo_depth(); draw_binder(rt); nv2a_metal_sync(); memcpy(bg, rt, sizeof bg);
        CHECK(d3d8_host_2d_metal_external(&d, ram, RAM_SIZE), "the host drew the program into the executor's surface (%s)",
              d3d8_host_2d_metal_last_error());
        nv2a_metal_sync(); memcpy(a, rt, sizeof a);
        memcpy(rt, bg0, sizeof bg0); logo_depth(); draw_binder(rt); nv2a_metal_sync(); memcpy(base2, rt, sizeof base2);
        CHECK(!memcmp(base2, bg, sizeof bg), "the executor's surface starts both arms the same");
        CHECK(d3d8_host_2d_metal_external(&r, ram, RAM_SIZE), "the reference drew into the executor's surface");
        nv2a_metal_sync(); memcpy(b, rt, sizeof b);
        memcpy(bg, bg0, sizeof bg);                        /* what the later steps reset RAM to */
        memcpy(bg0, base2, sizeof base2);
        d3d8_host_2d_diff(bg0, b, a, RTPITCH / 2, RTH, 0, &df); }
    printf("  programmable VS draw mode: reference changed %llu px, host %llu, differing %llu, max r%u g%u b%u\n",
           df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2]);
    CHECK(df.exec_changed > 2000 && df.mismatch == 0 && df.max_err[0] + df.max_err[1] + df.max_err[2] == 0,
          "programmable VS draw mode: in the executor's surface, identical to the CPU interpreter's");
    /* CONTROL: every other triangle dropped. */
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 1, (const uint16_t *)(ram + IB), NULL, NULL, &d);
    memcpy(rt, bg, sizeof bg); logo_depth(); draw_binder(rt);
    CHECK(!why && d3d8_host_2d_metal_external(&d, ram, RAM_SIZE), "programmable VS draw mode CONTROL: drew");
    nv2a_metal_sync(); memcpy(a, rt, sizeof a);
    d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
    CHECK(df.mismatch > 500, "programmable VS draw mode CONTROL: the surface holds the perturbed draw (%llu px differ)", df.mismatch);
    /* The controller. */
    {   D3D8Host2DBackend be; D3D8H2DStats s0, s1;
        memset(&be, 0, sizeof be);
        be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
        be.last_error = d3d8_host_2d_metal_last_error; be.external_draw = d3d8_host_2d_metal_external;
        be.exec_skip = fake_skip; be.exec_skipped = fake_skipped; be.exec_seen = fake_seen; be.external_binds = d3d8_host_2d_metal_binds;
        be.ff_vertex = nv2a_ff_vertex;
        d3d8_host_2d_set_backend(&be);
        memcpy(rt, bg, sizeof bg); logo_depth(); draw_binder(rt);
        case_vs(&c); snapshot_at_call(&c);
        d3d8_host_2d_get_stats(&s0);
        d3d8_host_2d_replace(&c);
        CHECK(g_fake_skip == 1, "programmable VS draw mode: replace() drew and turned the executor's skip on (%s)",
              d3d8_host_2d_metal_last_error());
        if (g_fake_skip) ++g_fake_skipped;
        d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s1);
        CHECK(g_fake_skip == 0 && s1.replaced_vs == s0.replaced_vs + 1 && s1.replaced == s0.replaced + 1,
              "programmable VS draw mode: after() turned it off; replaced once, counted as programmable");
        nv2a_metal_sync();
        d3d8_host_2d_diff(bg, b, rt, RTPITCH / 2, RTH, 0, &df);
        CHECK(df.exec_changed > 2000 && df.mismatch == 0, "programmable VS draw mode: what replace() drew is the reference (%llu px differ)",
              df.mismatch);
        /* A program the translator cannot take is left to the executor. */
        case_vs(&c); c.vs_nwords = 0; snapshot_at_call(&c);
        d3d8_host_2d_get_stats(&s0);
        d3d8_host_2d_replace(&c); d3d8_host_2d_after(&c);
        d3d8_host_2d_get_stats(&s1);
        CHECK(g_fake_skip == 0 && s1.replaced == s0.replaced, "programmable VS draw mode: no captured program, left to the executor");
    }
}
static void vs_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static D3D8H2DVertex rv[64];
    D3D8HostDrawCheck c; D3D8Host2DDraw d, r;
    const char *why;
    CHECK(d3d8_host_vs_mode() == 1, "RECOMP_D3D8_HOST_VS=shadow arms the programmable class");
    case_vs(&c);
    CHECK(d3d8_host_2d_class(&c) == 3, "an odd vertex-shader handle with a captured program is class 3");
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, (const uint16_t *)(ram + IB), NULL, NULL, &d);
    CHECK(!why && d.vs_nidx == 6 && d.vs_nin == 4, "the host builds the programmable draw (%s; %u indices over %u vertices, %u attributes)",
          why ? why : "built", d.vs_nidx, d.vs_nin, d.vs_nattrs);
    if (why) return;
    /* The reference: the program on the CPU interpreter, per corner. */
    vs_reference(&d, &r, rv);
    background(bg); memcpy(a, bg, sizeof bg); memcpy(b, bg, sizeof bg);
    CHECK(d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0,
          "the host drew it through the executor's translation (%s)", d3d8_host_2d_metal_last_error());
    CHECK(d3d8_host_2d_metal_render(&r, ram, RAM_SIZE, b, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0, "the reference drew");
    {   D3D8H2DDiff df;
        d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
        printf("  programmable VS: reference changed %llu px, host %llu, over tolerance %llu, max error r%u g%u b%u\n",
               df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2]);
        CHECK(df.exec_changed > 2000 && df.mismatch == 0, "programmable VS: the GPU program and the CPU interpreter draw the same"); }
    /* CONTROL. */
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 1, (const uint16_t *)(ram + IB), NULL, NULL, &d);
    memcpy(a, bg, sizeof bg);
    if (!why) d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    {   D3D8H2DDiff df;
        d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
        CHECK(!why && df.mismatch > 500, "programmable VS CONTROL: with every other triangle dropped it differs (%llu px)", df.mismatch); }
}

/* G51.2 ARMING: each class switch ALONE must arm the mirror (the in-game
 * bug: RECOMP_D3D8_HOST_VS=shadow printed its arming text and the mirror
 * stayed off, so the census was 0). The modes are read once a process, so
 * each case is a child with a clean environment. The cases come from the
 * class table itself: a class added there is tested here without an edit.
 * Then no gate outside d3d8_host_2d.c may keep its own list of classes. */
static void clear_host_env(void)
{
    extern char **environ;
    char name[128];
    for (int again = 1; again; ) {
        again = 0;
        for (char **e = environ; *e; ++e)
            if (!strncmp(*e, "RECOMP_D3D8_", 12)) {
                size_t n = strcspn(*e, "=");
                if (n >= sizeof name) continue;
                memcpy(name, *e, n); name[n] = 0; unsetenv(name); again = 1; break;
            }
    }
}
/* 1 armed, 0 not, in a child with only `name`=`value` set (name NULL: none). */
static int armed_alone(const char *name, const char *value, unsigned *mask)
{
    int fd[2], st = 0; pid_t pid; unsigned got[2] = { 0, 0 };
    if (pipe(fd)) return -1;
    pid = fork();
    if (pid == 0) {
        char why[160]; unsigned out[2];
        close(fd[0]); clear_host_env();
        if (name) setenv(name, value, 1);
        out[1] = d3d8_host_armed(why, sizeof why);
        out[0] = (unsigned)d3d8_host_mirror_armed(why, sizeof why);
        if (write(fd[1], out, sizeof out) != (ssize_t)sizeof out) _exit(2);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], got, sizeof got) != (ssize_t)sizeof got) got[0] = 99;
    close(fd[0]); waitpid(pid, &st, 0);
    if (mask) *mask = got[1];
    return (int)got[0];
}
static int source_mentions(const char *path, const char *needle, char *line_out, size_t n)
{
    FILE *f = fopen(path, "r"); char buf[4096]; int hit = 0, ln = 0;
    if (!f) return -1;
    while (fgets(buf, sizeof buf, f)) { ++ln; if (strstr(buf, needle)) { hit = ln; snprintf(line_out, n, "%.120s", buf); break; } }
    fclose(f);
    return hit;
}
static void arming_tests(void)
{
    unsigned mask = 0, n = d3d8_host_class_switches();
    CHECK(n >= 3, "the class table has the 2D, FF and VS switches (%u rows)", n);
    CHECK(armed_alone(NULL, NULL, &mask) == 0 && mask == 0, "nothing set: the mirror stays off");
    CHECK(armed_alone("RECOMP_D3D8_HOST_FF_STRIDE", "4", &mask) == 0, "a knob alone (FF_STRIDE) arms nothing");
    CHECK(armed_alone("RECOMP_D3D8_HOST_VERIFY", "8", &mask) == 0, "a knob alone (VERIFY) arms nothing");
    CHECK(armed_alone("RECOMP_D3D8_MIRROR", "1", &mask) == 1 && mask == 0, "RECOMP_D3D8_MIRROR=1 arms the mirror, no class");
    for (unsigned i = 0; i < n; ++i) {
        const char *sw = d3d8_host_class_switch(i);
        CHECK(armed_alone(sw, "shadow", &mask) == 1 && mask == (1u << i), "%s=shadow ALONE arms the mirror (class mask %X)", sw, mask);
        CHECK(armed_alone(sw, "off", &mask) == 0, "%s=off arms nothing", sw);
    }
    CHECK(armed_alone("RECOMP_D3D8_HOST_2D", "draw", &mask) == 1 && mask == 1u, "RECOMP_D3D8_HOST_2D=draw alone arms the mirror");
    CHECK(armed_alone("RECOMP_D3D8_HOST_FF", "draw", &mask) == 1 && mask == 2u, "RECOMP_D3D8_HOST_FF=draw alone arms the mirror");
    CHECK(armed_alone("RECOMP_D3D8_HOST_VS", "draw", &mask) == 1 && mask == 4u, "RECOMP_D3D8_HOST_VS=draw alone arms the mirror");
#ifdef JSRF_SRC_DIR
    {   static const char *const files[] = { JSRF_SRC_DIR "/diagnostics/jsrf_first_fault/d3d8_mirror.c",
                                             JSRF_SRC_DIR "/diagnostics/jsrf_first_fault/main.c" };
        static const char *const gates[] = { "d3d8_host_2d_mode()", "d3d8_host_ff_mode()", "d3d8_host_vs_mode()" };
        for (unsigned f = 0; f < 2; ++f)
            for (unsigned g = 0; g < 3; ++g) {
                char line[128] = ""; int at = source_mentions(files[f], gates[g], line, sizeof line);
                CHECK(at == 0, "%s keeps no class list of its own: no %s (line %d: %s)", strrchr(files[f], '/') + 1, gates[g], at, line);
            }
    }
#endif
}

/* G53: THE PROGRAMMABLE CLASS'S FOG. A program that writes oFog from its
 * fog attribute (MOV oPos,v0; MOV oD0,v3; MOV oFog,v5), D3D's LINEAR fog
 * table and fog final combiner. The host draws it through the executor's
 * translation (vs_gpu hands Out.fog to the host's fragment); the reference
 * runs the program on the CPU interpreter and draws its outputs, fog
 * coordinate included, as pre-transformed vertices. Exact; the fog visible
 * against the same draw unfogged; the control (fog colour moved) differs. */
static void vs_fog_tests(void)
{
    static uint16_t a[RTPITCH / 2 * RTH], b[RTPITCH / 2 * RTH], bg[RTPITCH / 2 * RTH];
    static D3D8H2DVertex rv[64];
    static uint32_t words[3][4];
    static const float P[4][3] = { { 8.0f, 6.0f, 0.0f }, { 120.0f, 10.0f, 60.0f }, { 12.0f, 90.0f, 20.0f }, { 116.0f, 92.0f, 100.0f } };
    static const uint16_t I[6] = { 0, 1, 2, 2, 1, 3 };
    static const unsigned io[3] = { 0, 3, 5 };
    D3D8HostDrawCheck c; D3D8Host2DDraw d, r; D3D8H2DDiff df; NV2AVshProgram prog;
    const char *why;
    for (int i = 0; i < 3; ++i) {
        VshIns x; memset(&x, 0, sizeof x);
        x.mac = 1; x.input_index = io[i];
        x.a.mux = 2; x.a.swz = SWZ_ID; x.b.mux = 2; x.b.swz = SWZ_ID; x.c.mux = 2; x.c.swz = SWZ_ID;
        x.out_mask = io[i] == 5 ? 0x8 : 0xF; x.out_reg = io[i]; x.final = i == 2;
        vsh_encode(words[i], &x);
    }
    base_check(&c);
    for (int i = 0; i < 4; ++i) {                           /* x, y, fog: 12 bytes */
        uint32_t v = VB + 12u * i;
        putf(v, P[i][0]); putf(v + 4, P[i][1]); putf(v + 8, P[i][2]);
    }
    memcpy(ram + IB, I, sizeof I);
    c.vs_handle = 0x00ABCDF1u; c.vs_kind = 1; c.vs_nwords = 12; memcpy(c.vs_words, words, sizeof words);
    c.draw_kind = 2; c.prim = 5; c.count = 6; c.idx_ptr = IB; c.nidx = 6;
    for (int k = 0; k < 6; ++k) c.idx[k] = I[k];
    c.va_on = (1u << 0) | (1u << 5);
    c.va_offset[0] = VB; c.va_format[0] = (12u << 8) | 0x22u;
    c.va_offset[5] = VB + 8; c.va_format[5] = (12u << 8) | 0x12u;
    c.ffc_cur.tss[0][D3D8FF_TSS_COLOROP] = D3D8FF_TOP_SELECTARG1; c.ffc_cur.tss[0][D3D8FF_TSS_COLORARG1] = D3D8FF_TA_DIFFUSE;
    c.ffc_cur.tss[0][D3D8FF_TSS_ALPHAOP] = D3D8FF_TOP_SELECTARG1; c.ffc_cur.tss[0][D3D8FF_TSS_ALPHAARG1] = D3D8FF_TA_DIFFUSE;
    set_fog(&c, 3, 0, 0xFF2080C0u);
    {   float f = 0.0f; memcpy(&c.fg_cur.start, &f, 4); f = 110.0f; memcpy(&c.fg_cur.end, &f, 4); }
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, (const uint16_t *)(ram + IB), NULL, NULL, &d);
    CHECK(!why && d.fog_enable && d.final_general && d.vs_nidx == 6, "VS fog: the host builds a programmable fogged draw (%s)",
          why ? why : "built");
    if (why) return;
    memset(&prog, 0, sizeof prog);
    nv2a_vsh_parse((const uint32_t *)words, 3, &prog);
    r = d; r.cls = 1; r.verts = rv; r.nverts = d.vs_nidx;
    for (unsigned k = 0; k < d.vs_nidx; ++k) {
        float in[16][4]; NV2AVshResult o; unsigned slot = 0;
        for (unsigned q = 0; q < 16; ++q) { in[q][0] = in[q][1] = in[q][2] = 0; in[q][3] = 1; }
        in[3][0] = in[3][1] = in[3][2] = 1;
        for (unsigned q = 0; q < 16; ++q) if (d.vs_inputs & (1u << q)) memcpy(in[q], d.vs_in[d.vs_idx[k] * d.vs_nattrs + slot++], 16);
        memset(&o, 0, sizeof o);
        nv2a_vsh_execute(&prog, (const float (*)[4])in, (const float (*)[4])d.vs_c, &o);
        memset(&rv[k], 0, sizeof rv[k]);
        rv[k].p[0] = fabsf(o.output[0][0]) < 1048576.0f ? truncf(o.output[0][0] * 16.0f) / 16.0f : o.output[0][0];
        rv[k].p[1] = fabsf(o.output[0][1]) < 1048576.0f ? truncf(o.output[0][1] * 16.0f) / 16.0f : o.output[0][1];
        rv[k].p[2] = o.output[0][2] / 16777215.0f; rv[k].p[3] = o.output[0][3];
        for (unsigned ch = 0; ch < 4; ++ch) rv[k].d0[ch] = fminf(1.0f, fmaxf(0.0f, o.output[3][ch]));
        rv[k].f[0] = o.output[5][0];
    }
    background(bg); memcpy(a, bg, sizeof bg); memcpy(b, bg, sizeof bg);
    CHECK(d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH) == 0,
          "VS fog: the host drew it through the executor's translation (%s)", d3d8_host_2d_metal_last_error());
    d3d8_host_2d_metal_render(&r, ram, RAM_SIZE, b, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
    printf("  VS fog: reference changed %llu px, host %llu, over tolerance %llu, max error r%u g%u b%u\n",
           df.exec_changed, df.host_changed, df.mismatch, df.max_err[0], df.max_err[1], df.max_err[2]);
    CHECK(df.exec_changed > 2000 && df.mismatch == 0, "VS fog: the program's oFog reaches the host's fog as the interpreter's does");
    {   D3D8Host2DDraw u = d; u.fog_enable = 0; u.final_general = 0; u.add_specular = 0;
        memcpy(a, bg, sizeof bg);
        d3d8_host_2d_metal_render(&u, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
        d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
        CHECK(df.mismatch > 1000, "VS fog: the fog is visible (%llu px differ from the unfogged draw)", df.mismatch); }
    d.fog_color ^= 0x00FF00FFu; memcpy(a, bg, sizeof bg);
    d3d8_host_2d_metal_render(&d, ram, RAM_SIZE, a, RTPITCH / 2, NULL, 0, 0, RTW, RTH);
    d3d8_host_2d_diff(bg, b, a, RTPITCH / 2, RTH, 1, &df);
    CHECK(df.mismatch > 1000, "VS fog CONTROL: the fog colour moved, the host differs (%llu px)", df.mismatch);
}

/* G56 DEFER-SAFE, the positive control. The executor (this thread, the
 * service thread) draws into A without a sync and then into B, so under
 * RECOMP_METAL_DEFER_SWAP A's pixels are on the GPU only; a "guest" thread
 * then asks for A as a D3D LockRect would, and this thread services the
 * mailbox the way the pusher loop does. The guest must get the drawn pixels.
 * Then: the bound, dirty surface; its depth; and a request nobody serves,
 * which must time out, not hang. */
typedef struct { uint8_t *p; size_t n; unsigned timeout; int r; volatile int done; } HrArg;
static void *hr_guest(void *v) { HrArg *a = v; a->r = nv2a_host_read_request(a->p, a->n, NV2A_HOST_READ_TEST, a->timeout); a->done = 1; return NULL; }
static int hr_ask(uint8_t *p, size_t n, unsigned timeout, int serve)
{
    HrArg a = { p, n, timeout, 0, 0 };
    pthread_t t;
    pthread_create(&t, NULL, hr_guest, &a);
    while (!a.done) { if (serve) nv2a_host_read_service(1); struct timespec ts = { 0, 100000 }; nanosleep(&ts, NULL); }
    pthread_join(t, NULL);
    return a.r;
}
static void hostread_tests(void)
{
    enum { RT2 = RT + 0x40000 };
    static uint16_t bg[RTPITCH / 2 * RTH];
    static uint8_t zbefore[RTW * 4 * RTH];
    D3D8HostDrawCheck c; D3D8Host2DDraw d;
    uint16_t *a = (uint16_t *)(ram + RT), *b = (uint16_t *)(ram + RT2);
    unsigned long long changed = 0;
    int r;
    nv2a_host_read_set_service_thread();
    background(bg); background(a); background(b);
    case_b(&c); memset(&d, 0, sizeof d); d.verts = verts;
    CHECK(!d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d), "host read: built");
    /* 1. A deferred slot. */
    g_exec_keep_surfaces = 1; g_exec_no_sync = 1;
    CHECK(exec_draw(&c, &d, a, ram + ZS) && exec_draw(&c, &d, b, ram + ZS), "host read: the executor drew into A, then B");
    for (size_t k = 0; k < sizeof bg / 2; ++k) changed += a[k] != bg[k];
    CHECK(changed == 0, "host read CONTROL: A's guest RAM is stale after the deferred swap");
    r = hr_ask((uint8_t *)a, RTPITCH * RTH, 2000, 1);
    changed = 0; for (size_t k = 0; k < sizeof bg / 2; ++k) changed += a[k] != bg[k];
    printf("  host read, deferred A: result %d, %llu px of the drawing in guest RAM\n", r, changed);
    CHECK(r > 0 && (r & 1) && changed > 1000, "host read: a guest-thread lock of a deferred render target gets the drawn pixels");
    /* 2. The bound surface, dirty. B is bound now and was drawn without a sync. */
    changed = 0; for (size_t k = 0; k < sizeof bg / 2; ++k) changed += b[k] != bg[k];
    CHECK(changed == 0, "host read CONTROL: the bound surface's guest RAM is stale mid-frame");
    r = hr_ask((uint8_t *)b, RTPITCH * RTH, 2000, 1);
    changed = 0; for (size_t k = 0; k < sizeof bg / 2; ++k) changed += b[k] != bg[k];
    CHECK(r > 0 && (r & 2) && changed > 1000, "host read: a lock of the bound, dirty target gets the drawn pixels (%llu px)", changed);
    /* 3. Its depth. Draw with a depth write, then lock the depth surface. */
    {   D3D8HostDrawCheck z = c;
        z.zs = 0x2345; z.zs_data = ZS; z.zs_format = 0x1u | (0x2Eu << 8);
        z.zs_size = (RTW - 1) | ((RTH - 1) << 12) | ((ZSPITCH / 64 - 1) << 24);
        set_state(&z, 0x30C, 1); set_state(&z, 0x354, 0x207); set_state(&z, 0x35C, 1);
        for (unsigned k = 0; k < RTW * RTH; ++k) memcpy(ram + ZS + 4 * k, &(uint32_t){ 0xFFFFFF00u }, 4);
        memcpy(zbefore, ram + ZS, sizeof zbefore);
        memset(&d, 0, sizeof d); d.verts = verts;
        CHECK(!d3d8_host_2d_build(&z, ram, RAM_SIZE, 0, &d) && exec_draw(&z, &d, b, ram + ZS), "host read: depth-writing draw");
        r = hr_ask(ram + ZS, RTW * 4 * RTH, 2000, 1);
        CHECK(r > 0 && (r & 4) && memcmp(zbefore, ram + ZS, sizeof zbefore) != 0,
              "host read: a lock of the depth surface gets the depth the GPU wrote"); }
    g_exec_keep_surfaces = 0; g_exec_no_sync = 0;
    /* 4. Nobody serving: a bounded wait, counted. */
    {   unsigned long long cnt[NV2A_HOST_READ_ENTRIES][3];
        r = hr_ask((uint8_t *)a, RTPITCH * RTH, 50, 0);
        nv2a_host_read_counts(cnt);
        CHECK(r == -1 && cnt[NV2A_HOST_READ_TEST][2] == 1, "host read: an unserved request times out (and is counted), it does not hang"); }
    nv2a_host_read_report();
}

/* G51.2: the texture shader stage modes are D3D's 0x1952B0 derivation.
 * The title's pixel shaders leave PROJECT2D on stages with no texture; D3D
 * writes NONE there, so the host must not refuse ("shader samples an unbound
 * stage", ~40% of the class) and must not sample. A fixed-function stage after
 * a BUMPENVMAP COLOROP is BUMPENVMAP, which the host does not draw: refused.
 * Each rule is checked against the old one (BISECT 4096) as its control. */
static void stage_mode_tests(void)
{
    D3D8HostDrawCheck c; D3D8Host2DDraw d; const char *why;
    const uint32_t F2D = 0x1u | 0x20u | (0x06u << 8) | (1u << 16) | (5u << 20) | (5u << 24);
    /* The derivation alone. */
    base_check(&c);
    c.tex[0] = 1; c.format[0] = F2D;
    CHECK(d3d8_host_stage_program(&c) == 0x00001u, "modes: FF, one 2D texture -> %05X", d3d8_host_stage_program(&c));
    c.tex[1] = 2; c.format[1] = F2D; c.tss[0][12] = 0x19u;
    CHECK(d3d8_host_stage_program(&c) == 0x000C1u, "modes: FF, BUMPENVMAP on stage 0 -> stage 1 mode 6 (%05X)", d3d8_host_stage_program(&c));
    c.tss[0][12] = 0x1Au;
    CHECK(d3d8_host_stage_program(&c) == 0x000E1u, "modes: FF, BUMPENVMAPLUMINANCE -> mode 7 (%05X)", d3d8_host_stage_program(&c));
    c.tss[0][12] = 4u; c.format[1] = F2D | 4u;
    CHECK(d3d8_host_stage_program(&c) == 0x00061u, "modes: FF, a cube texture -> CUBEMAP (%05X)", d3d8_host_stage_program(&c));
    c.format[1] = (F2D & ~0xF0u) | 0x30u;
    CHECK(d3d8_host_stage_program(&c) == 0x00041u, "modes: FF, a volume -> PROJECT3D (%05X)", d3d8_host_stage_program(&c));
    c.tex[1] = 0;
    c.ffc_ps = 0x80001000u; c.stage_prog_in[0] = 0; c.stage_prog_in[1] = 0x00021u;
    CHECK(d3d8_host_stage_program(&c) == 0x00021u, "modes: pixel shader, +0x378 clear -> +0x37C as it is");
    c.stage_prog_in[0] = 1;
    CHECK(d3d8_host_stage_program(&c) == 0x00001u, "modes: pixel shader, PROJECT2D on an unbound stage -> NONE (%05X)", d3d8_host_stage_program(&c));
    c.stage_prog_in[1] = 0x00081u | (5u << 10) | (0x11u << 15);
    CHECK(d3d8_host_stage_program(&c) == (0x00081u | (5u << 10) | (0x11u << 15)),
          "modes: pixel shader, PASSTHRU/CLIPPLANE/DOTPRODUCT kept with no texture (%05X)", d3d8_host_stage_program(&c));
    c.stage_prog_in[1] = 0x00001u; c.format[0] = (F2D & ~0xFF00u) | (0x2Cu << 8);
    CHECK(d3d8_host_stage_program(&c) == 0x00002u, "modes: pixel shader, a depth format -> PROJECT3D (%05X)", d3d8_host_stage_program(&c));
    c.stage_prog_in[1] = 0x0000Du; c.format[0] = F2D | 4u;
    CHECK(d3d8_host_stage_program(&c) == 0x0000Eu, "modes: pixel shader, DOT_STR_3D on a cube -> DOT_STR_CUBE (%05X)", d3d8_host_stage_program(&c));

    /* A pixel-shader 2D draw sampling T0 and naming PROJECT2D on stage 1 with nothing bound. */
    case_a(&c);
    c.ffc_ps = 0x80001000u; c.ps_bound = 1;
    c.ps[53] = 1u;                                   /* one combiner stage */
    c.ps[34] = 0x08200000u; c.ps[0] = 0x18200000u;   /* colour T0 * 1, alpha T0.a * 1 */
    c.ps[45] = 0x00000C00u; c.ps[26] = 0x00000C00u;  /* -> R0 */
    c.ps[8] = 0xCu; c.ps[9] = 0x1C80u;
    c.ps[54] = 0x00021u; c.stage_prog_in[0] = 1; c.stage_prog_in[1] = 0x00021u;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.tmask == 1u, "pixel shader, unbound PROJECT2D stage: built, samples stage 0 only (%s, tmask %X)",
          why ? why : "built", d.tmask);
    d3d8_host_2d_set_bisect(4096u);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why && !strcmp(why, "shader samples an unbound stage"), "control, BISECT 4096 (word 54): refused (%s)", why ? why : "built");
    d3d8_host_2d_set_bisect(0);
    c.stage_prog_in[0] = 0;                          /* D3D keeps the shader's modes: the executor has mode 1 on a disabled unit */
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why && !strcmp(why, "shader samples an unbound stage"), "pixel shader, D3D does not adjust: still refused (%s)", why ? why : "built");

    /* The same shader with no final combiner of its own (words 8/9 zero,
     * device +0x374 zero, as the characters' shaders): D3D's fog updater
     * writes the default program, so the host must use it, not 0/0. */
    c.stage_prog_in[0] = 1; c.ps[8] = 0; c.ps[9] = 0;
    c.fog_cur[0] = 0; c.fog_cur[1] = 0; c.fog_cur[2] = c.ffc_ps; c.fog_cur[3] = 0;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.final_cw0 == 0xCu && d.final_cw1 == 0x1C80u && !d.final_general,
          "pixel shader without a final combiner: D3D's default R0 program (%s, %08X/%08X)", why ? why : "built", d.final_cw0, d.final_cw1);
    c.fog_cur[1] = 1;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.final_cw0 == 0xEu && d.add_specular, "... with SPECULARENABLE: the specular-add program (%s, %08X)", why ? why : "built", d.final_cw0);
    c.fog_cur[1] = 0; c.fog_cur[3] = 1; c.ps[8] = 0xEu; c.ps[9] = 0x1C80u;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.final_cw0 == 0xEu, "pixel shader with its own final combiner (+0x374 set): its words (%s, %08X)", why ? why : "built", d.final_cw0);
    c.fog_cur[3] = 0; c.ps[8] = 0; c.ps[9] = 0;
    d3d8_host_2d_set_bisect(4096u); c.ps[54] = 0x00001u;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why || (d.final_cw0 == 0u && d.final_cw1 == 0u), "control, BISECT 4096: words 8/9 as they stand, 0/0 (%s, %08X/%08X)",
          why ? why : "built", d.final_cw0, d.final_cw1);
    d3d8_host_2d_set_bisect(0);

    /* Fixed function: stage 1 after a BUMPENVMAP COLOROP. */
    case_a(&c);
    c.tex[1] = 0x9ABC; c.data[1] = TEX; c.format[1] = c.format[0]; c.tss[1][0] = c.tss[1][1] = 1;
    c.ffc_cur.texture_bound_mask = 3; c.tss[0][12] = 0x19u;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(why && !strncmp(why, "texture shader mode (fixed function) 6", 38), "FF, stage 1 BUMPENVMAP: left to the executor (%s)", why ? why : "built");
    d3d8_host_2d_set_bisect(4096u);
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.tmask == 3u, "control, BISECT 4096: the old rule draws it as plain 2D (%s, tmask %X)", why ? why : "built", d.tmask);
    d3d8_host_2d_set_bisect(0);
    c.tss[0][12] = D3D8FF_TOP_MODULATE;
    memset(&d, 0, sizeof d); d.verts = verts;
    why = d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d);
    CHECK(!why && d.tmask == 3u, "FF, two plain 2D stages: built, both sampled (%s, tmask %X)", why ? why : "built", d.tmask);
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
    if (argc > 1 && (strcmp(argv[1], "ff") == 0 || strcmp(argv[1], "ffcontrol") == 0)) {   /* FF shadow alone */
        CHECK(ff_arm(strcmp(argv[1], "ffcontrol") == 0), "FF arm ran");
        if (strcmp(argv[1], "ff") == 0) { ff_cache_tests(); ff_fog_tests(); fog_2d_tests(); tex_cache_tests(); }
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "bench") == 0) {        /* GPU cost, printed only */
        setenv("RECOMP_D3D8_HOST_2D", "draw", 1);
        setenv("RECOMP_D3D8_HOST_FF", "draw", 1);
        bench_tests();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "ffgpu") == 0) {        /* G52: host FF vertices on the GPU unit */
        setenv("RECOMP_D3D8_HOST_FF_GPU", "1", 1);
        d3d8_host_2d_metal_set_spec_sync(1);
        compare_ff_gpu(0);
        compare_ff_gpu(1);
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "points") == 0) {       /* G74: points and lines on the host */
        setenv("RECOMP_D3D8_HOST_POINTS", "1", 1);
        setenv("RECOMP_D3D8_HOST_FF_GPU", "1", 1);          /* the GPU unit is armed; points still take the CPU one */
        d3d8_host_2d_metal_set_spec_sync(1);
        points_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "bump") == 0) {         /* G75: BUMPENVMAP on the host */
        d3d8_host_2d_metal_set_spec_sync(1);
        bump_tests();
        lin32_tests();
        up_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "stagemodes") == 0) {   /* G51.2: D3D's texture stage modes */
        stage_mode_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "arming") == 0) {       /* G51.2: one arming function */
        arming_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "vsdraw") == 0) {       /* G51.2: the programmable class, draw mode */
        setenv("RECOMP_D3D8_HOST_VS", "draw", 1);
        d3d8_host_2d_metal_set_spec_sync(1);
        vs_draw_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "vs") == 0) {           /* G51.2: the programmable class */
        setenv("RECOMP_D3D8_HOST_VS", "shadow", 1);
        vs_tests();
        vs_fog_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "hostread") == 0) {     /* G56: a guest-thread Lock gets the drawn pixels */
        hostread_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "defer") == 0) {        /* RECOMP_METAL_DEFER_SWAP: the host pays a slot's debt */
        /* The executor draws into A without a sync, then into B: the swap
         * defers A's write-back, so A's guest RAM is behind its texture. A
         * host texture read of A must pay first -- nv2a_metal_pay_debt, which
         * texture_for calls -- and afterwards the bytes are the rendering. */
        enum { RT2 = RT + 0x40000 };
        static uint16_t bg[RTPITCH / 2 * RTH];
        static uint8_t depth[RTW * 4 * RTH];
        D3D8HostDrawCheck c; D3D8Host2DDraw d;
        uint16_t *a = (uint16_t *)(ram + RT), *b = (uint16_t *)(ram + RT2);
        unsigned long long before = 0, after = 0;
        background(bg); background(a); background(b);
        case_b(&c); memset(&d, 0, sizeof d); d.verts = verts;
        CHECK(!d3d8_host_2d_build(&c, ram, RAM_SIZE, 0, &d), "defer: built");
        g_exec_keep_surfaces = 1; g_exec_no_sync = 1;
        CHECK(exec_draw(&c, &d, a, depth) && exec_draw(&c, &d, b, depth), "defer: the executor drew into A, then B");
        g_exec_keep_surfaces = 0; g_exec_no_sync = 0;
        for (size_t k = 0; k < sizeof bg / 2; ++k) before += a[k] != bg[k];
        int paid = nv2a_metal_pay_debt((uint8_t *)a, RTPITCH * RTH);
        for (size_t k = 0; k < sizeof bg / 2; ++k) after += a[k] != bg[k];
        printf("  defer: A's guest RAM changed %llu px before paying, %llu after (%d slot(s) paid)\n", before, after, paid);
        CHECK(before == 0, "defer CONTROL: the swap deferred A's write-back (guest RAM still the background)");
        CHECK(paid == 1 && after > 1000, "defer: paying the debt wrote A's rendering to guest RAM");
        CHECK(nv2a_metal_pay_debt((uint8_t *)a, RTPITCH * RTH) == 0, "defer: a debt is paid once");
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "seq") == 0) {          /* many draws in one batch, every bisect arm */
        setenv("RECOMP_D3D8_HOST_2D", "draw", 1);
        setenv("RECOMP_D3D8_HOST_FF", "draw", 1);
        seq_tests();
        printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
        return fails ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "draw") == 0) {         /* the draw-mode arm, a process of its own */
        setenv("RECOMP_D3D8_HOST_2D", "draw", 1);
        setenv("RECOMP_D3D8_HOST_FF", "draw", 1);
        CHECK(d3d8_host_2d_mode() == 2 && d3d8_host_ff_mode() == 2, "RECOMP_D3D8_HOST_2D=draw and RECOMP_D3D8_HOST_FF=draw arm draw mode");
        d3d8_host_2d_metal_set_spec_sync(1);                     /* test the specialised program, not its stand-in */
        async_spec_test();
        d3d8_host_2d_metal_set_spec_sync(1);
        draw_mode_tests();
        /* THE IN-RUN CHECK: on a verify flip a draw draw mode would replace is
         * left to the executor and shadowed -- pre token, executor draw, post
         * token, compared at the flip -- although neither mode is "shadow". */
        {   /* The token sequence d3d8_host.c runs: replace token (kind 3), the
             * executor's draw, check token (kind 1: after(), then verify_take()
             * and post()). On a verify flip replace() takes the pre snapshot
             * and leaves the draw to the executor -- no skip. */
            static uint16_t bg[RTPITCH / 2 * RTH], ex[RTPITCH / 2 * RTH];
            static uint8_t depth[RTW * 4 * RTH];
            D3D8HostDrawCheck v; D3D8Host2DDraw vd; D3D8Host2DBackend be; D3D8H2DStats a, b;
            case_b(&v); memset(&vd, 0, sizeof vd); vd.verts = verts;
            CHECK(!d3d8_host_2d_build(&v, ram, RAM_SIZE, 0, &vd), "verify: built");
            background(bg); memcpy(ex, bg, sizeof bg);
            CHECK(exec_draw(&v, &vd, ex, depth), "verify: the executor drew it");
            memset(&be, 0, sizeof be);
            be.render = d3d8_host_2d_metal_render; be.sync_range = fake_sync; be.ram = ram; be.ram_size = RAM_SIZE;
            be.last_error = d3d8_host_2d_metal_last_error; be.depth_peek = fake_peek;
            be.external_draw = d3d8_host_2d_metal_external; be.exec_skip = fake_skip; be.exec_skipped = fake_skipped;
            be.ff_vertex = nv2a_ff_vertex;
            d3d8_host_2d_set_backend(&be);
            memcpy(ram + RT, bg, sizeof bg); g_exec_result = ex;
            snapshot_at_call(&v);
            d3d8_host_2d_set_verify(1);
            d3d8_host_2d_get_stats(&a);
            d3d8_host_2d_replace(&v);
            CHECK(g_fake_skip == 0, "verify: on a verify flip replace() leaves the draw to the executor (no skip)");
            d3d8_host_2d_after(&v);
            if (d3d8_host_2d_verify_take(v.serial)) { D3D8HostDrawCheck w = v; w.verify = 1; d3d8_host_2d_post(&w, NULL); }
            d3d8_host_2d_flip();
            d3d8_host_2d_get_stats(&b);
            CHECK(b.compared == a.compared + 1 && b.mismatching == a.mismatching && b.replaced == a.replaced,
                  "verify: the draw was compared against the executor, not replaced (compared +%llu, mismatching +%llu)",
                  b.compared - a.compared, b.mismatching - a.mismatching);
            d3d8_host_2d_set_verify(0);
            d3d8_host_2d_get_stats(&a);
            snapshot_at_call(&v);
            d3d8_host_2d_replace(&v);
            CHECK(g_fake_skip == 1, "verify CONTROL: off a verify flip the same draw is replaced (skip on)");
            if (g_fake_skip) ++g_fake_skipped;
            d3d8_host_2d_after(&v);
            CHECK(!d3d8_host_2d_verify_take(v.serial), "verify CONTROL: nothing to compare");
            d3d8_host_2d_get_stats(&b);
            CHECK(b.replaced == a.replaced + 1, "verify CONTROL: replaced once");
        }
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
    {   /* D3D's cull state (RS 128 CULLMODE, 127 FRONTFACE), as 0x18EBD0 emits it:
         * enable = CullMode != 0, face = FRONT (0x404) when CullMode == FrontFace, else BACK. */
        D3D8HostDrawCheck c; D3D8Host2DDraw d; static uint32_t ffm[2048];
        case_ff(&c); c.rs_valid = 1; c.rs_cull = 0x900; c.rs_front = 0x900;
        d3d8_host_ff_registers(&c, ffm); memset(&d, 0, sizeof d); d.verts = verts;
        d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(d.cull_face == 0x404u && d.front_cw == 1, "cull: CULLMODE CW with FRONTFACE CW culls FRONT (%X)", d.cull_face);
        case_ff(&c); c.rs_valid = 1; c.rs_cull = 0x901; c.rs_front = 0x900;
        memset(&d, 0, sizeof d); d.verts = verts;
        d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(d.cull_face == 0x405u, "cull: CULLMODE CCW with FRONTFACE CW culls BACK (%X)", d.cull_face);
        case_ff(&c); c.rs_valid = 1; c.rs_cull = 0; c.rs_front = 0x900;
        memset(&d, 0, sizeof d); d.verts = verts;
        d3d8_host_draw_build(&c, ram, RAM_SIZE, 0, NULL, ffm, nv2a_ff_vertex, &d);
        CHECK(d.cull_face == 0 && d.nverts == 6, "cull: CULLMODE NONE culls nothing");
    }
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
        d3d8_host_2d_pre(c.serial, c.vs_handle, c.rt_data, c.rt_format, c.rt_size, 0, 0);
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
