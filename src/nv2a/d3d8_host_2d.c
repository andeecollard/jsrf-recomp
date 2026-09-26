/* See d3d8_host_2d.h. Pure C: no device, no guest globals -- guest memory
 * arrives as a pointer and a size, the renderer and the executor's sync as a
 * backend. That is what lets jsrf_d3d8_host_2d_test drive all of it. */
#include "d3d8_host_2d.h"
#include "d3d8_ff_combiner.h"
#include "nv2a_vsh.h"
#include "nv2a_texture_copy.h"
#include "d3d8_ff_vertex_state.h"
#include "../recomp_switch.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define h2d_mkdir(p) _mkdir(p)
#else
#define h2d_mkdir(p) mkdir(p, 0755)
#endif

#define RAM_MASK 0x03FFFFFFu

/* ---- classification ---- */
int d3d8_host_2d_is_2d(const D3D8HostDrawCheck *c)
{
    return c->draw_kind != 0 && d3d8_host_2d_is_fvf_xyzrhw(c->vs_handle);
}
int d3d8_host_2d_class(const D3D8HostDrawCheck *c)
{
    if (!c->draw_kind) return 0;
    if (d3d8_host_2d_is_fvf_xyzrhw(c->vs_handle)) return 1;
    if (d3d8_host_2d_is_fvf_ff(c->vs_handle)) return 2;
    if ((c->vs_handle & 1u) && c->vs_kind == 1u && c->vs_nwords) return 3;
    return 0;
}

/* ---- building the draw from D3D state ---- */
static const uint32_t k_st[11] = D3D8_HOST_STATE_METHODS;
static const uint32_t k_x[D3D8_HOST_2D_EXTRA_N] = D3D8_HOST_2D_EXTRA_METHODS;
/* D3D's last Simple push of `method`, or the NV2A reset value if it never pushed one. */
static uint32_t st(const D3D8HostDrawCheck *c, uint32_t method, uint32_t dflt)
{
    for (unsigned k = 0; k < 11; ++k)
        if (k_st[k] == method) return (c->st_seen & (1u << k)) ? c->st_val[k] : dflt;
    for (unsigned k = 0; k < D3D8_HOST_2D_EXTRA_N; ++k)
        if (k_x[k] == method) return (c->x_seen & (1u << k)) ? c->x_val[k] : dflt;
    return dflt;
}
/* Whether D3D pushed `method` at all (Simple). */
static int st_seen(const D3D8HostDrawCheck *c, uint32_t method)
{
    for (unsigned k = 0; k < 11; ++k) if (k_st[k] == method) return (c->st_seen >> k) & 1u;
    for (unsigned k = 0; k < D3D8_HOST_2D_EXTRA_N; ++k) if (k_x[k] == method) return (c->x_seen >> k) & 1u;
    return 0;
}
static int stencil_op_ok(uint32_t op)                 /* nv2a_texture_copy.c's list; nv2a_metal_stencil_op maps each */
{
    return op == 0u || op == 0x1E00u || op == 0x1E01u || op == 0x1E02u || op == 0x1E03u || op == 0x150Au ||
           op == 0x8507u || op == 0x8508u;
}
/* A depth surface with stencil: D24S8 / F24S8, swizzled or linear (D3D
 * formats 0x2A 0x2B 0x2E 0x2F in the format word's byte 1). */
static int zs_has_stencil(const D3D8HostDrawCheck *c)
{
    uint32_t f = (c->zs_format >> 8) & 0xFFu;
    return c->zs && (f == 0x2Au || f == 0x2Bu || f == 0x2Eu || f == 0x2Fu);
}
/* STENCIL, THE CLASS THE GAME DRAWS: func ALWAYS. Every value that decides
 * what is written must have been pushed by D3D; a value it never pushed is
 * refused, not taken from the NV2A's reset state, because D3D's device
 * setup writes the registers by a path the mirror does not see. */
static const char *stencil_from_d3d(const D3D8HostDrawCheck *c, D3D8Host2DDraw *d)
{
    int replace;
    if (!st_seen(c, 0x364)) return "stencil state not pushed";
    d->stencil_func = st(c, 0x364, 0x207);
    if (d->stencil_func != 0x207u) return "stencil func not ALWAYS";
    d->stencil_fail = st(c, 0x370, 0x1E00);               /* never applied under ALWAYS */
    d->stencil_zfail = st(c, 0x374, 0x1E00); d->stencil_zpass = st(c, 0x378, 0x1E00);
    d->stencil_mask = st(c, 0x360, 0xFF); d->stencil_ref = st(c, 0x368, 0); d->stencil_func_mask = st(c, 0x36C, 0xFF);
    if (!c->zs) return "stencil test without a depth surface";
    d->stencil_write = zs_has_stencil(c);
    if (!d->stencil_write) return "stencil test on a depth surface without stencil";
    if (!st_seen(c, 0x378) || !st_seen(c, 0x360) || (d->depth_test && !st_seen(c, 0x374))) return "stencil state not pushed";
    if (!stencil_op_ok(d->stencil_zpass) || !stencil_op_ok(d->stencil_zfail) || !stencil_op_ok(d->stencil_fail))
        return "stencil operation";
    replace = d->stencil_zpass == 0x1E01u || (d->depth_test && d->stencil_zfail == 0x1E01u);
    if (replace && !st_seen(c, 0x368)) return "stencil state not pushed";
    return NULL;
}
static int32_t trunc_scaled(int32_t v, float s) { return (int32_t)((float)((double)v * (double)s + 0.5)); }
static int blend_factor_ok(uint32_t f)
{
    return f == 0 || f == 1 || (f >= 0x300 && f <= 0x308) || (f >= 0x8001 && f <= 0x8004);
}
static int blend_eq_ok(uint32_t e)
{
    return e == 0x8006 || e == 0x8007 || e == 0x8008 || e == 0x800A || e == 0x800B;
}

/* One attribute of one vertex, as the executor's fetch_attr decodes it:
 * UB_D3D (a D3DCOLOR, stored B,G,R,A), F and UB_OGL. Anything else fails. */
static int fetch(const uint8_t *ram, size_t ram_size, uint32_t offset, uint32_t format,
                 uint32_t index, float out[4])
{
    uint32_t type = format & 0xFu, size = (format >> 4) & 0xFu, stride = format >> 8;
    uint64_t at = (uint64_t)(offset & RAM_MASK) + (uint64_t)index * stride;
    out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
    if (!size || !stride || at + 16u > ram_size) return 0;
    const uint8_t *p = ram + at;
    switch (type) {
    case 0:  if (size != 4) return 0;
             out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f;
             return 1;
    case 2:  for (uint32_t i = 0; i < size && i < 4; ++i) memcpy(&out[i], p + 4u * i, 4);
             return 1;
    case 4:  for (uint32_t i = 0; i < size && i < 4; ++i) out[i] = p[i] / 255.0f;
             return 1;
    default: return 0;
    }
}

/* G75: THE REFUSALS NAME WHAT THEY REFUSED. "texture shader mode" and
 * "texture format" were one count each, so a census could not tell a
 * bump-mapped mesh from a cube map or a 16-bit swizzled format from a
 * 32-bit linear one. The reasons are counted by pointer, so each (mode,
 * unit) and each format byte gets one stable string, written on first use
 * (the builder runs on the executor's thread alone). */
static const char *mode_reason(int ff, unsigned mode, unsigned unit)
{
    static char txt[2][32][4][72];
    static const char *const k_name[32] = { "none", "2D", "3D/volume", "cube map", "pass-through", "clip plane",
                                            "bump env", "bump env luminance", "BRDF", "dot ST", "dot ZW",
                                            "dot reflect diffuse", "dot reflect specular", "dot STR 3D",
                                            "dot STR cube", "dependent AR", "dependent GB", "dot product",
                                            "dot reflect specular const" };
    char *t = txt[ff != 0][mode & 31u][unit & 3u];
    if (!t[0]) snprintf(t, sizeof txt[0][0][0], "texture shader mode%s %u (%s) unit %u", ff ? " (fixed function)" : "",
                        mode & 31u, k_name[mode & 31u] ? k_name[mode & 31u] : "?", unit & 3u);
    return t;
}
static const char *format_reason(uint32_t fb)
{
    static char txt[256][32];
    char *t = txt[fb & 0xFFu];
    if (!t[0]) snprintf(t, sizeof txt[0], "texture format 0x%02X", fb & 0xFFu);
    return t;
}

static const char *texture_from_d3d(const D3D8HostDrawCheck *c, unsigned u, D3D8H2DTexture *t)
{
    uint32_t f = c->format[u], fb = (f >> 8) & 0xFFu;
    const uint32_t *tss = c->tss[u];
    memset(t, 0, sizeof *t);
    t->addr = c->data[u] & RAM_MASK; t->d3d_format = f; t->d3d_size = c->size[u]; t->fmt = fb;
    t->levels = (f >> 16) & 0xFu;
    if (fb == 0x11u) {                         /* LU_IMAGE_R5G6B5: pitch-linear */
        t->linear = 1; t->levels = 1;
        t->width = (c->size[u] & 0xFFFu) + 1u; t->height = ((c->size[u] >> 12) & 0xFFFu) + 1u;
        t->pitch = ((c->size[u] >> 24) + 1u) * 64u;
        if (t->pitch < t->width * 2u) return "texture pitch";
    } else if ((fb == 0x12u || fb == 0x1Eu) && d3d8_host_lin32_mode() > 0) {
        /* G75: LU_IMAGE_A8R8G8B8 / X8R8G8B8, G59's graffiti canvas: image
         * rectangles like 0x11, four bytes a texel, one level (the executor
         * refuses more), sampled by the renderer from their bytes with the
         * executor's own sampler, as the executor samples them. */
        if (t->levels != 1u) return "linear 32-bit texture mip levels";
        t->linear = 1;
        t->width = (c->size[u] & 0xFFFu) + 1u; t->height = ((c->size[u] >> 12) & 0xFFFu) + 1u;
        t->pitch = ((c->size[u] >> 24) + 1u) * 64u;
        if (t->pitch < t->width * 4u) return "texture pitch";
    } else if (fb == 0x0Cu || fb == 0x0Eu || fb == 0x06u || fb == 0x07u || fb == 0x03u || fb == 0x04u) {
        unsigned lw = (f >> 20) & 0xFu, lh = (f >> 24) & 0xFu;
        if (lw > 12 || lh > 12) return "texture size";
        t->width = 1u << lw; t->height = 1u << lh;
        t->pitch = fb == 0x0Cu ? ((t->width + 3u) / 4u) * 8u : fb == 0x0Eu ? ((t->width + 3u) / 4u) * 16u
                 : (fb == 0x03u || fb == 0x04u) ? t->width * 2u : t->width * 4u;
        if (t->levels > 1u + (lw > lh ? lw : lh)) return "texture levels";
    } else return format_reason(fb);
    if (!t->levels) return "texture levels";
    /* D3DTADDRESS 1 wrap, 2 mirror, 3 clamp; a linear image never repeats
     * (the executor's rule, and the hardware's: it has no wrap for them). */
    t->wrap_u = tss[0]; t->wrap_v = tss[1];
    if (t->wrap_u < 1 || t->wrap_u > 3 || t->wrap_v < 1 || t->wrap_v > 3) return "texture address mode";
    if (t->linear) t->wrap_u = t->wrap_v = 3;
    t->mag = tss[3] == 1u ? 1u : 2u;
    {   uint32_t mn = tss[4] == 1u ? 1u : 2u, mip = tss[5] > 2u ? 2u : tss[5];
        t->min_filter = mn + 2u * mip; }
    {   float bias; memcpy(&bias, &tss[6], 4);
        float scaled = (float)((double)bias * 256.0);
        int32_t q = (scaled >= -2147483648.0f && scaled < 2147483648.0f) ? (int32_t)scaled : 0;
        q = (int32_t)((uint32_t)q << 19) >> 19;              /* the register's 13 signed bits */
        t->lod_bias = (float)q / 256.0f; }
    return NULL;
}

/* G51.2: THE TEXTURE SHADER STAGE MODES D3D WRITES TO 0x1E70, transcribed
 * from LazySetShaderStageProgram (0x1952B0, XDK 4134), which runs at the
 * draw's flush. Stage 3 down to 0, five bits each:
 *   fixed function (device +0x370 == 0): no texture -> NONE (0); else
 *     BUMPENVMAP (6) / _LUMINANCE (7) when the PREVIOUS stage's COLOROP
 *     (TSS word 12) is 0x19 / 0x1A (stage 0 never), else CUBEMAP (3) for a
 *     cube texture (Format bit 2), PROJECT3D (2) for a volume (Format
 *     dimension 3), else PROJECT2D (1);
 *   pixel shader: device +0x37C's modes, as they are when device +0x378 is
 *     0; otherwise per stage: with no texture a mode that samples becomes
 *     NONE (PASSTHRU 4, CLIPPLANE 5, DOT_ZW 0xA and DOTPRODUCT 0x11 do not
 *     sample and are kept); with one, PROJECT2D/3D/CUBEMAP become CUBEMAP for
 *     a cube, PROJECT3D for a volume or a depth format (colour format
 *     0x2A..0x31), else PROJECT2D; DOT_STR_3D/_CUBE follow the cube bit.
 * The title's shaders leave PROJECT2D on stages they bind nothing to, and
 * D3D turns those off: the executor's unit is disabled and 0x1E70 says NONE
 * there (22 Sep-25 Sep: "shader samples an unbound stage", 40% of the
 * programmable class, every one with the executor's unit disabled). */
uint32_t d3d8_host_stage_program(const D3D8HostDrawCheck *c)
{
    uint32_t w = 0;
    for (int s = 3; s >= 0; --s) {
        uint32_t m, f = c->format[s];
        if (!c->ffc_ps) {
            if (!c->tex[s]) m = 0;
            else if (s > 0 && c->tss[s - 1][12] == 0x19u) m = 6;
            else if (s > 0 && c->tss[s - 1][12] == 0x1Au) m = 7;
            else if (f & 4u) m = 3;
            else if ((f & 0xF0u) == 0x30u) m = 2;
            else m = 1;
        } else if (!c->stage_prog_in[0]) {
            return c->stage_prog_in[1];
        } else {
            m = (c->stage_prog_in[1] >> (5u * (unsigned)s)) & 31u;
            if (!c->tex[s]) {
                if (m != 4u && m != 5u && m != 0xAu && m != 0x11u) m = 0;
            } else if (m >= 1u && m <= 3u) {
                uint32_t cf = f & 0xFF00u;
                m = (f & 4u) ? 3u : (f & 0xF0u) == 0x30u ? 2u : (cf >= 0x2A00u && cf <= 0x3100u) ? 2u : 1u;
            } else if (m == 0xDu || m == 0xEu) {
                m = (f & 4u) ? 0xEu : 0xDu;
            }
        }
        w = (w << 5) | m;
    }
    return w;
}

/* Facing, by the rule nv2a_texture_copy_front_facing() applies (it lives in
 * xbox_vsh, which this library must not depend on): screen-space area with y
 * down, flipped when an odd number of the three w are negative. */
static int culled(uint32_t cull_face, uint32_t front_cw, const float *a, const float *b, const float *cc)
{
    float ar = (b[0] - a[0]) * (cc[1] - a[1]) - (b[1] - a[1]) * (cc[0] - a[0]);
    int flip = (((a[3] < 0) + (b[3] < 0) + (cc[3] < 0)) & 1) != 0;
    int front = ((ar > 0) ^ flip) == (front_cw != 0);
    return cull_face == 0x408u || (cull_face == 0x404u && front) || (cull_face == 0x405u && !front);
}

/* ---- the index ring ---- */
#include <stdatomic.h>
static uint16_t s_iring[D3D8H2D_IDX_RING];
static uint64_t s_ireserve;                   /* producer only */
static _Atomic uint64_t s_ihead;              /* published end */
uint16_t *d3d8_host_2d_idx_reserve(uint32_t n, uint64_t *pos)
{
    uint64_t p = s_ireserve;
    if (!n || n > D3D8H2D_IDX_PER_DRAW) return NULL;
    if ((p % D3D8H2D_IDX_RING) + n > D3D8H2D_IDX_RING) p += D3D8H2D_IDX_RING - p % D3D8H2D_IDX_RING;
    *pos = p; s_ireserve = p + n;
    return &s_iring[p % D3D8H2D_IDX_RING];
}
void d3d8_host_2d_idx_publish(uint64_t pos, uint32_t n)
{
    atomic_store_explicit(&s_ihead, pos + n, memory_order_release);
}
int d3d8_host_2d_idx_copy(uint64_t pos, uint32_t n, uint16_t *out)
{
    uint64_t h = atomic_load_explicit(&s_ihead, memory_order_acquire);
    if (!n || pos + n > h || h - pos > D3D8H2D_IDX_RING) return 0;
    memcpy(out, &s_iring[pos % D3D8H2D_IDX_RING], (size_t)n * 2u);
    /* The producer may have reserved further meanwhile; if it has come round
     * onto these entries the copy is torn. Its reservation leads its publish,
     * so check against the reservation as well as the head it has published. */
    h = atomic_load_explicit(&s_ihead, memory_order_acquire);
    return h - pos + 2u * D3D8H2D_IDX_PER_DRAW <= D3D8H2D_IDX_RING;
}
/* ---- G75: the UP ring, the index ring's shape for DrawVerticesUP's bytes ---- */
static uint8_t s_upring[D3D8H2D_UP_RING];
static uint64_t s_upreserve;
static _Atomic uint64_t s_uphead;
uint8_t *d3d8_host_2d_up_reserve(uint32_t n, uint64_t *pos)
{
    uint64_t p = s_upreserve;
    if (!n || n > D3D8H2D_UP_PER_DRAW) return NULL;
    if ((p % D3D8H2D_UP_RING) + n > D3D8H2D_UP_RING) p += D3D8H2D_UP_RING - p % D3D8H2D_UP_RING;
    *pos = p; s_upreserve = p + n;
    return &s_upring[p % D3D8H2D_UP_RING];
}
void d3d8_host_2d_up_publish(uint64_t pos, uint32_t n)
{
    atomic_store_explicit(&s_uphead, pos + n, memory_order_release);
}
int d3d8_host_2d_up_copy(uint64_t pos, uint32_t n, uint8_t *out)
{
    uint64_t h = atomic_load_explicit(&s_uphead, memory_order_acquire);
    if (!n || pos + n > h || h - pos > D3D8H2D_UP_RING) return 0;
    memcpy(out, &s_upring[pos % D3D8H2D_UP_RING], n);
    h = atomic_load_explicit(&s_uphead, memory_order_acquire);
    return h - pos + 2u * D3D8H2D_UP_PER_DRAW <= D3D8H2D_UP_RING;
}
/* G73: THE HASH RUNS ON THE TITLE'S THREAD, before every replaced draw, and
 * it was most of the title's frame. Measured 25 Sep 2026 in Rokkaku-dai free
 * play with the lift on: the mirror's draw hooks cost 15.45 ms a flip on the
 * guest thread, 14.33 ms of it this function -- one byte per multiply, over
 * every enabled array's range, so an interleaved stream (position, normal,
 * texture coordinates in one buffer) was hashed once per attribute. With the
 * pusher's GPU waits removed (RECOMP_METAL_ASYNC_WRITEBACK) the frame is the
 * guest thread's, and this is what it was spending.
 *
 * Same question, same bytes: the enabled arrays' ranges are merged first, so
 * each byte is read once, and the bytes are folded eight at a time in four
 * independent lanes (the texture cache's hash64 shape). Both sides -- the
 * call (d3d8_mirror.c) and the token (d3d8_host_2d_post) -- use this one
 * function, so "did the bytes change between the two" is answered exactly as
 * before; only the value of the hash differs. */
static uint64_t vhash_bytes(uint64_t h, const uint8_t *p, size_t n)
{
    uint64_t h0 = h, h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull, w0, w1, w2, w3;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8); memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * 0x100000001B3ull; h1 = (h1 ^ w1) * 0x100000001B3ull;
        h2 = (h2 ^ w2) * 0x100000001B3ull; h3 = (h3 ^ w3) * 0x100000001B3ull;
        h0 ^= h0 >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29;
    }
    h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull) ^ (uint64_t)n;
    for (; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
uint64_t d3d8_host_2d_vertex_hash(const uint8_t *ram, size_t ram_size, const D3D8HostDrawCheck *c,
                                  uint32_t imin, uint32_t imax)
{
    uint64_t h = 0xCBF29CE484222325ull, lo[16], hi[16];
    unsigned n = 0;
    if (d3d8_host_2d_bisect() & 8192u) {         /* BISECT 8192: the byte-wise hash, per array (before G73) */
        for (unsigned i = 0; i < 16; ++i) {
            uint32_t stride = c->va_format[i] >> 8;
            if (!((c->va_on >> i) & 1u) || !stride) continue;
            uint64_t a = (uint64_t)(c->va_offset[i] & RAM_MASK) + (uint64_t)imin * stride;
            uint64_t b = (uint64_t)(c->va_offset[i] & RAM_MASK) + (uint64_t)(imax + 1u) * stride;
            if (b > ram_size || b < a || b - a > (1u << 22)) { h ^= 0xFFu; h *= 0x100000001B3ull; continue; }
            for (uint64_t k = a; k < b; ++k) { h ^= ram[k]; h *= 0x100000001B3ull; }
        }
        return h;
    }
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t stride = c->va_format[i] >> 8;
        if (!((c->va_on >> i) & 1u) || !stride) continue;
        uint64_t a = (uint64_t)(c->va_offset[i] & RAM_MASK) + (uint64_t)imin * stride;
        uint64_t b = (uint64_t)(c->va_offset[i] & RAM_MASK) + (uint64_t)(imax + 1u) * stride;
        if (b > ram_size || b < a || b - a > (1u << 22)) { h ^= 0xFFu; h *= 0x100000001B3ull; continue; }
        {   /* insertion by start, then merged below */
            unsigned k = n++;
            while (k && lo[k - 1] > a) { lo[k] = lo[k - 1]; hi[k] = hi[k - 1]; --k; }
            lo[k] = a; hi[k] = b;
        }
    }
    for (unsigned k = 0; k < n;) {
        uint64_t a = lo[k], b = hi[k];
        for (++k; k < n && lo[k] <= b; ++k) if (hi[k] > b) b = hi[k];
        h = vhash_bytes(h, ram + a, (size_t)(b - a));
    }
    return h;
}

