/* See recomp_frame_split.h. POSIX only (Windows gets the header's no-ops). */
#if !defined(_WIN32)
#include "recomp_frame_split.h"
#include "../recomp_switch.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int g_recomp_fs = -1;
int recomp_fs_read(void)
{
    int on = recomp_switch_on("RECOMP_FRAME_SPLIT");
    if (on && g_recomp_fs < 0)
        fprintf(stderr, "[FRAME-SPLIT] RECOMP_FRAME_SPLIT=1: the title's thread, the pusher and the GPU timed"
                        " per frame (G76; adds ~40 ns per timed boundary to the thread it times)\n");
    g_recomp_fs = on;
    return on;
}

unsigned long long recomp_fs_now(void)
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

static _Atomic unsigned long long s_win[RFS_N], s_win_n[RFS_N];
void recomp_fs_add(unsigned b, unsigned long long ns)
{
    if (b >= RFS_N) return;
    atomic_fetch_add_explicit(&s_win[b], ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_win_n[b], 1, memory_order_relaxed);
}

static _Thread_local int t_title, t_depth;
int recomp_fs_is_title(void) { return t_title; }
void recomp_fs_add_t(unsigned b, unsigned long long ns) { recomp_fs_add(t_title ? b : RFS_O_INNER, ns); }
int recomp_fs_in_d3d(void) { return t_depth > 0; }

#define KORD 400u
static _Atomic unsigned long long s_kern[KORD], s_kern_n[KORD];
void recomp_fs_kernel(unsigned ordinal, unsigned long long ns)
{
    recomp_fs_add(t_depth > 0 ? RFS_T_KERNEL_D3D : RFS_T_KERNEL, ns);
    if (ordinal < KORD) {
        atomic_fetch_add_explicit(&s_kern[ordinal], ns, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_kern_n[ordinal], 1, memory_order_relaxed);
    }
}

#define NENT 160u
static const char *const *s_names;
static const uint32_t *s_addrs;
static unsigned s_nent, s_swap = ~0u;
static _Atomic unsigned long long s_ent[NENT], s_ent_n[NENT], s_oent[NENT], s_oent_n[NENT];
void recomp_fs_d3d_names(const char *const *names, const uint32_t *addrs, unsigned n, unsigned swap_idx)
{
    s_names = names; s_addrs = addrs; s_nent = n < NENT ? n : NENT; s_swap = swap_idx;
}
unsigned long long recomp_fs_d3d_enter(unsigned idx)
{
    if (idx == s_swap) t_title = 1;
    return t_depth++ == 0 ? recomp_fs_now() : 0;
}
void recomp_fs_d3d_exit(unsigned idx, unsigned long long t0)
{
    --t_depth;
    if (!t0) return;
    {   unsigned long long ns = recomp_fs_now() - t0;
        recomp_fs_add(t_title ? RFS_T_D3D : RFS_O_D3D, ns);
        if (idx < s_nent) {
            atomic_fetch_add_explicit(t_title ? &s_ent[idx] : &s_oent[idx], ns, memory_order_relaxed);
            atomic_fetch_add_explicit(t_title ? &s_ent_n[idx] : &s_oent_n[idx], 1, memory_order_relaxed);
        } }
}

static double take(_Atomic unsigned long long *p) { return (double)atomic_exchange_explicit(p, 0, memory_order_relaxed); }

