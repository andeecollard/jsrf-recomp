/* G54: the drop registry. See nv2a_drop.h. */
#include "nv2a_drop.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define DROP_REASONS 64
#define DROP_STATES 4
typedef struct {
    const char *stage, *reason;
    int kind;
    unsigned long long total, reported;         /* reported: the total at the last report */
    uint32_t first_detail;
    unsigned gen;                               /* the window the states below belong to */
    unsigned nstates;
    NV2ADropState states[DROP_STATES];
    uint32_t details[DROP_STATES];
} DropReason;

static DropReason s_r[DROP_REASONS];
static unsigned s_n;
static unsigned long long s_overflow;
static unsigned long long s_batches, s_flips, s_batches_rep, s_flips_rep;
static unsigned s_gen;
static void (*s_fill)(NV2ADropState *);
static const char *const k_kind[3] = { "DROPPED", "SIMPLIFIED", "PARTIAL" };

void nv2a_drop_set_state_source(void (*fill)(NV2ADropState *)) { s_fill = fill; }
void nv2a_drop_batch(void) { ++s_batches; }
void nv2a_drop_flip(void) { ++s_flips; }

static DropReason *find(const char *stage, const char *reason, int kind)
{
    unsigned i;
    for (i = 0; i < s_n; ++i)                    /* literals: the pointer is the key */
        if (s_r[i].reason == reason && s_r[i].stage == stage && s_r[i].kind == kind) return &s_r[i];
    for (i = 0; i < s_n; ++i)                    /* the same text from another literal */
        if (s_r[i].kind == kind && !strcmp(s_r[i].reason, reason) && !strcmp(s_r[i].stage, stage)) return &s_r[i];
    return NULL;
}

void nv2a_drop(int kind, const char *stage, const char *reason, uint32_t detail)
{
    DropReason *r;
    NV2ADropState st;
    if (!reason) reason = "(unnamed)";
    if (!stage) stage = "?";
    if (kind < 0 || kind > 2) kind = 0;
    r = find(stage, reason, kind);
    if (!r) {
        if (s_n >= DROP_REASONS) { ++s_overflow; return; }
        r = &s_r[s_n];
        memset(r, 0, sizeof *r);
        r->stage = stage; r->reason = reason; r->kind = kind; r->first_detail = detail; r->gen = s_gen;
        memset(&st, 0, sizeof st);
        if (s_fill) s_fill(&st);
        /* A first line per reason, naming the state, as [FOG] does. */
        fprintf(stderr, "[DROP] first %s, %s: %s (detail 0x%X) at draw %u -- CW0 %08X texture modes %08X tex0 format %08X"
                        " transform MODE %u primitive %u\n", k_kind[kind], stage, reason, detail, st.draw, st.cw0,
                st.texmodes, st.tex0_format, st.mode_prim & 0xFFu, (st.mode_prim >> 8) & 0xFFu);
        fflush(stderr);
        s_n++;                                   /* published after it is filled */
    }
    ++r->total;
    if (kind == NV2A_DROP_PARTIAL) return;       /* per triangle: no per-draw states */
    if (r->gen != s_gen) { r->gen = s_gen; r->nstates = 0; }
    if (r->nstates < DROP_STATES) {
        unsigned k;
        memset(&st, 0, sizeof st);
        if (s_fill) s_fill(&st);
        for (k = 0; k < r->nstates; ++k)
            if (r->states[k].cw0 == st.cw0 && r->states[k].texmodes == st.texmodes
             && r->states[k].tex0_format == st.tex0_format && r->states[k].mode_prim == st.mode_prim
             && r->details[k] == detail) break;
        if (k == r->nstates) { r->states[k] = st; r->details[k] = detail; r->nstates++; }
    }
}