const char *d3d8_host_2d_build(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                               int control, D3D8Host2DDraw *d)
{
    return d3d8_host_2d_build_ex(c, ram, ram_size, control, NULL, d);
}

const char *d3d8_host_2d_build_ex(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                                  int control, const uint16_t *given_idx, D3D8Host2DDraw *d)
{
    if (!d3d8_host_2d_is_2d(c)) return "not pre-transformed";
    return d3d8_host_draw_build(c, ram, ram_size, control, given_idx, NULL, NULL, d);
}

static D3D8H2DFFGpuFn s_ff_gpu_fn;
void d3d8_host_2d_set_ff_gpu(D3D8H2DFFGpuFn fn) { s_ff_gpu_fn = fn; }
int d3d8_host_ff_gpu_mode(void)
{
    /* Read once, on the executor thread (the only caller: build). */
    static int m = -1;
    if (m < 0) {
        m = recomp_switch_on("RECOMP_D3D8_HOST_FF_GPU");
        if (m) fprintf(stderr, "[D3D8-HOST-FF] RECOMP_D3D8_HOST_FF_GPU=1: fixed-function draws the executor's GPU unit"
                               " accepts are transformed on the GPU (its RECOMP_METAL_FF vertex function), not per vertex"
                               " on the CPU\n");
    }
    return m;
}
static int s_inline_mode = -1;
void d3d8_host_2d_set_inline(int on) { s_inline_mode = on ? 1 : 0; }
int d3d8_host_inline_mode(void)
{
    if (s_inline_mode < 0) {
        s_inline_mode = recomp_switch_on("RECOMP_D3D8_HOST_INLINE");
        if (s_inline_mode) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_INLINE=1: DrawVerticesUP and Begin/End draws are"
                                           " mirrored, their vertices copied as the title gives them, and drawn by the host"
                                           " in draw mode (G75)\n");
    }
    return s_inline_mode;
}
static int s_lin32_mode = -1;
void d3d8_host_2d_set_lin32(int on) { s_lin32_mode = on ? 1 : 0; }
int d3d8_host_lin32_mode(void)
{
    if (s_lin32_mode < 0) {
        s_lin32_mode = recomp_switch_on("RECOMP_D3D8_HOST_LIN32");
        if (s_lin32_mode) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_LIN32=1: linear 32-bit textures (0x12/0x1E)"
                                          " are drawn by the host, sampled from their bytes as the executor samples"
                                          " them (G75)\n");
    }
    return s_lin32_mode;
}
static int s_bump_mode = -1;
void d3d8_host_2d_set_bump(int on) { s_bump_mode = on ? 1 : 0; }
int d3d8_host_bump_mode(void)
{
    if (s_bump_mode < 0) {
        s_bump_mode = recomp_switch_on("RECOMP_D3D8_HOST_BUMP");
        if (s_bump_mode) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_BUMP=1: BUMPENVMAP units (texture shader modes"
                                         " 6 and 7) are drawn by the host in draw mode, displaced as the executor"
                                         " displaces them (G75)\n");
    }
    return s_bump_mode;
}
/* G75: a BUMPENVMAP unit, taken as the executor takes it
 * (nv2a_texture_copy_prepare): units 1..3 whose input unit is an earlier
 * one. Unit 1 reads unit 0; units 2 and 3 read the unit NV097's
 * SHADER_OTHER_STAGE_INPUT (0x1E78) names, which is a pixel shader's
 * PSInputTexture (definition word 56); a fixed-function draw is only taken
 * on unit 1. The matrix and luminance words are D3D's texture-stage state
 * 22..27 (BUMPENVMAT00, 01, 11, 10, LSCALE, LOFFSET). SetTextureState_BumpEnv
 * (0x18F180) pushes stage s's to unit s under a pixel shader (device +0x370
 * set) and to unit s+1 without one, so unit u reads D3D stage u or u-1.
 * The texture must be one the executor's buffer sampler reads the way D3D
 * describes it: repeat (all wrap) or clamp, never mirror or mixed. */
