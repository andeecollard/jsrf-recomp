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
static _Atomic unsigned s_vs_printed, s_ff_printed;
static _Atomic unsigned long long s_ff_cmp, s_ff_match, s_ff_mv, s_ff_comp, s_ff_other;
static int32_t trunc_scaled(int32_t v, float s) { return (int32_t)((float)((double)v * (double)s + 0.5)); }
void d3d8_host_set_exec_source(void (*get)(D3D8ExecDrawTextures *)) { s_exec_source = get; }

/* ---- G41: vertex streams and indices ---- */
static _Atomic unsigned long long s_va_draws, s_va_all, s_va_arrays, s_va_exact, s_va_in, s_va_nod3d,
                                  s_va_stride, s_va_offset, s_va_format, s_va_exmiss;
static _Atomic unsigned long long s_ix_draws, s_ix_match, s_ix_count, s_ix_value, s_ix_indexed, s_ix_indexed_match;
static _Atomic unsigned long long s_hk_st_cmp, s_hk_st_match, s_hk_ib_cmp, s_hk_ib_match;
static _Atomic unsigned s_va_printed, s_ix_printed, s_hk_printed;
static int va_enabled(uint32_t format) { return ((format >> 4) & 0xFu) != 0; }

/* Does `off` lie within the first element of a bound stream whose stride is
 * `stride`? The element starts at Data + Stream.Offset + base*Stride; an
 * attribute sits inside it, so off - start < Stride. The XDK's vertex buffer
 * carries no size, so this is as much of "inside the buffer" as D3D knows. */
static int in_bound_stream(const D3D8HostDrawCheck *c, uint32_t off, uint32_t stride)
{
    for (unsigned s = 0; s < 16; ++s) {
        if (!c->st_vb[s] || c->st_stride[s] != stride) continue;
        uint32_t start = (c->st_data[s] + c->st_offset[s] + c->base_vertex * c->st_stride[s]) & 0x03FFFFFFu;
        uint32_t d = (off & 0x03FFFFFFu) - start;
        if (d < (stride ? stride : 1u)) return 1;
    }
    return 0;
}