void recomp_fs_report(unsigned long long frames, unsigned long long frame_us)
{
    double v[RFS_N], n[RFS_N], f = (double)frames, frame_ms;
    if (!recomp_fs_on() || !frames) return;
    for (unsigned b = 0; b < RFS_N; ++b) { v[b] = take(&s_win[b]) / 1e6 / f; n[b] = take(&s_win_n[b]) / f; }
    frame_ms = (double)frame_us / 1000.0 / f;
    fprintf(stderr, "  [FRAME-SPLIT] per frame over %llu flips (%.2f ms): title: game %.2f ms, d3d %.2f ms (%.0f calls;"
                    " of it waiting on the fence %.2f (%.0f), ring space %.2f (%.0f), state flush %.2f (%.0f), kickoff"
                    " %.2f (%.0f), mirror hooks %.2f, kernel inside %.2f), kernel outside d3d %.2f ms (%.0f calls)\n",
            frames, frame_ms, frame_ms - v[RFS_T_D3D] - v[RFS_T_KERNEL], v[RFS_T_D3D], n[RFS_T_D3D],
            v[RFS_T_WAIT], n[RFS_T_WAIT], v[RFS_T_SPACE], n[RFS_T_SPACE], v[RFS_T_STATE], n[RFS_T_STATE],
            v[RFS_T_KICK], n[RFS_T_KICK], v[RFS_T_HOOKS], v[RFS_T_KERNEL_D3D], v[RFS_T_KERNEL], n[RFS_T_KERNEL]);
    fprintf(stderr, "  [FRAME-SPLIT] per frame: other threads' d3d %.2f ms (%.0f calls; waits and flushes inside %.2f ms)\n",
            v[RFS_O_D3D], n[RFS_O_D3D], v[RFS_O_INNER]);
    fprintf(stderr, "  [FRAME-SPLIT] per frame: pusher: host tokens check %.2f ms (%.0f), replace %.2f ms (%.0f:"
                    " FF registers %.2f, build %.2f (%.0f; vertex fetch %.2f), host encode %.2f (%.0f)),"
                    " pre %.2f ms (%.0f); waiting for a software method's acknowledgement %.2f ms (%.0f) | gpu:"
                    " command buffers covered %.2f ms (%.0f completed)\n",
            v[RFS_P_CHECK], n[RFS_P_CHECK], v[RFS_P_REPLACE], n[RFS_P_REPLACE], v[RFS_P_REGS], v[RFS_P_BUILD],
            n[RFS_P_BUILD], v[RFS_P_FETCH], v[RFS_P_ENCODE], n[RFS_P_ENCODE], v[RFS_P_PRE], n[RFS_P_PRE],
            v[RFS_P_SWM], n[RFS_P_SWM], v[RFS_GPU], n[RFS_GPU]);
    {   /* The top D3D entry points and kernel ordinals by time, this window. */
        double ev[NENT], en[NENT], ov[NENT], on[NENT], kv[KORD], kn[KORD];
        for (unsigned i = 0; i < s_nent; ++i) { ev[i] = take(&s_ent[i]) / 1e6 / f; en[i] = take(&s_ent_n[i]) / f;
                                                ov[i] = take(&s_oent[i]) / 1e6 / f; on[i] = take(&s_oent_n[i]) / f; }
        for (unsigned i = 0; i < KORD; ++i) { kv[i] = take(&s_kern[i]) / 1e6 / f; kn[i] = take(&s_kern_n[i]) / f; }
        fprintf(stderr, "  [FRAME-SPLIT]   d3d by entry (ms, calls a frame):");
        for (unsigned r = 0; r < 12; ++r) {
            unsigned best = ~0u;
            for (unsigned i = 0; i < s_nent; ++i) if (ev[i] > 0.0 && (best == ~0u || ev[i] > ev[best])) best = i;
            if (best == ~0u) break;
            fprintf(stderr, " %s %.3f (%.0f);", s_names ? s_names[best] : "?", ev[best], en[best]);
            ev[best] = -1.0;
        }
        fprintf(stderr, "\n  [FRAME-SPLIT]   other threads' d3d by entry (ms, calls a frame):");
        for (unsigned r = 0; r < 4; ++r) {
            unsigned best = ~0u;
            for (unsigned i = 0; i < s_nent; ++i) if (ov[i] > 0.0 && (best == ~0u || ov[i] > ov[best])) best = i;
            if (best == ~0u) break;
            fprintf(stderr, " %s %.3f (%.0f);", s_names ? s_names[best] : "?", ov[best], on[best]);
            ov[best] = -1.0;
        }
        fprintf(stderr, "\n  [FRAME-SPLIT]   title kernel by ordinal (ms, calls a frame):");
        for (unsigned r = 0; r < 8; ++r) {
            unsigned best = ~0u;
            for (unsigned i = 0; i < KORD; ++i) if (kv[i] > 0.0 && (best == ~0u || kv[i] > kv[best])) best = i;
            if (best == ~0u) break;
            fprintf(stderr, " %u %.3f (%.0f);", best, kv[best], kn[best]);
            kv[best] = -1.0;
        }
        fprintf(stderr, "\n");
    }
}
#else /* _WIN32 */
/* RECOMP_FRAME_SPLIT is POSIX-only (G76), and the header's inline stubs keep
 * it off on Windows. The D3D census overlay and the lifted gen declare these
 * entry points themselves rather than include the header, so this host needs
 * them as functions too: every one a no-op, the instrument reading off. This
 * branch does not include the header, whose inline stubs would collide. */
#include <stdint.h>
int recomp_fs_read(void) { return 0; }
unsigned long long recomp_fs_now(void) { return 0; }
void recomp_fs_add_t(unsigned bucket, unsigned long long ns) { (void)bucket; (void)ns; }
void recomp_fs_d3d_names(const char *const *names, const uint32_t *addrs, unsigned n, unsigned swap_idx)
{ (void)names; (void)addrs; (void)n; (void)swap_idx; }
unsigned long long recomp_fs_d3d_enter(unsigned idx) { (void)idx; return 0; }
void recomp_fs_d3d_exit(unsigned idx, unsigned long long t0) { (void)idx; (void)t0; }
#endif