static const char *bump_from_d3d(const D3D8HostDrawCheck *c, unsigned u, uint32_t mode, int ps, D3D8Host2DDraw *d)
{
    unsigned in, s;
    const uint32_t *t;
    if (d3d8_host_bump_mode() <= 0) return mode_reason(!ps, mode, u);
    if (u < 1u) return "bump env on unit 0";
    if (ps) in = u == 1u ? 0u : (c->ps[56] >> (16u + 4u * (u - 2u))) & 15u;
    else { if (u != 1u) return "bump env (fixed function) past unit 1"; in = 0u; }
    if (in >= u) return "bump env reads a later unit";
    if (!c->tex[u]) return "bump env on an unbound stage";
    s = ps ? u : u - 1u; t = c->tss[s];
    d->bump[u] = mode; d->bump_in[u] = in;
    memcpy(&d->bump_mat[u][0], &t[22], 4);     /* M00 */
    memcpy(&d->bump_mat[u][1], &t[23], 4);     /* M01 */
    memcpy(&d->bump_mat[u][2], &t[25], 4);     /* M10 */
    memcpy(&d->bump_mat[u][3], &t[24], 4);     /* M11 */
    memcpy(&d->bump_scale[u], &t[26], 4); memcpy(&d->bump_offset[u], &t[27], 4);
    d->tmask |= 1u << u;
    return NULL;
}
int d3d8_host_points_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *v = getenv("RECOMP_D3D8_HOST_POINTS");
        m = recomp_switch_on("RECOMP_D3D8_HOST_POINTS");
        /* =mark: an instrument, not a fix -- every point is drawn 12 pixels
         * wide, so a presented frame shows where the title puts them. */
        if (m && v && strcmp(v, "mark") == 0) m = 2;
        if (m == 2) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_POINTS=mark: points drawn 12 px wide (an instrument)\n");
        if (m) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_POINTS=1: fixed-function points and lines are drawn by"
                               " the host in draw mode, as the executor's quads (G74)\n");
    }
    return m;
}
/* D3D's point updater 0x195140, POINTSCALEENABLE off (loc_0019517F onward):
 * the x87 works in double (the recompiled FPU stack does too), the sum is
 * stored as a float and cvttss2si truncates it; a result above 0x1FF, or
 * negative (compared unsigned), is 0x1FF. Constants from the XBE: 64.0 at
 * 0x19B0F0, 8.0 at 0x1CEED0, 0.5 at 0x1C4550. */
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
uint32_t d3d8_host_point_size_reg(const uint32_t pt_rs[8], uint32_t dev_scale)
{
    double s = (double)u2f(pt_rs[0]) * (double)u2f(dev_scale);
    double mn = u2f(pt_rs[1]), mx = u2f(pt_rs[7]);
    float f;
    if (s < mn) s = mn;                  /* fcom; jp: kept when >= or unordered */
    if (s > mx) s = mx;                  /* fcom; test ah,0x41; jne: kept when <= or unordered */
    if (s > 64.0) s = 64.0;
    f = (float)(s * 8.0 + 0.5);
    if (!(f > -1.0f) || !(f < 2147483648.0f)) return 0x1FFu;   /* cvttss2si: negative or 0x80000000 */
    {   uint32_t v = (uint32_t)(int32_t)f;
        return v > 0x1FFu ? 0x1FFu : v; }
}
const char *d3d8_host_draw_build(const D3D8HostDrawCheck *c, const uint8_t *ram, size_t ram_size,
                                 int control, const uint16_t *given_idx, const uint32_t *ffm,
                                 D3D8H2DFFVertexFn ffv, D3D8Host2DDraw *d)
{
    D3D8H2DVertex *verts = d->verts;
    int cls = d3d8_host_2d_class(c);
    const uint8_t *vram = ram;
    size_t vram_size = ram_size;
    memset(d, 0, sizeof *d);
    d->verts = verts;
    d->serial = c->serial; d->fvf = c->vs_handle; d->prim = c->prim; d->count = c->count;
    d->draw_kind = c->draw_kind; d->ss_x = c->ss_x; d->ss_y = c->ss_y;
    d->idx_min = UINT32_MAX; d->idx_max = 0;
    d->cls = (uint32_t)cls;
    if (!verts) return "no vertex buffer";
    if (!cls) return "not a host class";
    /* G75: a DrawVerticesUP draw's vertices are the mirror's copy of the
     * caller's (kind 3), a Begin/End draw's the vertices the mirror
     * assembled from SetVertexData (kind 4); the arrays are offsets into the
     * copy. */
    if (c->draw_kind == 3u || c->draw_kind == 4u) {
        static uint8_t upbuf[D3D8H2D_UP_PER_DRAW + 16u];
        if (d3d8_host_inline_mode() <= 0) return "inline vertices (RECOMP_D3D8_HOST_INLINE off)";
        if (c->up_over || !c->up_bytes) return c->draw_kind == 4u ? "Begin/End: vertices not assembled" : "DrawVerticesUP: vertices not copied";
        if (!d3d8_host_2d_up_copy(c->up_pos, c->up_bytes, upbuf)) return "inline vertices: copy overwritten";
        memset(upbuf + c->up_bytes, 0, 16u);
        vram = upbuf; vram_size = c->up_bytes + 16u;   /* fetch reads 16 bytes at a time */
    }
    if (cls == 2 && (!ffm || !ffv)) return "no fixed-function register file or evaluator";
    if (cls == 3 && d3d8_host_vs_mode() <= 0) return "programmable vertex shader (RECOMP_D3D8_HOST_VS off)";

    /* Target. */
    if (!c->rt) return "no render target";
    d->rt_fmt = (c->rt_format >> 8) & 0xFFu;
    if (d->rt_fmt != 0x11u) return "target format";            /* LU_IMAGE_R5G6B5 only */
    d->rt_addr = c->rt_data & RAM_MASK;
    d->rt_w = (c->rt_size & 0xFFFu) + 1u; d->rt_h = ((c->rt_size >> 12) & 0xFFFu) + 1u;
    d->rt_pitch = ((c->rt_size >> 24) + 1u) * 64u;
    if (d->rt_pitch < d->rt_w * 2u || (uint64_t)d->rt_addr + (uint64_t)d->rt_pitch * d->rt_h > ram_size)
        return "target bounds";

    /* Scissor: the viewport, supersample-scaled and cut to the target -- G39's derivation. */
    d->sc_x0 = trunc_scaled(c->vp_x, c->ss_x); d->sc_y0 = trunc_scaled(c->vp_y, c->ss_y);
    d->sc_x1 = trunc_scaled(c->vp_x + c->vp_w, c->ss_x) - 1; d->sc_y1 = trunc_scaled(c->vp_y + c->vp_h, c->ss_y) - 1;
    if (d->sc_x0 < 0) d->sc_x0 = 0;
    if (d->sc_y0 < 0) d->sc_y0 = 0;
    if (d->sc_x1 > (int32_t)d->rt_w - 1) d->sc_x1 = (int32_t)d->rt_w - 1;
    if (d->sc_y1 > (int32_t)d->rt_h - 1) d->sc_y1 = (int32_t)d->rt_h - 1;
    if (d->sc_x1 < d->sc_x0 || d->sc_y1 < d->sc_y0) return "empty viewport";

    /* Fragment state, as D3D pushed it. Depth and stencil: the shadow has no
     * depth buffer, so a draw that could be depth-occluded is not drawn;
     * ALWAYS with no stencil changes no colour and is. */
    /* Depth: drawn against the executor's own depth (the shadow seeds a real
     * attachment from it). An unwritten DEPTH_FUNC is LEQUAL, as the
     * executor reads it (NV2A_GUEST_DEPTH_FUNC_DEFAULT); ZWRITEENABLE's
     * default is on. Stencil is still refused. */
    d->depth_test = st(c, 0x30C, 0) != 0; d->depth_func = st(c, 0x354, 0x203);
    if (!d->depth_func) d->depth_func = 0x203;
    d->depth_write = st(c, 0x35C, 1) != 0;
    d->stencil_test = st(c, 0x32C, 0) != 0;
    if (d->stencil_test && (d3d8_host_2d_bisect() & 256u)) return "stencil test (bisect 256)";
    if (d->stencil_test) { const char *sw = stencil_from_d3d(c, d); if (sw) return sw; }
    if (d->depth_test && (d->depth_func < 0x200u || d->depth_func > 0x207u)) return "depth func";
    if (d->depth_test || d->stencil_test) {
        if (!c->zs) return "depth test without a depth surface";
        d->zs_addr = c->zs_data & RAM_MASK; d->zs_pitch = ((c->zs_size >> 24) + 1u) * 64u;
        if (d->zs_pitch < d->rt_w * 4u || (uint64_t)d->zs_addr + (uint64_t)d->zs_pitch * d->rt_h > ram_size)
            return "depth surface bounds";
    }
    /* G53: FOG, as the executor now draws it. D3D's fog updater's registers
     * from its inputs (d3d8_ff_fog): FOG_MODE and FOG_PARAMS here, the gen
     * mode in the vertex unit's registers for the fixed-function class; the
     * colour as SetRenderState_FogColor writes it. The factor is formed per
     * pixel from the vertex's fog coordinate, the executor's formula. */
    d->fog_enable = 0;
    if (c->ffv_valid && c->fg_cur.enable) {
        D3D8FFFog fo;
        d3d8_ff_fog(&c->fg_cur, &fo);
        d->fog_enable = 1; d->fog_mode = fo.mode;
        memcpy(&d->fog_p0, &fo.params[0], 4); memcpy(&d->fog_p1, &fo.params[1], 4);
        d->fog_color = d3d8_ff_fog_color(c->ffv_fog_color);
        /* The 2D class runs D3D's pass-through program, which D3D picks by
         * FOGTABLEMODE (0x1903A0): only the vertex-fog one (NONE: the
         * coordinate is the specular alpha) is modelled here. */
        if (cls == 1 && fo.gen_mode != 0) return "2D fog from a fog table";
        if (cls == 1 && !((c->va_on >> 4) & 1u)) return "2D fog without a specular array";
    }
    d->alpha_test = st(c, 0x300, 0); d->alpha_func = st(c, 0x33C, 0x207); d->alpha_ref = st(c, 0x340, 0);
    if (d->alpha_test && (d->alpha_func < 0x200u || d->alpha_func > 0x207u)) return "alpha func";
    d->blend = st(c, 0x304, 0);
    d->blend_src = st(c, 0x344, 1); d->blend_dst = st(c, 0x348, 0); d->blend_eq = st(c, 0x350, 0x8006);
    d->blend_color = st(c, 0x34C, 0);
    if (d->blend && (!blend_factor_ok(d->blend_src) || !blend_factor_ok(d->blend_dst) || !blend_eq_ok(d->blend_eq)))
        return "blend";
    d->dither = st(c, 0x310, 0) != 0;
    d->color_mask = st(c, 0x358, 0x01010101u);

    /* Combiners. */
    d->pixel_shader = c->ffc_ps;
    if (c->ffc_ps) {
        const uint32_t *w = c->ps;
        if (!c->ps_bound) return "pixel shader definition missing";
        d->control = w[53]; d->cc = w[53] & 0xFu;
        for (unsigned i = 0; i < 8; ++i) {
            d->ai[i] = w[i]; d->k0[i] = w[10 + i]; d->k1[i] = w[18 + i]; d->ao[i] = w[26 + i];
            d->ci[i] = w[34 + i]; d->co[i] = w[45 + i];
        }
        d->final_cw0 = w[8]; d->final_cw1 = w[9]; d->sf0 = w[43]; d->sf1 = w[44];
        /* The final combiner registers are D3D's too: its fog updater
         * (0x195610) writes CW0/CW1 unless a pixel shader with its own final
         * combiner is set (device +0x370 and +0x374 both nonzero). The
         * characters' shaders have none -- words 8/9 are 0, which would draw
         * nothing -- and D3D gives them the fog/specular program, as it does
         * a fixed-function draw. BISECT 4096 reads words 8/9 again. */
        if (!(d3d8_host_2d_bisect() & 4096u) && !(c->fog_cur[2] && c->fog_cur[3]) &&
            !d3d8_ff_final_combiner(c->fog_cur[0], c->fog_cur[1], c->fog_cur[2], c->fog_cur[3],
                                    &d->final_cw0, &d->final_cw1))
            return "final combiner not written";
        /* The stage modes are D3D's derivation (d3d8_host_stage_program),
         * not the definition's word 54; BISECT 4096 reads word 54 again. */
        uint32_t modes = (d3d8_host_2d_bisect() & 4096u) ? w[54] : d3d8_host_stage_program(c);
        d->stage_modes = modes; d->ps_word54 = w[54]; d->modes_adjusted = (modes & 0xFFFFFu) != (w[54] & 0xFFFFFu);
        for (unsigned u = 0; u < 4; ++u) {
            uint32_t mode = (modes >> (5u * u)) & 31u;
            if (mode == 6u || mode == 7u) { const char *why = bump_from_d3d(c, u, mode, 1, d); if (why) return why; continue; }
            if (mode > 1u) return mode_reason(0, mode, u);
            if (mode == 1u) { if (!c->tex[u]) return "shader samples an unbound stage"; d->tmask |= 1u << u; }
        }
    } else {
        D3D8FFCombiners o;
        if (!c->ffc_valid) return "no combiner state";
        if (d3d8_ff_combiners(&c->ffc_cur, &o) != 0 || !o.emitted) return "combiner unresolved";
        d->control = o.combiner_control; d->cc = o.combiner_control & 0xFu;
        memcpy(d->ci, o.color_icw, sizeof d->ci); memcpy(d->co, o.color_ocw, sizeof d->co);
        memcpy(d->ai, o.alpha_icw, sizeof d->ai); memcpy(d->ao, o.alpha_ocw, sizeof d->ao);
        d3d8_ff_texture_factor(c->tfactor, 0, d->k0, d->k1);
        if (!d3d8_ff_final_combiner(c->fog_cur[0], c->fog_cur[1], c->fog_cur[2], c->fog_cur[3],
                                    &d->final_cw0, &d->final_cw1))
            return "final combiner not written";
        if (d3d8_host_2d_bisect() & 4096u) {
            for (unsigned u = 0; u < 4; ++u) if (c->tex[u]) d->tmask |= 1u << u;
        } else {
            /* A bound stage is sampled in 2D unless D3D gives it another mode:
             * BUMPENVMAP after a BUMPENVMAP COLOROP (the executor displaces it,
             * G60), a cube map or a volume. Those stay with the executor. */
            uint32_t modes = d3d8_host_stage_program(c);
            for (unsigned u = 0; u < 4; ++u) {
                uint32_t mode = (modes >> (5u * u)) & 31u;
                if (mode == 6u || mode == 7u) { const char *why = bump_from_d3d(c, u, mode, 0, d); if (why) return why; continue; }
                if (mode > 1u) return mode_reason(1, mode, u);
                if (mode == 1u) d->tmask |= 1u << u;
            }
        }
    }
    if (!d->cc || d->cc > 8u) return "combiner count";
    /* The final combiner as the executor models it (G53): the two fog-off
     * programs as R0 (+ specular), anything else nv2a_final_combine accepts
     * in full. */
    if ((d->final_cw0 != 0xCu && d->final_cw0 != 0xEu) || d->final_cw1 != 0x1C80u) {
        if (nv2a_final_combiner_supported(d->final_cw0, d->final_cw1)) return "final combiner";
        d->final_general = 1;
    }
    d->add_specular = !d->final_general && d->final_cw0 == 0xEu;

    for (unsigned u = 0; u < 4; ++u) {
        const char *why;
        if (!(d->tmask & (1u << u))) continue;
        if ((why = texture_from_d3d(c, u, &d->tex[u]))) return why;
        if ((uint64_t)d->tex[u].addr + d->tex[u].pitch > ram_size) return "texture bounds";
        /* The executor's buffer sampler repeats or clamps both axes alike. */
        if (d->bump[u] && (d->tex[u].wrap_u != d->tex[u].wrap_v || d->tex[u].wrap_u == 2u ||
                           (d->tex[u].wrap_u == 1u && c->tss[u][2] != 1u)))
            return "bump env unit's address mode (mirror or mixed)";
    }

    /* Vertices: G41's arrays at the draw's indices. */
    if (!(c->va_on & 1u)) return "no position array";
    if (cls == 1 && (c->va_format[0] & 0xFFu) != 0x42u) return "position format";          /* float x 4 */
    if (cls == 2 && (c->va_format[0] & 0xFFu) != 0x32u && (c->va_format[0] & 0xFFu) != 0x42u) return "position format";
    /* Triangles, strips, fans, quads and quad strips, triangulated exactly as
     * nv2a_metal_draw does (the order decides facing). Points, lines and
     * POLYGON (10) the executor refuses, so the host does not draw them. */
    /* Points (1) and lines (2-4) and POLYGON (10): nv2a_metal_draw rejects
     * "primitive" and the CPU fallback's switch draws nothing ("points and
     * lines: not yet"), so the executor's picture is unchanged by them. The
     * host describes that exactly: no vertices. */
    if (((d->prim >= 1u && d->prim <= 4u) || d->prim == 10u) && (d3d8_host_2d_bisect() & 512u)) return "primitive (bisect 512)";
    /* G54: the executor now DRAWS points and lines (screen-space quads in
     * nv2a_metal.m), so describing them as "nothing" would make draw mode
     * skip real geometry. POLYGON (10) is still drawn by nobody.
     * G74: with RECOMP_D3D8_HOST_POINTS the host draws the fixed-function
     * ones the same way. The class the title draws is one kind: a POINTLIST
     * of ONE vertex in a model's own format (FVF 0x102/0x112/0x152/0x212/0x252),
     * textured, depth-tested, 1 pixel, no sprite and no attenuation -- one
     * pixel per draw. The executor keeps them off its GPU vertex paths and expands
     * the CPU unit's screen positions; so does the host. Pre-transformed and
     * programmable points, and attenuated point size, which the executor
     * only approximates, stay with the executor. */
    const int pl = d->prim >= 1u && d->prim <= 4u;
    if (pl) {
        if (d3d8_host_points_mode() <= 0) return "points and lines (left to the executor)";
        if (cls != 2) return cls == 1 ? "points and lines, pre-transformed (left to the executor)"
                                      : "points and lines, vertex program (left to the executor)";
        if (!c->pt_valid) return "points and lines: no point state";
        if (d->prim == 1u && c->pt_rs[3]) return "points with POINTSCALEENABLE (left to the executor)";
        d->point_reg = d3d8_host_point_size_reg(c->pt_rs, c->pt_dev_scale);
        if (d3d8_host_points_mode() == 2) d->point_reg = 96u;
        d->point_sprite = d->prim == 1u && c->pt_rs[2] != 0;
    }
    if (d->prim == 10u) { d->prim_empty = 1; d->nverts = 0; d->bb_x0 = 1; d->bb_x1 = 0; return NULL; }
    if (!pl && (d->prim < 5u || d->prim > 9u)) return "primitive";
    if (c->count > 16384u) return "vertex count";
    {
        static uint32_t idx[16384 + 4];
        uint32_t n = c->count, ntri = 0, t3[3], cull = st(c, 0x308, 0) ? st(c, 0x39C, 0x405) : 0;
        uint32_t front_cw = st(c, 0x3A0, 0x901) == 0x900u;
        if (c->rs_valid) {                         /* D3D's own CULLMODE/FRONTFACE, as 0x18EBD0 emits them */
            cull = c->rs_cull ? 0x404u + (c->rs_cull != c->rs_front) : 0u;
            front_cw = c->rs_front == 0x900u;
        }
        if (pl) cull = 0;                          /* the NV2A culls neither */
        d->cull_face = cull; d->front_cw = front_cw;
        for (uint32_t k = 0; k < n; ++k) {
            if (c->draw_kind == 2 && given_idx) idx[k] = given_idx[k];
            else if (c->draw_kind == 2) {
                uint64_t at = (uint64_t)(c->idx_ptr & RAM_MASK) + 2u * k;
                if (!c->idx_ptr || at + 2u > ram_size) return "index bounds";
                idx[k] = (uint32_t)ram[at] | (uint32_t)ram[at + 1] << 8;
            } else idx[k] = c->start + k;
        }
        /* G51.2: THE TITLE'S OWN VERTEX PROGRAM. Program and constants come
         * from D3D (the shader object's program fragment, SetVertexShaderConstant
         * at slot = register + 96, and the viewport pair D3D keeps at slots 58
         * and 59); the transform is the executor's own translation, run on the
         * GPU by the host's draw. The host fetches each referenced vertex's
         * inputs once, packed in ascending attribute order as vs_gpu reads them,
         * and hands it the triangle list -- assembled here in the executor's
         * order, which decides facing. Clipping and culling are the GPU's, as
         * on the executor's GPU path. */
        /* G52: a fixed-function draw the executor's GPU unit accepts goes the
         * same way, with the FF vertex function and nv2a_ff_constants in place
         * of the program and its constants (RECOMP_D3D8_HOST_FF_GPU). */
        int ffg = cls == 2 && !pl && n && ffm && s_ff_gpu_fn && d3d8_host_ff_gpu_mode() > 0 && s_ff_gpu_fn(ffm, d);
        if (cls == 3 || ffg) {
            static float (*vin)[4]; static uint32_t vin_cap; static uint32_t *vidx; static uint32_t vidx_cap;
            NV2AVshProgram prog;
            uint32_t lo = UINT32_MAX, hi = 0, range, nattrs = 0, nt = 0, slot_of[16];
            if (!n) return "empty";
            memset(&prog, 0, sizeof prog);
            if (ffg) {
                d->ff_gpu = 1; prog.inputs_read = (uint16_t)d->vs_inputs;
            } else {
                nv2a_vsh_parse(c->vs_words, (int)(c->vs_nwords / 4u), &prog);
                if (!prog.valid || !prog.has_final || prog.length <= 0) return "vertex program not translatable";
                d->vs_words = c->vs_words; d->vs_len = (uint32_t)prog.length; d->vs_inputs = prog.inputs_read;
            }
            for (unsigned a = 0; a < 16; ++a) if (prog.inputs_read & (1u << a)) slot_of[a] = nattrs++;
            if (ffg && nattrs != d->vs_nattrs) return "host ff gpu: attribute count disagrees with the unit's";
            d->vs_nattrs = nattrs;
            for (uint32_t k = 0; k < n; ++k) { if (idx[k] < lo) lo = idx[k]; if (idx[k] > hi) hi = idx[k]; }
            range = hi - lo + 1u;
            if (range > 65536u) return "index range";
            {   uint32_t need = range * (nattrs ? nattrs : 1u);
                if (need > vin_cap) { float (*g)[4] = realloc(vin, need * sizeof *g); if (!g) return "out of memory"; vin = g; vin_cap = need; }
                if (3u * n > vidx_cap) { uint32_t *g = realloc(vidx, 3u * n * sizeof *g); if (!g) return "out of memory"; vidx = g; vidx_cap = 3u * n; } }
            for (uint32_t v = 0; v < range; ++v)
                for (unsigned a = 0; a < 16; ++a) {
                    float x[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    if (!(prog.inputs_read & (1u << a))) continue;
                    if (a == 3u) x[0] = x[1] = x[2] = 1.0f;                 /* diffuse without an array: white */
                    if ((c->va_on >> a) & 1u) { if (!fetch(vram, vram_size, c->va_offset[a], c->va_format[a], lo + v, x)) d->vs_in_unfetched |= 1u << a; }
                    else d->vs_in_noarray |= 1u << a;
                    memcpy(vin[v * nattrs + slot_of[a]], x, 16);
                }
            for (uint32_t k = 0; ; ++k) {
                uint32_t t[3];
                if (d->prim == 5u)      { if (3u * k + 2u >= n) break; t[0] = 3u*k; t[1] = 3u*k+1u; t[2] = 3u*k+2u; }
                else if (d->prim == 6u) { if (k + 2u >= n) break; t[0] = k + (k & 1u); t[1] = k + 1u - (k & 1u); t[2] = k + 2u; }
                else if (d->prim == 7u) { if (k + 2u >= n) break; t[0] = 0; t[1] = k + 1u; t[2] = k + 2u; }
                else if (d->prim == 8u) { uint32_t q = k / 2u; if (4u * q + 3u >= n) break;
                                          if (k & 1u) { t[0] = 4u*q; t[1] = 4u*q+2u; t[2] = 4u*q+3u; } else { t[0] = 4u*q; t[1] = 4u*q+1u; t[2] = 4u*q+2u; } }
                else                    { uint32_t q = k / 2u; if (2u * q + 3u >= n) break;
                                          if (k & 1u) { t[0] = 2u*q; t[1] = 2u*q+3u; t[2] = 2u*q+2u; } else { t[0] = 2u*q; t[1] = 2u*q+1u; t[2] = 2u*q+3u; } }
                if (control && (k & 1u)) continue;       /* the positive control: every other triangle missing */
                for (unsigned j = 0; j < 3; ++j) vidx[3u * nt + j] = idx[t[j]] - lo;
                ++nt;
            }
            {   size_t at = 0; d->vs_fmt_text[0] = 0;
                for (unsigned a = 0; a < 16 && at + 12 < sizeof d->vs_fmt_text; ++a)
                    if ((prog.inputs_read >> a) & 1u)
                        at += (size_t)snprintf(d->vs_fmt_text + at, sizeof d->vs_fmt_text - at, "%s%u:%02X", at ? " " : "", a,
                                               ((c->va_on >> a) & 1u) ? c->va_format[a] & 0xFFu : 0xFFu); }
            d->vs_in = (const float (*)[4])vin; d->vs_nin = range; d->vs_idx = vidx; d->vs_nidx = 3u * nt;
            d->nverts = 3u * nt; d->idx_min = lo; d->idx_max = hi;
            if (control) d->control_perturbed = 1;
            /* The constant file: what D3D was handed, and the viewport pair it
             * keeps itself (c-38 scale, c-37 offset) where it was not written. */
            if (!ffg) memcpy(d->vs_c, c->vc, sizeof d->vs_c);
            if (!ffg && !((c->vc_written[58u / 32u] >> (58u % 32u)) & 1u)) {
                float sx = (float)c->vp_w * c->ss_x * 0.5f, sy = (float)c->vp_h * c->ss_y * 0.5f;
                d->vs_c[58][0] = sx; d->vs_c[58][1] = -sy; d->vs_c[58][2] = 16777215.0f * (c->vp_maxz - c->vp_minz); d->vs_c[58][3] = 0.0f;
                d->vs_c[59][0] = (float)c->vp_x * c->ss_x + sx + D3D8H2D_SCREEN_OFFSET;
                d->vs_c[59][1] = (float)c->vp_y * c->ss_y + sy + D3D8H2D_SCREEN_OFFSET;
                d->vs_c[59][2] = 16777215.0f * c->vp_minz; d->vs_c[59][3] = 0.0f;
            }
            /* The whole target: the GPU decides where it lands. Never early. */
            d->bb_x0 = 0; d->bb_y0 = 0; d->bb_x1 = (int32_t)d->rt_w - 1; d->bb_y1 = (int32_t)d->rt_h - 1;
            d->z_min = -1.0f; d->z_max = 2.0f; d->w_min = -1.0f;
            return NULL;
        }
        /* FIXED-FUNCTION VERTICES ARE EVALUATED ONCE PER INDEX VALUE, not once
         * per index: an indexed mesh names each vertex about six times, and
         * nv2a_ff_vertex (fetch, transform, lighting, texgen) was the largest
         * cost of FF draw mode (18 fps against 83). A generation stamp per
         * index in the draw's range says whether this draw has evaluated it. */
        static D3D8H2DVertex *vc; static uint8_t *vstat; static const char **verr; static uint32_t *vstamp;
        static uint32_t vcap, vgen;
        uint32_t vbase = 0;
        if (cls == 2 && n) {
            uint32_t lo = UINT32_MAX, hi = 0, range;
            for (uint32_t k = 0; k < n; ++k) { if (idx[k] < lo) lo = idx[k]; if (idx[k] > hi) hi = idx[k]; }
            range = hi - lo + 1u; vbase = lo;
            if (range > 1u << 20) return "index range";
            if (range > vcap) {
                D3D8H2DVertex *a = realloc(vc, range * sizeof *a); uint8_t *b = a ? realloc(vstat, range) : NULL;
                const char **e = b ? realloc(verr, range * sizeof *e) : NULL; uint32_t *st4 = e ? realloc(vstamp, range * 4u) : NULL;
                if (a) vc = a;
                if (b) vstat = b;
                if (e) verr = e;
                if (!st4) return "out of memory";
                vstamp = st4; memset(vstamp + vcap, 0, (range - vcap) * 4u); vcap = range;
            }
            if (++vgen == 0) { memset(vstamp, 0, vcap * 4u); vgen = 1; }
        }
        uint32_t pl_segs = 0;
        if (pl) {                                  /* draw_points_lines' segment count */
            pl_segs = d->prim == 1u ? n : d->prim == 2u ? n / 2u : n >= 2u ? (d->prim == 4u ? n - 1u : n) : 0u;
            if (d->prim == 3u && n == 2u) pl_segs = 1u;   /* a two-vertex loop is one segment */
        }
        for (uint32_t k = 0; ; ++k) {
            /* Triangle k of the primitive, as the executor assembles it; for
             * points and lines, point or segment k as (A, B, B). */
            if (pl)                 { if (k >= pl_segs) break;
                                      if (d->prim == 1u) t3[0] = t3[1] = t3[2] = k;
                                      else { t3[0] = d->prim == 2u ? 2u*k : k;
                                             t3[1] = t3[2] = d->prim == 2u ? 2u*k+1u : (k + 1u < n ? k + 1u : 0u); } }
            else if (d->prim == 5u) { if (3u * k + 2u >= n) break; t3[0] = 3u*k; t3[1] = 3u*k+1u; t3[2] = 3u*k+2u; }
            else if (d->prim == 6u) { if (k + 2u >= n) break;
                                      if (k & 1u) { t3[0] = k + 1u; t3[1] = k; t3[2] = k + 2u; }
                                      else        { t3[0] = k; t3[1] = k + 1u; t3[2] = k + 2u; } }
            else if (d->prim == 7u) { if (k + 2u >= n) break; t3[0] = 0; t3[1] = k + 1u; t3[2] = k + 2u; }
            else if (d->prim == 8u) { uint32_t q = k / 2u; if (4u * q + 3u >= n) break;
                                      if (k & 1u) { t3[0] = 4u*q; t3[1] = 4u*q+2u; t3[2] = 4u*q+3u; }
                                      else        { t3[0] = 4u*q; t3[1] = 4u*q+1u; t3[2] = 4u*q+2u; } }
            else                    { uint32_t q = k / 2u; if (2u * q + 3u >= n) break;       /* quad strip */
                                      if (k & 1u) { t3[0] = 2u*q; t3[1] = 2u*q+3u; t3[2] = 2u*q+2u; }
                                      else        { t3[0] = 2u*q; t3[1] = 2u*q+1u; t3[2] = 2u*q+3u; } }
            if (d->nverts + (pl ? 6u : 3u) > D3D8H2D_MAX_VERTS) return "vertex count";
            D3D8H2DVertex *v = &d->verts[d->nverts];
            int ok = 1;
            for (unsigned j = 0; j < 3 && ok; ++j) {
                uint32_t i = idx[t3[j]];
                float pos[4];
                memset(&v[j], 0, sizeof v[j]);
                if (cls == 2) {
                    /* The executor's fixed-function vertex: every enabled
                     * array fetched over defaults (w = 1; diffuse white, which
                     * the executor's current slot 3 was measured to hold),
                     * then its own unit -- once per index value. */
                    uint32_t slot = i - vbase;
                    if ((d3d8_host_2d_bisect() & 32u) && vstamp[slot] == vgen) vstamp[slot] = vgen - 1u;
                    if (vstamp[slot] != vgen) {
                        float in[16][4], out[16][4];
                        D3D8H2DVertex *o = &vc[slot];
                        uint8_t stt = 0;
                        const char *why = NULL;
                        vstamp[slot] = vgen; ++d->ff_evals;
                        for (unsigned a = 0; a < 16; ++a) { in[a][0] = in[a][1] = in[a][2] = 0.0f; in[a][3] = 1.0f; }
                        in[3][0] = in[3][1] = in[3][2] = 1.0f;
                        for (unsigned a = 0; a < 16 && !why; ++a)
                            if (((c->va_on >> a) & 1u) && !fetch(vram, vram_size, c->va_offset[a], c->va_format[a], i, in[a]))
                                why = "vertex format";
                        if (!why) why = ffv(ffm, (const float (*)[4])in, out);
                        if (!why) {
                            o->p[0] = out[0][0]; o->p[1] = out[0][1]; o->p[2] = out[0][2] / 16777215.0f; o->p[3] = out[0][3];
                            memcpy(o->d0, out[3], 16); memcpy(o->d1, out[4], 16);
                            for (unsigned u = 0; u < 4; ++u) memcpy(o->t[u], out[9 + u], 16);
                            memcpy(o->f, out[5], 16);             /* G53: the fog coordinate */
                            for (unsigned q = 0; q < 4; ++q) if (!isfinite(o->p[q]) || !isfinite(o->d0[q]) || !isfinite(o->d1[q])) stt = 1;
                            for (unsigned u = 0; u < 4 && !stt; ++u)   /* nv2a_metal.m vertex_valid(): q > 0 per textured unit */
                                if ((d->tmask >> u) & 1u) {
                                    for (unsigned q = 0; q < 4; ++q) if (!isfinite(o->t[u][q])) stt = 2;
                                    if (!(o->t[u][3] > 0.0f)) stt = 2;
                                }
                        }
                        vstat[slot] = stt; verr[slot] = why;
                    }
                    if (verr[slot]) return verr[slot];
                    if (i < d->idx_min) d->idx_min = i; if (i > d->idx_max) d->idx_max = i;
                    v[j] = vc[slot];
                    if (vstat[slot] == 1) { ok = 0; break; }
                    /* A point sprite's coordinates are replaced (q = 1): the q test passes. */
                    if (vstat[slot] == 2 && ok == 1 && !d->point_sprite) { ++d->tris_dropped_q; ok = -1; }
                    continue;
                }
                if (!fetch(vram, vram_size, c->va_offset[0], c->va_format[0], i, pos)) return "vertex bounds";
                float rhw = pos[3];
                v[j].p[0] = d3d8_host_2d_snap(pos[0] * c->ss_x + D3D8H2D_SCREEN_OFFSET);
                v[j].p[1] = d3d8_host_2d_snap(pos[1] * c->ss_y + D3D8H2D_SCREEN_OFFSET); v[j].p[2] = pos[2];
                v[j].p[3] = 1.0f / rhw;                         /* inf for rhw 0: dropped below */
                if (i < d->idx_min) d->idx_min = i; if (i > d->idx_max) d->idx_max = i;
                if ((c->va_on >> 3) & 1u) { if (!fetch(vram, vram_size, c->va_offset[3], c->va_format[3], i, v[j].d0)) return "diffuse format"; }
                else { v[j].d0[0] = v[j].d0[1] = v[j].d0[2] = v[j].d0[3] = 1.0f; }
                if ((c->va_on >> 4) & 1u) { if (!fetch(vram, vram_size, c->va_offset[4], c->va_format[4], i, v[j].d1)) return "specular format"; }
                /* G53: D3D's vertex-fog pass-through: the coordinate is the specular alpha. */
                if (d->fog_enable) v[j].f[0] = v[j].d1[3] < 0.0f ? 0.0f : v[j].d1[3] > 1.0f ? 1.0f : v[j].d1[3];
                for (unsigned u = 0; u < 4; ++u) {
                    v[j].t[u][3] = 1.0f;
                    if ((c->va_on >> (9u + u)) & 1u &&
                        !fetch(vram, vram_size, c->va_offset[9 + u], c->va_format[9 + u], i, v[j].t[u])) return "texcoord format";
                }
                for (unsigned q = 0; q < 4; ++q) if (!isfinite(v[j].p[q]) || !isfinite(v[j].d0[q]) || !isfinite(v[j].d1[q])) ok = 0;
                if (!isfinite(v[j].p[3])) ok = 0;
            }
            if (ok == -1) continue;                                /* counted in tris_dropped_q */
            if (!ok) { ++d->tris_dropped_w; continue; }           /* the executor drops it too */
            if (pl) {
                /* nv2a_metal.m draw_points_lines, in its float arithmetic:
                 * corners 0..3 around the point, or A+n, A-n, B-n, B+n with n
                 * half a pixel along the segment's normal; triangles (0,1,2)
                 * and (0,2,3); corners 0 and 1 carry A, 2 and 3 carry B. */
                static const unsigned tri[6] = { 0, 1, 2, 0, 2, 3 };
                D3D8H2DVertex A = v[0], B = v[1];
                float cn[4][2];
                if (d->prim == 1u) {
                    float size = (float)(d->point_reg & 0x1FFu) / 8.0f, h = (size > 1.0f ? size : 1.0f) * 0.5f;
                    const float *p = A.p;
                    cn[0][0] = p[0]-h; cn[0][1] = p[1]-h; cn[1][0] = p[0]+h; cn[1][1] = p[1]-h;
                    cn[2][0] = p[0]+h; cn[2][1] = p[1]+h; cn[3][0] = p[0]-h; cn[3][1] = p[1]+h;
                } else {
                    float dx = B.p[0]-A.p[0], dy = B.p[1]-A.p[1], len = sqrtf(dx*dx+dy*dy), nx, ny;
                    if (!(len > 0.0f) || !isfinite(len)) continue;
                    nx = -dy/len*0.5f; ny = dx/len*0.5f;
                    cn[0][0] = A.p[0]+nx; cn[0][1] = A.p[1]+ny; cn[1][0] = A.p[0]-nx; cn[1][1] = A.p[1]-ny;
                    cn[2][0] = B.p[0]-nx; cn[2][1] = B.p[1]-ny; cn[3][0] = B.p[0]+nx; cn[3][1] = B.p[1]+ny;
                }
                for (unsigned m = 0; m < 6; ++m) {
                    unsigned q = tri[m];
                    v[m] = q < 2u ? A : B;
                    v[m].p[0] = cn[q][0]; v[m].p[1] = cn[q][1];
                    if (d->point_sprite)
                        for (unsigned u = 0; u < 4; ++u) {
                            v[m].t[u][0] = (q == 1u || q == 2u) ? 1.0f : 0.0f; v[m].t[u][1] = q >= 2u ? 1.0f : 0.0f;
                            v[m].t[u][2] = 0.0f; v[m].t[u][3] = 1.0f;
                        }
                }
                d->nverts += 6u; ntri += 2u; ++d->pl_segs;
                continue;
            }
            if (cull && culled(cull, front_cw, v[0].p, v[1].p, v[2].p)) continue;
            d->nverts += 3u; ++ntri;
        }
        (void)ntri;
    }
    if (control) {
        for (unsigned k = 0; k < d->nverts; ++k) { d->verts[k].p[0] += 2.0f; d->verts[k].d0[0] = 1.0f - d->verts[k].d0[0]; }
        d->control_perturbed = 1;
    }
    /* Bounding box: every pixel either renderer could reach, with a margin so
     * a half-pixel disagreement in placement lands inside the compared crop. */
    d->bb_x0 = 1; d->bb_x1 = 0;
    if (d->nverts) {
        float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
        d->z_min = INFINITY; d->z_max = -INFINITY; d->w_min = INFINITY;
        for (unsigned k = 0; k < d->nverts; ++k) {
            const float *p = d->verts[k].p;
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
            if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
            if (p[2] < d->z_min) d->z_min = p[2]; if (p[2] > d->z_max) d->z_max = p[2];
            if (!(p[3] >= d->w_min)) d->w_min = p[3];
        }
        int32_t bx0 = (int32_t)floorf(x0) - 2, by0 = (int32_t)floorf(y0) - 2;
        int32_t bx1 = (int32_t)ceilf(x1) + 2, by1 = (int32_t)ceilf(y1) + 2;
        d->bb_x0 = bx0 < d->sc_x0 ? d->sc_x0 : bx0; d->bb_y0 = by0 < d->sc_y0 ? d->sc_y0 : by0;
        d->bb_x1 = bx1 > d->sc_x1 ? d->sc_x1 : bx1; d->bb_y1 = by1 > d->sc_y1 ? d->sc_y1 : by1;
    }
    return NULL;
}

/* ---- the comparison ---- */
void d3d8_host_2d_diff(const uint16_t *pre, const uint16_t *exec, const uint16_t *host,
                       unsigned w, unsigned h, unsigned tol, D3D8H2DDiff *o)
{
    memset(o, 0, sizeof *o);
    for (size_t k = 0, n = (size_t)w * h; k < n; ++k) {
        uint16_t p = pre[k], e = exec[k], s = host[k];
        ++o->pixels;
        if (e != p) ++o->exec_changed;
        if (s != p) ++o->host_changed;
        if (e == s) continue;
        int er[3] = { (int)(e >> 11) - (int)(s >> 11), (int)((e >> 5) & 63) - (int)((s >> 5) & 63),
                      (int)(e & 31) - (int)(s & 31) };
        unsigned worst = 0;
        for (int ch = 0; ch < 3; ++ch) {
            unsigned a = (unsigned)abs(er[ch]);
            if (a > o->max_err[ch]) o->max_err[ch] = a;
            if (a > worst) worst = a;
        }
        if (worst > tol) ++o->mismatch;
    }
}

unsigned long long d3d8_host_2d_depth_diff(const float *exec, const float *host, size_t n,
                                           unsigned tol, unsigned *max_steps)
{
    unsigned long long bad = 0;
    unsigned worst = 0;
    for (size_t k = 0; k < n; ++k) {
        uint32_t a = (uint32_t)((double)fminf(1.0f, fmaxf(0.0f, exec[k])) * 16777215.0 + 0.5);
        uint32_t b = (uint32_t)((double)fminf(1.0f, fmaxf(0.0f, host[k])) * 16777215.0 + 0.5);
        unsigned dd = a > b ? a - b : b - a;
        if (dd > worst) worst = dd;
        if (dd > tol) ++bad;
    }
    if (max_steps) *max_steps = worst;
    return bad;
}

int d3d8_host_2d_depth_proof(uint32_t f, float zmin, float zmax, float smin, float smax)
{
    switch (f) {
    case 0x200: return -1;
    case 0x201: return zmax < smin ? 1 : zmin >= smax ? -1 : 0;     /* LESS */
    case 0x202: return (zmin == zmax && smin == smax && zmin == smin) ? 1 : (zmax < smin || zmin > smax) ? -1 : 0;
    case 0x203: return zmax <= smin ? 1 : zmin > smax ? -1 : 0;     /* LEQUAL */
    case 0x204: return zmin > smax ? 1 : zmax <= smin ? -1 : 0;     /* GREATER */
    case 0x205: return (zmax < smin || zmin > smax) ? 1 : (zmin == zmax && smin == smax && zmin == smin) ? -1 : 0;
    case 0x206: return zmin >= smax ? 1 : zmax < smin ? -1 : 0;     /* GEQUAL */
    default:    return 1;                                            /* ALWAYS */
    }
}

/* ---- the shadow bookkeeping ----
 * Everything below runs on the pusher thread: the pre and post tokens and
 * FLIP_STALL are all dispatched there, in ring order. The atexit report reads
 * the counters from another thread; a torn read of a counter is acceptable
 * there and nowhere else. */
static D3D8Host2DBackend s_be;
static int s_have_be;
static int s_mode = -1, s_control, s_every = 600;
static unsigned s_tol = 1, s_dump_max = 8, s_dumped;
static char s_dump_dir[512];

typedef struct {
    uint32_t serial, x0, y0, w, h, exec_active, exec_mode;
    uint16_t *pre, *exec, *host;
    float *zexec, *zhost;                    /* depth after the draw, both sides; NULL without the test */
    uint16_t idx6[6]; float pos3[3][4];      /* for the report: the indices and first vertices drawn */
    uint16_t idxs[48]; uint32_t nidx_rec, snap_n;   /* up to 32 first and 16 last indices; counts */
    D3D8Host2DDraw info;                     /* verts pointer cleared: summary only */
} Rec;
#define MAX_RECS 256u
static Rec s_rec[MAX_RECS];
static uint8_t s_rec_cls[MAX_RECS], s_rec_bad[MAX_RECS];
static unsigned s_nrec;
static unsigned long long s_rec_dropped;

static uint16_t *s_snap; static size_t s_snap_cap;
static uint32_t s_snap_serial, s_snap_addr, s_snap_pitch, s_snap_h, s_snap_valid;
/* The depth the draw starts from: rt_w x rt_h, rows rt_w apart. */
static float *s_zsnap, *s_zpost; static size_t s_zsnap_cap;
static uint32_t s_zsnap_valid, s_zsnap_addr, s_zsnap_w, s_zsnap_h, s_zsnap_src;   /* src 1 texture, 2 guest RAM */
/* The depth histogram: every 2D draw, by what it asks of the depth unit. */
static unsigned long long s_z_off, s_z_func[8], s_z_write, s_z_zbucket[5], s_z_from_tex, s_z_from_ram, s_z_none,
                          s_z_proof_pass, s_z_proof_reject, s_z_proof_depends, s_z_draws, s_z_px, s_z_px_mm,
                          s_z_draws_mm;
static unsigned s_z_max_steps;

static unsigned long long s_flips, s_draws, s_pre, s_pre_skipped, s_no_pre, s_no_backend, s_built, s_empty,
                          s_rendered, s_render_failed, s_compared, s_exact, s_within, s_mismatching,
                          s_px, s_px_exec, s_px_host, s_px_mm, s_2d_prog, s_2d_not_prog, s_exec_inactive,
                          s_vsflag_pass, s_idx_changed, s_sync_calls, s_ctx_b, s_diffuse_default,
                          s_tss_ci_off, s_modes_disagree;
static unsigned s_max_err[3];
static unsigned long long s_idx_snap, s_vtx_changed, s_exec_outside, s_pt_const_match, s_pt_const_differ;
static uint16_t s_last_idx[6], s_last_idxs[48];
static unsigned long long s_tris_dropped_w, s_draws_dropped_w;
static unsigned s_printed_mm, s_printed_consts, s_printed_frames, s_printed_z, s_printed_zmm, s_printed_pt;
#define NREASON 40
static struct { const char *why; unsigned long long n; } s_reason[NREASON];
/* G74: WHAT THE POINTS AND LINES ARE. Every draw-mode token with a point
 * or line primitive, by class, primitive, count, FVF, sprite, point size
 * (RS[106] and the device scale, and the SET_POINT_SIZE they give), the
 * bound stages and blend/depth, with how many the host drew. Printed with
 * the draw mode report; the table keeps the first 24 kinds. */
typedef struct {
    uint32_t cls, prim, count, fvf, sprite, scale_en, size_rs, dev_scale, reg, tmask, fmt0, fmt3, blend, zfunc;
    unsigned long long n, drawn;
} PlKind;
static PlKind s_pl_kind[24];
/* ... and their VERIFY verdicts: compared, exact, within tolerance,
 * mismatching, pixels either side changed; the first few are dumped
 * (RECOMP_D3D8_HOST_2D_DUMP) whether they match or not, for a zoomed look. */
static unsigned long long s_pl_cmp, s_pl_exact, s_pl_within, s_pl_mm, s_pl_exec_px, s_pl_host_px;
static unsigned s_pl_dumped;
/* G75: the same for BUMPENVMAP draws. */
static unsigned long long s_bump_cmp, s_bump_exact, s_bump_within, s_bump_mm, s_bump_exec_px, s_bump_host_px;
static unsigned s_bump_dumped;
static unsigned s_pl_nkind;
static unsigned long long s_pl_tokens, s_pl_drawn, s_pl_segs, s_pl_other;
static void pl_census(const D3D8HostDrawCheck *c, int cls, const D3D8Host2DDraw *d, int drawn)
{
    PlKind k;
    memset(&k, 0, sizeof k);
    k.cls = (uint32_t)cls; k.prim = c->prim; k.count = c->count > 64u ? 64u : c->count; k.fvf = c->vs_handle;
    if (c->pt_valid) {
        k.sprite = c->pt_rs[2]; k.scale_en = c->pt_rs[3]; k.size_rs = c->pt_rs[0]; k.dev_scale = c->pt_dev_scale;
        k.reg = d3d8_host_point_size_reg(c->pt_rs, c->pt_dev_scale);
    }
    for (unsigned u = 0; u < 4; ++u) if (c->tex[u]) k.tmask |= 1u << u;
    k.fmt0 = c->tex[0] ? (c->format[0] >> 8) & 0xFFu : 0xFFu; k.fmt3 = c->tex[3] ? (c->format[3] >> 8) & 0xFFu : 0xFFu;
    k.blend = st(c, 0x304, 0) ? (st(c, 0x344, 1) << 16 | st(c, 0x348, 0)) : 0u; k.zfunc = st(c, 0x30C, 0) ? st(c, 0x354, 0x203) : 0u;
    ++s_pl_tokens; if (drawn) { ++s_pl_drawn; s_pl_segs += d->pl_segs; }
    for (unsigned i = 0; i < s_pl_nkind; ++i) {
        PlKind *e = &s_pl_kind[i];
        if (e->cls == k.cls && e->prim == k.prim && e->count == k.count && e->fvf == k.fvf && e->sprite == k.sprite &&
            e->scale_en == k.scale_en && e->size_rs == k.size_rs && e->dev_scale == k.dev_scale && e->tmask == k.tmask &&
            e->fmt0 == k.fmt0 && e->fmt3 == k.fmt3 && e->blend == k.blend && e->zfunc == k.zfunc) {
            ++e->n; if (drawn) ++e->drawn; return;
        }
    }
    if (s_pl_nkind < 24u) { k.n = 1; k.drawn = drawn != 0; s_pl_kind[s_pl_nkind++] = k; }
    else ++s_pl_other;
}
static void count_reason(const char *why)
{
    for (unsigned i = 0; i < NREASON; ++i) {
        if (s_reason[i].why == why) { ++s_reason[i].n; return; }
        if (!s_reason[i].why) { s_reason[i].why = why; s_reason[i].n = 1; return; }
    }
}

static void h2d_exit(void) { d3d8_host_2d_report("exit"); }
static int s_exit_registered;

/* ---- G51.3: the fixed-function class's own verdicts and refusals ---- */
static int s_ffmode = -1;
static unsigned long long s_ff_draws, s_ff_built, s_ff_compared, s_ff_exact, s_ff_within, s_ff_mm, s_ff_px, s_ff_px_mm,
                          s_ff_exec_changed, s_ff_host_changed, s_ff_tris_q;
static unsigned s_ff_max_err[3], s_printed_vpoff, s_printed_cull, s_printed_diffuse, s_printed_stencil, s_printed_stencil_x;
static unsigned long long s_stencil_match, s_stencil_differ;
static unsigned long long s_tx_units, s_tx_min, s_tx_mag, s_tx_bias, s_tx_wrap, s_tx_levels;
static unsigned s_printed_tx;
static unsigned long long s_ff_diffuse_white, s_ff_diffuse_other, s_ff_prim[16];
/* Mismatching FF draws by size and kind, so a run classifies all of them. */
static unsigned long long s_ff_mm_size[4], s_ff_mm_cov_exec, s_ff_mm_cov_host, s_ff_mm_cov_same,
                          s_ff_depth_only, s_ff_z_worst[4];
static unsigned long long s_cull_match, s_cull_differ;
static unsigned long long s_ff_fixed, s_ff_not_fixed, s_ff_exec_inactive, s_ff_diffuse_default,
                          s_ff_composite_match, s_ff_composite_differ, s_ff_vpoff_match, s_ff_vpoff_differ;
static struct { const char *why; unsigned long long n; } s_ff_reason[NREASON];
static void count_reason_ff(const char *why)
{
    for (unsigned i = 0; i < NREASON; ++i) {
        if (s_ff_reason[i].why == why) { ++s_ff_reason[i].n; return; }
        if (!s_ff_reason[i].why) { s_ff_reason[i].why = why; s_ff_reason[i].n = 1; return; }
    }
}
static void read_knobs(void);
static unsigned s_ff_stride = 60;
static unsigned long long s_verified;
static unsigned long long s_ff_unsampled;
static int ff_sampled(void) { return s_ff_stride <= 1 || (s_flips % s_ff_stride) == 0; }
/* G51.2: RECOMP_D3D8_HOST_VS=shadow. Programmable-VS draws are shadowed on the
 * same stride as the FF shadow (RECOMP_D3D8_HOST_FF_STRIDE): each one renders
 * and reads back the whole target. */
static int s_vsmode = -1;
static unsigned long long s_vs_seen, s_vs_draws, s_vs_built, s_vs_compared, s_vs_exact, s_vs_within, s_vs_mm;
static unsigned long long s_vs_px, s_vs_px_mm, s_vs_exec_changed, s_vs_host_changed, s_vs_mm_cov_exec, s_vs_mm_cov_host;
static unsigned long long s_vs_cmp_adj[2], s_vs_mm_adj[2], s_vs_printed_mm;
static unsigned long long s_vs_const_match, s_vs_const_differ, s_vs_const_slot_differ[192];
static unsigned s_vs_max_err[3], s_vs_printed_const;
static unsigned long long s_vs_exec_prog, s_vs_exec_not_prog;
static unsigned long long s_vs_unbound_exec_on, s_vs_unbound_exec_off, s_vs_unbound_unknown; static unsigned s_printed_unbound;
static unsigned long long s_cls_seen[4];   /* every check the host saw, by class: 0 none, 1 2D, 2 FF, 3 programmable */
static uint32_t s_vs_prog_hash[128]; static unsigned s_vs_progs; static unsigned long long s_vs_prog_overflow;
/* THE CLASS SWITCHES, once. A new host class adds its row here and nowhere
 * else; d3d8_host_armed is what every gate asks. The value-carrying knobs
 * (RECOMP_D3D8_HOST_FF_STRIDE, _2D_TOL, _2D_CONTROL, _VERIFY, _BISECT,
 * _2D_DUMP, _2D_EVERY) only modify an armed class; set without one they are
 * reported, not silently ignored. */
int d3d8_host_vs_mode(void);
static const struct { const char *name; int (*mode)(void); } k_host_classes[] = {
    { "RECOMP_D3D8_HOST_2D", d3d8_host_2d_mode },
    { "RECOMP_D3D8_HOST_FF", d3d8_host_ff_mode },
    { "RECOMP_D3D8_HOST_VS", d3d8_host_vs_mode },
};
unsigned d3d8_host_class_switches(void) { return (unsigned)(sizeof k_host_classes / sizeof k_host_classes[0]); }
const char *d3d8_host_class_switch(unsigned i) { return i < d3d8_host_class_switches() ? k_host_classes[i].name : NULL; }
unsigned d3d8_host_armed(char *why, size_t why_size)
{
    unsigned mask = 0, i;
    size_t used = 0;
    if (why && why_size) why[0] = 0;
    for (i = 0; i < d3d8_host_class_switches(); ++i) {
        if (k_host_classes[i].mode() <= 0) continue;
        mask |= 1u << i;
        if (why && used < why_size) used += (size_t)snprintf(why + used, why_size - used, "%s%s", used ? " " : "", k_host_classes[i].name);
    }
    return mask;
}
int d3d8_host_mirror_armed(char *why, size_t why_size)
{
    static const char *const knobs[] = { "RECOMP_D3D8_HOST_FF_STRIDE", "RECOMP_D3D8_HOST_2D_TOL", "RECOMP_D3D8_HOST_2D_CONTROL",
                                         "RECOMP_D3D8_HOST_VERIFY", "RECOMP_D3D8_HOST_BISECT", "RECOMP_D3D8_HOST_2D_DUMP",
                                         "RECOMP_D3D8_HOST_2D_EVERY", "RECOMP_D3D8_HOST_2D_DUMP_MAX",
                                         "RECOMP_D3D8_HOST_POINTS", "RECOMP_D3D8_HOST_VERIFY_AFTER" };
    int m = recomp_switch_on("RECOMP_D3D8_MIRROR");
    unsigned armed = d3d8_host_armed(why, why_size);
    if (m && !armed && why && why_size) snprintf(why, why_size, "RECOMP_D3D8_MIRROR");
    if (!armed) {
        static int told;
        for (unsigned k = 0; k < sizeof knobs / sizeof knobs[0]; ++k) {
            const char *v = getenv(knobs[k]);
            if (v && v[0] && !told++)
                fprintf(stderr, "[D3D8-HOST] %s=%s is set but no host class is armed (%s, %s or %s): it does nothing\n",
                        knobs[k], v, k_host_classes[0].name, k_host_classes[1].name, k_host_classes[2].name);
        }
    }
    return m || armed != 0;
}

int d3d8_host_vs_mode(void)
{
    /* Read from the guest thread (the mirror) and the executor's: once, as
     * d3d8_host_ff_mode does it. */
    static atomic_flag s_init = ATOMIC_FLAG_INIT;
    static _Atomic int s_ready;
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return s_vsmode;
    if (atomic_flag_test_and_set(&s_init)) {
        while (!atomic_load_explicit(&s_ready, memory_order_acquire)) { }
        return s_vsmode;
    }
    {
        const char *e = getenv("RECOMP_D3D8_HOST_VS");
        s_vsmode = e && (!strcmp(e, "shadow") || !strcmp(e, "1")) ? 1 : e && !strcmp(e, "draw") ? 2 : 0;
        if (e && e[0] && !s_vsmode && strcmp(e, "0") && strcmp(e, "off"))
            fprintf(stderr, "[D3D8-HOST-VS] RECOMP_D3D8_HOST_VS=%s not understood; off (shadow|draw)\n", e);
        if (s_vsmode == 1) {
            /* The shadow's stride is the FF shadow's knob; it was read only
             * when RECOMP_D3D8_HOST_FF=shadow was set, so VS alone ran at 60
             * whatever was asked. */
            const char *v = getenv("RECOMP_D3D8_HOST_FF_STRIDE");
            if (v && *v && atoi(v) > 0) s_ff_stride = (unsigned)atoi(v);
            read_knobs();
            fprintf(stderr, "[D3D8-HOST-VS] RECOMP_D3D8_HOST_VS=shadow: draws through the title's own vertex programs are"
                            " drawn beside the executor from D3D state (program from the shader object, constants from"
                            " SetVertexShaderConstant and the viewport, the executor's VSH->MSL translation as the"
                            " transform) and compared, 1 flip in %u (RECOMP_D3D8_HOST_FF_STRIDE)%s\n", s_ff_stride,
                    s_control ? " -- POSITIVE CONTROL: every other triangle dropped; covered draws MUST mismatch" : "");
            if (!s_exit_registered) { s_exit_registered = 1; atexit(h2d_exit); }
        } else if (s_vsmode == 2) {
            read_knobs();
            fprintf(stderr, "[D3D8-HOST-VS] RECOMP_D3D8_HOST_VS=draw: draws through the title's own vertex programs that the"
                            " host can describe are drawn by the host into the executor's target (the executor's"
                            " VSH->MSL translation on D3D's program and constants), and the executor skips them%s\n",
                    s_control ? " -- POSITIVE CONTROL: every other triangle dropped; the characters on screen MUST look"
                                " wrong" : "");
            if (!s_exit_registered) { s_exit_registered = 1; atexit(h2d_exit); }
        }
    }
    atomic_store_explicit(&s_ready, 1, memory_order_release);
    return s_vsmode;
}
static void vs_note_program(const D3D8HostDrawCheck *c)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < c->vs_nwords; ++i) { h ^= c->vs_words[i]; h *= 16777619u; }
    for (unsigned i = 0; i < s_vs_progs; ++i) if (s_vs_prog_hash[i] == h) return;
    if (s_vs_progs < 128) s_vs_prog_hash[s_vs_progs++] = h; else ++s_vs_prog_overflow;
}
int d3d8_host_ff_mode(void)
{
    static atomic_flag s_init = ATOMIC_FLAG_INIT;
    static _Atomic int s_ready;
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return s_ffmode;
    if (atomic_flag_test_and_set(&s_init)) {
        while (!atomic_load_explicit(&s_ready, memory_order_acquire)) { }
        return s_ffmode;
    }
    {
        const char *e = getenv("RECOMP_D3D8_HOST_FF");
        s_ffmode = e && (!strcmp(e, "shadow") || !strcmp(e, "1")) ? 1 : e && !strcmp(e, "draw") ? 2 : 0;
        if (e && e[0] && !s_ffmode && strcmp(e, "0") && strcmp(e, "off"))
            fprintf(stderr, "[D3D8-HOST-FF] RECOMP_D3D8_HOST_FF=%s not understood; off (shadow|draw)\n", e);
        if (s_ffmode == 2) {
            read_knobs();
            fprintf(stderr, "[D3D8-HOST-FF] RECOMP_D3D8_HOST_FF=draw: fixed-function 3D draws the host can describe are"
                            " drawn by the host into the executor's target (its own vertex unit on D3D's registers), and"
                            " the executor skips them%s\n",
                    s_control ? " -- POSITIVE CONTROL: host geometry +2 px, diffuse red inverted; the 3D on screen MUST"
                                " look wrong" : "");
            if (!s_exit_registered) { s_exit_registered = 1; atexit(h2d_exit); }
        } else if (s_ffmode) {
            const char *v = getenv("RECOMP_D3D8_HOST_FF_STRIDE");
            if (v && *v && atoi(v) > 0) s_ff_stride = (unsigned)atoi(v);
            read_knobs();                                 /* the shared knobs (tolerance, dump, control) */
            fprintf(stderr, "[D3D8-HOST-FF] RECOMP_D3D8_HOST_FF=shadow: fixed-function 3D draws are drawn again by the"
                            " host from D3D state (G42/G42b/G43 registers, the executor's own vertex unit) and compared"
                            " with the executor, on every %u%s flip (RECOMP_D3D8_HOST_FF_STRIDE); reports every %d flips"
                            " and every 10 s; tolerance %u, dump %s%s\n", s_ff_stride, s_ff_stride == 1 ? "st" : "th",
                    s_every, s_tol, s_dump_dir[0] ? s_dump_dir : "off",
                    s_control ? " -- POSITIVE CONTROL: host geometry +2 px, diffuse red inverted; every covered FF draw"
                                " MUST mismatch" : "");
            if (!s_exit_registered) { s_exit_registered = 1; atexit(h2d_exit); }
        }
    }
    atomic_store_explicit(&s_ready, 1, memory_order_release);
    return s_ffmode;
}
int d3d8_host_shadow_wants_handle(uint32_t h)
{
    return (d3d8_host_2d_mode() == 1 && d3d8_host_2d_is_fvf_xyzrhw(h)) || (d3d8_host_ff_mode() == 1 && d3d8_host_2d_is_fvf_ff(h))
        || (d3d8_host_vs_mode() == 1 && (h & 1u));
}
int d3d8_host_shadow_wants(const D3D8HostDrawCheck *c)
{
    int cls = d3d8_host_2d_class(c);
    return (cls == 1 && d3d8_host_2d_mode() == 1) || (cls == 2 && d3d8_host_ff_mode() == 1)
        || (cls == 3 && d3d8_host_vs_mode() == 1);
}
int d3d8_host_replaces_handle(uint32_t h)
{
    return (d3d8_host_2d_mode() == 2 && d3d8_host_2d_is_fvf_xyzrhw(h)) || (d3d8_host_ff_mode() == 2 && d3d8_host_2d_is_fvf_ff(h))
        || (d3d8_host_vs_mode() == 2 && (h & 1u));
}
int d3d8_host_any_draw_mode(void) { return d3d8_host_2d_mode() == 2 || d3d8_host_ff_mode() == 2 || d3d8_host_vs_mode() == 2; }
static unsigned s_verify;
/* G74: RECOMP_D3D8_HOST_VERIFY_AFTER=<flips>: no verify flip before it. A
 * verify flip at the title slowed it enough that the stage harness's START
 * pulse went undelivered in six of six arms; the stage is what is verified. */
