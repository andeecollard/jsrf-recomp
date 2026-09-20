#include "irq_latency.h"
#include "../recomp_switch.h"
/* LARGE_INTEGER and QueryPerformanceCounter/Frequency are Win32. This host
 * reaches them through the POSIX shim that irq_latency.h drags in; a real
 * Windows build has to be told, or they are implicitly declared and QuadPart
 * is a member of nothing. */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <time.h>

#define NBUCKET 16u

static unsigned long long g_pending_us;   /* 0 = line low / already delivered */
static unsigned long      g_bucket[NBUCKET];
static unsigned long      g_count;
static unsigned long      g_lost;         /* line dropped before any ISR ran */
static unsigned long long g_max_us;
static unsigned long long g_total_us;
static int                g_on = -1;
static int                g_forced = -1;

int recomp_irq_latency_on(void)
{
    if (g_forced >= 0) return g_forced;
    if (g_on < 0) g_on = recomp_switch_on("RECOMP_IRQ_LATENCY");
    return g_on;
}

static unsigned long long now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart ? (unsigned long long)(c.QuadPart * 1000000ll / f.QuadPart) : 0ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000);
#endif
}

void recomp_irq_latency_raise(void)
{
    if (!recomp_irq_latency_on()) return;
    if (g_pending_us) return;             /* already high: same interrupt */
    g_pending_us = now_us();
    if (!g_pending_us) g_pending_us = 1;  /* never let a real raise read as low */
}

void recomp_irq_latency_clear(void)
{
    if (!recomp_irq_latency_on()) return;
    if (g_pending_us) { ++g_lost; g_pending_us = 0; }
}

static void note(unsigned long long us)
{
    unsigned i = 0;
    unsigned long long v = us;
    while (v && i < NBUCKET - 1u) { v >>= 1; ++i; }
    ++g_bucket[i];
    ++g_count;
    g_total_us += us;
    if (us > g_max_us) g_max_us = us;
}

void recomp_irq_latency_deliver(void)
{
    unsigned long long t;
    if (!recomp_irq_latency_on()) return;
    if (!g_pending_us) return;            /* nothing outstanding */
    t = now_us();
    note(t > g_pending_us ? t - g_pending_us : 0ull);
    g_pending_us = 0;
}

void recomp_irq_latency_report(void)
{
    unsigned i;
    if (!recomp_irq_latency_on()) {
        fprintf(stderr, "  [IRQ-LATENCY] OFF (RECOMP_IRQ_LATENCY=1 to arm)\n");
        return;
    }
    fprintf(stderr, "  [IRQ-LATENCY] %lu delivered, %lu lines dropped undelivered,"
                    " mean %llu us, max %llu us\n",
            g_count, g_lost,
            g_count ? g_total_us / g_count : 0ull, g_max_us);
    fprintf(stderr, "  [IRQ-LATENCY] histogram (us, log2):");
    for (i = 0; i < NBUCKET; ++i)
        if (g_bucket[i])
            fprintf(stderr, " <%llu:%lu", (unsigned long long)1ull << i, g_bucket[i]);
    fprintf(stderr, "\n");
}

unsigned long recomp_irq_latency_count(void) { return g_count; }
unsigned long recomp_irq_latency_bucket(unsigned i) { return i < NBUCKET ? g_bucket[i] : 0ul; }
unsigned long long recomp_irq_latency_max_us(void) { return g_max_us; }
void recomp_irq_latency_force_on_for_test(int on) { g_forced = on; }
void recomp_irq_latency_note_for_test(unsigned long long us) { note(us); }
void recomp_irq_latency_reset_for_test(void)
{
    unsigned i;
    for (i = 0; i < NBUCKET; ++i) g_bucket[i] = 0;
    g_count = g_lost = 0; g_max_us = g_total_us = 0; g_pending_us = 0;
}