int d3d8_host_check_streams(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e)
{
    int all = 1, ix_ok = 1;
    if (!c->draw_kind || !e->va_valid) return 1;
    /* Streams: every array the executor enabled, against D3D's derivation. */
    atomic_fetch_add(&s_va_draws, 1);
    for (unsigned i = 0; i < 16; ++i) {
        int ex = va_enabled(e->va_format[i]), d3 = (c->va_on >> i) & 1u;
        const char *why = NULL;
        if (!ex) {
            if (d3) { atomic_fetch_add(&s_va_exmiss, 1); why = "D3D enables it, the executor does not"; }
        } else {
            uint32_t stride = e->va_format[i] >> 8;
            atomic_fetch_add(&s_va_arrays, 1);
            if (in_bound_stream(c, e->va_offset[i], stride)) atomic_fetch_add(&s_va_in, 1);
            if (!d3) { atomic_fetch_add(&s_va_nod3d, 1); why = "no D3D stream"; }
            else if (stride != (c->va_format[i] >> 8)) { atomic_fetch_add(&s_va_stride, 1); why = "stride"; }
            else if ((e->va_format[i] & 0xFFu) != (c->va_format[i] & 0xFFu)) { atomic_fetch_add(&s_va_format, 1); why = "format"; }
            else if (e->va_offset[i] != c->va_offset[i]) { atomic_fetch_add(&s_va_offset, 1); why = "offset"; }
            else atomic_fetch_add(&s_va_exact, 1);
        }
        if (!why) continue;
        all = 0;
        if (atomic_fetch_add(&s_va_printed, 1) < 12) {
            uint32_t s = c->va_stream[i] & 15u;
            fprintf(stderr, "[D3D8-MIRROR] draw %u array %u MISMATCH (%s): d3d stream %u vb=%08X data=%08X stride=%u"
                            " offset=%u base=%u -> %08X fmt=%08X | exec offset=%08X fmt=%08X\n",
                    c->serial, i, why, c->va_stream[i], c->st_vb[s], c->st_data[s], c->st_stride[s],
                    c->st_offset[s], c->base_vertex, c->va_offset[i], c->va_format[i],
                    e->va_offset[i], e->va_format[i]);
        }
    }
    if (all) atomic_fetch_add(&s_va_all, 1);
    /* Indices: the count, and the first few, as D3D was handed them. */
    atomic_fetch_add(&s_ix_draws, 1);
    if (c->draw_kind == 2) atomic_fetch_add(&s_ix_indexed, 1);
    {
        const char *why = NULL; unsigned k = 0, n = c->nidx < D3D8_HOST_IDX_N ? c->nidx : D3D8_HOST_IDX_N;
        if (e->idx_count != c->count) { atomic_fetch_add(&s_ix_count, 1); why = "count"; }
        else {
            for (k = 0; k < n; ++k) if (c->idx[k] != e->idx[k]) break;
            if (k < n) { atomic_fetch_add(&s_ix_value, 1); why = "value"; }
        }
        if (!why) {
            atomic_fetch_add(&s_ix_match, 1);
            if (c->draw_kind == 2) atomic_fetch_add(&s_ix_indexed_match, 1);
        } else {
            ix_ok = 0;
            if (atomic_fetch_add(&s_ix_printed, 1) < 12)
                fprintf(stderr, "[D3D8-MIRROR] draw %u indices MISMATCH (%s) %s prim %u: d3d count=%u first %u %u %u %u"
                                " | exec count=%u first %u %u %u %u\n", c->serial, why,
                        c->draw_kind == 2 ? "DrawIndexedVertices" : "DrawVertices", c->prim, c->count,
                        c->idx[0], c->idx[1], c->idx[2], c->idx[3], e->idx_count, e->idx[0], e->idx[1], e->idx[2], e->idx[3]);
        }
    }
    /* Cross-check: the hooks' view of the bindings against the device's. */
    for (unsigned s = 0; s < 16; ++s) {
        if (!(c->hk_stream_seen & (1u << s))) continue;
        atomic_fetch_add(&s_hk_st_cmp, 1);
        if (c->hk_vb[s] == c->st_vb[s] && c->hk_stride[s] == c->st_stride[s]) atomic_fetch_add(&s_hk_st_match, 1);
        else if (atomic_fetch_add(&s_hk_printed, 1) < 8)
            fprintf(stderr, "[D3D8-MIRROR] draw %u stream %u hook/device MISMATCH: SetStreamSource vb=%08X stride=%u"
                            " | device vb=%08X stride=%u\n", c->serial, s, c->hk_vb[s], c->hk_stride[s], c->st_vb[s], c->st_stride[s]);
    }
    if (c->hk_ib_seen) {
        atomic_fetch_add(&s_hk_ib_cmp, 1);
        if (c->hk_ib == c->ib && c->hk_base == c->base_vertex) atomic_fetch_add(&s_hk_ib_match, 1);
        else if (atomic_fetch_add(&s_hk_printed, 1) < 8)
            fprintf(stderr, "[D3D8-MIRROR] draw %u indices hook/device MISMATCH: SetIndices ib=%08X base=%u | device ib=%08X base=%u\n",
                    c->serial, c->hk_ib, c->hk_base, c->ib, c->base_vertex);
    }
    return all && ix_ok;
}

/* ---- G43: fixed-function combiners ----
 *
 * WHICH D3D STATE THE REGISTERS CAME FROM. D3D builds the combiners lazily:
 * the flusher 0x1964A0 (called at the head of every draw) runs the builder
 * 0x197F90 only when dirty bit 0x800 of 0x19DED8 is set, and the fog updater
 * 0x195610 (the only fixed-function writer of SPECULAR_FOG_CW0/1) only on
 * 0x2000. Whatever the executor holds at a draw is what those two last
 * emitted. So the reference is the transcription evaluated on the inputs they
 * had when they last EMITTED -- captured by hooks on their entries in the
 * staged gen (ffc_emit / fog_emit) -- and not on the state at the draw.
 *
 * The state at the draw is still transcribed beside it, and every draw where
 * the two disagree is counted, with which of the two the executor matched.
 * Read statically, they should never disagree: every input the builder reads
 * dirties 0x800 when it changes -- SetTexture (0x18DF10) sets 0x800 on a
 * change to or from NULL, SetPixelShader(NULL) (0x199BE0) sets 0x4800 -- and
 * the draw itself changes none of them between its flush and the mirror's
 * snapshot. A nonzero count is a dirty-bit path the static read missed.
 *
 * FACTOR0/1 are NOT lazy. SetRenderState_TextureFactor (0x18ECC0) writes all
 * sixteen at once whenever no pixel shader is bound and otherwise only stores
 * D3D_g_RenderState[129]; SetPixelShader(NULL) clears device +0x370 and then
 * calls it with RenderState[129]. So at a fixed-function draw the registers
 * hold the current RenderState[129], and that is what is transcribed. */
