/* RECOMP_METAL_DEBT_WATCH: count guest accesses to guest RAM the GPU is ahead
 * of -- colour owed by a deferred swap, depth never written back. See
 * nv2a_debt_watch.c. Off by default; every call is a no-op unless armed. */
#ifndef NV2A_DEBT_WATCH_H
#define NV2A_DEBT_WATCH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NV2A_DEBT_COLOUR = 0, NV2A_DEBT_DEPTH = 1 };
/* The two host views of GPU memory: low_base + addr (physical, what the
 * executor uses) and high_base + addr for addr in [heap_lo, heap_hi) (the
 * contiguous window the title's Lock returns). Installs the handler, chaining
 * whatever SIGSEGV/SIGBUS handlers are already in place. */
void nv2a_debt_watch_configure(uintptr_t low_base, uintptr_t high_base, uint32_t heap_lo, uint32_t heap_hi);
int  nv2a_debt_watch_on(void);
/* [p, p+bytes) (low view) now differs from what the GPU holds. */
void nv2a_debt_watch_arm(const uint8_t *p, size_t bytes, int kind);
/* It is about to be made current (written back): stop watching it. */
void nv2a_debt_watch_paid(const uint8_t *p, size_t bytes);
void nv2a_debt_watch_report(void);
/* Per kind (colour, then depth): armed, paid clean, guest reads, guest writes,
 * runtime reads, runtime writes. For the unit test. */
#define NV2A_DEBT_WATCH_COUNTS 12
void nv2a_debt_watch_counts(unsigned long long out[NV2A_DEBT_WATCH_COUNTS]);
#ifdef __cplusplus
}
#endif
#endif