void nv2a_drop_report(const char *why)
{
    unsigned long long batches = s_batches - s_batches_rep, flips = s_flips - s_flips_rep;
    unsigned n = s_n, i, printed = 0;
    char line[4096];
    size_t used;
    used = (size_t)snprintf(line, sizeof line, "[DROP] %s window: %llu flips, %llu batches |", why ? why : "report",
                            flips, batches);
    for (i = 0; i < n; ++i) {
        DropReason *r = &s_r[i];
        unsigned long long d = r->total - r->reported;
        if (!d) continue;
        ++printed;
        if (used < sizeof line && r->kind == NV2A_DROP_PARTIAL)
            used += (size_t)snprintf(line + used, sizeof line - used, " %s %s: %s = %llu (%.2f/flip, triangles);",
                                     k_kind[r->kind], r->stage, r->reason, d, flips ? (double)d / (double)flips : 0.0);
        else if (used < sizeof line)
            used += (size_t)snprintf(line + used, sizeof line - used, " %s %s: %s = %llu (%.2f/flip, %.2f%% of batches);",
                                     k_kind[r->kind], r->stage, r->reason, d, flips ? (double)d / (double)flips : 0.0,
                                     batches ? 100.0 * (double)d / (double)batches : 0.0);
    }
    if (!printed && used < sizeof line) used += (size_t)snprintf(line + used, sizeof line - used, " nothing dropped");
    if (s_overflow && used < sizeof line)
        snprintf(line + used, sizeof line - used, " (+%llu drops under reasons past the table's %u)", s_overflow, DROP_REASONS);
    fprintf(stderr, "%s\n", line);
    for (i = 0; i < n; ++i) {
        DropReason *r = &s_r[i];
        unsigned long long d = r->total - r->reported;
        if (r->kind != NV2A_DROP_PARTIAL && batches && d * 100u > batches) {
            fprintf(stderr, "[DROP] WARNING %s %s: %s -- %llu of %llu batches (%.1f%%) this window; states:\n",
                    k_kind[r->kind], r->stage, r->reason, d, batches, 100.0 * (double)d / (double)batches);
            for (unsigned k = 0; k < r->nstates && r->gen == s_gen; ++k)
                fprintf(stderr, "[DROP]   draw %u: CW0 %08X texture modes %08X tex0 format %08X transform MODE %u primitive %u"
                                " detail 0x%X\n", r->states[k].draw, r->states[k].cw0, r->states[k].texmodes,
                        r->states[k].tex0_format, r->states[k].mode_prim & 0xFFu, (r->states[k].mode_prim >> 8) & 0xFFu,
                        r->details[k]);
        }
        r->reported = r->total;
    }
    s_batches_rep = s_batches; s_flips_rep = s_flips;
    ++s_gen;
    fflush(stderr);
}

static unsigned clip_poly(float (*in)[2], unsigned n, float (*out)[2], int axis, float lim, int keep_below)
{
    unsigned m = 0;
    for (unsigned i = 0; i < n; ++i) {
        const float *p = in[i], *q = in[(i + 1) % n];
        int pin = keep_below ? p[axis] <= lim : p[axis] >= lim, qin = keep_below ? q[axis] <= lim : q[axis] >= lim;
        if (pin) { out[m][0] = p[0]; out[m][1] = p[1]; ++m; }
        if (pin != qin) {
            float t = (lim - p[axis]) / (q[axis] - p[axis]);
            out[m][0] = p[0] + t * (q[0] - p[0]); out[m][1] = p[1] + t * (q[1] - p[1]); ++m;
        }
    }
    return m;
}
float nv2a_clipped_triangle_area(const float a[4], const float b[4], const float c[4], float w, float h)
{
    float p0[8][2], p1[8][2];
    unsigned n = 3;
    double area = 0;
    if (!(a[3] > 0 && b[3] > 0 && c[3] > 0)) return 0;
    if (!isfinite(a[0]) || !isfinite(a[1]) || !isfinite(b[0]) || !isfinite(b[1]) || !isfinite(c[0]) || !isfinite(c[1])) return 0;
    p0[0][0] = a[0]; p0[0][1] = a[1]; p0[1][0] = b[0]; p0[1][1] = b[1]; p0[2][0] = c[0]; p0[2][1] = c[1];
    n = clip_poly(p0, n, p1, 0, 0.0f, 0); if (n < 3) return 0;
    n = clip_poly(p1, n, p0, 0, w, 1);    if (n < 3) return 0;
    n = clip_poly(p0, n, p1, 1, 0.0f, 0); if (n < 3) return 0;
    n = clip_poly(p1, n, p0, 1, h, 1);    if (n < 3) return 0;
    for (unsigned i = 0; i < n; ++i) area += (double)p0[i][0] * p0[(i + 1) % n][1] - (double)p0[(i + 1) % n][0] * p0[i][1];
    return (float)fabs(area) * 0.5f;
}

unsigned long long nv2a_drop_count(const char *reason)
{
    unsigned long long t = 0;
    for (unsigned i = 0; i < s_n; ++i) if (!strcmp(s_r[i].reason, reason)) t += s_r[i].total;
    return t;
}
