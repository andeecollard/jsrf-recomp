/* Guest-memory ownership boundary for GPU-resident surfaces.
 *
 * See recomp_gpu_own.h for why this exists.  The rules that shaped the code
 * here are worth stating, because each of them is a way the obvious version is
 * wrong:
 *
 * The seam cannot tell a read from a write.  XBOX_PTR is the one place every
 * translated access passes through -- scalar loads, the signed and floating
 * accessors, the FS-segmented forms, the XMM lane helpers, and, importantly,
 * the `rep movs`/`rep stos` loops that the lifter expands into MEM32 lvalues
 * and which therefore never reach the explicit store helper.  A hook that only
 * synchronised would be correct for reads and silently wrong for those block
 * stores, which would then be overwritten by a stale GPU surface.  So the
 * reconcile is conservative: it downloads the surface *and* drops GPU
 * ownership, which is right for either direction.  Splitting reads from writes
 * needs the block operations routed through a store seam first.
 *
 * The map is a count, not a flag.  Two surfaces can cover one granule, and a
 * flag would be cleared by whichever released first.
 *
 * Reconcile must not recurse.  The backend copies pixels with ordinary host
 * pointers today, but a future path that reaches back through XBOX_PTR would
 * otherwise re-enter this function underneath itself.
 */

#include "recomp_gpu_own.h"

#include <stdio.h>
#include <stdlib.h>

/* Resolved at final link.  This file is deliberately free of any dependency on
 * xbox_kernel so the standalone renderer regressions, which link the graphics
 * backend without the kernel, can supply their own guest window. */
extern ptrdiff_t g_xbox_mem_offset;

unsigned char g_recomp_gpu_own_map[RECOMP_GPU_OWN_SLOTS];

/* Every alias of one range: the low RAM window plus whatever the layout adds. */
#define RECOMP_GPU_OWN_MAX_ALIASES 32u

static RecompGpuOwnSyncFn  s_sync;
static RecompGpuOwnAliasFn s_aliases;

static uint64_t s_touches;      /* slow path entered */
static uint64_t s_hits;         /* ... and a surface really overlapped */
static uint64_t s_reentrant;    /* ... and was declined to avoid recursion */
static uint64_t s_armed_slots;  /* granules currently held by some surface */

#if defined(_MSC_VER)
#define RECOMP_GPU_OWN_TLS __declspec(thread)
#else
#define RECOMP_GPU_OWN_TLS __thread
#endif
static RECOMP_GPU_OWN_TLS int s_in_reconcile;

/* RECOMP_GPU_OWN=0 stops the map ever being armed.
 *
 * The negative control for every claim made about this seam. With nothing
 * armed the fast path always falls through and the reconcile never runs, which
 * is behaviourally the same as a build without RECOMP_GPU_OWNERSHIP -- so a
 * result that survives this flag was never the ownership map's doing. It is
 * checked once and cached: this is read on the arming path, not the hot one. */
static int arming_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_GPU_OWN");
        enabled = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;
        if (!enabled)
            fprintf(stderr, "[GPU-OWN] disarmed by RECOMP_GPU_OWN=0;"
                            " translated accesses will not reconcile\n");
    }
    return enabled;
}

void recomp_gpu_own_set_sync(RecompGpuOwnSyncFn fn)   { s_sync = fn; }
void recomp_gpu_own_set_aliases(RecompGpuOwnAliasFn fn) { s_aliases = fn; }

uintptr_t recomp_gpu_own_reconcile(uint32_t guest_va)
{
    uintptr_t host = (uintptr_t)guest_va + (uintptr_t)g_xbox_mem_offset;
    RecompGpuOwnSyncFn sync = s_sync;

    ++s_touches;
    if (!sync) return host;
    if (s_in_reconcile) { ++s_reentrant; return host; }

    s_in_reconcile = 1;
    if (sync((void *)host, RECOMP_GPU_OWN_MAX_ACCESS)) ++s_hits;
    s_in_reconcile = 0;
    return host;
}

/* The granules an access may touch.
 *
 * The low end is widened by one access width, not by a whole granule: the seam
 * sees only the base address, so a load of up to RECOMP_GPU_OWN_MAX_ACCESS
 * bytes that starts just below an armed range still reaches into it and has to
 * be observed.  A surface aligned to a granule boundary -- which a framebuffer
 * always is -- therefore arms the granule below it as well, and accesses there
 * take a slow path that finds no overlap and returns.  That is the price of
 * having no width at the seam; it is bounded, and the counters separate those
 * from the touches that really hit something. */
static void slot_span(uint32_t va, size_t bytes, uint32_t *first, uint32_t *last)
{
    uint64_t end = (uint64_t)va + bytes;
    uint32_t skew = RECOMP_GPU_OWN_MAX_ACCESS - 1u;
    uint32_t lo = va > skew ? va - skew : 0u;
    if (end > 0x100000000ULL) end = 0x100000000ULL;
    *first = lo >> RECOMP_GPU_OWN_SHIFT;
    *last = (uint32_t)((end - 1) >> RECOMP_GPU_OWN_SHIFT);
}

static void map_adjust(uint32_t va, size_t bytes, int delta)
{
    uint32_t first, last, i;

    if (!bytes) return;
    slot_span(va, bytes, &first, &last);
    for (i = first; i <= last; ++i) {
        unsigned char before = g_recomp_gpu_own_map[i];
        if (delta > 0) {
            /* Saturating: a granule covered by 255 surfaces stays armed, and
             * a leaked hold costs a slow path rather than a missed one. */
            if (before != 0xFF) g_recomp_gpu_own_map[i] = (unsigned char)(before + 1);
            if (!before) ++s_armed_slots;
        } else if (before) {
            if (before != 0xFF) g_recomp_gpu_own_map[i] = (unsigned char)(before - 1);
            if (before == 1) --s_armed_slots;
        }
    }
}

static void adjust_all_aliases(const void *host, size_t bytes, int delta)
{
    uint32_t va[RECOMP_GPU_OWN_MAX_ALIASES];
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uintptr_t p = (uintptr_t)host;
    uint32_t primary;
    unsigned n = 0, i;

    if (!host || !bytes || p < base || p - base > 0xFFFFFFFFu) return;
    primary = (uint32_t)(p - base);

    if (s_aliases)
        n = s_aliases(primary, bytes, va, RECOMP_GPU_OWN_MAX_ALIASES);
    if (!n) { va[0] = primary; n = 1; }
    for (i = 0; i < n; ++i) map_adjust(va[i], bytes, delta);
}

void recomp_gpu_own_hold(const void *host, size_t bytes)
{
    if (!arming_enabled()) return;
    adjust_all_aliases(host, bytes, +1);
}

void recomp_gpu_own_release(const void *host, size_t bytes)
{
    if (!arming_enabled()) return;
    adjust_all_aliases(host, bytes, -1);
}

void recomp_gpu_own_counters(uint64_t *touches, uint64_t *hits,
                             uint64_t *armed_slots)
{
    if (touches) *touches = s_touches;
    if (hits) *hits = s_hits;
    if (armed_slots) *armed_slots = s_armed_slots;
}

void recomp_gpu_own_report(void)
{
    fprintf(stderr, "[GPU-OWN] %llu guest touches, %llu hit a resident surface,"
                    " %llu declined re-entrant; %llu granules armed now\n",
            (unsigned long long)s_touches, (unsigned long long)s_hits,
            (unsigned long long)s_reentrant,
            (unsigned long long)s_armed_slots);
    fflush(stderr);
}
