/* RECOMP_METAL_DEBT_WATCH -- who reads guest RAM that the GPU is ahead of?
 *
 * WHY. RECOMP_METAL_DEFER_SWAP is worth +24 fps on the plain executor (83.0 ->
 * 107.2, tutorial, 24 Sep 2026) and ships off for one reason (G3 A2): while a
 * cached surface OWES guest RAM -- its pixels are on the GPU, the swap that
 * would have written them back was deferred -- a guest CPU read of those bytes
 * is not intercepted and would see stale memory. Nobody has measured whether
 * the title ever does that. The same question stands for depth: with
 * RECOMP_METAL_NO_DEPTH_SYNC (the default) depth is never written back at all.
 *
 * WHAT. While a range is owed it is made inaccessible, and the first access
 * is counted -- by view, by thread, read or write, and by host PC -- before
 * the protection is lifted and the access proceeds. It is an instrument: the
 * access still sees the stale bytes, exactly as it would without the watch.
 * Reads of the report tell whether deferral can default on as it is, or needs
 * the debt paid on first access.
 *
 * THE TWO VIEWS. JSRF's GPU memory is visible twice: at its physical address
 * (the low view, g_memory_offset + addr, which the executor and the host read
 * and write) and through the contiguous window at 0x80000000 + addr (the high
 * view, which D3D's Lock hands the title -- xbox_EnablePhysicalHeapAlias).
 * Both are watched. The executor's own thread touching the LOW view is the
 * runtime at work (a depth upload, a CPU clear): only the low pages are
 * released for it, and the high view stays guarded -- the aliasing pattern
 * CLAUDE.md asks for, no writable window on the guest's view. Any other
 * access releases the whole range and is counted as the guest's.
 *
 * Granularity is the host page (16 KB). A fault on a watched page but outside
 * the owed bytes is counted as a neighbour; it releases the range all the
 * same, so a neighbour-heavy report under-counts, and says so.
 *
 * A system call handed a watched buffer gets EFAULT instead of a signal. No
 * path is known to do that with a render target; the report counts nothing
 * for it, so it would show as a failure elsewhere, not here. Off by default;
 * zero cost unless armed. */
#include "nv2a_debt_watch.h"
#include "../recomp_switch.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__) && defined(__aarch64__)
#include <signal.h>
#include <sys/ucontext.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>

#define DW_REGIONS 32
#define DW_PCS 48
typedef struct {
    uintptr_t lo, hi;          /* the owed bytes, low view */
    int kind;                  /* NV2A_DEBT_COLOUR / NV2A_DEBT_DEPTH */
    int armed;                 /* guarded (high view, and low unless released for the runtime) */
    int low_released;          /* the executor touched the low view; its pages are open */
    int touched;               /* a non-runtime access happened while armed */
} DwRegion;
static DwRegion s_r[DW_REGIONS];
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static int s_on = -1, s_configured;
static uintptr_t s_low_base, s_high_base;
static uint32_t s_heap_lo, s_heap_hi;
static uintptr_t s_page = 16384;
static pthread_t s_runtime; static int s_have_runtime;
static struct sigaction s_old_segv, s_old_bus;
static struct { uintptr_t pc; unsigned n; int kind, write, high; } s_pc[DW_PCS];
static _Atomic unsigned long long s_armed[2], s_paid_clean[2];
static _Atomic unsigned long long s_guest_read[2], s_guest_write[2], s_runtime_read[2], s_runtime_write[2];
static _Atomic unsigned long long s_neighbour, s_high_faults, s_low_faults, s_full, s_pc_full;

static void lock(void) { while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) { } }
static void unlock(void) { atomic_flag_clear_explicit(&s_lock, memory_order_release); }

int nv2a_debt_watch_on(void)
{
    if (s_on < 0) s_on = recomp_switch_on("RECOMP_METAL_DEBT_WATCH");
    return s_on && s_configured;
}