static unsigned long long s_verify_after;
/* The two agents added the same guard under two names; both are honoured. */
/* RECOMP_D3D8_HOST_VERIFY_FROM=<flip> (G73): no verify flip before this one.
 * A verify flip can stall for ~1.8 s, and at the title that loses the
 * harness's first input: map_run arms with VERIFY on missed the chapter
 * jump on 13 of 15 first attempts (every lift arm with the async write-back
 * and VERIFY, whose unpaced title reaches a verify flip every 0.4 s).
 * Starting after the title keeps VERIFY for the stage. */
static unsigned long long s_verify_from;
static int s_verify_pending; static uint32_t s_verify_serial;
int d3d8_host_verify_enabled(void) { return s_verify && d3d8_host_any_draw_mode(); }
void d3d8_host_2d_set_verify(unsigned every) { s_verify = every; }
int d3d8_host_2d_verify_take(uint32_t serial)
{
    int hit = s_verify_pending && s_verify_serial == serial;
    s_verify_pending = 0;
    return hit;
}

/* Read once. The mirror (guest thread) and the ring consumer (pusher
 * thread) can both ask first; the flag makes exactly one of them read the
 * environment, print and register the exit report, and the other spins the
 * few microseconds that takes. */
/* The knobs both shadows share -- tolerance, dump directory and cap, report
 * interval, and the positive control. Read once, by whichever mode arms
 * first: RECOMP_D3D8_HOST_FF alone used to leave them all at their defaults,
 * which is why an FF-only control run perturbed nothing and dumped nothing. */
