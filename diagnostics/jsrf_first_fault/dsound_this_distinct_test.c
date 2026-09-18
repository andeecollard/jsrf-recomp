/* IS THE `this` THE OWNER[] NUMBERS WERE READ THROUGH A SINGLE OBJECT?
 *
 * The DirectSound probe fires at the ISR's entry, so a raise-time read of
 * owner[h] goes through a `this` left behind by an EARLIER ISR entry. That is
 * harmless if the title keeps one device object and fatal to the measurement
 * if it keeps several -- in which case "of those NULLs, 0 were h==0", the
 * control that retired the owner guard, was read against the wrong table.
 *
 * jsrf_dsound_this_seen() answers that in one session. This test exists
 * because the answer is a COUNT, and a count that silently fails to
 * distinguish two pointers reads 1 -- which is exactly the reassuring result,
 * and indistinguishable from the real one. The same shape as the stale
 * "default on" comment this tree has now been bitten by twice: an instrument
 * that is not working reads like an instrument that is.
 *
 * `calls` is the positive control and is asserted alongside every distinct
 * count, because distinct=0 means "no probe installed" far more often than it
 * means "no device".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

/* Mirrors DSOUND_THIS_MAX in src/apu/apu_vp.c. If that grows, this fails
 * loudly at the overflow case rather than drifting quietly. */
#define EXPECT_MAX 8u

extern void jsrf_dsound_this_seen(unsigned int t);
extern unsigned int  g_jsrf_dsound_this;
extern unsigned int  g_jsrf_dsound_this_seen[];
extern unsigned long g_jsrf_dsound_this_distinct;
extern unsigned long g_jsrf_dsound_this_calls;
extern unsigned long g_jsrf_dsound_this_over;

static int fails;

static void check(const char *what, unsigned long got, unsigned long want)
{
    if (got == want) {
        printf("  ok   %-42s %lu\n", what, got);
    } else {
        printf("  FAIL %-42s got %lu, want %lu\n", what, got, want);
        ++fails;
    }
}

int main(void)
{
    unsigned i;

    /* Nothing has fired: the not-measured state. distinct=0 AND calls=0 is
     * what "the probe is not installed" looks like. */
    check("fresh: calls", g_jsrf_dsound_this_calls, 0);
    check("fresh: distinct", g_jsrf_dsound_this_distinct, 0);

    /* One object, seen repeatedly -- the result that makes the owner[]
     * numbers trustworthy. It must NOT count once per sighting. */
    for (i = 0; i < 100; ++i)
        jsrf_dsound_this_seen(0xDEADBEEFu);
    check("one object x100: calls", g_jsrf_dsound_this_calls, 100);
    check("one object x100: distinct", g_jsrf_dsound_this_distinct, 1);
    check("last capture retained", g_jsrf_dsound_this, 0xDEADBEEFu);

    /* A second object. This is the finding that reopens hypothesis 7, so a
     * missed increment here is the expensive failure. */
    jsrf_dsound_this_seen(0xCAFEBABEu);
    check("second object: distinct", g_jsrf_dsound_this_distinct, 2);
    check("second object: calls", g_jsrf_dsound_this_calls, 101);

    /* Interleaving must not re-count either one. */
    jsrf_dsound_this_seen(0xDEADBEEFu);
    jsrf_dsound_this_seen(0xCAFEBABEu);
    check("interleaved: distinct", g_jsrf_dsound_this_distinct, 2);
    check("interleaved: calls", g_jsrf_dsound_this_calls, 103);

    /* A zero `this` is the ISR being entered before the object exists. It is
     * a call, not an object, and must not be stored -- otherwise the NOT
     * MEASURED branch of the report can never be reached again. */
    jsrf_dsound_this_seen(0u);
    check("this==0: calls still counted", g_jsrf_dsound_this_calls, 104);
    check("this==0: distinct unchanged", g_jsrf_dsound_this_distinct, 2);

    /* Fill the table, then overflow it. distinct saturates at the table size
     * and `over` carries the "and more" signal. */
    for (i = 2; i < EXPECT_MAX; ++i)
        jsrf_dsound_this_seen(0x1000u + i);
    check("table full: distinct", g_jsrf_dsound_this_distinct, EXPECT_MAX);
    check("table full: over", g_jsrf_dsound_this_over, 0);

    jsrf_dsound_this_seen(0x9999u);
    check("overflow: distinct saturates", g_jsrf_dsound_this_distinct, EXPECT_MAX);
    check("overflow: over moves", g_jsrf_dsound_this_over, 1);

    /* A value already in the table must still dedupe after overflow. */
    jsrf_dsound_this_seen(0xDEADBEEFu);
    check("post-overflow dedupe: over", g_jsrf_dsound_this_over, 1);

    printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