/* The high-view address of low-view `p`, or 0 when it is outside the aliased heap. */
static uintptr_t high_of(uintptr_t p)
{
    uintptr_t addr;
    if (!s_high_base || p < s_low_base) return 0;
    addr = p - s_low_base;
    if (addr < s_heap_lo || addr >= s_heap_hi) return 0;
    return s_high_base + addr;
}
static uintptr_t pg_lo(uintptr_t a) { return a & ~(s_page - 1); }
static uintptr_t pg_hi(uintptr_t a) { return (a + s_page - 1) & ~(s_page - 1); }

/* Is page [pg, pg+s_page) of the given view still needed by another armed region? */
static int page_needed(uintptr_t pg, int high, int skip)
{
    for (int i = 0; i < DW_REGIONS; ++i) {
        uintptr_t lo, hi;
        if (i == skip || !s_r[i].armed) continue;
        if (high) { lo = high_of(s_r[i].lo); if (!lo) continue; hi = lo + (s_r[i].hi - s_r[i].lo); }
        else { if (s_r[i].low_released) continue; lo = s_r[i].lo; hi = s_r[i].hi; }
        if (pg + s_page > pg_lo(lo) && pg < pg_hi(hi)) return 1;
    }
    return 0;
}
static void protect_view(int i, int high, int prot)
{
    uintptr_t lo = high ? high_of(s_r[i].lo) : s_r[i].lo, hi;
    if (!lo) return;
    hi = lo + (s_r[i].hi - s_r[i].lo);
    if (prot == PROT_NONE) { mprotect((void *)pg_lo(lo), pg_hi(hi) - pg_lo(lo), PROT_NONE); return; }
    for (uintptr_t pg = pg_lo(lo); pg < pg_hi(hi); pg += s_page)
        if (!page_needed(pg, high, i)) mprotect((void *)pg, s_page, PROT_READ | PROT_WRITE);
}
static void release(int i)
{
    s_r[i].armed = 0;
    protect_view(i, 0, PROT_READ | PROT_WRITE);
    protect_view(i, 1, PROT_READ | PROT_WRITE);
}

static void note_pc(uintptr_t pc, int kind, int write, int high)
{
    for (int i = 0; i < DW_PCS; ++i) {
        if (s_pc[i].n && s_pc[i].pc == pc && s_pc[i].kind == kind && s_pc[i].write == write && s_pc[i].high == high) { ++s_pc[i].n; return; }
        if (!s_pc[i].n) { s_pc[i].pc = pc; s_pc[i].kind = kind; s_pc[i].write = write; s_pc[i].high = high; s_pc[i].n = 1; return; }
    }
    atomic_fetch_add(&s_pc_full, 1);
}

