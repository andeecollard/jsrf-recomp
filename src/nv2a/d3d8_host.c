/* See d3d8_host.h. */
#include "d3d8_host.h"
#include "nv2a_pusher.h"
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SLOTS 8192u                 /* > the 512 KB ring's worth of 8-byte tokens is not
                                     * needed: a slot is busy only between enqueue and
                                     * replay, and a full queue falls back, never waits */
typedef struct {
    _Atomic uint32_t state;         /* 0 free, 1 filling, 2 published */
    uint32_t kind;                  /* 0 methods, 1 draw check */
    D3D8HostDrawCheck check;
    uint32_t n;
    uint32_t method[D3D8_HOST_MAX_METHODS], param[D3D8_HOST_MAX_METHODS];
} Slot;
static Slot s_slot[SLOTS];
static _Atomic uint32_t s_next;
static _Atomic unsigned long long s_enq, s_rep, s_mrep, s_full, s_bad;
static atomic_int s_installed;
static void (*s_exec_source)(D3D8ExecDrawTextures *);
static _Atomic unsigned long long s_chk, s_chk_inactive, s_u_cmp, s_u_match, s_u_missing, s_u_addr, s_u_shape;
static _Atomic unsigned s_mismatch_printed, s_surf_printed;
static _Atomic unsigned long long s_rt_cmp, s_rt_match, s_rt_addr, s_rt_pitch,
                                  s_zs_cmp, s_zs_match, s_zs_missing, s_zs_addr, s_zs_pitch;
static uint32_t size_pitch(uint32_t size) { return ((size >> 24) + 1u) * 64u; }
static const uint32_t k_state_methods[11] = D3D8_HOST_STATE_METHODS;
static _Atomic unsigned long long s_vp_cmp, s_vp_match, s_vp_win, s_vp_z;
static _Atomic unsigned long long s_st_cmp[11], s_st_match[11], s_st_unseen[11];
static _Atomic unsigned s_vp_printed, s_st_printed, s_vc_printed;
static _Atomic unsigned long long s_vc_cmp, s_vc_match, s_vc_all, s_vc_draws;
static const uint32_t k_ps_pairs[D3D8_HOST_PS_N][2] = D3D8_HOST_PS_PAIRS;
static _Atomic unsigned long long s_ps_draws, s_ps_fixed, s_ps_all, s_ps_cmp, s_ps_match, s_ps_word_mm[D3D8_HOST_PS_N];
static _Atomic unsigned s_ps_printed, s_tss_printed;
static _Atomic unsigned long long s_tss_cmp, s_tss_match, s_tss_addr, s_tss_mag, s_tss_min, s_tss_bias;
static _Atomic unsigned long long s_vs_prog, s_vs_fixed, s_vs_unparsed, s_vs_match, s_vs_words;
static _Atomic unsigned s_vs_printed;
static int32_t trunc_scaled(int32_t v, float s) { return (int32_t)((float)((double)v * (double)s + 0.5)); }
void d3d8_host_set_exec_source(void (*get)(D3D8ExecDrawTextures *)) { s_exec_source = get; }

