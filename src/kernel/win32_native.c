/*
 * Host helpers that exist for POSIX in src/platform/win32_compat.c but have no
 * Windows body, because `platform` is an INTERFACE library there and that file
 * is never compiled. Only the handful of xbox_*-prefixed helpers need one --
 * everything else in win32_compat.c reimplements a Win32 API that Windows
 * already provides.
 */
#include <windows.h>

/* Monotonic seconds since the first call, for the [FB] and OHCI trace lines.
 * QPC rather than GetTickCount: the trace prints hundredths and the tick
 * counter's ~15 ms granularity shows up as visible stair-stepping. */
double xbox_TraceSeconds(void)
{
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER now;

    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

/* Suspend/resume accounting that win32_compat.c keeps for its POSIX thread
 * emulation. Windows suspends threads itself, so there is nothing here to
 * count and nothing to print -- deliberately silent rather than a zeroed line
 * that would read as a measurement. */
void w32_thread_trace_report(void) { }