static _Atomic unsigned long long s_ffc_draws, s_ffc_ps, s_ffc_all, s_ffc_builder, s_ffc_factor, s_ffc_final,
                                  s_ffc_final_skip, s_ffc_unres, s_ffc_noemit, s_ffc_fresh, s_ffc_lazy,
                                  s_ffc_cur_only, s_ffc_emit_only, s_ffc_word_mm[D3D8_HOST_FFC_N];
static _Atomic unsigned s_ffc_printed;

const char *d3d8_host_ffc_name(unsigned k, char *buf, unsigned n)
{
    static const char *const grp[6] = { "COLOR_ICW", "COLOR_OCW", "ALPHA_ICW", "ALPHA_OCW", "FACTOR0", "FACTOR1" };
    if (k == 0) snprintf(buf, n, "COMBINER_CONTROL");
    else if (k < 49) snprintf(buf, n, "%s[%u]", grp[(k - 1u) / 8u], (k - 1u) % 8u);
    else snprintf(buf, n, "SPECULAR_FOG_CW%u", k - 49u);
    return buf;
}
#define FFC_BUILDER_END 33u   /* words 0..32 come from 0x197F90 */
#define FFC_FACTOR_END  49u   /* 33..48 from 0x18ECC0, 49..50 from 0x195610 */
static void ffc_from_builder(const D3D8FFCombiners *o, D3D8CombinerRegs *x)
{
    x->w[0] = o->combiner_control;
    memcpy(&x->w[1], o->color_icw, 32); memcpy(&x->w[9], o->color_ocw, 32);
    memcpy(&x->w[17], o->alpha_icw, 32); memcpy(&x->w[25], o->alpha_ocw, 32);
}
static int ffc_builder_eq(const D3D8CombinerRegs *a, const D3D8CombinerRegs *b)
{
    return !memcmp(a->w, b->w, FFC_BUILDER_END * sizeof a->w[0]);
}
/* The builder's inputs, one group per stage until the chain ends. */
static void ffc_fmt_inputs(char *buf, size_t n, const D3D8FFCombinerIn *in, uint32_t tfactor, const uint32_t fog[4])
{
    size_t at = 0;
    at += (size_t)snprintf(buf + at, n - at, "sprite=%u spec=%u tex=%X dev8=%08X tf=%08X fog=%u |",
                           in->point_sprite_enable, in->specular_enable, in->texture_bound_mask,
                           in->device_flags, tfactor, fog[0]);
    for (unsigned s = in->point_sprite_enable ? 3u : 0u; s < 4 && at < n; ++s) {
        const uint32_t *t = in->tss[s];
        at += (size_t)snprintf(buf + at, n - at, " s%u cop %u(%X,%X,%X) aop %u(%X,%X,%X) res %u", s,
                               t[12], t[13], t[14], t[15], t[16], t[17], t[18], t[19], t[20]);
        if (t[12] == D3D8FF_TOP_DISABLE) break;
    }
}
static void ffc_fmt_regs(char *buf, size_t n, const D3D8CombinerRegs *r, unsigned stages)
{
    size_t at = (size_t)snprintf(buf, n, "ctl=%X", r->w[0]);
    for (unsigned i = 0; i < stages && at < n; ++i)
        at += (size_t)snprintf(buf + at, n - at, " [%u] c %08X>%08X a %08X>%08X", i,
                               r->w[1 + i], r->w[9 + i], r->w[17 + i], r->w[25 + i]);
    if (at < n)
        snprintf(buf + at, n - at, " f0=%08X f1=%08X cw=%08X,%08X", r->w[33], r->w[41], r->w[49], r->w[50]);
}

/* Discovery tally: key = the inputs the registers were built from (the stage
 * words of the stages the builder consumed, their texture-bound bits, the
 * COLOROP that ended the chain, point sprite, TFACTOR, FOGENABLE,
 * SPECULARENABLE); value = the executor's registers. Filled on the ring
 * consumer thread; the lock only keeps a print from reading a half-written
 * entry. */