/* NV2A format byte from a D3D Format word, and the shape it implies. */
static void check_draw(const D3D8HostDrawCheck *c)
{
    D3D8ExecDrawTextures e;
    atomic_fetch_add(&s_chk, 1);
    if (!s_exec_source) return;
    memset(&e, 0, sizeof e);
    s_exec_source(&e);
    if (!e.active) { atomic_fetch_add(&s_chk_inactive, 1); return; }
    for (unsigned u = 0; u < 4; ++u) {
        if (!(e.mask & (1u << u))) continue;
        atomic_fetch_add(&s_u_cmp, 1);
        const char *why = NULL;
        if (!c->tex[u]) { atomic_fetch_add(&s_u_missing, 1); why = "D3D has no texture bound"; }
        else if ((c->data[u] & 0x03FFFFFFu) != (e.addr[u] & 0x03FFFFFFu)) { atomic_fetch_add(&s_u_addr, 1); why = "address"; }
        else {
            uint32_t f = c->format[u], fb = (f >> 8) & 0xFFu, lv = (f >> 16) & 0xFu;
            uint32_t w = 1u << ((f >> 20) & 0xFu), h = 1u << ((f >> 24) & 0xFu);
            if (fb == 0x11u) { w = (c->size[u] & 0xFFFu) + 1u; h = ((c->size[u] >> 12) & 0xFFFu) + 1u; lv = 1; }
            if (fb != e.fmt[u] || w != e.width[u] || h != e.height[u] || lv != e.levels[u]) {
                atomic_fetch_add(&s_u_shape, 1); why = "format/shape";
            } else atomic_fetch_add(&s_u_match, 1);
        }
        if (why && atomic_fetch_add(&s_mismatch_printed, 1) < 12)
            fprintf(stderr, "[D3D8-MIRROR] draw %u unit %u MISMATCH (%s): d3d tex=%08X data=%08X fmt=%08X size=%08X"
                            " | exec addr=%08X fmt=%02X %ux%u levels=%u\n",
                    c->serial, u, why, c->tex[u], c->data[u], c->format[u], c->size[u],
                    e.addr[u], e.fmt[u], e.width[u], e.height[u], e.levels[u]);
    }
    /* Colour target: every draw has one. */
    {
        const char *why = NULL;
        atomic_fetch_add(&s_rt_cmp, 1);
        if ((c->rt_data & 0x03FFFFFFu) != (e.target_addr & 0x03FFFFFFu)) { atomic_fetch_add(&s_rt_addr, 1); why = "colour address"; }
        else if (size_pitch(c->rt_size) != e.target_pitch) { atomic_fetch_add(&s_rt_pitch, 1); why = "colour pitch"; }
        else atomic_fetch_add(&s_rt_match, 1);
        if (why && atomic_fetch_add(&s_surf_printed, 1) < 12)
            fprintf(stderr, "[D3D8-MIRROR] draw %u %s MISMATCH: d3d rt=%08X data=%08X fmt=%08X size=%08X (pitch %u)"
                            " | exec target=%08X pitch=%u bpp=%u\n", c->serial, why, c->rt, c->rt_data,
                    c->rt_format, c->rt_size, size_pitch(c->rt_size), e.target_addr, e.target_pitch, e.target_bpp);
    }
    /* Depth: only where the executor used one (depth or stencil test on). */
    if (e.depth_used) {
        const char *why = NULL;
        atomic_fetch_add(&s_zs_cmp, 1);
        if (!c->zs) { atomic_fetch_add(&s_zs_missing, 1); why = "depth surface missing in D3D"; }
        else if ((c->zs_data & 0x03FFFFFFu) != (e.depth_addr & 0x03FFFFFFu)) { atomic_fetch_add(&s_zs_addr, 1); why = "depth address"; }
        else if (size_pitch(c->zs_size) != e.depth_pitch) { atomic_fetch_add(&s_zs_pitch, 1); why = "depth pitch"; }
        else atomic_fetch_add(&s_zs_match, 1);
        if (why && atomic_fetch_add(&s_surf_printed, 1) < 12)
            fprintf(stderr, "[D3D8-MIRROR] draw %u %s MISMATCH: d3d zs=%08X data=%08X fmt=%08X size=%08X (pitch %u)"
                            " | exec depth=%08X pitch=%u\n", c->serial, why, c->zs, c->zs_data, c->zs_format,
                    c->zs_size, size_pitch(c->zs_size), e.depth_addr, e.depth_pitch);
    }
    /* Viewport: D3D's rectangle, supersample-scaled and cut to the surface clip,
     * against the executor's effective scissor; MinZ/MaxZ against its z range. */
    {
        const char *why = NULL;
        int32_t x0 = trunc_scaled(c->vp_x, c->ss_x), y0 = trunc_scaled(c->vp_y, c->ss_y);
        int32_t x1 = trunc_scaled(c->vp_x + c->vp_w, c->ss_x) - 1, y1 = trunc_scaled(c->vp_y + c->vp_h, c->ss_y) - 1;
        int32_t cx0 = (int32_t)e.clip_x, cy0 = (int32_t)e.clip_y;
        int32_t cx1 = (int32_t)(e.clip_x + e.clip_w) - 1, cy1 = (int32_t)(e.clip_y + e.clip_h) - 1;
        if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0; if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
        float zmin = c->vp_minz * 16777215.0f, zmax = c->vp_maxz * 16777215.0f;
        atomic_fetch_add(&s_vp_cmp, 1);
        if ((uint32_t)x0 != e.win_x0 || (uint32_t)y0 != e.win_y0 || (uint32_t)x1 != e.win_x1 || (uint32_t)y1 != e.win_y1) {
            atomic_fetch_add(&s_vp_win, 1); why = "window";
        } else if (fabsf(zmin - e.z_min) > 1.0f || fabsf(zmax - e.z_max) > 1.0f) {
            atomic_fetch_add(&s_vp_z, 1); why = "z range";
        } else atomic_fetch_add(&s_vp_match, 1);
        if (why && atomic_fetch_add(&s_vp_printed, 1) < 10)
            fprintf(stderr, "[D3D8-MIRROR] draw %u viewport MISMATCH (%s): d3d vp=%d,%d %dx%d z=%g..%g ss=%g,%g -> %d,%d..%d,%d"
                            " | exec window=%u,%u..%u,%u clip=%u,%u %ux%u z=%g..%g\n", c->serial, why,
                    c->vp_x, c->vp_y, c->vp_w, c->vp_h, c->vp_minz, c->vp_maxz, c->ss_x, c->ss_y, x0, y0, x1, y1,
                    e.win_x0, e.win_y0, e.win_x1, e.win_y1, e.clip_x, e.clip_y, e.clip_w, e.clip_h, e.z_min, e.z_max);
    }
    /* Vertex shader constants: every slot D3D was asked to write, bit for bit. */
    {
        int all = 1; unsigned long long n = 0, ok = 0;
        for (unsigned k = 0; k < 192; ++k) {
            if (!(c->vc_written[k / 32] & (1u << (k % 32)))) continue;
            ++n;
            if (!memcmp(c->vc[k], e.vc[k], sizeof c->vc[k])) { ++ok; continue; }
            all = 0;
            if (atomic_fetch_add(&s_vc_printed, 1) < 10)
                fprintf(stderr, "[D3D8-MIRROR] draw %u constant MISMATCH slot %u (c%d): d3d %g %g %g %g | exec %g %g %g %g\n",
                        c->serial, k, (int)k - 96, c->vc[k][0], c->vc[k][1], c->vc[k][2], c->vc[k][3],
                        e.vc[k][0], e.vc[k][1], e.vc[k][2], e.vc[k][3]);
        }
        atomic_fetch_add(&s_vc_cmp, n); atomic_fetch_add(&s_vc_match, ok);
        atomic_fetch_add(&s_vc_draws, 1); if (all) atomic_fetch_add(&s_vc_all, 1);
    }
    /* Pixel shader: every register SetPixelShader writes, from the definition. */
    atomic_fetch_add(&s_ps_draws, 1);
    if (!c->ps_bound) atomic_fetch_add(&s_ps_fixed, 1);
    else {
        int all = 1;
        for (unsigned k = 0; k < D3D8_HOST_PS_N; ++k) {
            uint32_t want = c->ps[k_ps_pairs[k][1]];
            atomic_fetch_add(&s_ps_cmp, 1);
            if (want == e.ps_reg[k]) { atomic_fetch_add(&s_ps_match, 1); continue; }
            all = 0; atomic_fetch_add(&s_ps_word_mm[k], 1);
            if (atomic_fetch_add(&s_ps_printed, 1) < 12)
                fprintf(stderr, "[D3D8-MIRROR] draw %u pixel shader MISMATCH reg %04X (def word %u): d3d %08X | exec %08X\n",
                        c->serial, k_ps_pairs[k][0], k_ps_pairs[k][1], want, e.ps_reg[k]);
        }
        if (all) atomic_fetch_add(&s_ps_all, 1);
    }
    /* Texture-stage state against the unit's NV2A registers, per the mapping the
     * 23 Sep discovery pass showed: ADDRESSU/V/W (words 0-2) are the address
     * bytes; MAGFILTER (3) is filter bits 24-27; MINFILTER (4) and MIPFILTER (5)
     * give the min field as MIN + 2*MIP; MIPMAPLODBIAS (6, float) is the low 13
     * bits as bias*256 truncated. */
    for (unsigned u = 0; u < 4; ++u) {
        if (!(e.mask & (1u << u))) continue;
        const uint32_t *t = c->tss[u];
        uint32_t f = e.tex_filter[u];
        float bias; memcpy(&bias, &t[6], 4);
        float scaled = (float)((double)bias * 256.0);
        uint32_t want_bias = ((scaled >= -2147483648.0f && scaled < 2147483648.0f) ? (uint32_t)(int32_t)scaled : 0x80000000u) & 0x1FFFu;
        const char *why = NULL;
        atomic_fetch_add(&s_tss_cmp, 1);
        if ((t[0] | (t[1] << 8) | (t[2] << 16)) != e.tex_address[u]) { atomic_fetch_add(&s_tss_addr, 1); why = "address"; }
        else if (((f >> 24) & 0xFu) != t[3]) { atomic_fetch_add(&s_tss_mag, 1); why = "mag filter"; }
        else if (((f >> 16) & 0xFFu) != t[4] + 2u * t[5]) { atomic_fetch_add(&s_tss_min, 1); why = "min/mip filter"; }
        else if ((f & 0x1FFFu) != want_bias) { atomic_fetch_add(&s_tss_bias, 1); why = "lod bias"; }
        else atomic_fetch_add(&s_tss_match, 1);
        if (why && atomic_fetch_add(&s_tss_printed, 1) < 12)
            fprintf(stderr, "[D3D8-MIRROR] draw %u unit %u texture-stage MISMATCH (%s): d3d addr %u,%u,%u mag %u min %u mip %u bias %g"
                            " | nv2a address=%08X filter=%08X\n", c->serial, u, why, t[0], t[1], t[2], t[3], t[4], t[5],
                    bias, e.tex_address[u], f);
    }
    /* Vertex program: the words D3D's shader object carries against the
     * executor's program memory from slot 0. */
    if (c->vs_kind == 0) atomic_fetch_add(&s_vs_fixed, 1);
    else if (c->vs_kind == 2) atomic_fetch_add(&s_vs_unparsed, 1);
    else {
        atomic_fetch_add(&s_vs_prog, 1);
        atomic_fetch_add(&s_vs_words, c->vs_nwords);
        unsigned k;
        for (k = 0; k < c->vs_nwords; ++k) if (c->vs_words[k] != e.vs_words[k]) break;
        if (k == c->vs_nwords) atomic_fetch_add(&s_vs_match, 1);
        else if (atomic_fetch_add(&s_vs_printed, 1) < 8)
            fprintf(stderr, "[D3D8-MIRROR] draw %u vertex program MISMATCH handle %08X at word %u of %u: d3d %08X | exec %08X\n",
                    c->serial, c->vs_handle, k, c->vs_nwords, c->vs_words[k], e.vs_words[k]);
    }
    /* Blend / alpha / depth / stencil registers: D3D's last Simple push against
     * the executor's register file at this draw. */
    for (unsigned k = 0; k < 11; ++k) {
        if (!(c->st_seen & (1u << k))) { atomic_fetch_add(&s_st_unseen[k], 1); continue; }
        atomic_fetch_add(&s_st_cmp[k], 1);
        if (c->st_val[k] == e.st_reg[k]) atomic_fetch_add(&s_st_match[k], 1);
        else if (atomic_fetch_add(&s_st_printed, 1) < 12)
            fprintf(stderr, "[D3D8-MIRROR] draw %u state MISMATCH method %04X: d3d pushed %08X, executor has %08X\n",
                    c->serial, k_state_methods[k], c->st_val[k], e.st_reg[k]);
    }
}