static void handler(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = (ucontext_t *)context;
    uintptr_t f = si ? (uintptr_t)si->si_addr : 0;
    int hit = -1, high = 0;
    lock();
    for (int i = 0; i < DW_REGIONS && hit < 0; ++i) {
        uintptr_t lo, hi;
        if (!s_r[i].armed) continue;
        lo = s_r[i].lo; hi = s_r[i].hi;
        if (!s_r[i].low_released && f >= pg_lo(lo) && f < pg_hi(hi)) { hit = i; high = 0; break; }
        lo = high_of(s_r[i].lo);
        if (lo) { hi = lo + (s_r[i].hi - s_r[i].lo); if (f >= pg_lo(lo) && f < pg_hi(hi)) { hit = i; high = 1; } }
    }
    if (hit >= 0) {
        DwRegion *r = &s_r[hit];
        uintptr_t lo = high ? high_of(r->lo) : r->lo, hi = lo + (r->hi - r->lo);
        int write = uc && uc->uc_mcontext ? (int)((uc->uc_mcontext->__es.__esr >> 6) & 1u) : 0;
        int kind = r->kind & 1;
        int runtime = s_have_runtime && pthread_equal(pthread_self(), s_runtime);
        if (high) atomic_fetch_add(&s_high_faults, 1); else atomic_fetch_add(&s_low_faults, 1);
        if (f < lo || f >= hi) atomic_fetch_add(&s_neighbour, 1);
        if (!high && runtime) {
            atomic_fetch_add(write ? &s_runtime_write[kind] : &s_runtime_read[kind], 1);
            r->low_released = 1;
            protect_view(hit, 0, PROT_READ | PROT_WRITE);
        } else {
            atomic_fetch_add(write ? &s_guest_write[kind] : &s_guest_read[kind], 1);
            note_pc(uc && uc->uc_mcontext ? (uintptr_t)uc->uc_mcontext->__ss.__pc : 0, kind, write, high);
            r->touched = 1;
            release(hit);
        }
        unlock();
        return;                          /* the faulting instruction runs again, unguarded */
    }
    unlock();
    {
        struct sigaction *old = sig == SIGBUS ? &s_old_bus : &s_old_segv;
        if (old->sa_flags & SA_SIGINFO) { if (old->sa_sigaction) { old->sa_sigaction(sig, si, context); return; } }
        else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) { old->sa_handler(sig); return; }
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void nv2a_debt_watch_configure(uintptr_t low_base, uintptr_t high_base, uint32_t heap_lo, uint32_t heap_hi)
{
    struct sigaction sa;
    if (s_on < 0) s_on = recomp_switch_on("RECOMP_METAL_DEBT_WATCH");
    if (!s_on || s_configured) return;
    s_low_base = low_base; s_high_base = high_base; s_heap_lo = heap_lo; s_heap_hi = heap_hi;
    s_page = (uintptr_t)sysconf(_SC_PAGESIZE);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_old_segv) || sigaction(SIGBUS, &sa, &s_old_bus)) {
        fprintf(stderr, "[DEBT-WATCH] could not install the handler; the watch is off\n");
        return;
    }
    s_configured = 1;
    fprintf(stderr, "[DEBT-WATCH] on: owed colour ranges and never-written-back depth are guarded until first touched"
                    " (low view %p, high view %p, heap 0x%08X..0x%08X, page %lu)\n",
            (void *)low_base, (void *)high_base, heap_lo, heap_hi, (unsigned long)s_page);
}

void nv2a_debt_watch_arm(const uint8_t *p, size_t bytes, int kind)
{
    uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
    int slot = -1;
    if (!nv2a_debt_watch_on() || !p || !bytes) return;
    if (!s_have_runtime) { s_runtime = pthread_self(); s_have_runtime = 1; }
    lock();
    for (int i = 0; i < DW_REGIONS; ++i)
        if (s_r[i].armed && s_r[i].lo == lo && s_r[i].hi == hi) {
            if (s_r[i].low_released) { s_r[i].low_released = 0; protect_view(i, 0, PROT_NONE); }
            unlock(); return;            /* already owed: the same debt, still unread */
        }
    for (int i = 0; i < DW_REGIONS && slot < 0; ++i) if (!s_r[i].armed) slot = i;
    if (slot < 0) { atomic_fetch_add(&s_full, 1); unlock(); return; }
    s_r[slot].lo = lo; s_r[slot].hi = hi; s_r[slot].kind = kind; s_r[slot].armed = 1;
    s_r[slot].low_released = 0; s_r[slot].touched = 0;
    protect_view(slot, 0, PROT_NONE);
    protect_view(slot, 1, PROT_NONE);
    atomic_fetch_add(&s_armed[kind & 1], 1);
    unlock();
}

int nv2a_debt_watch_guarded_read(const uint8_t *p, size_t bytes, void (*fn)(const uint8_t *, size_t, void *), void *ctx)
{
    uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
    if (!nv2a_debt_watch_on()) { fn(p, bytes, ctx); return 1; }
    lock();
    for (int i = 0; i < DW_REGIONS; ++i)
        if (s_r[i].armed && pg_lo(s_r[i].lo) < pg_hi(hi) && pg_lo(lo) < pg_hi(s_r[i].hi)) { unlock(); return 0; }
    fn(p, bytes, ctx);            /* arming takes this lock, so nothing is armed under the read */
    unlock();
    return 1;
}