static void read_knobs(void)
{
    static int done;
    const char *v;
    if (done) return;
    done = 1;
    if ((v = getenv("RECOMP_D3D8_HOST_2D_TOL")) && *v) s_tol = (unsigned)atoi(v);
    if ((v = getenv("RECOMP_D3D8_HOST_2D_DUMP_MAX")) && *v) s_dump_max = (unsigned)atoi(v);
    if ((v = getenv("RECOMP_D3D8_HOST_2D_EVERY")) && *v && atoi(v) > 0) s_every = atoi(v);
    if ((v = getenv("RECOMP_D3D8_HOST_VERIFY_FROM")) && *v) s_verify_from = strtoull(v, NULL, 10);
    if ((v = getenv("RECOMP_D3D8_HOST_VERIFY")) && *v && atoi(v) > 0) {
        s_verify = (unsigned)atoi(v);
        fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_VERIFY=%u: in draw mode, 1 flip in %u is drawn by the executor"
                        " and the host's pipeline is compared against it per draw (the shadow's verdict lines)\n",
                s_verify, s_verify);
        if ((v = getenv("RECOMP_D3D8_HOST_VERIFY_AFTER")) && *v && atoll(v) > 0) {
            s_verify_after = (unsigned long long)atoll(v);
            fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_VERIFY_AFTER=%llu: no verify flip before flip %llu\n",
                    s_verify_after, s_verify_after);
        }
    }
    if ((v = getenv("RECOMP_D3D8_HOST_2D_DUMP")) && *v) {
        snprintf(s_dump_dir, sizeof s_dump_dir, "%s", v);
        if (h2d_mkdir(s_dump_dir) != 0 && errno != EEXIST) {
            fprintf(stderr, "[D3D8-HOST-2D] cannot create dump dir %s: %s\n", s_dump_dir, strerror(errno));
            s_dump_dir[0] = 0;
        }
    }
    s_control = recomp_switch_on("RECOMP_D3D8_HOST_2D_CONTROL");
}

int d3d8_host_2d_mode(void)
{
    static atomic_flag s_init = ATOMIC_FLAG_INIT;
    static _Atomic int s_ready;
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return s_mode;
    if (atomic_flag_test_and_set(&s_init)) {
        while (!atomic_load_explicit(&s_ready, memory_order_acquire)) { }
        return s_mode;
    }
    {
        const char *e = getenv("RECOMP_D3D8_HOST_2D"), *v;
        s_mode = 0;
        if (e && (!strcmp(e, "shadow") || !strcmp(e, "1"))) s_mode = 1;
        else if (e && !strcmp(e, "draw")) s_mode = 2;
        else if (e && e[0] && strcmp(e, "0") && strcmp(e, "off"))
            fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_2D=%s not understood; off (shadow|draw)\n", e);
        (void)v;
        if (s_mode) {
            read_knobs();
            if (s_mode == 1)
                fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_2D=shadow: pre-transformed 2D draws are drawn again"
                                " by the host from D3D state and compared with the executor (tolerance %u step%s in 565,"
                                " dump %s, max %u)%s\n", s_tol, s_tol == 1 ? "" : "s",
                        s_dump_dir[0] ? s_dump_dir : "off", s_dump_max,
                        s_control ? " -- POSITIVE CONTROL: host geometry +2 px, diffuse red inverted; every covered draw MUST mismatch" : "");
            else
                fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_2D=draw: pre-transformed 2D draws the host can describe"
                                " are drawn by the host into the executor's target, and the executor skips them%s\n",
                        s_control ? " -- POSITIVE CONTROL: host geometry +2 px, diffuse red inverted; the 2D layer on"
                                    " screen MUST look wrong" : "");
            if (!s_exit_registered) { s_exit_registered = 1; atexit(h2d_exit); }
        }
    }
    atomic_store_explicit(&s_ready, 1, memory_order_release);
    return s_mode;
}

void d3d8_host_2d_set_backend(const D3D8Host2DBackend *b)
{
    s_be = *b; s_have_be = b && b->render && b->sync_range && b->ram && b->ram_size;
}

/* The executor's depth for a surface: its hardware attachment when it holds
 * one, else the guest's D24S8 bytes -- which is what it would upload on the
 * next rebuild of that surface, so either way it is what the draw meets. */
static int depth_read(uint32_t zaddr, uint32_t zpitch, uint32_t w, uint32_t h, float *out, uint32_t *src)
{
    if (s_be.depth_peek && s_be.depth_peek(s_be.ram + zaddr, w, h, out)) { *src = 1; return 1; }
    if ((uint64_t)zaddr + (uint64_t)zpitch * h > s_be.ram_size || zpitch < w * 4u) return 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *z = s_be.ram + zaddr + (size_t)y * zpitch + 4u * x;
            uint32_t q = (uint32_t)z[1] | (uint32_t)z[2] << 8 | (uint32_t)z[3] << 16;
            out[(size_t)y * w + x] = (float)q / 16777215.0f;
        }
    *src = 2;
    return 1;
}

void d3d8_host_2d_pre(uint32_t serial, uint32_t vs_handle, uint32_t rt_data, uint32_t rt_format, uint32_t rt_size,
                      uint32_t zs_data, uint32_t zs_size)
{
    uint32_t addr = rt_data & RAM_MASK, h = ((rt_size >> 12) & 0xFFFu) + 1u, pitch = ((rt_size >> 24) + 1u) * 64u;
    uint32_t w = (rt_size & 0xFFFu) + 1u;
    size_t bytes = (size_t)pitch * h;
    s_snap_valid = 0; s_zsnap_valid = 0;
    if (!d3d8_host_armed(NULL, 0)) return;
    /* this flip is not shadowed; in FF draw mode an FF pre token is a verify draw, always compared */
    if (d3d8_host_2d_is_fvf_ff(vs_handle) && d3d8_host_ff_mode() == 1 && !ff_sampled()) return;
    if ((vs_handle & 1u) && d3d8_host_vs_mode() == 1 && !ff_sampled()) return;
    ++s_pre;
    if (!s_have_be || ((rt_format >> 8) & 0xFFu) != 0x11u || addr + bytes > s_be.ram_size) { ++s_pre_skipped; return; }
    if (bytes > s_snap_cap) {
        uint16_t *n = realloc(s_snap, bytes);
        if (!n) { ++s_pre_skipped; return; }
        s_snap = n; s_snap_cap = bytes;
    }
    ++s_sync_calls;
    s_be.sync_range(s_be.ram + addr, bytes);
    memcpy(s_snap, s_be.ram + addr, bytes);
    s_snap_serial = serial; s_snap_addr = addr; s_snap_pitch = pitch; s_snap_h = h; s_snap_valid = 1;
    /* And the depth it starts from, whenever D3D has a depth surface: the
     * post token decides whether the draw tests against it. */
    if (zs_data) {
        size_t n = (size_t)w * h;
        if (n > s_zsnap_cap) {
            float *a = realloc(s_zsnap, n * sizeof *a), *b = a ? realloc(s_zpost, n * sizeof *b) : NULL;
            if (a) s_zsnap = a;
            if (b) s_zpost = b;
            if (!a || !b) return;
            s_zsnap_cap = n;
        }
        if (depth_read(zs_data & RAM_MASK, ((zs_size >> 24) + 1u) * 64u, w, h, s_zsnap, &s_zsnap_src)) {
            s_zsnap_valid = 1; s_zsnap_addr = zs_data & RAM_MASK; s_zsnap_w = w; s_zsnap_h = h;
        }
    }
}

static uint16_t *crop(const uint8_t *base, uint32_t pitch, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    uint16_t *o = malloc((size_t)w * h * 2u);
    if (!o) return NULL;
    for (uint32_t y = 0; y < h; ++y) memcpy(o + (size_t)y * w, base + (size_t)(y0 + y) * pitch + 2u * x0, 2u * w);
    return o;
}

