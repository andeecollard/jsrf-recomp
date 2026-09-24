/* RECOMP_METAL_DEBT_WATCH, end to end in one process: two views of a guarded
 * range (standing in for the low and high views of GPU memory), a "runtime"
 * thread that arms debts and touches the low view, and a "guest" thread that
 * touches either. Every access must complete (the watch releases and the
 * instruction re-runs) and be counted on the right side. */
#include "nv2a_debt_watch.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: " __VA_ARGS__); printf("\n"); } else { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
enum { SZ = 1 << 20, HEAP_LO = 0x40000, RANGE = 0x20000 };
static volatile uint8_t *g_low, *g_high;
static volatile int g_sink;
static void *guest_read_high(void *a) { (void)a; g_sink = g_high[HEAP_LO + 100]; return NULL; }
static void *guest_write_high(void *a) { (void)a; g_high[HEAP_LO + 5] = 7; return NULL; }
static void *guest_read_low(void *a) { (void)a; g_sink = g_low[HEAP_LO + 200]; return NULL; }
static int g_ran;
static void sum_fn(const uint8_t *p, size_t n, void *ctx) { (void)ctx; unsigned t = 0; for (size_t i = 0; i < n; ++i) t += p[i]; g_sink = (int)t; g_ran = 1; }
static void *probe_guarded(void *a) { (void)a; g_ran = nv2a_debt_watch_guarded_read((const uint8_t *)g_low + HEAP_LO, 4096, sum_fn, NULL) ? g_ran : -1; return NULL; }
static void run(void *(*f)(void *)) { pthread_t t; pthread_create(&t, NULL, f, NULL); pthread_join(t, NULL); }

int main(void)
{
    unsigned long long c[NV2A_DEBT_WATCH_COUNTS], d[NV2A_DEBT_WATCH_COUNTS];
    setenv("RECOMP_METAL_DEBT_WATCH", "1", 1);
    g_low = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    g_high = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g_low == MAP_FAILED || g_high == MAP_FAILED) return 2;
    nv2a_debt_watch_configure((uintptr_t)g_low, (uintptr_t)g_high, HEAP_LO, SZ);
    CHECK(nv2a_debt_watch_on(), "watch configured");

    /* 1. A guest read through the high view while owed. */
    nv2a_debt_watch_arm((const uint8_t *)g_low + HEAP_LO, RANGE, NV2A_DEBT_COLOUR);
    nv2a_debt_watch_counts(c);
    run(guest_read_high);
    nv2a_debt_watch_counts(d);
    CHECK(d[0] == c[0] && d[2] == c[2] + 1 && d[3] == c[3], "a guest read of the high view is counted (reads +%llu)", d[2] - c[2]);
    run(guest_read_high);
    nv2a_debt_watch_counts(c);
    CHECK(c[2] == d[2], "released after the first access: the second read is free");

    /* 2. The runtime's own low-view write keeps the guest's view guarded. */
    nv2a_debt_watch_arm((const uint8_t *)g_low + HEAP_LO, RANGE, NV2A_DEBT_COLOUR);
    nv2a_debt_watch_counts(c);
    g_low[HEAP_LO + 10] = 3;                        /* this thread armed it: the runtime */
    nv2a_debt_watch_counts(d);
    CHECK(d[5] == c[5] + 1 && d[3] == c[3], "the runtime's low-view write is counted as the runtime's");
    run(guest_write_high);
    nv2a_debt_watch_counts(c);
    CHECK(c[3] == d[3] + 1 && g_high[HEAP_LO + 5] == 7, "the high view stayed guarded: a guest write after it is caught, and lands");

    /* 3. Another thread on the low view is not the runtime. */
    nv2a_debt_watch_arm((const uint8_t *)g_low + HEAP_LO, RANGE, NV2A_DEBT_DEPTH);
    nv2a_debt_watch_counts(c);
    run(guest_read_low);
    nv2a_debt_watch_counts(d);
    CHECK(d[8] == c[8] + 1, "depth: a read of the low view from another thread is counted as the guest's");

    /* 4. Paid before anyone looked. */
    nv2a_debt_watch_arm((const uint8_t *)g_low + HEAP_LO, RANGE, NV2A_DEBT_COLOUR);
    nv2a_debt_watch_counts(c);
    nv2a_debt_watch_paid((const uint8_t *)g_low + HEAP_LO, RANGE);
    run(guest_read_high);
    nv2a_debt_watch_counts(d);
    CHECK(d[1] == c[1] + 1 && d[2] == c[2], "paid first: counted clean, and the later read is not guarded");

    /* 5. A runtime reader on another thread (the framebuffer probe) reads under
     *    the watch's lock: skipped while a debt is armed, run once it is paid,
     *    and never a fault either way. */
    nv2a_debt_watch_arm((const uint8_t *)g_low + HEAP_LO, RANGE, NV2A_DEBT_COLOUR);
    nv2a_debt_watch_counts(c);
    g_ran = 0; run(probe_guarded);
    nv2a_debt_watch_counts(d);
    CHECK(g_ran == -1 && d[2] == c[2], "guarded read: skipped while the range is owed, no fault counted");
    nv2a_debt_watch_paid((const uint8_t *)g_low + HEAP_LO, RANGE);
    g_ran = 0; run(probe_guarded);
    CHECK(g_ran == 1, "guarded read: runs once the debt is paid");

    /* 6. CONTROL: an unwatched address still reaches the old handler -- here, none,
     *    so only check that unarmed memory is untouched by the watch. */
    g_high[0] = 1; g_low[0] = 1;
    CHECK(g_high[0] == 1 && g_low[0] == 1, "memory outside every debt is never guarded");
    nv2a_debt_watch_report();
    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
