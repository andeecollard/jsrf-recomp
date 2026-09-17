/* XC_AUDIO must answer stereo PCM by default, and must still be able to
 * answer the old mono+AC3 word on request.
 *
 * Registered more than once, because the accessor caches its getenv in a
 * static and one process can only exercise one value. The arm that matters is
 * the DEFAULT one: the bug being fixed was a default, and a test that only
 * proved the override works would have passed against the old code too.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *want = getenv("JSRF_EXPECT_AUDIO");
    unsigned long expected = want && *want ? strtoul(want, NULL, 0) : 0;
    ULONG value = 0xDEADBEEF, type = 0, length = 0;
    NTSTATUS st = xbox_ExQueryNonVolatileSetting(XC_AUDIO, &type, &value,
                                                 sizeof(value), &length);

    if (st != STATUS_SUCCESS || type != 4 || length != sizeof(value)) {
        fprintf(stderr, "FAIL: status=0x%lX type=%lu length=%lu\n",
                (unsigned long)st, (unsigned long)type, (unsigned long)length);
        return 1;
    }
    if ((unsigned long)value != expected) {
        fprintf(stderr, "FAIL: XC_AUDIO answered 0x%08lX, expected 0x%08lX\n",
                (unsigned long)value, expected);
        return 1;
    }
    /* Name the encoding in the pass line, so a reader who has never seen
     * XC_AUDIO_FLAGS does not have to go and look it up to know what passed. */
    printf("PASS: XC_AUDIO = 0x%08lX (%s%s)\n", (unsigned long)value,
           (value & 3) == 0 ? "stereo" : (value & 3) == 1 ? "MONO" : "surround",
           (value & 0x10000) ? " + AC3" : " PCM");
    return 0;
}