static unsigned long long s_sp_match[4], s_sp_differ[4], s_sp_old_match, s_sp_old_differ, s_sp_printed;
static unsigned long long s_fc_match, s_fc_differ, s_fc_old_match, s_fc_old_differ, s_fc_printed;
void d3d8_host_2d_post(const D3D8HostDrawCheck *c, void (*exec_source)(D3D8ExecDrawTextures *))
{
    static D3D8ExecDrawTextures e;
    static D3D8H2DVertex *verts;
    static D3D8Host2DDraw d;
    const char *why;
    int cls = d3d8_host_2d_class(c);
    ++s_cls_seen[cls & 3];
    if (cls == 3 && d3d8_host_vs_mode() > 0) { ++s_vs_seen; vs_note_program(c); }
    if (!d3d8_host_shadow_wants(c) && !c->verify) return;
    if ((cls == 2 || cls == 3) && !c->verify && !ff_sampled()) { ++s_ff_unsampled; return; }
    if (cls == 3) ++s_vs_draws;
    if (c->verify) ++s_verified;
    ++s_draws;
    if (cls == 2) ++s_ff_draws;
    memset(&e, 0, sizeof e);
    if (exec_source) exec_source(&e);
    /* Cross-checks, from the executor's side, never used to draw: is the class
     * in the executor's PROGRAM mode (D3D's pass-through program), did it draw at all, and what did D3D program as
     * the pass-through's constants (c-38, c-37: slots 58, 59)? */
    if (exec_source && cls == 1) {
        if (NV2A_XF_IS_PROGRAM(e.exec_mode)) ++s_2d_prog; else ++s_2d_not_prog;
        if (!e.active) ++s_exec_inactive;
    }
    if (exec_source && cls == 3) {   /* the executor agrees this is a program: MODE PROGRAM */
        if (NV2A_XF_IS_PROGRAM(e.exec_mode)) ++s_vs_exec_prog; else ++s_vs_exec_not_prog;
    }
    if (exec_source && cls == 2) {
        if (NV2A_XF_IS_FIXED(e.exec_mode)) ++s_ff_fixed; else ++s_ff_not_fixed;
        if (!e.active) ++s_ff_exec_inactive;
    }
    /* G51.2: the texture shader stage modes the host derives
     * (d3d8_host_stage_program, D3D's 0x1952B0) against the executor's
     * latched 0x1E70; the rule it replaced (word 54 for a pixel shader,
     * PROJECT2D on every bound stage otherwise) is tallied beside it as the
     * negative control. */
    if (e.regs_valid && cls) {
        uint32_t ex = e.regs[0x1E70u / 4u] & 0xFFFFFu, mine = d3d8_host_stage_program(c) & 0xFFFFFu, old = 0;
        if (c->ffc_ps) old = c->ps_bound ? c->ps[54] & 0xFFFFFu : 0xFFFFFFFFu;
        else for (unsigned u = 0; u < 4; ++u) if (c->tex[u]) old |= 1u << (5u * u);
        if (mine == ex) ++s_sp_match[cls & 3];
        else {
            ++s_sp_differ[cls & 3];
            if (s_sp_printed++ < 8)
                fprintf(stderr, "[D3D8-HOST] draw %u class %d: stage modes derived %05X, executor 0x1E70 %05X"
                                " (pixel shader %08X, +0x378 %08X, +0x37C %08X, word 54 %08X, textures %X)\n",
                        c->serial, cls, mine, ex, c->ffc_ps, c->stage_prog_in[0], c->stage_prog_in[1], c->ps[54],
                        (c->tex[0] ? 1u : 0u) | (c->tex[1] ? 2u : 0u) | (c->tex[2] ? 4u : 0u) | (c->tex[3] ? 8u : 0u));
        }
        if (old == ex) ++s_sp_old_match; else ++s_sp_old_differ;
    }
    /* ... and the final combiner words D3D leaves in 0x288/0x28C, derived the
     * same way the host builds them, against the executor's latched pair; the
     * pixel shader's words 8/9 as they stand are the control. */
    if (e.regs_valid && cls && c->ffc_ps && c->ps_bound) {
        uint32_t w0 = c->ps[8], w1 = c->ps[9];
        if (!(c->fog_cur[2] && c->fog_cur[3])) (void)d3d8_ff_final_combiner(c->fog_cur[0], c->fog_cur[1], c->fog_cur[2], c->fog_cur[3], &w0, &w1);
        if (w0 == e.regs[0x288u / 4u] && w1 == e.regs[0x28Cu / 4u]) ++s_fc_match; else {
            ++s_fc_differ;
            if (s_fc_printed++ < 6)
                fprintf(stderr, "[D3D8-HOST] draw %u: final combiner derived %08X/%08X, executor %08X/%08X (words 8/9 %08X/%08X,"
                                " fog inputs %X %X %08X %08X)\n", c->serial, w0, w1, e.regs[0x288u / 4u], e.regs[0x28Cu / 4u],
                        c->ps[8], c->ps[9], c->fog_cur[0], c->fog_cur[1], c->fog_cur[2], c->fog_cur[3]);
        }
        if (c->ps[8] == e.regs[0x288u / 4u] && c->ps[9] == e.regs[0x28Cu / 4u]) ++s_fc_old_match; else ++s_fc_old_differ;
    }
    if (cls == 1 && c->ffv_valid && (c->ffv_vs_flags & 0x2u)) ++s_vsflag_pass;
    for (unsigned u = 0; u < 4; ++u) if (c->tex[u] && (c->format[u] & 3u) == 2u) { ++s_ctx_b; break; }
    for (unsigned u = 0; u < 4; ++u) if (c->tex[u] && c->tss[u][28] != u) { ++s_tss_ci_off; break; }
    if (cls == 1 && !((c->va_on >> 3) & 1u)) ++s_diffuse_default;
    if (cls == 2 && !((c->va_on >> 3) & 1u)) {
        /* The host reads white; the executor reads its current slot 3. */
        ++s_ff_diffuse_default;
        if (e.cur_valid) {
            if (e.cur[3][0] == 1.0f && e.cur[3][1] == 1.0f && e.cur[3][2] == 1.0f && e.cur[3][3] == 1.0f) ++s_ff_diffuse_white;
            else {
                ++s_ff_diffuse_other;
                if (s_printed_diffuse++ < 4)
                    fprintf(stderr, "[D3D8-HOST-FF] draw %u fvf %03X has no diffuse array; executor's current diffuse is"
                                    " %g %g %g %g, the host assumes white\n", c->serial, c->vs_handle,
                            e.cur[3][0], e.cur[3][1], e.cur[3][2], e.cur[3][3]);
            }
        }
    }
    if (cls == 2 && e.regs_valid && s_printed_stencil < 6 &&
        (st(c, 0x32C, 0) || (c->prim < 5u || c->prim > 9u))) {
        /* What the draws the host refuses are: stencil (shadow volumes?) and odd primitives. */
        const uint32_t *R = e.regs;
        ++s_printed_stencil;
        fprintf(stderr, "[D3D8-HOST-FF] draw %u refused-class detail: fvf %03X prim %u count %u | stencil test %u func %X ref %u"
                        " mask %X/%X ops fail %X zfail %X zpass %X | colour mask %08X depth test %u func %X write %u |"
                        " cull %u face %X front %X | blend %u %X/%X\n", c->serial, c->vs_handle, c->prim, c->count,
                R[0x32Cu / 4u], R[0x364u / 4u], R[0x368u / 4u], R[0x36Cu / 4u], R[0x360u / 4u], R[0x370u / 4u],
                R[0x374u / 4u], R[0x378u / 4u], R[0x358u / 4u], R[0x30Cu / 4u], R[0x354u / 4u], R[0x35Cu / 4u],
                R[0x308u / 4u], R[0x39Cu / 4u], R[0x3A0u / 4u], R[0x304u / 4u], R[0x344u / 4u], R[0x348u / 4u]);
    }
    if (cls == 2 && (c->prim < 5u || c->prim > 9u)) ++s_ff_prim[c->prim & 15u];
    /* SAMPLING AS THE HOST WOULD DO IT, against the executor's texture
     * registers: TEXTURE_FILTER (min 16..19, mag 24..27, LOD bias 0..12),
     * TEXTURE_ADDRESS (U 0..3, V 8..11), the level count. Distant, alpha-tested
     * geometry is where a sampling difference shows, as holes. */
    if (cls == 2 && e.regs_valid) {
        static D3D8Host2DDraw td;
        memset(&td, 0, sizeof td);
        for (unsigned u = 0; u < 4; ++u) {
            D3D8H2DTexture t;
            uint32_t f = e.tex_filter[u], a = e.tex_address[u];
            if (!c->tex[u] || !(e.mask & (1u << u)) || texture_from_d3d(c, u, &t)) continue;
            unsigned emin = (f >> 16) & 0xFu, emag = (f >> 24) & 0xFu, eu = a & 0xFu, ev = (a >> 8) & 0xFu;
            int32_t eb = (int32_t)(f << 19) >> 19;
            int bad = 0;
            ++s_tx_units;
            if (emin != t.min_filter) { ++s_tx_min; bad = 1; }
            if (emag != t.mag) { ++s_tx_mag; bad = 1; }
            if ((float)eb / 256.0f != t.lod_bias) { ++s_tx_bias; bad = 1; }
            if (eu != t.wrap_u || ev != t.wrap_v) { ++s_tx_wrap; bad = 1; }
            if (e.levels[u] != t.levels) { ++s_tx_levels; bad = 1; }
            if (bad && s_printed_tx++ < 8)
                fprintf(stderr, "[D3D8-HOST-FF] draw %u unit %u sampling: host min %u mag %u bias %g wrap %u/%u levels %u |"
                                " executor min %u mag %u bias %g wrap %u/%u levels %u (filter %08X address %08X control0 %08X)\n",
                        c->serial, u, t.min_filter, t.mag, t.lod_bias, t.wrap_u, t.wrap_v, t.levels, emin, emag,
                        (float)eb / 256.0f, eu, ev, e.levels[u], f, a, e.tex_control0[u]);
        }
    }
    if (e.regs_valid && st(c, 0x32C, 0)) {                 /* stencil as the host would draw it, against the executor */
        static D3D8Host2DDraw sd;
        const uint32_t *R = e.regs;
        const char *sw;
        memset(&sd, 0, sizeof sd);
        sd.depth_test = st(c, 0x30C, 0) != 0; sd.stencil_test = 1;
        if ((sw = stencil_from_d3d(c, &sd))) { if (s_printed_stencil_x < 4 && ++s_printed_stencil_x)
                fprintf(stderr, "[D3D8-HOST] draw %u stencil refused (%s): executor func %X ops %X/%X/%X mask %X ref %X control0 %X\n",
                        c->serial, sw, R[0x364u / 4u], R[0x370u / 4u], R[0x374u / 4u], R[0x378u / 4u], R[0x360u / 4u],
                        R[0x368u / 4u], R[0x290u / 4u]);
        } else {
            int same = R[0x32Cu / 4u] == 1u && R[0x364u / 4u] == sd.stencil_func && R[0x378u / 4u] == sd.stencil_zpass &&
                       (!sd.depth_test || R[0x374u / 4u] == sd.stencil_zfail) && (R[0x360u / 4u] & 0xFFu) == (sd.stencil_mask & 0xFFu) &&
                       (R[0x290u / 4u] & 1u) == sd.stencil_write && (R[0x368u / 4u] & 0xFFu) == (sd.stencil_ref & 0xFFu);
            if (same) ++s_stencil_match;
            else {
                ++s_stencil_differ;
                if (s_printed_stencil_x++ < 8)
                    fprintf(stderr, "[D3D8-HOST] draw %u stencil: D3D func %X zfail %X zpass %X mask %X ref %X write %u |"
                                    " executor enable %X func %X zfail %X zpass %X mask %X ref %X control0 %X\n", c->serial,
                            sd.stencil_func, sd.stencil_zfail, sd.stencil_zpass, sd.stencil_mask, sd.stencil_ref, sd.stencil_write,
                            R[0x32Cu / 4u], R[0x364u / 4u], R[0x374u / 4u], R[0x378u / 4u], R[0x360u / 4u], R[0x368u / 4u],
                            R[0x290u / 4u]);
            }
        }
    }
    if (c->rs_valid && e.regs_valid) {                      /* D3D's cull state against the executor's registers */
        uint32_t en = c->rs_cull != 0, face = 0x404u + (c->rs_cull != c->rs_front);
        if (e.regs[0x308u / 4u] != en || (en && e.regs[0x39Cu / 4u] != face) || e.regs[0x3A0u / 4u] != c->rs_front) {
            ++s_cull_differ;
            if (s_printed_cull++ < 4)
                fprintf(stderr, "[D3D8-HOST] draw %u cull: D3D CULLMODE %X FRONTFACE %X | executor enable %u face %X front %X\n",
                        c->serial, c->rs_cull, c->rs_front, e.regs[0x308u / 4u], e.regs[0x39Cu / 4u], e.regs[0x3A0u / 4u]);
        } else ++s_cull_match;
    }
    /* The pass-through's constants as the executor holds them: the viewport
     * pair c-38 (scale: W/2, -H/2, 16777215) and c-37 (offset: W/2 + b,
     * H/2 + b), NV2A slots 58 and 59. Measured in tutorial run 4: c0/c1
     * (slots 96, 97) are zero on these draws, so the bias b is read here as
     * c-37.x - c-38.x and c-37.y + c-38.y, and must be the host's 0.53125,
     * with the z scale 16777215. */
    if (cls == 1 && e.regs_valid) {
        float bx = e.vc[59][0] - e.vc[58][0], by = e.vc[59][1] + e.vc[58][1];
        if (bx != D3D8H2D_SCREEN_OFFSET || by != D3D8H2D_SCREEN_OFFSET || e.vc[58][2] != 16777215.0f) {
            ++s_pt_const_differ;
            if (s_printed_pt++ < 4)
                fprintf(stderr, "[D3D8-HOST-2D] draw %u pass-through bias %g,%g z scale %g -- NOT the host's %g\n",
                        c->serial, bx, by, e.vc[58][2], D3D8H2D_SCREEN_OFFSET);
        } else ++s_pt_const_match;
    }
    if (cls == 1 && s_printed_consts < 4 && e.regs_valid) {
        ++s_printed_consts;
        fprintf(stderr, "[D3D8-HOST-2D] draw %u fvf=%03X exec MODE %u RANGE %u prog_start %u: constants"
                        " c0 = %g %g %g %g, c1 = %g %g %g %g, c-38 = %g %g %g %g, c-37 = %g %g %g %g | host scale"
                        " ss=%g,%g offset %g,%g then 1/16 truncation, z as given\n",
                c->serial, c->vs_handle, NV2A_XF_MODE(e.exec_mode), NV2A_XF_RANGE(e.exec_mode), e.prog_start,
                e.vc[96][0], e.vc[96][1], e.vc[96][2], e.vc[96][3], e.vc[97][0], e.vc[97][1], e.vc[97][2], e.vc[97][3],
                e.vc[58][0], e.vc[58][1], e.vc[58][2], e.vc[58][3], e.vc[59][0], e.vc[59][1], e.vc[59][2], e.vc[59][3],
                c->ss_x, c->ss_y, D3D8H2D_SCREEN_OFFSET, D3D8H2D_SCREEN_OFFSET);
    }
    if (!s_have_be) { ++s_no_backend; return; }
    if (!s_snap_valid || s_snap_serial != c->serial) { ++s_no_pre; return; }
    s_snap_valid = 0;
    if (!verts && !(verts = malloc(sizeof *verts * D3D8H2D_MAX_VERTS))) { count_reason("out of memory"); return; }
    d.verts = verts;
    /* The indices the draw CALL saw (see D3D8HostDrawCheck.idx_snap_*), which
     * are the ones D3D copied into the ring and the executor drew. */
    {
        static uint16_t idx[D3D8H2D_IDX_PER_DRAW];
        const uint16_t *use = NULL;
        if (c->draw_kind == 2) {
            if (c->idx_snap_over) { count_reason("more indices than the snapshot takes"); return; }
            if (c->count && (c->idx_snap_n != c->count || !d3d8_host_2d_idx_copy(c->idx_snap_pos, c->count, idx))) {
                count_reason("index snapshot missing or overwritten"); return;
            }
            use = idx; ++s_idx_snap;
            for (uint32_t k = 0; k < c->count; ++k) {            /* and what pIndexData holds now */
                uint64_t at = (uint64_t)(c->idx_ptr & RAM_MASK) + 2u * k;
                if (at + 2u > s_be.ram_size || (uint16_t)(s_be.ram[at] | s_be.ram[at + 1] << 8) != idx[k]) { ++s_idx_changed; break; }
            }
        }
        if (cls == 2) {
            /* G51.3: the vertex unit's registers from D3D, evaluated by the
             * executor's own nv2a_ff_vertex (handed in by the backend). */
            static uint32_t ffm[2048];
            if (!s_be.ff_vertex) { count_reason_ff("no fixed-function evaluator in the backend"); return; }
            /* Cross-check, never used to draw: the host's COMPOSITE and
             * VIEWPORT_OFFSET against the executor's latched registers. */
            if (!d3d8_host_ff_registers(c, ffm) && e.regs_valid) {
                double worst = 0, scale = 1e-6;
                for (unsigned k = 0; k < 16; ++k) { float a; memcpy(&a, &ffm[0x680u / 4u + k], 4); if (fabs(a) > scale) scale = fabs(a); }
                for (unsigned k = 0; k < 16; ++k) {
                    float a, b; memcpy(&a, &ffm[0x680u / 4u + k], 4); memcpy(&b, &e.regs[0x680u / 4u + k], 4);
                    if (fabs((double)a - b) / scale > worst) worst = fabs((double)a - b) / scale;
                }
                if (worst > 1e-3) ++s_ff_composite_differ; else ++s_ff_composite_match;
                if (ffm[0xA20u / 4u] != e.regs[0xA20u / 4u] || ffm[0xA24u / 4u] != e.regs[0xA24u / 4u]) {
                    ++s_ff_vpoff_differ;
                    if (s_printed_vpoff++ < 4) {
                        float x, y; memcpy(&x, &e.regs[0xA20u / 4u], 4); memcpy(&y, &e.regs[0xA24u / 4u], 4);
                        fprintf(stderr, "[D3D8-HOST-FF] draw %u VIEWPORT_OFFSET: executor %g,%g, host 0.53125,0.53125\n", c->serial, x, y);
                    }
                } else ++s_ff_vpoff_match;
            }
            if ((why = d3d8_host_ff_registers(c, ffm)) ||
                (why = d3d8_host_draw_build(c, s_be.ram, s_be.ram_size, s_control, use, ffm, s_be.ff_vertex, &d))) {
                count_reason_ff(why); return;
            }
            ++s_ff_built; s_ff_tris_q += d.tris_dropped_q;
        } else if ((why = d3d8_host_draw_build(c, s_be.ram, s_be.ram_size, s_control, use, NULL, NULL, &d))) {
            /* G51.2: the programmable class's largest refusal. The pixel
             * shader's texture mode word selects PROGRAM_2D on a stage D3D has
             * no texture on; what does the executor have in that unit? */
            if (cls == 3 && !strcmp(why, "shader samples an unbound stage") && c->ps_bound) {
                for (unsigned u = 0; u < 4; ++u) {
                    if (((c->ps[54] >> (5u * u)) & 31u) != 1u || c->tex[u]) continue;
                    if (!exec_source) ++s_vs_unbound_unknown;
                    else if (e.mask & (1u << u)) ++s_vs_unbound_exec_on; else ++s_vs_unbound_exec_off;
                    if (s_printed_unbound++ < 4)
                        fprintf(stderr, "[D3D8-HOST-VS] draw %u: pixel shader samples stage %u, D3D has no texture there;"
                                        " the executor's unit %u is %s (texture mask %X, address %08X)\n", c->serial, u, u,
                                exec_source ? (e.mask & (1u << u)) ? "ENABLED" : "disabled" : "unknown", e.mask, e.addr[u]);
                    break;
                }
            }
            count_reason(why); return;
        }
        if (cls == 3) {
            ++s_vs_built;
            /* The constant file the host derived, against the executor's --
             * every slot D3D wrote, and the viewport pair. Never used to draw. */
            if (e.regs_valid) {
                unsigned bad = 0, first = 999;
                for (unsigned k = 0; k < 192; ++k) {
                    int want = ((c->vc_written[k / 32u] >> (k % 32u)) & 1u) || k == 58u || k == 59u;
                    if (!want) continue;
                    if (memcmp(d.vs_c[k], e.vc[k], 16)) { ++bad; ++s_vs_const_slot_differ[k]; if (first == 999) first = k; }
                }
                if (bad) {
                    ++s_vs_const_differ;
                    if (s_vs_printed_const++ < 6)
                        fprintf(stderr, "[D3D8-HOST-VS] draw %u: %u constant slot(s) differ from the executor's; first %u:"
                                        " host %g %g %g %g, executor %g %g %g %g\n", c->serial, bad, first,
                                d.vs_c[first][0], d.vs_c[first][1], d.vs_c[first][2], d.vs_c[first][3],
                                e.vc[first][0], e.vc[first][1], e.vc[first][2], e.vc[first][3]);
                } else ++s_vs_const_match;
            }
        }
        /* Vertices: did the bytes the draw reaches change between the call and now? */
        if (c->vtx_hash_ok) {
            uint32_t imin = UINT32_MAX, imax = 0;
            for (uint32_t k = 0; k < c->count; ++k) {
                uint32_t i = use ? use[k] : c->start + k;
                if (i < imin) imin = i; if (i > imax) imax = i;
            }
            if (c->count && d3d8_host_2d_vertex_hash(s_be.ram, s_be.ram_size, c, imin, imax) != c->vtx_hash) ++s_vtx_changed;
        }
        for (unsigned k = 0; k < 6; ++k) s_last_idx[k] = (uint16_t)(k >= c->count ? 0 : use ? use[k] : c->start + k);
        for (unsigned k = 0; k < 48; ++k) {                 /* the first 32 and the last 16 */
            uint32_t at = k < 32 ? k : (c->count > 16 ? c->count - 48 + k : k);
            s_last_idxs[k] = (uint16_t)(at >= c->count ? 0 : use ? use[at] : c->start + at);
        }
        if (d.tris_dropped_w) { ++s_draws_dropped_w; s_tris_dropped_w += d.tris_dropped_w; }
    }
    ++s_built;
    /* The texture shader stage modes the host implies (PROJECT2D per sampled
     * stage) against the executor's 0x1E70. */
    if (e.regs_valid) {
        uint32_t want = 0;
        for (unsigned u = 0; u < 4; ++u) if (d.tmask & (1u << u)) want |= 1u << (5u * u);
        if (want != (e.regs[0x1E70u / 4u] & 0xFFFFFu)) ++s_modes_disagree;
    }
    if (d.bb_x1 < d.bb_x0 || d.bb_y1 < d.bb_y0 || d.rt_addr != s_snap_addr || d.rt_pitch != s_snap_pitch) {
        if (d.rt_addr != s_snap_addr || d.rt_pitch != s_snap_pitch) count_reason("target moved between the tokens");
        else ++s_empty;
        return;
    }
    /* The depth histogram, over every built 2D draw: what the depth unit is
     * asked, where the vertex z sit, and whether the stored depth under the
     * draw's box PROVES the outcome (see d3d8_host_2d_depth_proof). The proof
     * is reported, never used: the host draws against the real depth. */
    if (!d.depth_test) ++s_z_off;
    else {
        ++s_z_func[d.depth_func & 7u];
        if (d.depth_write) ++s_z_write;
        ++s_z_zbucket[d.z_max < 0.0f || d.z_min > 1.0f ? 4 : d.z_max == 0.0f ? 0 : d.z_min == 1.0f ? 2
                      : (d.z_min >= 0.0f && d.z_max <= 1.0f) ? 1 : 3];
        if (!s_zsnap_valid || s_zsnap_addr != d.zs_addr || s_zsnap_w < d.rt_w || s_zsnap_h < d.rt_h) {
            ++s_z_none; count_reason("no executor depth for the depth-tested draw"); return;
        }
        if (s_zsnap_src == 1) ++s_z_from_tex; else ++s_z_from_ram;
        {
            float smin = INFINITY, smax = -INFINITY;
            for (int32_t y = d.bb_y0; y <= d.bb_y1; ++y)
                for (int32_t x = d.bb_x0; x <= d.bb_x1; ++x) {
                    float v = s_zsnap[(size_t)y * s_zsnap_w + (size_t)x];
                    if (v < smin) smin = v; if (v > smax) smax = v;
                }
            int pr = d3d8_host_2d_depth_proof(d.depth_func, d.z_min, d.z_max, smin, smax);
            if (pr > 0) ++s_z_proof_pass; else if (pr < 0) ++s_z_proof_reject; else ++s_z_proof_depends;
            if (s_printed_z < 6) {
                ++s_printed_z;
                fprintf(stderr, "[D3D8-HOST-2D] draw %u depth: func %X write %u vertex z %.7g..%.7g stored %.7g..%.7g"
                                " (%s) -> %s\n", c->serial, d.depth_func, d.depth_write, d.z_min, d.z_max, smin, smax,
                        s_zsnap_src == 1 ? "executor texture" : "guest RAM",
                        pr > 0 ? "every fragment passes" : pr < 0 ? "every fragment fails" : "depends per pixel");
            }
        }
    }
    if (s_nrec >= MAX_RECS) { ++s_rec_dropped; return; }
    ++s_sync_calls;
    s_be.sync_range(s_be.ram + d.rt_addr, (size_t)d.rt_pitch * d.rt_h);
    /* The compared box is the host's box UNION every pixel the executor
     * changed. A box from the host's geometry alone cannot see the host
     * drawing in the wrong place: the executor's pixels outside it would
     * simply never be looked at. */
    {
        int32_t ex0 = INT32_MAX, ey0 = INT32_MAX, ex1 = -1, ey1 = -1;
        for (uint32_t y = 0; y < d.rt_h; ++y) {
            const uint16_t *a = (const uint16_t *)((const uint8_t *)s_snap + (size_t)y * s_snap_pitch);
            const uint16_t *b = (const uint16_t *)(s_be.ram + d.rt_addr + (size_t)y * d.rt_pitch);
            if (!memcmp(a, b, (size_t)d.rt_w * 2u)) continue;
            for (uint32_t x = 0; x < d.rt_w; ++x)
                if (a[x] != b[x]) {
                    if ((int32_t)x < ex0) ex0 = (int32_t)x; if ((int32_t)x > ex1) ex1 = (int32_t)x;
                    if ((int32_t)y < ey0) ey0 = (int32_t)y; ey1 = (int32_t)y;
                }
        }
        if (ex1 >= 0) {
            if (ex0 < d.bb_x0 || ey0 < d.bb_y0 || ex1 > d.bb_x1 || ey1 > d.bb_y1) ++s_exec_outside;
            if (ex0 < d.bb_x0) d.bb_x0 = ex0; if (ey0 < d.bb_y0) d.bb_y0 = ey0;
            if (ex1 > d.bb_x1) d.bb_x1 = ex1; if (ey1 > d.bb_y1) d.bb_y1 = ey1;
        }
    }
    {
        Rec *r = &s_rec[s_nrec];
        uint32_t x0 = (uint32_t)d.bb_x0, y0 = (uint32_t)d.bb_y0, w = (uint32_t)(d.bb_x1 - d.bb_x0 + 1), h = (uint32_t)(d.bb_y1 - d.bb_y0 + 1);
        memset(r, 0, sizeof *r);
        r->serial = c->serial; r->x0 = x0; r->y0 = y0; r->w = w; r->h = h;
        r->exec_active = exec_source ? (uint32_t)e.active : 1u; r->exec_mode = e.exec_mode;
        r->pre = crop((const uint8_t *)s_snap, s_snap_pitch, x0, y0, w, h);
        r->exec = crop(s_be.ram + d.rt_addr, d.rt_pitch, x0, y0, w, h);
        r->host = r->pre ? malloc((size_t)w * h * 2u) : NULL;
        if (!r->pre || !r->exec || !r->host) { free(r->pre); free(r->exec); free(r->host); count_reason("out of memory"); return; }
        memcpy(r->host, r->pre, (size_t)w * h * 2u);
        if (d.depth_test) {
            /* The host's depth starts from the pre-draw snapshot; the
             * executor's after the draw is read now, the same way. */
            uint32_t src;
            r->zhost = malloc((size_t)w * h * sizeof(float)); r->zexec = malloc((size_t)w * h * sizeof(float));
            if (!r->zhost || !r->zexec || !depth_read(d.zs_addr, d.zs_pitch, d.rt_w, d.rt_h, s_zpost, &src)) {
                free(r->pre); free(r->exec); free(r->host); free(r->zhost); free(r->zexec);
                count_reason("executor depth after the draw unreadable"); return;
            }
            for (uint32_t y = 0; y < h; ++y) {
                memcpy(r->zhost + (size_t)y * w, s_zsnap + (size_t)(y0 + y) * s_zsnap_w + x0, w * sizeof(float));
                memcpy(r->zexec + (size_t)y * w, s_zpost + (size_t)(y0 + y) * d.rt_w + x0, w * sizeof(float));
            }
        }
        if (s_be.render(&d, s_be.ram, s_be.ram_size, r->host, w, r->zhost, x0, y0, w, h) != 0) {
            ++s_render_failed;
            count_reason(s_be.last_error ? s_be.last_error() : "render failed");
            free(r->pre); free(r->exec); free(r->host); free(r->zhost); free(r->zexec);
            return;
        }
        ++s_rendered;
        r->info = d; r->info.verts = NULL;
        memcpy(r->idx6, s_last_idx, sizeof r->idx6);
        memcpy(r->idxs, s_last_idxs, sizeof r->idxs); r->nidx_rec = c->count; r->snap_n = c->idx_snap_n;
        for (unsigned k = 0; k < 3 && k < d.nverts; ++k) memcpy(r->pos3[k], d.verts[k].p, sizeof r->pos3[k]);
        ++s_nrec;
    }
}

/* ---- draw mode ---- */
static unsigned long long s_rep_tokens, s_replaced, s_rep_refused, s_rep_unbound, s_rep_noskip, s_replaced_2d, s_replaced_ff;
static unsigned long long s_replaced_vs, s_rep_vs_tokens, s_rep_vs_refused;
static unsigned long long s_replaced_empty, s_replaced_stencil;
static uint32_t s_skip_serial; static int s_skip_on; static unsigned long long s_skip_base, s_seen_base;
static int s_skip_host_empty;
static unsigned long long s_rep_stop_empty, s_rep_stop_drew;
static unsigned s_printed_stop;
static void skip_open(const D3D8HostDrawCheck *c, int host_empty)
{
    s_be.exec_skip(1); s_skip_on = 1; s_skip_serial = c->serial; s_skip_host_empty = host_empty;
    s_skip_base = s_be.exec_skipped ? s_be.exec_skipped() : 0;
    s_seen_base = s_be.exec_seen ? s_be.exec_seen() : 0;
}

unsigned long long d3d8_host_2d_flip_count(void) { return s_flips; }
static unsigned s_bisect; static int s_bisect_read;
unsigned d3d8_host_2d_bisect(void)
{
    if (!s_bisect_read) {
        const char *e = getenv("RECOMP_D3D8_HOST_BISECT");
        s_bisect_read = 1;
        s_bisect = e && *e ? (unsigned)strtoul(e, NULL, 0) : 0u;
        if (s_bisect) fprintf(stderr, "[D3D8-HOST-2D] RECOMP_D3D8_HOST_BISECT=0x%X:%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n", s_bisect,
                              s_bisect & 1 ? " own-pass" : "", s_bisect & 2 ? " buffer-per-draw" : "",
                              s_bisect & 4 ? " no-early-tests" : "", s_bisect & 8 ? " generic-shader" : "",
                              s_bisect & 16 ? " hash-every-draw" : "", s_bisect & 32 ? " no-vertex-cache" : "",
                              s_bisect & 64 ? " wait-every-draw" : "", s_bisect & 128 ? " late-executor-skip" : "",
                              s_bisect & 256 ? " no-stencil-class" : "", s_bisect & 512 ? " no-points" : "", s_bisect & 1024 ? " d3d-bind-geometry" : "", s_bisect & 2048 ? " inline-compile" : "", s_bisect & 4096 ? " old-stage-modes" : "", s_bisect & 8192 ? " byte-wise-vertex-hash" : "");
    }
    return s_bisect;
}
void d3d8_host_2d_set_bisect(unsigned mask) { s_bisect = mask; s_bisect_read = 1; }
static unsigned long long s_rep_ns_regs, s_rep_ns_build, s_rep_ff_evals, s_rep_ff_indices, s_rep_ff_gpu;
static inline unsigned long long h2d_now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

