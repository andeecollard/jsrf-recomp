/*
 * KeSetBasePriorityThread / KeQueryBasePriorityThread must round-trip exactly.
 *
 * The Xbox kernel's thread base priority is a full integer. This runtime used
 * to store it as one of seven Win32 buckets and convert back, so everything
 * outside {-15,-2,-1,0,1,2,15} was lost: set 16, read 15.
 *
 * JSRF sets 16 -- measured from the scheduler trace, `KeSetBasePriority ...
 * ra=0x00147CF8 extra=0x00000010`, ra inside XAPI's SetThreadPriority -- and
 * then polls until the query agrees. It never could. The per-thread ordinal
 * histogram caught the consequence during a cutscene hang: 64,522,063 /
 * 64,522,064 / 64,517,185 calls to ObReferenceObjectByHandle,
 * ObfDereferenceObject and KeQueryBasePriorityThread on a single thread, a
 * three-call cycle run 64.5 million times.
 *
 * 16 is the case that mattered, so it is checked by name. The rest of the
 * range is checked because a fix that only rescues 16 would be a patch on one
 * symptom rather than the defect.
 */
#include <stdio.h>

#include "../../src/platform/win32_compat.h"
#include "../../src/kernel/kernel.h"

static int failures;

static void roundtrip(LONG set)
{
    LONG got;
    xbox_KeSetBasePriorityThread(GetCurrentThread(), set);
    got = xbox_KeQueryBasePriorityThread(GetCurrentThread());
    if (got == set) { printf("  ok   set %-5ld reads back %ld\n", (long)set, (long)got); return; }
    printf("  FAIL set %-5ld reads back %ld\n", (long)set, (long)got);
    ++failures;
}

int main(void)
{
    /* The value JSRF actually sets. */
    roundtrip(16);

    /* The seven that survived bucketing, which must not regress. */
    roundtrip(-15); roundtrip(-2); roundtrip(-1);
    roundtrip(0);   roundtrip(1);  roundtrip(2);  roundtrip(15);

    /* And the ones that did not: every value the old mapping collapsed. */
    roundtrip(-14); roundtrip(-9); roundtrip(-3);
    roundtrip(3);   roundtrip(9);  roundtrip(14);
    roundtrip(17);  roundtrip(31);

    /* The previous value must also be the EXACT one, not a bucket: a caller
     * that saves and restores would otherwise corrupt what it restores. */
    {
        LONG prev;
        xbox_KeSetBasePriorityThread(GetCurrentThread(), 9);
        prev = xbox_KeSetBasePriorityThread(GetCurrentThread(), 0);
        if (prev == 9) printf("  ok   the returned previous value is exact\n");
        else { printf("  FAIL previous reads %ld, want 9\n", (long)prev); ++failures; }
    }

    printf("thread_priority_roundtrip_test: %s\n", failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