void nv2a_debt_watch_paid(const uint8_t *p, size_t bytes)
{
    uintptr_t lo = (uintptr_t)p, hi = lo + bytes;
    if (!nv2a_debt_watch_on() || !p || !bytes) return;
    lock();
    for (int i = 0; i < DW_REGIONS; ++i)
        if (s_r[i].armed && s_r[i].lo < hi && lo < s_r[i].hi) {
            atomic_fetch_add(&s_paid_clean[s_r[i].kind & 1], 1);
            release(i);
        }
    unlock();
}

void nv2a_debt_watch_counts(unsigned long long out[NV2A_DEBT_WATCH_COUNTS])
{
    for (int k = 0; k < 2; ++k) {
        out[6 * k + 0] = atomic_load(&s_armed[k]); out[6 * k + 1] = atomic_load(&s_paid_clean[k]);
        out[6 * k + 2] = atomic_load(&s_guest_read[k]); out[6 * k + 3] = atomic_load(&s_guest_write[k]);
        out[6 * k + 4] = atomic_load(&s_runtime_read[k]); out[6 * k + 5] = atomic_load(&s_runtime_write[k]);
    }
}

void nv2a_debt_watch_report(void)
{
    static const char *kn[2] = { "colour owed by a deferred swap", "depth never written back" };
    if (!nv2a_debt_watch_on()) return;
    for (int k = 0; k < 2; ++k)
        fprintf(stderr, "[DEBT-WATCH] %s: armed %llu, paid before any guest access %llu | FIRST GUEST ACCESS while owed:"
                        " reads %llu, writes %llu | the executor's own thread on the low view: reads %llu, writes %llu\n",
                kn[k], atomic_load(&s_armed[k]), atomic_load(&s_paid_clean[k]), atomic_load(&s_guest_read[k]),
                atomic_load(&s_guest_write[k]), atomic_load(&s_runtime_read[k]), atomic_load(&s_runtime_write[k]));
    fprintf(stderr, "[DEBT-WATCH] faults: high view %llu, low view %llu; outside the owed bytes (same page) %llu;"
                    " regions full %llu; PC table full %llu\n", atomic_load(&s_high_faults), atomic_load(&s_low_faults),
            atomic_load(&s_neighbour), atomic_load(&s_full), atomic_load(&s_pc_full));
    for (int i = 0; i < DW_PCS && s_pc[i].n; ++i) {
        Dl_info di; const char *sym = "?", *img = "?"; uintptr_t off = 0;
        if (s_pc[i].pc && dladdr((void *)s_pc[i].pc, &di)) {
            if (di.dli_sname) { sym = di.dli_sname; off = s_pc[i].pc - (uintptr_t)di.dli_saddr; }
            if (di.dli_fname) { img = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname; }
        }
        fprintf(stderr, "[DEBT-WATCH]   %u x %s %s via the %s view at pc %p (%s+0x%lx in %s)\n", s_pc[i].n,
                s_pc[i].kind ? "depth" : "colour", s_pc[i].write ? "write" : "read", s_pc[i].high ? "high" : "low",
                (void *)s_pc[i].pc, sym, (unsigned long)off, img);
    }
}
#else
int nv2a_debt_watch_on(void) { return 0; }
void nv2a_debt_watch_configure(uintptr_t a, uintptr_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
void nv2a_debt_watch_arm(const uint8_t *p, size_t bytes, int kind) { (void)p; (void)bytes; (void)kind; }
void nv2a_debt_watch_paid(const uint8_t *p, size_t bytes) { (void)p; (void)bytes; }
void nv2a_debt_watch_report(void) { }
int nv2a_debt_watch_guarded_read(const uint8_t *p, size_t bytes, void (*fn)(const uint8_t *, size_t, void *), void *ctx) { fn(p, bytes, ctx); return 1; }
void nv2a_debt_watch_counts(unsigned long long out[NV2A_DEBT_WATCH_COUNTS]) { memset(out, 0, sizeof(unsigned long long) * NV2A_DEBT_WATCH_COUNTS); }
#endif
