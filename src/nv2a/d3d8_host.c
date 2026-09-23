/* See d3d8_host.h. */
#include "d3d8_host.h"
#include "nv2a_pusher.h"
#include <stdatomic.h>
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
}