uint32_t d3d8_host_enqueue_check(const D3D8HostDrawCheck *c)
{
    uint32_t i, expect = 0;
    d3d8_host_install();
    i = atomic_fetch_add(&s_next, 1u) % SLOTS;
    if (!atomic_compare_exchange_strong(&s_slot[i].state, &expect, 1u)) {
        atomic_fetch_add(&s_full, 1);
        return 0;
    }
    s_slot[i].kind = 1; s_slot[i].n = 0; s_slot[i].check = *c;
    atomic_store_explicit(&s_slot[i].state, 2u, memory_order_release);
    atomic_fetch_add(&s_enq, 1);
    return i + 1u;
}

/* Token parameter = slot index + 1, so 0 never names a slot. */
static void on_token(uint32_t parameter)
{
    uint32_t i = parameter - 1u;
    if (!parameter || i >= SLOTS ||
        atomic_load_explicit(&s_slot[i].state, memory_order_acquire) != 2u) {
        atomic_fetch_add(&s_bad, 1);
        return;
    }
    if (s_slot[i].kind == 1) check_draw(&s_slot[i].check);
    for (uint32_t k = 0; k < s_slot[i].n; ++k)
        nv2a_pusher_dispatch_host(0, s_slot[i].method[k], s_slot[i].param[k]);
    atomic_fetch_add(&s_mrep, s_slot[i].n);
    atomic_fetch_add(&s_rep, 1);
    atomic_store_explicit(&s_slot[i].state, 0u, memory_order_release);
}

