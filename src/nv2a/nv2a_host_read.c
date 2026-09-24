/* G56 DEFER-SAFE: pay GPU-held guest memory before a CPU reader sees it.
 *
 * WHY. RECOMP_METAL_DEFER_SWAP leaves a swapped-away surface's pixels on the
 * GPU and guest RAM behind them, and NO_DEPTH_SYNC never writes depth back;
 * even with both off, the BOUND render target is ahead of guest RAM for the
 * whole frame. The executor pays for its own texture reads. A CPU reader --
 * the title through D3D's LockRect, CopyRects, GetBackBuffer or
 * GetDepthStencilSurface -- was not intercepted, and the debt watch (36ad170)
 * measured none in the tutorial, which proves only the scenes tested. This
 * makes those entries correct in every scene: the mirror's hooks call
 * nv2a_host_read_request with the resource's memory before the title can
 * touch it.
 *
 * WHY A MAILBOX. The payment is a GPU read-back on the executor's Metal state,
 * which lives on the pusher thread (draw_thread_check). The guest thread posts
 * the range and waits; the pusher loop, which polls continuously, services it
 * -- and only once it has caught up with the push buffer, so every draw the
 * title issued before the lock is in the bytes it gets.
 *
 * DEADLOCK. The pusher can be parked in a software-method spin waiting on the
 * guest; a guest thread parked here would then wait for it. The wait is
 * bounded (timeout_ms) and a timeout is counted per entry, so the worst case
 * is a stale read that the report names, never a hang. One request at a time:
 * a second guest thread queues on the mutex. */
#include "nv2a_host_read.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern int nv2a_metal_make_current(uint8_t *p, size_t bytes);

static pthread_t s_service; static _Atomic int s_have_service;
static pthread_mutex_t s_one = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_p; static size_t s_bytes;
static _Atomic int s_state;               /* 0 idle, 1 posted, 2 done */
static int s_result;
static _Atomic unsigned long long s_req[NV2A_HOST_READ_ENTRIES], s_paid[NV2A_HOST_READ_ENTRIES], s_timeout[NV2A_HOST_READ_ENTRIES];
static _Atomic unsigned long long s_bits[4];
static const char *const k_entry[NV2A_HOST_READ_ENTRIES] = {
    "D3DSurface_LockRect", "D3DTexture_LockRect", "D3DDevice_CopyRects (source)",
    "D3DDevice_GetBackBuffer", "D3DDevice_GetDepthStencilSurface", "test"
};

void nv2a_host_read_set_service_thread(void) { s_service = pthread_self(); atomic_store(&s_have_service, 1); }

static void note(unsigned entry, int r)
{
    if (r > 0) atomic_fetch_add(&s_paid[entry], 1);
    for (int b = 0; b < 4; ++b) if (r > 0 && (r & (1 << b))) atomic_fetch_add(&s_bits[b], 1);
}

int nv2a_host_read_request(uint8_t *p, size_t bytes, unsigned entry, unsigned timeout_ms)
{
    struct timespec t0, t;
    int r;
    if (entry >= NV2A_HOST_READ_ENTRIES) entry = NV2A_HOST_READ_TEST;
    atomic_fetch_add(&s_req[entry], 1);
    if (!atomic_load(&s_have_service)) return -2;
    if (pthread_equal(pthread_self(), s_service)) { r = nv2a_metal_make_current(p, bytes); note(entry, r); return r; }
    pthread_mutex_lock(&s_one);
    s_p = p; s_bytes = bytes;
    atomic_store_explicit(&s_state, 1, memory_order_release);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (atomic_load_explicit(&s_state, memory_order_acquire) == 2) { r = s_result; break; }
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= (long)timeout_ms) {
            /* Withdraw it -- unless the service took it in the meantime. */
            int expect = 1;
            if (atomic_compare_exchange_strong(&s_state, &expect, 0)) { atomic_fetch_add(&s_timeout[entry], 1); r = -1; break; }
            continue;
        }
        { struct timespec ns = { 0, 50000 }; nanosleep(&ns, NULL); }
    }
    atomic_store(&s_state, 0);
    pthread_mutex_unlock(&s_one);
    note(entry, r);
    return r;
}

void nv2a_host_read_service(int caught_up)
{
    int expect = 1;
    if (!caught_up || atomic_load_explicit(&s_state, memory_order_acquire) != 1) return;
    /* Claim it (the requester may be withdrawing it on timeout). */
    if (!atomic_compare_exchange_strong(&s_state, &expect, 3)) return;
    s_result = nv2a_metal_make_current(s_p, s_bytes);
    atomic_store_explicit(&s_state, 2, memory_order_release);
}

void nv2a_host_read_counts(unsigned long long out[NV2A_HOST_READ_ENTRIES][3])
{
    for (int i = 0; i < NV2A_HOST_READ_ENTRIES; ++i) {
        out[i][0] = atomic_load(&s_req[i]); out[i][1] = atomic_load(&s_paid[i]); out[i][2] = atomic_load(&s_timeout[i]);
    }
}

void nv2a_host_read_report(void)
{
    int any = 0;
    for (int i = 0; i < NV2A_HOST_READ_ENTRIES; ++i) {
        unsigned long long q = atomic_load(&s_req[i]);
        if (!q) continue;
        if (!any) { fprintf(stderr, "[HOST-READ] CPU readers of GPU memory made current first (G56):"); any = 1; }
        fprintf(stderr, " %s requests %llu, something to pay %llu, TIMED OUT (read stale) %llu;", k_entry[i], q,
                atomic_load(&s_paid[i]), atomic_load(&s_timeout[i]));
    }
    if (any)
        fprintf(stderr, " | owed slots written back %llu, bound colour written back %llu, depth read back %llu,"
                        " ranges on the bound colour surface %llu\n", atomic_load(&s_bits[0]), atomic_load(&s_bits[1]),
                atomic_load(&s_bits[2]), atomic_load(&s_bits[3]));
}
