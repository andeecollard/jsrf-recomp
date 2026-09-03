/**
 * RDTSC for recompiled code.
 *
 * Titles use the timestamp counter as a clock, and they use it to judge the
 * hardware. JSRF reads it through a QueryPerformanceCounter that is literally
 * `rdtsc` (guest 0x00145560), pairs it with a QueryPerformanceFrequency that
 * returns the constant 733333333 (0x2BB5C755, the Xbox CPU clock), times a
 * startup step, and if the interval exceeds 15000000 ticks -- about 20.5 ms --
 * concludes the disc is unreadable and puts up "There's a problem with the
 * disc you're using. It may be dirty or damaged."
 *
 * Lifted as `/-* TODO: rdtsc *-/`, the instruction left EAX and EDX holding
 * whatever they held before it, so the title sampled two stale registers,
 * subtracted them, and compared the result against its threshold. It failed
 * whenever the garbage happened to be large, which was every run.
 *
 * A monotonic host clock scaled to the guest's declared frequency, so the
 * ticks a title divides by 733333333 come out as the seconds that actually
 * passed. Per-process rather than per-thread: the counter a title samples on
 * one thread it may compare against on another, and it must not go backwards.
 */

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* The Xbox CPU clock, and what the title's own QueryPerformanceFrequency
 * reports. Scaling to anything else makes every derived duration wrong. */
#define RECOMP_TSC_HZ  733333333ull

uint64_t recomp_rdtsc(void)
{
#if defined(_WIN32)
    LARGE_INTEGER now, freq;
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&freq)
            || freq.QuadPart <= 0)
        return 0;
    /* Split the multiply so a long-running process cannot overflow: whole
     * seconds first, then the remainder. */
    {
        uint64_t f = (uint64_t)freq.QuadPart;
        uint64_t t = (uint64_t)now.QuadPart;
        return (t / f) * RECOMP_TSC_HZ + ((t % f) * RECOMP_TSC_HZ) / f;
    }
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * RECOMP_TSC_HZ
         + ((uint64_t)ts.tv_nsec * RECOMP_TSC_HZ) / 1000000000ull;
#endif
}
