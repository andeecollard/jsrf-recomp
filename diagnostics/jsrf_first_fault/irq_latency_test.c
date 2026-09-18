/* HOW LONG DOES A RAISED DEVICE INTERRUPT WAIT?
 *
 * The whole argument about the crash rests on an unmeasured number. xemu asserts
 * a real PCI IRQ and the guest takes it at the next instruction boundary;
 * pci_irq_assert here was `{ (void)d; }` and the ISR runs only when a guest
 * thread calls a wait function, rate-limited to 8 ms. Microseconds against
 * milliseconds -- argued from source on both sides, and the handover says so in
 * its own words: "NOT DEMONSTRATED ... the crash rate has not been measured
 * against a shorter window."
 *
 * Microsoft's answer to the same problem is a poll emitted every ~79 guest
 * bytes, so their generated code is interruptible everywhere. Adopting that here
 * costs a translator change and a full regeneration, which invalidates every
 * archived baseline. So measure the window before paying for it.
 *
 * WHAT THIS TEST PINS DOWN. The instrument is level-triggered, and that is the
 * part that can be silently wrong: a device holds its line high and asserts
 * repeatedly, so timing EVERY assert would restart the clock constantly and
 * report a latency near zero for a line that has been high for 8 ms -- a
 * confident, precise, completely wrong answer. Only the rising edge may be
 * timed.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "../../src/kernel/irq_latency.h"

static int fails;
static void ok(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-48s %ld\n", what, got);
    else { printf("  FAIL %-48s got %ld, want %ld\n", what, got, want); ++fails; }
}

int main(void)
{
    unsigned i, nz;

    /* Unarmed: the gate must swallow everything, so an unrelated run pays
     * nothing and reads nothing. */
    recomp_irq_latency_force_on_for_test(0);
    recomp_irq_latency_reset_for_test();
    recomp_irq_latency_raise();
    recomp_irq_latency_deliver();
    ok("unarmed: records nothing", (long)recomp_irq_latency_count(), 0);

    recomp_irq_latency_force_on_for_test(1);
    recomp_irq_latency_reset_for_test();

    /* One raise, one delivery, one sample. */
    recomp_irq_latency_raise();
    recomp_irq_latency_deliver();
    ok("raise+deliver records one", (long)recomp_irq_latency_count(), 1);

    /* THE LEVEL-TRIGGERED CASE, AND IT MUST BE MEASURED IN TIME, NOT IN COUNT.
     *
     * A device holds its line high and asserts repeatedly. Those are ONE
     * undelivered interrupt and the wait runs from the FIRST assert. Counting
     * samples cannot detect a failure here -- ten asserts and one delivery is
     * one sample either way. What breaks is the CLOCK: a re-arming instrument
     * reports the time since the LAST assert, which is near zero, and would
     * report microseconds for a line that has been high for milliseconds. That
     * is precisely the number this whole instrument exists to get right, so the
     * assertion has to be about elapsed time.
     *
     * (An earlier version of this test asserted the count, passed with the
     * rising-edge check deleted, and was therefore vacuous.) */
    recomp_irq_latency_reset_for_test();
    recomp_irq_latency_raise();
    usleep(4000);                        /* line stays high for 4 ms */
    for (i = 0; i < 10; ++i) recomp_irq_latency_raise();
    recomp_irq_latency_deliver();
    ok("ten asserts on a high line = one sample",
       (long)recomp_irq_latency_count(), 1);
    ok("...and the wait is timed from the FIRST assert",
       recomp_irq_latency_max_us() >= 2000 ? 1 : 0, 1);

    /* A delivery with nothing outstanding must not invent a sample. */
    recomp_irq_latency_reset_for_test();
    recomp_irq_latency_deliver();
    recomp_irq_latency_deliver();
    ok("delivery with no raise records nothing",
       (long)recomp_irq_latency_count(), 0);

    /* A line that drops before the guest ever ran its ISR is a LOST interrupt,
     * and must not be counted as a fast one -- that would flatter the very
     * number this exists to measure. */
    recomp_irq_latency_reset_for_test();
    recomp_irq_latency_raise();
    recomp_irq_latency_clear();
    recomp_irq_latency_deliver();
    ok("dropped line is not a zero-latency sample",
       (long)recomp_irq_latency_count(), 0);

    /* The histogram must separate microseconds from milliseconds, since that is
     * the entire question. Buckets are log2 microseconds. */
    recomp_irq_latency_reset_for_test();
    recomp_irq_latency_note_for_test(1);        /* 1 us   */
    recomp_irq_latency_note_for_test(1000);     /* 1 ms   */
    recomp_irq_latency_note_for_test(8000);     /* 8 ms   */
    ok("three samples", (long)recomp_irq_latency_count(), 3);
    ok("max is the 8 ms one", (long)recomp_irq_latency_max_us(), 8000);
    nz = 0;
    for (i = 0; i < 16; ++i) if (recomp_irq_latency_bucket(i)) ++nz;
    ok("they land in three distinct buckets", (long)nz, 3);
    /* The separation is the whole point: a microsecond sample and a
     * millisecond sample must not share a bucket, or the histogram cannot
     * answer the question it exists for. 1 us -> bucket 1, 1 ms -> 10,
     * 8 ms -> 13. */
    ok("1 us lands in a low bucket",   (long)recomp_irq_latency_bucket(1), 1);
    ok("1 ms lands far from it",       (long)recomp_irq_latency_bucket(10), 1);
    ok("8 ms further still",           (long)recomp_irq_latency_bucket(13), 1);
    ok("nothing between 1 us and 1 ms", (long)recomp_irq_latency_bucket(5), 0);

    recomp_irq_latency_reset_for_test();
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