#define FFC_KEY_N 42u
#define FFC_TALLY_SLOTS 4096u
typedef struct { uint64_t hash; unsigned long long count; uint32_t key[FFC_KEY_N]; D3D8CombinerRegs regs; } FfcTally;
static FfcTally s_ffc_tally[FFC_TALLY_SLOTS];
static unsigned s_ffc_tally_used;
static _Atomic unsigned long long s_ffc_tally_overflow;
static atomic_flag s_ffc_tally_lock = ATOMIC_FLAG_INIT;
static void ffc_lock(void) { while (atomic_flag_test_and_set_explicit(&s_ffc_tally_lock, memory_order_acquire)) { } }
static void ffc_unlock(void) { atomic_flag_clear_explicit(&s_ffc_tally_lock, memory_order_release); }
static uint64_t fnv(uint64_t h, const uint32_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) { h ^= w[i]; h *= 0x100000001B3ull; }
    return h;
}
static void ffc_key(uint32_t key[FFC_KEY_N], const D3D8FFCombinerIn *in, const D3D8FFCombiners *o, int unres,
                    uint32_t tfactor, const uint32_t fog[4])
{
    unsigned first = in->point_sprite_enable ? 3u : 0u, count = o->combiner_control & 0xFu, last;
    memset(key, 0, FFC_KEY_N * sizeof key[0]);
    last = unres || !count ? 4u : first + count;       /* one past the last consumed stage */
    for (unsigned s = first; s < 4; ++s) {
        if (s < last) {
            for (unsigned k = 0; k < 9; ++k) key[9u * s + k] = in->tss[s][12 + k];
            key[36] |= in->texture_bound_mask & (1u << s);
        } else { key[9u * s] = in->tss[s][12]; break; }  /* the COLOROP that ended the chain */
    }
    key[37] = in->point_sprite_enable != 0; key[38] = tfactor; key[39] = fog[0] != 0; key[40] = in->specular_enable != 0;
    key[41] = (uint32_t)unres;
}
static void ffc_tally_add(const uint32_t key[FFC_KEY_N], const D3D8CombinerRegs *r)
{
    uint64_t h = fnv(fnv(0xCBF29CE484222325ull, key, FFC_KEY_N), r->w, D3D8_HOST_FFC_N);
    if (!h) h = 1;
    ffc_lock();
    for (unsigned p = 0; p < FFC_TALLY_SLOTS; ++p) {
        FfcTally *t = &s_ffc_tally[(h + p) % FFC_TALLY_SLOTS];
        if (!t->hash) {
            if (s_ffc_tally_used >= FFC_TALLY_SLOTS * 3u / 4u) break;
            t->hash = h; t->count = 1; memcpy(t->key, key, sizeof t->key); t->regs = *r;
            ++s_ffc_tally_used; ffc_unlock(); return;
        }
        if (t->hash == h && !memcmp(t->key, key, sizeof t->key) && !memcmp(&t->regs, r, sizeof *r)) {
            ++t->count; ffc_unlock(); return;
        }
    }
    ffc_unlock();
    atomic_fetch_add(&s_ffc_tally_overflow, 1);
}
void d3d8_host_ffc_tally_report(const char *why, unsigned top)
{
    static FfcTally snap[FFC_TALLY_SLOTS];
    static char line[2600], a[1024], b[1200];
    unsigned n = 0, inputs = 0, ambiguous = 0;
    unsigned long long total = 0;
    ffc_lock();
    for (unsigned i = 0; i < FFC_TALLY_SLOTS; ++i) if (s_ffc_tally[i].hash) snap[n++] = s_ffc_tally[i];
    ffc_unlock();
    for (unsigned i = 0; i < n; ++i) {
        int seen = 0, other = 0;
        total += snap[i].count;
        for (unsigned j = 0; j < n; ++j) {
            if (j == i || memcmp(snap[j].key, snap[i].key, sizeof snap[i].key)) continue;
            if (j < i) seen = 1;
            other = 1;
        }
        if (!seen) { ++inputs; if (other) ++ambiguous; }
    }
    fprintf(stderr, "[D3D8-MIRROR] %s ff combiner setups: %u distinct (stage words, TFACTOR) -> executor-register pairings"
                    " over %llu fixed-function draws; %u distinct inputs, %u of them seen with more than one register set;"
                    " tally overflow %llu\n", why, n, total, inputs, ambiguous, atomic_load(&s_ffc_tally_overflow));
    for (unsigned r = 0; r < top && r < n; ++r) {
        unsigned best = r, first;
        size_t at = 0;
        for (unsigned j = r + 1; j < n; ++j) if (snap[j].count > snap[best].count) best = j;
        if (best != r) { FfcTally t = snap[r]; snap[r] = snap[best]; snap[best] = t; }
        const uint32_t *k = snap[r].key;
        first = k[37] ? 3u : 0u;
        a[0] = 0;
        for (unsigned s = first; s < 4 && at < sizeof a; ++s) {
            const uint32_t *t = &k[9u * s];
            if (!t[0] && !k[41]) break;
            if (t[0] == D3D8FF_TOP_DISABLE && s > first) {
                at += (size_t)snprintf(a + at, sizeof a - at, " s%u:end", s); break; }
            at += (size_t)snprintf(a + at, sizeof a - at, " s%u:c%u(%X,%X,%X) a%u(%X,%X,%X) r%u%s", s,
                                   t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8],
                                   (k[36] >> s) & 1u ? "" : " notex");
        }
        {   unsigned st = snap[r].regs.w[0] & 0xFu; if (!st) st = 1; if (st > 8) st = 8;
            ffc_fmt_regs(b, sizeof b, &snap[r].regs, st); }
        snprintf(line, sizeof line, "[D3D8-MIRROR] %s ff setup #%u: %llu draws (%.2f%%)%s tf=%08X fog=%u spec=%u%s%s -> %s\n",
                 why, r + 1, snap[r].count, total ? 100.0 * (double)snap[r].count / (double)total : 0.0, a,
                 k[38], k[39], k[40], k[37] ? " pointsprite" : "", k[41] ? " UNRESOLVED" : "", b);
        fputs(line, stderr);
    }
    fflush(stderr);
}