void d3d8_host_install(void)
{
    int expect = 0;
    if (atomic_compare_exchange_strong(&s_installed, &expect, 1))
        nv2a_pusher_set_host_token_handler(on_token);
}

uint32_t d3d8_host_enqueue(const uint32_t *methods, const uint32_t *params, unsigned n)
{
    uint32_t i, expect = 0;
    if (!n || n > D3D8_HOST_MAX_METHODS)
        return 0;
    d3d8_host_install();
    i = atomic_fetch_add(&s_next, 1u) % SLOTS;
    if (!atomic_compare_exchange_strong(&s_slot[i].state, &expect, 1u)) {
        atomic_fetch_add(&s_full, 1);
        return 0;
    }
    s_slot[i].kind = 0;
    s_slot[i].n = n;
    memcpy(s_slot[i].method, methods, n * sizeof *methods);
    memcpy(s_slot[i].param, params, n * sizeof *params);
    atomic_store_explicit(&s_slot[i].state, 2u, memory_order_release);
    atomic_fetch_add(&s_enq, 1);
    return i + 1u;
}

void d3d8_host_get_stats(D3D8HostStats *o)
{
    o->enqueued = atomic_load(&s_enq); o->replayed = atomic_load(&s_rep);
    o->methods_replayed = atomic_load(&s_mrep); o->full = atomic_load(&s_full);
    o->bad_token = atomic_load(&s_bad);
    o->checks = atomic_load(&s_chk); o->check_inactive = atomic_load(&s_chk_inactive);
    o->units_compared = atomic_load(&s_u_cmp); o->units_match = atomic_load(&s_u_match);
    o->units_missing = atomic_load(&s_u_missing); o->units_addr = atomic_load(&s_u_addr);
    o->units_shape = atomic_load(&s_u_shape);
    o->rt_compared = atomic_load(&s_rt_cmp); o->rt_match = atomic_load(&s_rt_match);
    o->rt_addr = atomic_load(&s_rt_addr); o->rt_pitch = atomic_load(&s_rt_pitch);
    o->zs_compared = atomic_load(&s_zs_cmp); o->zs_match = atomic_load(&s_zs_match);
    o->zs_missing = atomic_load(&s_zs_missing); o->zs_addr = atomic_load(&s_zs_addr);
    o->zs_pitch = atomic_load(&s_zs_pitch);
    o->vp_compared = atomic_load(&s_vp_cmp); o->vp_match = atomic_load(&s_vp_match);
    o->vp_window = atomic_load(&s_vp_win); o->vp_z = atomic_load(&s_vp_z);
    o->vc_slots_compared = atomic_load(&s_vc_cmp); o->vc_slots_match = atomic_load(&s_vc_match);
    o->vc_draws_all_match = atomic_load(&s_vc_all); o->vc_draws = atomic_load(&s_vc_draws);
    o->ps_draws = atomic_load(&s_ps_draws); o->ps_draws_fixed = atomic_load(&s_ps_fixed);
    o->ps_draws_all_match = atomic_load(&s_ps_all); o->ps_words_compared = atomic_load(&s_ps_cmp);
    o->ps_words_match = atomic_load(&s_ps_match);
    for (unsigned k = 0; k < D3D8_HOST_PS_N; ++k) o->ps_word_mismatch[k] = atomic_load(&s_ps_word_mm[k]);
    o->tss_compared = atomic_load(&s_tss_cmp); o->tss_match = atomic_load(&s_tss_match);
    o->tss_addr = atomic_load(&s_tss_addr); o->tss_mag = atomic_load(&s_tss_mag);
    o->tss_min = atomic_load(&s_tss_min); o->tss_bias = atomic_load(&s_tss_bias);
    o->vs_draws_prog = atomic_load(&s_vs_prog); o->vs_draws_fixed = atomic_load(&s_vs_fixed);
    o->vs_draws_unparsed = atomic_load(&s_vs_unparsed); o->vs_draws_match = atomic_load(&s_vs_match);
    o->vs_words_compared = atomic_load(&s_vs_words);
    for (unsigned k = 0; k < 11; ++k) {
        o->st_compared[k] = atomic_load(&s_st_cmp[k]); o->st_match[k] = atomic_load(&s_st_match[k]);
        o->st_unseen[k] = atomic_load(&s_st_unseen[k]);
    }
}

