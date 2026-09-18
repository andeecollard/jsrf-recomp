/* RECOMP_IRQ_LATENCY -- how long does a raised device interrupt wait?
 *
 * WHY THIS EXISTS BEFORE THE FIX DOES. The handover of 18 Sep 2026 argues that
 * the raise/dispatch window is ours rather than xemu's: xemu asserts a real PCI
 * IRQ and the guest takes it at the next instruction boundary, whereas
 * pci_irq_assert here is `{ (void)d; }` and the ISR runs only when some guest
 * thread calls a wait function, rate-limited to BRIDGE_DEVICE_IRQ_PERIOD_MS = 8.
 * Microseconds against milliseconds. It also says, in its own words, "NOT
 * DEMONSTRATED ... the crash rate has not been measured against a shorter
 * window."
 *
 * Microsoft's answer to the same problem, read out of their recompiler on
 * 18 Sep, is a one-byte poll emitted roughly every 79 guest bytes -- at backward
 * branches, at returns, and before every interrupt-disable escape -- so guest
 * code is interruptible everywhere and delivery never waits for a blocking call.
 * Adopting that here means a translator change and a full regeneration, which
 * invalidates every archived gen baseline.
 *
 * So measure the window before paying for it. If the wait is already
 * microseconds, the window is not the mechanism and the regeneration is saved.
 * If it is milliseconds, the argument is demonstrated rather than reasoned.
 *
 * Level-triggered, so only the RISING edge is timed: repeated asserts while the
 * line is already high are the same undelivered interrupt, and timing each one
 * would report a latency of nearly zero for a line that has been high for 8 ms.
 */
#ifndef RECOMP_IRQ_LATENCY_H
#define RECOMP_IRQ_LATENCY_H
#include <stdint.h>

/* Rising edge of a device interrupt line. Cheap and safe to call on every
 * assert; it does nothing once the line is already recorded as high. */
void recomp_irq_latency_raise(void);
/* The line went low without the guest ever running its ISR. */
void recomp_irq_latency_clear(void);
/* An ISR is about to run: closes the outstanding raise and records the wait. */
void recomp_irq_latency_deliver(void);
void recomp_irq_latency_report(void);
int  recomp_irq_latency_on(void);

/* For the test: counts and the bucketed histogram. */
unsigned long recomp_irq_latency_count(void);
unsigned long recomp_irq_latency_bucket(unsigned i);   /* i = 0..15, log2 us */
unsigned long long recomp_irq_latency_max_us(void);
void recomp_irq_latency_reset_for_test(void);
void recomp_irq_latency_force_on_for_test(int on);
/* Injects a synthetic pair so the histogram can be tested without a device. */
void recomp_irq_latency_note_for_test(unsigned long long us);
#endif