int d3d8_host_check_combiners(const D3D8HostDrawCheck *c, const D3D8ExecDrawTextures *e, D3D8CombinerRegs *expect)
{
    D3D8FFCombiners o, oc;
    D3D8CombinerRegs x, xc;
    const D3D8FFCombinerIn *in;
    const uint32_t *fog;
    uint32_t key[FFC_KEY_N];
    int unres, fin, all = 1, ok_b = 1, ok_f = 1, ok_c = 1;
    unsigned nbad = 0;
    if (!c->ffc_valid || !e->ffc_valid) return 1;
    if (c->ffc_ps) { atomic_fetch_add(&s_ffc_ps, 1); return 1; }
    atomic_fetch_add(&s_ffc_draws, 1);
    if (c->ffc_emit_fresh) atomic_fetch_add(&s_ffc_fresh, 1);
    if (!c->ffc_emit_seen) atomic_fetch_add(&s_ffc_noemit, 1);
    in = c->ffc_emit_seen ? &c->ffc_emit : &c->ffc_cur;
    fog = c->fog_emit_seen ? c->fog_emit : c->fog_cur;
    memset(&x, 0, sizeof x); memset(&o, 0, sizeof o);
    unres = d3d8_ff_combiners(in, &o) < 0;
    if (unres) atomic_fetch_add(&s_ffc_unres, 1);
    ffc_from_builder(&o, &x);
    d3d8_ff_texture_factor(c->tfactor, 0, &x.w[33], &x.w[41]);
    fin = d3d8_ff_final_combiner(fog[0], fog[1], fog[2], fog[3], &x.w[49], &x.w[50]);
    if (!fin) atomic_fetch_add(&s_ffc_final_skip, 1);
    /* Laziness, measured: would the state at the draw have given other words? */
    if (c->ffc_emit_seen) {
        memset(&xc, 0, sizeof xc); memset(&oc, 0, sizeof oc);
        d3d8_ff_combiners(&c->ffc_cur, &oc);
        ffc_from_builder(&oc, &xc);
        if (!ffc_builder_eq(&x, &xc)) {
            int m_emit = ffc_builder_eq(&x, &e->ffc), m_cur = ffc_builder_eq(&xc, &e->ffc);
            atomic_fetch_add(&s_ffc_lazy, 1);
            if (m_cur && !m_emit) atomic_fetch_add(&s_ffc_cur_only, 1);
            if (m_emit && !m_cur) atomic_fetch_add(&s_ffc_emit_only, 1);
        }
    }
    if (c->ffc_control) x.w[1] ^= 1u;       /* positive control: COLOR_ICW[0] must mismatch */
    if (expect) *expect = x;
    for (unsigned k = 0; k < D3D8_HOST_FFC_N; ++k) {
        if (k < FFC_BUILDER_END && unres) continue;
        if (k >= FFC_FACTOR_END && !fin) continue;
        if (x.w[k] == e->ffc.w[k]) continue;
        atomic_fetch_add(&s_ffc_word_mm[k], 1);
        ++nbad; all = 0;
        if (k < FFC_BUILDER_END) ok_b = 0; else if (k < FFC_FACTOR_END) ok_f = 0; else ok_c = 0;
    }
    if (!unres && ok_b) atomic_fetch_add(&s_ffc_builder, 1);
    if (ok_f) atomic_fetch_add(&s_ffc_factor, 1);
    if (fin && ok_c) atomic_fetch_add(&s_ffc_final, 1);
    if (all && !unres && fin) atomic_fetch_add(&s_ffc_all, 1);
    if (!all && atomic_fetch_add(&s_ffc_printed, 1) < 8) {
        char ib[1024], rb[1200], nm[32];
        unsigned st = x.w[0] & 0xFu, se = e->ffc.w[0] & 0xFu;
        if (se > st) st = se;
        if (!st) st = 1;
        if (st > 8) st = 8;
        ffc_fmt_inputs(ib, sizeof ib, in, c->tfactor, fog);
        fprintf(stderr, "[D3D8-MIRROR] draw %u ff combiner MISMATCH, %u registers%s: inputs (%s, %s) %s\n",
                c->serial, nbad, c->ffc_control ? " [control]" : "",
                c->ffc_emit_seen ? "as last emitted" : "at the draw, no emission seen",
                c->ffc_emit_fresh ? "emitted in this draw's flush" : "emitted by an earlier draw", ib);
        for (unsigned k = 0; k < D3D8_HOST_FFC_N; ++k) {
            if ((k < FFC_BUILDER_END && unres) || (k >= FFC_FACTOR_END && !fin) || x.w[k] == e->ffc.w[k]) continue;
            fprintf(stderr, "[D3D8-MIRROR]   %-18s %04X: d3d %08X | exec %08X\n",
                    d3d8_host_ffc_name(k, nm, sizeof nm), d3d8_host_ffc_method(k), x.w[k], e->ffc.w[k]);
        }
        ffc_fmt_regs(rb, sizeof rb, &x, st);
        fprintf(stderr, "[D3D8-MIRROR]   d3d  %s\n", rb);
        ffc_fmt_regs(rb, sizeof rb, &e->ffc, st);
        fprintf(stderr, "[D3D8-MIRROR]   exec %s\n", rb);
    }
    ffc_key(key, in, &o, unres, c->tfactor, fog);
    ffc_tally_add(key, &e->ffc);
    return all;
}