void d3d8_host_2d_replace(const D3D8HostDrawCheck *c)
{
    static D3D8H2DVertex *verts;
    static D3D8Host2DDraw d;
    static uint16_t idx[D3D8H2D_IDX_PER_DRAW];
    const uint16_t *use = NULL;
    const char *why;
    int cls = d3d8_host_2d_class(c);
    if (!d3d8_host_replaces_handle(c->vs_handle) || !cls) return;
    ++s_rep_tokens; if (cls == 3) ++s_rep_vs_tokens;
    if (s_verify && s_flips >= s_verify_after && s_flips >= s_verify_from && (s_flips % s_verify) == 0) {   /* a verify flip: the executor draws, the host shadows */
        d3d8_host_2d_pre(c->serial, c->vs_handle, c->rt_data, c->rt_format, c->rt_size, c->zs_data, c->zs_size);
        s_verify_pending = 1; s_verify_serial = c->serial;
        return;
    }
    if (s_skip_on) {                                      /* never left on across draws */
        s_be.exec_skip(0); s_skip_on = 0; count_reason("skip still on at the next draw");
    }
    if (!s_have_be || !s_be.external_draw || !s_be.exec_skip) { ++s_no_backend; return; }
    if (!verts && !(verts = malloc(sizeof *verts * D3D8H2D_MAX_VERTS))) { count_reason("out of memory"); return; }
    d.verts = verts;
    if (c->draw_kind == 2) {
        if (c->idx_snap_over) { ++s_rep_refused; count_reason("more indices than the snapshot takes"); return; }
        if (c->count && (c->idx_snap_n != c->count || !d3d8_host_2d_idx_copy(c->idx_snap_pos, c->count, idx))) {
            ++s_rep_refused; count_reason("index snapshot missing or overwritten"); return;
        }
        use = idx;
    }
    if (cls == 2) {
        static uint32_t ffm[2048];
        unsigned long long t0 = h2d_now_ns(), t1;
        if (!s_be.ff_vertex) { ++s_rep_refused; count_reason("no fixed-function evaluator in the backend"); return; }
        why = d3d8_host_ff_registers(c, ffm);
        t1 = h2d_now_ns(); s_rep_ns_regs += t1 - t0;
        if (!why) why = d3d8_host_draw_build(c, s_be.ram, s_be.ram_size, s_control, use, ffm, s_be.ff_vertex, &d);
        s_rep_ns_build += h2d_now_ns() - t1;
        if (c->prim >= 1u && c->prim <= 4u) pl_census(c, cls, &d, !why);
        if (why) { ++s_rep_refused; count_reason(why); return; }
        s_rep_ff_evals += d.ff_evals; s_rep_ff_indices += c->count; if (d.ff_gpu) ++s_rep_ff_gpu;
    } else if ((why = d3d8_host_draw_build(c, s_be.ram, s_be.ram_size, s_control, use, NULL, NULL, &d))) {
        if (c->prim >= 1u && c->prim <= 4u) pl_census(c, cls, &d, 0);
        ++s_rep_refused; if (cls == 3) ++s_rep_vs_refused; count_reason(why); return;
    }
    ++s_built;
    if (d.tris_dropped_w) { ++s_draws_dropped_w; s_tris_dropped_w += d.tris_dropped_w; }
    if (d.prim_empty) {                                   /* nothing either renderer draws: no encoder, no bind */
        ++s_replaced; ++s_replaced_empty; if (cls == 2) ++s_replaced_ff; else if (cls == 3) ++s_replaced_vs; else ++s_replaced_2d;
        skip_open(c, 1);
        return;
    }
    if (d.stencil_test) ++s_replaced_stencil;
    /* Nothing to draw is still a draw the host has fully described: the
     * executor would draw nothing either (every triangle it would keep is
     * one the host kept). Replace it like any other. */
    if (!s_be.external_draw(&d, s_be.ram, s_be.ram_size)) {
        ++s_rep_unbound; count_reason(s_be.last_error ? s_be.last_error() : "executor target not bound"); return;
    }
    ++s_replaced; if (cls == 2) ++s_replaced_ff; else if (cls == 3) ++s_replaced_vs; else ++s_replaced_2d;
    skip_open(c, d.nverts == 0);
}

void d3d8_host_2d_after(const D3D8HostDrawCheck *c)
{
    if (!s_skip_on) return;
    s_be.exec_skip(0); s_skip_on = 0;
    if (c->serial != s_skip_serial) count_reason("skip closed by a different draw's check");
    /* The positive control on the skip itself. A replaced draw none of whose
     * batches the executor skipped is one of two things. Either its batches
     * reached the executor and it stopped each one before the rasteriser --
     * a point draw's single index, a refused state -- and drew nothing; or no
     * batch arrived between the tokens at all, and whatever the draw emitted
     * falls outside the skip: the double draw. exec_seen tells them apart. */
    if (s_be.exec_skipped && s_be.exec_skipped() == s_skip_base) {
        ++s_rep_noskip;
        if (s_be.exec_seen && s_be.exec_seen() != s_seen_base) {
            if (s_skip_host_empty) ++s_rep_stop_empty;
            else {
                ++s_rep_stop_drew;
                if (s_printed_stop++ < 6)
                    fprintf(stderr, "[D3D8-HOST-2D] draw %u fvf %03X prim %u count %u: the host drew it and the executor"
                                    " stopped it before its rasteriser\n", c->serial, c->vs_handle, c->prim, c->count);
            }
        }
    }
}

/* One PPM per mismatching draw: starting pixels | executor | host | difference
 * (white over tolerance, grey within it, black equal), side by side with a
 * magenta rule between panels. */