void d3d8_host_report(const char *why)
{
    D3D8HostStats st; d3d8_host_get_stats(&st);
    fprintf(stderr, "[D3D8-HOST] %s enqueued=%llu replayed=%llu methods=%llu full=%llu bad_token=%llu\n",
            why, st.enqueued, st.replayed, st.methods_replayed, st.full, st.bad_token);
    if (st.checks)
        fprintf(stderr, "[D3D8-MIRROR] %s draws checked=%llu (executor inactive %llu) texture units compared=%llu"
                        " MATCH=%llu | missing-in-d3d=%llu address=%llu format/shape=%llu\n",
                why, st.checks, st.check_inactive, st.units_compared, st.units_match,
                st.units_missing, st.units_addr, st.units_shape);
    if (st.checks)
        fprintf(stderr, "[D3D8-MIRROR] %s surfaces: colour compared=%llu MATCH=%llu (address %llu, pitch %llu)"
                        " | depth compared=%llu MATCH=%llu (missing %llu, address %llu, pitch %llu)\n",
                why, st.rt_compared, st.rt_match, st.rt_addr, st.rt_pitch,
                st.zs_compared, st.zs_match, st.zs_missing, st.zs_addr, st.zs_pitch);
    if (st.checks) {
        fprintf(stderr, "[D3D8-MIRROR] %s viewport: compared=%llu MATCH=%llu (window %llu, z range %llu)\n",
                why, st.vp_compared, st.vp_match, st.vp_window, st.vp_z);
        fprintf(stderr, "[D3D8-MIRROR] %s vertex constants: slots compared=%llu MATCH=%llu | draws %llu, all slots matching in %llu\n",
                why, st.vc_slots_compared, st.vc_slots_match, st.vc_draws, st.vc_draws_all_match);
        fprintf(stderr, "[D3D8-MIRROR] %s pixel shader: draws %llu (fixed-function %llu), all registers matching in %llu;"
                        " words compared=%llu MATCH=%llu\n", why, st.ps_draws, st.ps_draws_fixed,
                st.ps_draws_all_match, st.ps_words_compared, st.ps_words_match);
        fprintf(stderr, "[D3D8-MIRROR] %s vertex program: programmable draws %llu, identical %llu (words %llu) | fixed-function %llu, unparsed %llu\n",
                why, st.vs_draws_prog, st.vs_draws_match, st.vs_words_compared, st.vs_draws_fixed, st.vs_draws_unparsed);
        fprintf(stderr, "[D3D8-MIRROR] %s texture stages: units compared=%llu MATCH=%llu (address %llu, mag %llu, min/mip %llu, lod bias %llu)\n",
                why, st.tss_compared, st.tss_match, st.tss_addr, st.tss_mag, st.tss_min, st.tss_bias);
        for (unsigned k = 0; k < D3D8_HOST_PS_N; ++k)
            if (st.ps_word_mismatch[k])
                fprintf(stderr, "[D3D8-MIRROR] %s pixel shader reg %04X (def word %u): %llu mismatches\n",
                        why, k_ps_pairs[k][0], k_ps_pairs[k][1], st.ps_word_mismatch[k]);
        for (unsigned k = 0; k < 11; ++k)
            fprintf(stderr, "[D3D8-MIRROR] %s state %04X: compared=%llu MATCH=%llu never-pushed-by-D3D=%llu\n",
                    why, k_state_methods[k], st.st_compared[k], st.st_match[k], st.st_unseen[k]);
    }
}