/* NV2A format byte from a D3D Format word, and the shape it implies. */
static void check_draw(const D3D8HostDrawCheck *c)
{
    D3D8ExecDrawTextures e;
    atomic_fetch_add(&s_chk, 1);
    if (!s_exec_source) return;
    memset(&e, 0, sizeof e);
    s_exec_source(&e);
    d3d8_host_check_streams(c, &e);
    /* G43: the combiner registers are latched method words, meaningful
     * whether or not the executor drew. The tally is printed cumulatively
     * every 100,000 fixed-function draws, because runs here end in kill -9
     * and an atexit print alone would never be seen. */
    d3d8_host_check_combiners(c, &e, NULL);
    {
        unsigned long long n = atomic_load(&s_ffc_draws);
        if (n && n % 100000u == 0 && c->ffc_valid && !c->ffc_ps) d3d8_host_ffc_tally_report("periodic", 20);
    }
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
    /* Fixed-function transform (execution mode 4). Measured 23 Sep: the NV2A
     * model-view register block is transpose(WORLD*VIEW) and the composite is
     * transpose(WORLD*VIEW*PROJECTION*VIEWPORT), D3D's row-vector matrices, with
     * VIEWPORT scaling x by W/2 and y by -H/2 about the centre (supersample
     * scaled) and z by 16777215*(MaxZ-MinZ) from 16777215*MinZ. Checked with a
     * relative tolerance: D3D composes in single precision in its own order. */
    if (c->vs_kind == 0 && e.exec_mode == 4u && (c->xf_seen & 7u) == 7u) {
        double wv[4][4], wvp[4][4], m[4][4], vp[4][4] = {{0}};
        const float *W = c->xf_world, *V = c->xf_view, *P = c->xf_proj;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
            double a = 0; for (int k = 0; k < 4; ++k) a += (double)W[i*4+k] * V[k*4+j]; wv[i][j] = a; }
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
            double a = 0; for (int k = 0; k < 4; ++k) a += wv[i][k] * P[k*4+j]; wvp[i][j] = a; }
        double sx = c->vp_w * 0.5 * c->ss_x, sy = c->vp_h * 0.5 * c->ss_y;
        vp[0][0] = sx; vp[1][1] = -sy; vp[2][2] = 16777215.0 * (c->vp_maxz - c->vp_minz); vp[3][3] = 1;
        vp[3][0] = c->vp_x * c->ss_x + sx; vp[3][1] = c->vp_y * c->ss_y + sy; vp[3][2] = 16777215.0 * c->vp_minz;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
            double a = 0; for (int k = 0; k < 4; ++k) a += wvp[i][k] * vp[k][j]; m[i][j] = a; }
        double worst_mv = 0, worst_c = 0, scale_mv = 1e-6, scale_c = 1e-6;
        for (int r = 0; r < 4; ++r) for (int q = 0; q < 4; ++q) {
            if (fabs(wv[q][r]) > scale_mv) scale_mv = fabs(wv[q][r]);
            if (fabs(m[q][r]) > scale_c) scale_c = fabs(m[q][r]);
        }
        for (int r = 0; r < 4; ++r) for (int q = 0; q < 4; ++q) {
            double d1 = fabs(wv[q][r] - e.ff_modelview[r*4+q]) / scale_mv;   /* register r*4+q = M[q][r] */
            double d2 = fabs(m[q][r] - e.ff_composite[r*4+q]) / scale_c;
            if (d1 > worst_mv) worst_mv = d1; if (d2 > worst_c) worst_c = d2;
        }
        atomic_fetch_add(&s_ff_cmp, 1);
        if (worst_mv <= 1e-4 && worst_c <= 1e-3) atomic_fetch_add(&s_ff_match, 1);
        else {
            if (worst_mv > 1e-4) atomic_fetch_add(&s_ff_mv, 1); else atomic_fetch_add(&s_ff_comp, 1);
            if (atomic_fetch_add(&s_ff_printed, 1) < 8)
                fprintf(stderr, "[D3D8-MIRROR] draw %u fixed-function transform MISMATCH: worst relative error model-view %.3g composite %.3g"
                                " | expected composite row0 %g %g %g %g, exec %g %g %g %g\n", c->serial, worst_mv, worst_c,
                        m[0][0], m[1][0], m[2][0], m[3][0], e.ff_composite[0], e.ff_composite[1], e.ff_composite[2], e.ff_composite[3]);
        }
    } else if (c->vs_kind == 0) atomic_fetch_add(&s_ff_other, 1);
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
    o->ff_compared = atomic_load(&s_ff_cmp); o->ff_match = atomic_load(&s_ff_match);
    o->ff_mv = atomic_load(&s_ff_mv); o->ff_comp = atomic_load(&s_ff_comp); o->ff_other = atomic_load(&s_ff_other);
    o->va_draws = atomic_load(&s_va_draws); o->va_draws_all_match = atomic_load(&s_va_all);
    o->va_arrays = atomic_load(&s_va_arrays); o->va_exact = atomic_load(&s_va_exact);
    o->va_in_stream = atomic_load(&s_va_in); o->va_no_d3d = atomic_load(&s_va_nod3d);
    o->va_stride = atomic_load(&s_va_stride); o->va_offset = atomic_load(&s_va_offset);
    o->va_format = atomic_load(&s_va_format); o->va_exec_missing = atomic_load(&s_va_exmiss);
    o->idx_draws = atomic_load(&s_ix_draws); o->idx_match = atomic_load(&s_ix_match);
    o->idx_count_bad = atomic_load(&s_ix_count); o->idx_value_bad = atomic_load(&s_ix_value);
    o->idx_indexed = atomic_load(&s_ix_indexed); o->idx_indexed_match = atomic_load(&s_ix_indexed_match);
    o->hk_stream_cmp = atomic_load(&s_hk_st_cmp); o->hk_stream_match = atomic_load(&s_hk_st_match);
    o->hk_ib_cmp = atomic_load(&s_hk_ib_cmp); o->hk_ib_match = atomic_load(&s_hk_ib_match);
    o->ffc_draws = atomic_load(&s_ffc_draws); o->ffc_ps_skipped = atomic_load(&s_ffc_ps);
    o->ffc_all_match = atomic_load(&s_ffc_all); o->ffc_builder_match = atomic_load(&s_ffc_builder);
    o->ffc_factor_match = atomic_load(&s_ffc_factor); o->ffc_final_match = atomic_load(&s_ffc_final);
    o->ffc_final_skipped = atomic_load(&s_ffc_final_skip); o->ffc_unresolved = atomic_load(&s_ffc_unres);
    o->ffc_no_emit = atomic_load(&s_ffc_noemit); o->ffc_fresh = atomic_load(&s_ffc_fresh);
    o->ffc_lazy_differs = atomic_load(&s_ffc_lazy); o->ffc_exec_cur_only = atomic_load(&s_ffc_cur_only);
    o->ffc_exec_emit_only = atomic_load(&s_ffc_emit_only);
    for (unsigned k = 0; k < D3D8_HOST_FFC_N; ++k) o->ffc_word_mismatch[k] = atomic_load(&s_ffc_word_mm[k]);
    o->ffc_tally_overflow = atomic_load(&s_ffc_tally_overflow);
    ffc_lock(); o->ffc_tally_pairs = s_ffc_tally_used; ffc_unlock();
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
        fprintf(stderr, "[D3D8-MIRROR] %s fixed-function transform: mode-4 draws compared=%llu MATCH=%llu (model-view %llu, composite %llu)"
                        " | other fixed-function draws %llu\n", why, st.ff_compared, st.ff_match, st.ff_mv, st.ff_comp, st.ff_other);
        fprintf(stderr, "[D3D8-MIRROR] %s texture stages: units compared=%llu MATCH=%llu (address %llu, mag %llu, min/mip %llu, lod bias %llu)\n",
                why, st.tss_compared, st.tss_match, st.tss_addr, st.tss_mag, st.tss_min, st.tss_bias);
        fprintf(stderr, "[D3D8-MIRROR] %s streams: draws %llu, all arrays matching in %llu | executor arrays=%llu EXACT=%llu"
                        " inside-a-bound-stream=%llu (no D3D stream %llu, stride %llu, format %llu, offset %llu)"
                        " | D3D-enabled arrays the executor lacks %llu\n", why, st.va_draws, st.va_draws_all_match,
                st.va_arrays, st.va_exact, st.va_in_stream, st.va_no_d3d, st.va_stride, st.va_format, st.va_offset,
                st.va_exec_missing);
        fprintf(stderr, "[D3D8-MIRROR] %s indices: draws %llu MATCH=%llu (count %llu, values %llu) | DrawIndexedVertices %llu MATCH=%llu\n",
                why, st.idx_draws, st.idx_match, st.idx_count_bad, st.idx_value_bad, st.idx_indexed, st.idx_indexed_match);
        fprintf(stderr, "[D3D8-MIRROR] %s stream hooks: SetStreamSource bindings compared=%llu MATCH=%llu"
                        " | SetIndices compared=%llu MATCH=%llu\n", why, st.hk_stream_cmp, st.hk_stream_match,
                st.hk_ib_cmp, st.hk_ib_match);
        fprintf(stderr, "[D3D8-MIRROR] %s ff combiners: fixed-function draws %llu (pixel-shader draws skipped %llu),"
                        " all registers matching in %llu | builder CONTROL+COLOR/ALPHA ICW/OCW MATCH=%llu,"
                        " FACTOR0/1 MATCH=%llu, SPECULAR_FOG_CW0/1 MATCH=%llu (not written %llu)"
                        " | unresolved %llu, before any emission %llu\n", why, st.ffc_draws, st.ffc_ps_skipped,
                st.ffc_all_match, st.ffc_builder_match, st.ffc_factor_match, st.ffc_final_match, st.ffc_final_skipped,
                st.ffc_unresolved, st.ffc_no_emit);
        fprintf(stderr, "[D3D8-MIRROR] %s ff combiner laziness: builder ran in this draw's flush %llu of %llu;"
                        " state at the draw transcribes differently from the last emission in %llu"
                        " (executor matches the draw-time state only %llu, the last emission only %llu)"
                        " | distinct setups %u\n", why, st.ffc_fresh, st.ffc_draws, st.ffc_lazy_differs,
                st.ffc_exec_cur_only, st.ffc_exec_emit_only, st.ffc_tally_pairs);
        for (unsigned k = 0; k < D3D8_HOST_FFC_N; ++k)
            if (st.ffc_word_mismatch[k]) {
                char nm[32];
                fprintf(stderr, "[D3D8-MIRROR] %s ff combiner reg %04X %s: %llu mismatches\n", why,
                        d3d8_host_ffc_method(k), d3d8_host_ffc_name(k, nm, sizeof nm), st.ffc_word_mismatch[k]);
            }
        for (unsigned k = 0; k < D3D8_HOST_PS_N; ++k)
            if (st.ps_word_mismatch[k])
                fprintf(stderr, "[D3D8-MIRROR] %s pixel shader reg %04X (def word %u): %llu mismatches\n",
                        why, k_ps_pairs[k][0], k_ps_pairs[k][1], st.ps_word_mismatch[k]);
        for (unsigned k = 0; k < 11; ++k)
            fprintf(stderr, "[D3D8-MIRROR] %s state %04X: compared=%llu MATCH=%llu never-pushed-by-D3D=%llu\n",
                    why, k_state_methods[k], st.st_compared[k], st.st_match[k], st.st_unseen[k]);
    }
}