static void rgb565(uint16_t v, uint8_t o[3])
{
    unsigned r = v >> 11, g = (v >> 5) & 63u, b = v & 31u;
    o[0] = (uint8_t)((r << 3) | (r >> 2)); o[1] = (uint8_t)((g << 2) | (g >> 4)); o[2] = (uint8_t)((b << 3) | (b >> 2));
}
static void dump(const Rec *r, const D3D8H2DDiff *df)
{
    char path[640];
    unsigned W = 4u * r->w + 3u;
    FILE *f;
    snprintf(path, sizeof path, "%s/h2d_f%06llu_d%08u.ppm", s_dump_dir, s_flips, r->serial);
    if (!(f = fopen(path, "wb"))) { fprintf(stderr, "[D3D8-HOST-2D] dump %s: %s\n", path, strerror(errno)); return; }
    fprintf(f, "P6\n%u %u\n255\n", W, r->h);
    for (uint32_t y = 0; y < r->h; ++y) {
        for (unsigned panel = 0; panel < 4; ++panel) {
            for (uint32_t x = 0; x < r->w; ++x) {
                size_t k = (size_t)y * r->w + x;
                uint8_t px[3];
                if (panel == 0) rgb565(r->pre[k], px);
                else if (panel == 1) rgb565(r->exec[k], px);
                else if (panel == 2) rgb565(r->host[k], px);
                else {
                    uint16_t e = r->exec[k], s = r->host[k];
                    unsigned worst = 0;
                    int d3[3] = { (int)(e >> 11) - (int)(s >> 11), (int)((e >> 5) & 63) - (int)((s >> 5) & 63), (int)(e & 31) - (int)(s & 31) };
                    for (int ch = 0; ch < 3; ++ch) if ((unsigned)abs(d3[ch]) > worst) worst = (unsigned)abs(d3[ch]);
                    px[0] = px[1] = px[2] = (uint8_t)(worst > s_tol ? 255 : worst ? 96 : 0);
                }
                fwrite(px, 1, 3, f);
            }
            if (panel < 3) { static const uint8_t m[3] = { 255, 0, 255 }; fwrite(m, 1, 3, f); }
        }
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/h2d_index.txt", s_dump_dir);
    if ((f = fopen(path, "a"))) {
        const D3D8Host2DDraw *i = &r->info;
        fprintf(f, "flip %llu draw %u bbox %u,%u %ux%u mismatch %llu of %llu max_err %u,%u,%u exec_changed %llu"
                   " host_changed %llu | fvf %03X prim %u count %u tmask %X tex0 fmt %02X %ux%u cc %u ps %u"
                   " blend %u %X/%X eq %X alpha %u func %X ref %u dither %u mask %08X exec_active %u xf MODE %u RANGE %u\n",
                s_flips, r->serial, r->x0, r->y0, r->w, r->h, df->mismatch, df->pixels, df->max_err[0], df->max_err[1],
                df->max_err[2], df->exec_changed, df->host_changed, i->fvf, i->prim, i->count, i->tmask,
                i->tex[0].fmt, i->tex[0].width, i->tex[0].height, i->cc, i->pixel_shader != 0, i->blend,
                i->blend_src, i->blend_dst, i->blend_eq, i->alpha_test, i->alpha_func, i->alpha_ref, i->dither,
                i->color_mask, r->exec_active, NV2A_XF_MODE(r->exec_mode), NV2A_XF_RANGE(r->exec_mode));
        fclose(f);
    }
}

void d3d8_host_2d_flip(void)
{
    unsigned long long px = 0, mm = 0, fe = 0, fh = 0;
    unsigned worst[3] = { 0, 0, 0 }, bad = 0, nrec = s_nrec;
    if (!d3d8_host_armed(NULL, 0)) return;
    ++s_flips;
    if (d3d8_host_any_draw_mode() && s_skip_on) { s_be.exec_skip(0); s_skip_on = 0; count_reason("skip still on at the flip"); }
    if (d3d8_host_any_draw_mode() && !s_nrec) {
        static time_t last;
        time_t now = time(NULL);
        if (!last) last = now;
        if (s_flips % (unsigned long long)s_every == 0 || now - last >= 10) { last = now; d3d8_host_2d_report("periodic"); }
        return;
    }
    for (unsigned k = 0; k < s_nrec; ++k) {
        Rec *r = &s_rec[k];
        D3D8H2DDiff df;
        d3d8_host_2d_diff(r->pre, r->exec, r->host, r->w, r->h, s_tol, &df);
        s_rec_cls[k] = (uint8_t)r->info.cls; s_rec_bad[k] = df.mismatch != 0;
        ++s_compared;
        if (r->info.prim >= 1u && r->info.prim <= 4u) {
            ++s_pl_cmp; s_pl_exec_px += df.exec_changed; s_pl_host_px += df.host_changed;
            if (df.mismatch) ++s_pl_mm; else if (df.max_err[0] | df.max_err[1] | df.max_err[2]) ++s_pl_within; else ++s_pl_exact;
            if (s_dump_dir[0] && s_pl_dumped < 6 && df.exec_changed) {
                ++s_pl_dumped; dump(r, &df);
                fprintf(stderr, "[D3D8-HOST-2D] points/lines dump: flip %llu draw %u prim %u count %u bbox %u,%u %ux%u reg %u"
                                " sprite %u: %llu px over tolerance, executor changed %llu, host %llu, max r%u g%u b%u\n",
                        s_flips, r->serial, r->info.prim, r->info.count, r->x0, r->y0, r->w, r->h, r->info.point_reg,
                        r->info.point_sprite, df.mismatch, df.exec_changed, df.host_changed,
                        df.max_err[0], df.max_err[1], df.max_err[2]);
                if (r->zexec && r->zhost)       /* where the executor drew: its depth after, the host's (the seed if it failed) */
                    for (size_t q = 0; q < (size_t)r->w * r->h; ++q)
                        if (r->exec[q] != r->pre[q]) {
                            fprintf(stderr, "[D3D8-HOST-2D]   at %u,%u: depth executor %.9g host %.9g | host point z %.9g w %.9g,"
                                            " func %X write %u\n", r->x0 + (unsigned)(q % r->w), r->y0 + (unsigned)(q / r->w),
                                    r->zexec[q], r->zhost[q], r->info.z_min, r->info.w_min, r->info.depth_func,
                                    r->info.depth_write);
                            break;
                        }
            }
        }
        /* G75: BUMPENVMAP draws get their own verdicts, and the first few
         * are dumped whether they match or not, for a zoomed look. */
        if (r->info.bump[1] | r->info.bump[2] | r->info.bump[3]) {
            ++s_bump_cmp; s_bump_exec_px += df.exec_changed; s_bump_host_px += df.host_changed;
            if (df.mismatch) ++s_bump_mm; else if (df.max_err[0] | df.max_err[1] | df.max_err[2]) ++s_bump_within; else ++s_bump_exact;
            if (s_dump_dir[0] && s_bump_dumped < 6 && df.exec_changed > 64) {
                ++s_bump_dumped; dump(r, &df);
                fprintf(stderr, "[D3D8-HOST-2D] bump dump: flip %llu draw %u class %u prim %u count %u bbox %u,%u %ux%u modes %X"
                                " (unit 1 input %u, M %g %g %g %g): %llu px over tolerance, executor changed %llu, host %llu,"
                                " max r%u g%u b%u\n", s_flips, r->serial, r->info.cls, r->info.prim, r->info.count, r->x0, r->y0,
                        r->w, r->h, r->info.stage_modes, r->info.bump_in[1], r->info.bump_mat[1][0], r->info.bump_mat[1][1],
                        r->info.bump_mat[1][2], r->info.bump_mat[1][3], df.mismatch, df.exec_changed, df.host_changed,
                        df.max_err[0], df.max_err[1], df.max_err[2]);
            }
        }
        px += df.pixels; mm += df.mismatch; fe += df.exec_changed; fh += df.host_changed;
        if (r->info.cls == 3) {
            ++s_vs_compared; s_vs_px += df.pixels; s_vs_px_mm += df.mismatch;
            s_vs_exec_changed += df.exec_changed; s_vs_host_changed += df.host_changed;
            ++s_vs_cmp_adj[r->info.modes_adjusted != 0];
            if (df.mismatch) {
                const D3D8Host2DDraw *i = &r->info;
                ++s_vs_mm; ++s_vs_mm_adj[i->modes_adjusted != 0];
                if (df.exec_changed > df.host_changed) ++s_vs_mm_cov_exec; else if (df.host_changed > df.exec_changed) ++s_vs_mm_cov_host;
                if (s_vs_printed_mm++ < 12)
                    fprintf(stderr, "[D3D8-HOST-VS] flip %llu draw %u MISMATCH bbox %u,%u %ux%u: %llu px over, executor changed %llu,"
                                    " host %llu, max r%u g%u b%u | program len %u inputs %04X, stage modes %05X (word 54 %05X%s),"
                                    " tmask %X, cc %u, final %08X/%08X, alpha test %u func %X ref %u, blend %u %X/%X,"
                                    " depth %u func %X write %u, stencil %u, cull %X, fog %u, tris %u | inputs not decoded %04X"
                                    " (formats %s), without an array %04X\n",
                            s_flips, r->serial, r->x0, r->y0, r->w, r->h, df.mismatch, df.exec_changed, df.host_changed,
                            df.max_err[0], df.max_err[1], df.max_err[2], i->vs_len, i->vs_inputs, i->stage_modes & 0xFFFFFu,
                            i->ps_word54 & 0xFFFFFu, i->modes_adjusted ? ", ADJUSTED" : "", i->tmask, i->cc,
                            i->final_cw0, i->final_cw1, i->alpha_test, i->alpha_func, i->alpha_ref, i->blend, i->blend_src,
                            i->blend_dst, i->depth_test, i->depth_func, i->depth_write, i->stencil_test, i->cull_face,
                            i->fog_enable, i->vs_nidx / 3u, i->vs_in_unfetched, i->vs_fmt_text, i->vs_in_noarray);
            } else if (df.max_err[0] | df.max_err[1] | df.max_err[2]) ++s_vs_within; else ++s_vs_exact;
            for (int ch = 0; ch < 3; ++ch) if (df.max_err[ch] > s_vs_max_err[ch]) s_vs_max_err[ch] = df.max_err[ch];
        }
        if (r->info.cls == 2) {
            ++s_ff_compared; s_ff_px += df.pixels; s_ff_px_mm += df.mismatch;
            s_ff_exec_changed += df.exec_changed; s_ff_host_changed += df.host_changed;
            if (df.mismatch) {
                ++s_ff_mm;
                ++s_ff_mm_size[df.mismatch <= 4 ? 0 : df.mismatch <= 32 ? 1 : df.mismatch <= 512 ? 2 : 3];
                if (df.exec_changed > df.host_changed) ++s_ff_mm_cov_exec;
                else if (df.host_changed > df.exec_changed) ++s_ff_mm_cov_host; else ++s_ff_mm_cov_same;
            } else if (df.max_err[0] | df.max_err[1] | df.max_err[2]) ++s_ff_within; else ++s_ff_exact;
            for (int ch = 0; ch < 3; ++ch) if (df.max_err[ch] > s_ff_max_err[ch]) s_ff_max_err[ch] = df.max_err[ch];
        }
        for (int ch = 0; ch < 3; ++ch) {
            if (df.max_err[ch] > worst[ch]) worst[ch] = df.max_err[ch];
            if (df.max_err[ch] > s_max_err[ch]) s_max_err[ch] = df.max_err[ch];
        }
        if (df.mismatch) {
            ++s_mismatching; ++bad;
            if (s_printed_mm < 24) {
                const D3D8Host2DDraw *i = &r->info;
                ++s_printed_mm;
                fprintf(stderr, "[D3D8-HOST-%s] flip %llu draw %u MISMATCH bbox %u,%u %ux%u: %llu of %llu px over"
                                " tolerance, max error r%u g%u b%u; executor changed %llu, host %llu%s | fvf %03X prim %u"
                                " count %u tmask %X tex0 %02X %ux%u cc %u%s blend %u %X/%X alpha %u>%u dither %u\n",
                        r->info.cls == 2 ? "FF" : "2D", s_flips, r->serial, r->x0, r->y0, r->w, r->h, df.mismatch, df.pixels,
                        df.max_err[0], df.max_err[1], df.max_err[2], df.exec_changed, df.host_changed,
                        r->exec_active ? "" : " (EXECUTOR DID NOT DRAW IT)", i->fvf, i->prim, i->count, i->tmask,
                        i->tex[0].fmt, i->tex[0].width, i->tex[0].height, i->cc, i->pixel_shader ? " ps" : "",
                        i->blend, i->blend_src, i->blend_dst, i->alpha_test, i->alpha_ref, i->dither);
                fprintf(stderr, "[D3D8-HOST-2D]   draw %u %s: %u indices (snapshot %u), range %u..%u, %u triangles"
                                " dropped for non-finite w; host vertices (%g,%g,%g,%g) (%g,%g,%g,%g) (%g,%g,%g,%g)\n",
                        r->serial, i->draw_kind == 2 ? "DrawIndexedVertices" : "DrawVertices", r->nidx_rec, r->snap_n,
                        i->idx_min, i->idx_max, i->tris_dropped_w,
                        r->pos3[0][0], r->pos3[0][1], r->pos3[0][2], r->pos3[0][3], r->pos3[1][0], r->pos3[1][1],
                        r->pos3[1][2], r->pos3[1][3], r->pos3[2][0], r->pos3[2][1], r->pos3[2][2], r->pos3[2][3]);
                {   char buf[400]; size_t at = 0;
                    unsigned nfirst = r->nidx_rec < 32 ? r->nidx_rec : 32;
                    for (unsigned k = 0; k < nfirst && at < sizeof buf; ++k) at += (size_t)snprintf(buf + at, sizeof buf - at, " %u", r->idxs[k]);
                    if (r->nidx_rec > 48 && at < sizeof buf) at += (size_t)snprintf(buf + at, sizeof buf - at, " ...");
                    for (unsigned k = 32; r->nidx_rec > 32 && k < 48 && at < sizeof buf; ++k)
                        if (r->nidx_rec > 48 || k < r->nidx_rec) at += (size_t)snprintf(buf + at, sizeof buf - at, " %u", r->idxs[k]);
                    fprintf(stderr, "[D3D8-HOST-2D]   draw %u indices:%s\n", r->serial, buf); }
            }
            if (s_dump_dir[0] && s_dumped < s_dump_max) { ++s_dumped; dump(r, &df); }
        } else if (df.max_err[0] | df.max_err[1] | df.max_err[2]) ++s_within;
        else ++s_exact;
        if (r->zhost && r->zexec) {
            unsigned steps = 0;
            unsigned long long zb = d3d8_host_2d_depth_diff(r->zexec, r->zhost, (size_t)r->w * r->h, 1, &steps);
            ++s_z_draws; s_z_px += (unsigned long long)r->w * r->h; s_z_px_mm += zb;
            if (r->info.cls == 2 && zb) {
                ++s_ff_z_worst[steps <= 4 ? 0 : steps <= 64 ? 1 : steps <= 65535 ? 2 : 3];
                if (!df.mismatch) ++s_ff_depth_only;
            }
            if (steps > s_z_max_steps) s_z_max_steps = steps;
            if (zb) {
                ++s_z_draws_mm;
                if (s_printed_zmm < 12) {
                    ++s_printed_zmm;
                    fprintf(stderr, "[D3D8-HOST-%s] flip %llu draw %u DEPTH MISMATCH bbox %u,%u %ux%u: %llu px differ"
                                    " by more than one 24-bit step, worst %u steps (func %X write %u)\n",
                            r->info.cls == 2 ? "FF" : "2D", s_flips, r->serial, r->x0, r->y0, r->w, r->h, zb, steps,
                            r->info.depth_func, r->info.depth_write);
                }
            }
        }
        free(r->pre); free(r->exec); free(r->host); free(r->zhost); free(r->zexec);
    }
    s_px += px; s_px_mm += mm; s_px_exec += fe; s_px_host += fh;
    s_nrec = 0;
    if (nrec && (s_printed_frames < 30 || (bad && s_printed_frames < 200))) {
        unsigned nff = 0, bff = 0;
        for (unsigned k = 0; k < nrec; ++k) if (s_rec_cls[k] == 2) { ++nff; if (s_rec_bad[k]) ++bff; }
        ++s_printed_frames;
        fprintf(stderr, "[D3D8-HOST-2D] flip %llu: %u draws compared (%u 2D, %u fixed-function), %u mismatching (%u 2D,"
                        " %u FF) | %llu px in their boxes, executor changed %llu, host %llu, %llu over tolerance, max"
                        " error r%u g%u b%u\n", s_flips, nrec, nrec - nff, nff, bad, bad - bff, bff, px, fe, fh, mm,
                worst[0], worst[1], worst[2]);
    }
    {   /* Every s_every flips, and every 10 s: a slow shadowed scene reaches few flips. */
        static time_t last;
        time_t now = time(NULL);
        if (!last) last = now;
        if (s_flips % (unsigned long long)s_every == 0 || now - last >= 10) { last = now; d3d8_host_2d_report("periodic"); }
    }
}

void d3d8_host_2d_get_stats(D3D8H2DStats *o)
{
    o->draws = s_draws; o->built = s_built; o->rendered = s_rendered; o->compared = s_compared;
    o->exact = s_exact; o->within = s_within; o->mismatching = s_mismatching;
    o->px = s_px; o->px_mismatch = s_px_mm; o->dumped = s_dumped;
    o->depth_draws = s_z_draws; o->depth_px = s_z_px; o->depth_px_mismatch = s_z_px_mm;
    o->depth_draws_mismatching = s_z_draws_mm; o->z_from_texture = s_z_from_tex; o->z_from_ram = s_z_from_ram;
    o->proof_pass = s_z_proof_pass; o->proof_reject = s_z_proof_reject; o->proof_depends = s_z_proof_depends;
    o->idx_from_snapshot = s_idx_snap; o->idx_changed = s_idx_changed; o->vtx_changed = s_vtx_changed;
    o->exec_outside_host_box = s_exec_outside;
    o->replace_tokens = s_rep_tokens; o->replaced = s_replaced; o->replace_refused = s_rep_refused;
    o->replace_unbound = s_rep_unbound; o->replaced_without_skip = s_rep_noskip;
    o->replaced_exec_stopped_host_empty = s_rep_stop_empty; o->replaced_exec_stopped_host_drew = s_rep_stop_drew;
    o->exec_batches_skipped = s_have_be && s_be.exec_skipped ? s_be.exec_skipped() : 0;
    o->ff_draws = s_ff_draws; o->ff_built = s_ff_built; o->ff_compared = s_ff_compared; o->ff_exact = s_ff_exact;
    o->ff_within = s_ff_within; o->ff_mismatching = s_ff_mm; o->ff_px = s_ff_px; o->ff_px_mismatch = s_ff_px_mm;
    o->replaced_2d = s_replaced_2d; o->replaced_ff = s_replaced_ff; o->replaced_vs = s_replaced_vs;
}

static void ff_report(const char *why)
{
    int any = 0;
    if (s_ffmode <= 0) return;
    fprintf(stderr, "[D3D8-HOST-FF] %s fixed-function draws=%llu built=%llu compared=%llu EXACT=%llu within_tolerance=%llu"
                    " MISMATCHING=%llu | pixels in boxes=%llu executor_changed=%llu host_changed=%llu over_tolerance=%llu"
                    " max_error r%u g%u b%u | triangles dropped for q <= 0 %llu\n",
            why, s_ff_draws, s_ff_built, s_ff_compared, s_ff_exact, s_ff_within, s_ff_mm, s_ff_px, s_ff_exec_changed,
            s_ff_host_changed, s_ff_px_mm, s_ff_max_err[0], s_ff_max_err[1], s_ff_max_err[2], s_ff_tris_q);
    fprintf(stderr, "[D3D8-HOST-FF] %s cross-checks: executor MODE FIXED %llu, other %llu; executor did not draw %llu;"
                    " diffuse defaulted to white %llu | host COMPOSITE vs executor within 1e-3 %llu, beyond %llu |"
                    " VIEWPORT_OFFSET as the host assumes %llu, different %llu | cull state (D3D RS 127/128) as the"
                    " executor's %llu, different %llu (both classes) | stencil state as the executor's %llu, different %llu"
                    " | texture units %llu, sampling differs: min %llu mag %llu LOD bias %llu wrap %llu levels %llu"
                    " | FF draws on unshadowed flips %llu (stride %u)\n",
            why, s_ff_fixed, s_ff_not_fixed, s_ff_exec_inactive, s_ff_diffuse_default, s_ff_composite_match,
            s_ff_composite_differ, s_ff_vpoff_match, s_ff_vpoff_differ, s_cull_match, s_cull_differ, s_stencil_match,
            s_stencil_differ, s_tx_units, s_tx_min, s_tx_mag, s_tx_bias, s_tx_wrap, s_tx_levels, s_ff_unsampled,
            s_ff_stride);
    fprintf(stderr, "[D3D8-HOST-FF] %s mismatching draws by pixels over tolerance: 1-4 %llu, 5-32 %llu, 33-512 %llu,"
                    " >512 %llu | coverage: executor more %llu, host more %llu, same %llu | depth-mismatching draws by worst"
                    " step: <=4 %llu (float order), 5-64 %llu, 65-65535 %llu, >65535 %llu (a fragment one side kept); of"
                    " them colour-clean %llu | no diffuse array: executor's current diffuse white %llu, other %llu |"
                    " refused primitives:", why,
            s_ff_mm_size[0], s_ff_mm_size[1], s_ff_mm_size[2], s_ff_mm_size[3], s_ff_mm_cov_exec, s_ff_mm_cov_host,
            s_ff_mm_cov_same, s_ff_z_worst[0], s_ff_z_worst[1], s_ff_z_worst[2], s_ff_z_worst[3], s_ff_depth_only,
            s_ff_diffuse_white, s_ff_diffuse_other);
    for (unsigned k = 0; k < 16; ++k) if (s_ff_prim[k]) fprintf(stderr, " prim %u=%llu", k, s_ff_prim[k]);
    fprintf(stderr, "\n");
    for (unsigned i = 0; i < NREASON && s_ff_reason[i].why; ++i) {
        if (!any) { fprintf(stderr, "[D3D8-HOST-FF] %s not drawn by the host:", why); any = 1; }
        fprintf(stderr, " %s=%llu;", s_ff_reason[i].why, s_ff_reason[i].n);
    }
    if (any) fprintf(stderr, "\n");
}

void d3d8_host_2d_report(const char *why)
{
    if (!d3d8_host_armed(NULL, 0)) return;
    if (s_sp_match[1] + s_sp_match[2] + s_sp_match[3] + s_sp_differ[1] + s_sp_differ[2] + s_sp_differ[3])
        fprintf(stderr, "[D3D8-HOST] %s stage modes (0x1E70), derived as D3D's 0x1952B0 vs the executor's: 2D %llu/%llu,"
                        " fixed-function %llu/%llu, programmable %llu/%llu agree | the old rule (word 54 / every bound"
                        " stage PROJECT2D): %llu agree, %llu differ\n", why,
                s_sp_match[1], s_sp_match[1] + s_sp_differ[1], s_sp_match[2], s_sp_match[2] + s_sp_differ[2],
                s_sp_match[3], s_sp_match[3] + s_sp_differ[3], s_sp_old_match, s_sp_old_differ);
    if (s_fc_match + s_fc_differ)
        fprintf(stderr, "[D3D8-HOST] %s pixel-shader final combiner (0x288/0x28C), derived as D3D's 0x195610 vs the executor's:"
                        " %llu agree, %llu differ | the definition's words 8/9 as they stand: %llu agree, %llu differ\n",
                why, s_fc_match, s_fc_differ, s_fc_old_match, s_fc_old_differ);
    if (s_mode == 2 || s_ffmode == 2 || s_vsmode == 2) {
        if (s_vsmode == 2)
            fprintf(stderr, "[D3D8-HOST-VS] %s draw mode: programmable tokens %llu, REPLACED %llu, refused %llu (reasons in the"
                            " shared 'not drawn by the host' line)\n", why, s_rep_vs_tokens, s_replaced_vs, s_rep_vs_refused);
        {   /* G52's gap, measured: of the batches that reach the executor's
             * rasteriser, how many the host drew instead (skipped) and how
             * many the executor still draws, by transform MODE. */
            unsigned long long xm[9] = { 0 };
            if (s_have_be && s_be.exec_mode_counts) {
                s_be.exec_mode_counts(xm);
                unsigned long long sk = xm[3] + xm[4] + xm[5], dr = xm[6] + xm[7] + xm[8];
                fprintf(stderr, "[D3D8-HOST] %s coverage: rasterised batches %llu, the host's (skipped) %llu = %.1f%%, the"
                                " executor's %llu (%.1f a flip) | FIXED host %llu executor %llu | PROGRAM host %llu executor"
                                " %llu | reserved host %llu executor %llu\n", why, sk + dr, sk,
                        sk + dr ? 100.0 * (double)sk / (double)(sk + dr) : 0.0, dr, s_flips ? (double)dr / (double)s_flips : 0.0,
                        xm[3], xm[6], xm[4], xm[7], xm[5], xm[8]);
            }
        }
        fprintf(stderr, "[D3D8-HOST-2D] %s draw mode: flips=%llu tokens=%llu REPLACED=%llu (2D %llu, fixed-function %llu; of which the host bound"
                        " the target first %llu; executor batches skipped %llu, replaced draws the executor did not skip"
                        " %llu) | left to the executor: refused %llu, target not bound %llu, no backend %llu | triangles"
                        " dropped for non-finite w %llu\n",
                why, s_flips, s_rep_tokens, s_replaced, s_replaced_2d, s_replaced_ff,
                s_have_be && s_be.external_binds ? s_be.external_binds() : 0ull,
                s_have_be && s_be.exec_skipped ? s_be.exec_skipped() : 0ull, s_rep_noskip, s_rep_refused,
                s_rep_unbound, s_no_backend, s_tris_dropped_w);
        fprintf(stderr, "[D3D8-HOST-2D] %s draw mode classes: stencil (func ALWAYS) %llu, points/lines (nothing drawn) %llu |"
                        " not skipped %llu = executor stopped it before the rasteriser and the host drew nothing %llu +"
                        " executor stopped it and the host drew it %llu + NO BATCH BETWEEN THE TOKENS (double draw) %llu\n",
                why, s_replaced_stencil, s_replaced_empty, s_rep_noskip, s_rep_stop_empty, s_rep_stop_drew,
                s_rep_noskip - s_rep_stop_empty - s_rep_stop_drew);
        if (s_have_be && s_be.spec_stats) {
            unsigned long long b = 0, h = 0, f = 0, ns = 0;
            s_be.spec_stats(&b, &h, &f, &ns);
            fprintf(stderr, "[D3D8-HOST-2D] %s specialised fragment pipelines: built %llu (compile %.1f ms), hits %llu,"
                            " draws on the generic interpreter (compiling, full or failed) %llu | binding the target (a GPU drain each) %.1f ms over"
                            " %llu binds\n", why, b, ns / 1e6, h, f,
                    s_be.bind_ns ? s_be.bind_ns() / 1e6 : 0.0, s_be.external_binds ? s_be.external_binds() : 0ull);
            if (s_be.pipe_stats) {
                char pb[320];
                s_be.pipe_stats(pb, sizeof pb);
                fprintf(stderr, "[D3D8-HOST-2D] %s %s\n", why, pb);
            }
            if (s_be.geom_stats) {
                unsigned long long gd = 0, gv = 0;
                s_be.geom_stats(&gd, &gv);
                fprintf(stderr, "[D3D8-HOST-2D] %s binds whose D3D geometry differed from the executor's slot %llu; binds that"
                                " found the target held more than once %llu\n", why, gd, gv);
            }
        }
        {   /* In-process timers: where a replaced draw's host time goes. */
            unsigned long long th = 0, tb = 0, thash = 0, nt = 0, ne = 0, r = s_replaced ? s_replaced : 1, rf = s_replaced_ff ? s_replaced_ff : 1;
            if (s_have_be && s_be.external_stats) s_be.external_stats(&th, &tb, &thash, &nt, &ne);
            fprintf(stderr, "[D3D8-HOST-2D] %s draw mode cost per replaced draw: FF registers %.1f us, FF build %.1f us"
                            " (vertex evaluations %llu for %llu indices), texture %.1f us, whole host draw %.1f us | texture"
                            " cache hits %llu, decodes %llu, hashes %llu | fixed-function draws built for the GPU unit"
                            " (RECOMP_D3D8_HOST_FF_GPU) %llu\n", why,
                    s_rep_ns_regs / 1000.0 / rf, s_rep_ns_build / 1000.0 / rf, s_rep_ff_evals, s_rep_ff_indices,
                    nt / 1000.0 / r, ne / 1000.0 / r, th, tb, thash, s_rep_ff_gpu);
        }
        {
            int any = 0;
            for (unsigned i = 0; i < NREASON && s_reason[i].why; ++i) {
                if (!any) { fprintf(stderr, "[D3D8-HOST-2D] %s left to the executor:", why); any = 1; }
                fprintf(stderr, " %s=%llu;", s_reason[i].why, s_reason[i].n);
            }
            if (any) fprintf(stderr, "\n");
        }
        if (s_pl_tokens) {
            fprintf(stderr, "[D3D8-HOST-2D] %s points and lines: tokens %llu, host drew %llu (%llu points or segments),"
                            " left to the executor %llu, kinds past the table %llu | VERIFY compared %llu: EXACT %llu"
                            " within_tolerance %llu MISMATCHING %llu, px changed executor %llu host %llu\n", why, s_pl_tokens,
                    s_pl_drawn, s_pl_segs, s_pl_tokens - s_pl_drawn, s_pl_other, s_pl_cmp, s_pl_exact, s_pl_within, s_pl_mm,
                    s_pl_exec_px, s_pl_host_px);
            for (unsigned i = 0; i < s_pl_nkind; ++i) {
                const PlKind *e = &s_pl_kind[i];
                fprintf(stderr, "[D3D8-HOST-2D] %s   points/lines kind %u: n %llu drawn %llu | class %u prim %u count %u%s fvf %03X |"
                                " sprite %u scale %u size %g x dev %g -> reg %u (%.3f px) | stages %X fmt0 %02X fmt3 %02X |"
                                " blend %X zfunc %X\n", why, i, e->n, e->drawn, e->cls, e->prim, e->count, e->count >= 64u ? "+" : "",
                        e->fvf, e->sprite, e->scale_en, u2f(e->size_rs), u2f(e->dev_scale), e->reg, e->reg / 8.0, e->tmask,
                        e->fmt0, e->fmt3, e->blend, e->zfunc);
            }
        }
        if (s_bump_cmp)
            fprintf(stderr, "[D3D8-HOST-2D] %s bump env: VERIFY compared %llu: EXACT %llu within_tolerance %llu MISMATCHING %llu,"
                            " px changed executor %llu host %llu\n", why, s_bump_cmp, s_bump_exact, s_bump_within, s_bump_mm,
                    s_bump_exec_px, s_bump_host_px);
        if (s_verify) fprintf(stderr, "[D3D8-HOST-2D] %s VERIFY (1 flip in %u drawn by the executor, host shadowed):"
                                      " %llu draws compared -- the verdicts are the shadow lines below\n", why, s_verify, s_verified);
        if (s_ffmode != 1 && s_mode != 1 && s_vsmode != 1 && !s_verify) { fflush(stderr); return; }
    }
    fprintf(stderr, "[D3D8-HOST-2D] %s flips=%llu 2d_draws=%llu (executor MODE PROGRAM, the pass-through: %llu, other %llu; executor did not"
                    " draw %llu; D3D object pass-through flag %llu) pre_tokens=%llu (skipped %llu) no_pre=%llu"
                    " no_backend=%llu built=%llu empty=%llu rendered=%llu render_failed=%llu frame_full=%llu"
                    " executor_syncs=%llu\n",
            why, s_flips, s_draws, s_2d_prog, s_2d_not_prog, s_exec_inactive, s_vsflag_pass, s_pre, s_pre_skipped,
            s_no_pre, s_no_backend, s_built, s_empty, s_rendered, s_render_failed, s_rec_dropped, s_sync_calls);
    fprintf(stderr, "[D3D8-HOST-2D] %s compared=%llu EXACT=%llu within_tolerance=%llu MISMATCHING=%llu | pixels in"
                    " boxes=%llu executor_changed=%llu host_changed=%llu over_tolerance=%llu max_error r%u g%u b%u"
                    " (565 steps, tolerance %u)\n",
            why, s_compared, s_exact, s_within, s_mismatching, s_px, s_px_exec, s_px_host, s_px_mm,
            s_max_err[0], s_max_err[1], s_max_err[2], s_tol);
    fprintf(stderr, "[D3D8-HOST-2D] %s inputs: textures in DMA context B %llu, TEXCOORDINDEX != stage %llu,"
                    " diffuse defaulted to white %llu, texture stage modes differ from the executor's 0x1E70 %llu |"
                    " indices from the draw-time snapshot %llu, of which pIndexData held different ones at the token"
                    " %llu | vertex bytes changed between the draw call and the token %llu | executor changed pixels"
                    " outside the host's box %llu | executor pass-through bias (c-37 - c-38) and z scale as the host"
                    " assumes %llu, different %llu | triangles dropped for non-finite w (rhw 0) %llu in %llu draws\n",
            why, s_ctx_b, s_tss_ci_off, s_diffuse_default, s_modes_disagree, s_idx_snap, s_idx_changed,
            s_vtx_changed, s_exec_outside, s_pt_const_match, s_pt_const_differ, s_tris_dropped_w, s_draws_dropped_w);
    fprintf(stderr, "[D3D8-HOST-2D] %s depth: test off %llu | on: func NEVER %llu LESS %llu EQUAL %llu LEQUAL %llu"
                    " GREATER %llu NOTEQUAL %llu GEQUAL %llu ALWAYS %llu; write on %llu; vertex z all 0 %llu, inside"
                    " (0,1) %llu, all 1 %llu, mixed %llu, outside [0,1] %llu | seeded from executor texture %llu,"
                    " guest RAM %llu, none %llu | stored depth under the box proves: pass %llu, reject %llu,"
                    " depends %llu\n", why, s_z_off, s_z_func[0], s_z_func[1], s_z_func[2], s_z_func[3], s_z_func[4],
            s_z_func[5], s_z_func[6], s_z_func[7], s_z_write, s_z_zbucket[0], s_z_zbucket[1], s_z_zbucket[2],
            s_z_zbucket[3], s_z_zbucket[4], s_z_from_tex, s_z_from_ram, s_z_none, s_z_proof_pass,
            s_z_proof_reject, s_z_proof_depends);
    fprintf(stderr, "[D3D8-HOST-2D] %s depth compared: draws=%llu mismatching=%llu | px=%llu over one 24-bit step=%llu,"
                    " worst %u steps\n", why, s_z_draws, s_z_draws_mm, s_z_px, s_z_px_mm, s_z_max_steps);
    {
        int any = 0;
        for (unsigned i = 0; i < NREASON && s_reason[i].why; ++i) {
            if (!any) { fprintf(stderr, "[D3D8-HOST-2D] %s not drawn by the host:", why); any = 1; }
            fprintf(stderr, " %s=%llu;", s_reason[i].why, s_reason[i].n);
        }
        if (any) fprintf(stderr, "\n");
    }
    ff_report(why);
    if (s_vsmode >= 1) {
        unsigned long long top[3] = { 0, 0, 0 }; unsigned slot[3] = { 0, 0, 0 };
        for (unsigned k = 0; k < 192; ++k)
            for (unsigned t = 0; t < 3; ++t) if (s_vs_const_slot_differ[k] > top[t]) {
                for (unsigned u = 2; u > t; --u) { top[u] = top[u - 1]; slot[u] = slot[u - 1]; }
                top[t] = s_vs_const_slot_differ[k]; slot[t] = k; break; }
        unsigned long long xm[9] = { 0 };
        if (s_have_be && s_be.exec_mode_counts) s_be.exec_mode_counts(xm);
        /* THE POSITIVE CONTROL: the executor's own batches by 0x1E94 MODE,
         * beside what the host classified. MODE PROGRAM is every program --
         * D3D's 2D pass-through (host class 1) and the title's shaders (class
         * 3) -- so it is compared with classes 1 + 3; FIXED with class 2. A
         * draw can be several batches, so the executor's numbers are the
         * larger; a zero beside a non-zero is the arming or the classifier. */
        unsigned long long hp = s_cls_seen[1] + s_cls_seen[3];
        fprintf(stderr, "[D3D8-HOST-VS] %s control: executor batches MODE PROGRAM %llu (%.1f a flip), FIXED %llu (%.1f), other %llu"
                        " | host checks: programmable %llu (%.1f a flip) + 2D pass-through %llu = program %llu (%.1f), FF %llu (%.1f),"
                        " unclassified %llu | of the host's programmable, executor MODE PROGRAM %llu, not %llu%s\n",
                why, xm[1], s_flips ? (double)xm[1] / (double)s_flips : 0.0, xm[0], s_flips ? (double)xm[0] / (double)s_flips : 0.0, xm[2],
                s_cls_seen[3], s_flips ? (double)s_cls_seen[3] / (double)s_flips : 0.0, s_cls_seen[1], hp,
                s_flips ? (double)hp / (double)s_flips : 0.0, s_cls_seen[2], s_flips ? (double)s_cls_seen[2] / (double)s_flips : 0.0,
                s_cls_seen[0], s_vs_exec_prog, s_vs_exec_not_prog,
                !(s_have_be && s_be.exec_mode_counts) ? " (no executor count registered)"
                : xm[1] && !hp ? " -- HOST SEES NONE: arming or classifier broken" : "");
        fprintf(stderr, "[D3D8-HOST-VS] %s census: programmable-VS draws %llu over %llu flips (%.1f a flip), distinct programs"
                        " %u%s | shadowed %llu, built %llu, compared %llu: EXACT %llu within_tolerance %llu MISMATCHING %llu"
                        " (executor more %llu, host more %llu) | px %llu over tolerance %llu, max error r%u g%u b%u |"
                        " executor changed %llu, host %llu | constants as the executor's %llu, different %llu (slots most"
                        " often different: %u x%llu, %u x%llu, %u x%llu) | refused for an unbound sampled stage: executor"
                        " unit enabled %llu, disabled %llu, unknown %llu | compared with a stage D3D turned off %llu"
                        " (mismatching %llu), without %llu (mismatching %llu)\n", why, s_vs_seen, s_flips,
                s_flips ? (double)s_vs_seen / (double)s_flips : 0.0, s_vs_progs, s_vs_prog_overflow ? "+" : "",
                s_vs_draws, s_vs_built, s_vs_compared, s_vs_exact, s_vs_within, s_vs_mm, s_vs_mm_cov_exec, s_vs_mm_cov_host,
                s_vs_px, s_vs_px_mm, s_vs_max_err[0], s_vs_max_err[1], s_vs_max_err[2], s_vs_exec_changed, s_vs_host_changed,
                s_vs_const_match, s_vs_const_differ, slot[0], top[0], slot[1], top[1], slot[2], top[2],
                s_vs_unbound_exec_on, s_vs_unbound_exec_off, s_vs_unbound_unknown,
                s_vs_cmp_adj[1], s_vs_mm_adj[1], s_vs_cmp_adj[0], s_vs_mm_adj[0]);
    }
    fflush(stderr);
}
