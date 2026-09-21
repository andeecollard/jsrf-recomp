/* WHICH HALF OF THE PIPELINE STILL HAS THE PICTURE.
 *
 * nv2a_surface_census_verdict() is the reading of RECOMP_SURFACE_CENSUS, and
 * it is the only part of that instrument that can be a ctest case: the
 * gathering needs a Metal device, the verdict is integer comparison over a
 * plain struct. That split is deliberate -- see NV2ASurfaceCensus.
 *
 * WHY THE VERDICT NEEDS A TEST AT ALL, when it is four comparisons. Because
 * the failure mode is not "it returns the wrong enum", it is "it returns
 * GPU-BLACK for a census that never read a texture", and that answer is
 * indistinguishable from a real finding in a log. A black screen reported by
 * an instrument that did not look is exactly the shape of wrong conclusion
 * CLAUDE.md's absence-measurement rule exists to stop, and it was reachable
 * here with one `continue` in the wrong place.
 *
 * The four cases below are the four states a real run can be in, written as
 * the black-screen session would see them:
 *
 *   NOTHING_HELD   nothing was read -- says nothing, and must not say black
 *   GPU_BLACK      the textures are black too: the draws made no pixels
 *   OWED           a texture has the frame, guest RAM does not: write-back
 *   PICTURE_HELD   both have it: the loss, if any, is past this point
 */
#include "nv2a_metal_state.h"
#include <stdio.h>

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got == want) return;
    fprintf(stderr, "FAIL %s: got %d want %d\n", what, got, want);
    ++failures;
}

int main(void)
{
    /* The load-menu reproducer's shape: three colour surfaces rotating, one
     * of them the one the guest names at the flip. */
    NV2ASurfaceCensus e[3];
    unsigned i;

    for (i = 0; i < 3; ++i) {
        e[i].target = 0x005F0000u + i * 0x98000u;
        e[i].w = 640; e[i].h = 480;
        e[i].bound = (i == 0);
        e[i].presented = (i == 0);
        e[i].owes_guest_ram = 0;
        e[i].gpu_nonzero = -1;
        e[i].guest_nonzero = -1;
    }

    /* 1. NOTHING READ. Every texture count is -1, which is what a census that
     *    could not read says. It must NOT come back black, and it must not
     *    come back black just because guest RAM is zero either. */
    for (i = 0; i < 3; ++i) e[i].guest_nonzero = 0;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_NOTHING_HELD, "no texture read");
    expect(nv2a_surface_census_verdict(NULL, 3),
           NV2A_SURFACE_CENSUS_NOTHING_HELD, "null census");
    expect(nv2a_surface_census_verdict(e, 0),
           NV2A_SURFACE_CENSUS_NOTHING_HELD, "empty census");

    /* 2. THE BLACK FRAME, measured. Textures read and all black: the draws
     *    themselves produced no pixels. */
    for (i = 0; i < 3; ++i) e[i].gpu_nonzero = 0;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_GPU_BLACK, "all textures black");

    /* 3. THE WRITE-BACK LOST IT. One surface holds a frame its guest RAM has
     *    not got -- the state clear_unbound_slot() creates on purpose and the
     *    one no guest-RAM instrument on this path can see. */
    e[1].gpu_nonzero = 123456; e[1].guest_nonzero = 0;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_OWED, "one surface owes guest RAM");

    /* 3b. OWED OUTRANKS a healthy surface elsewhere. The frame going missing
     *     is the finding; another slot being fine does not cancel it. */
    e[2].gpu_nonzero = 999; e[2].guest_nonzero = 999;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_OWED, "owed outranks a held surface");

    /* 4. THE POSITIVE CONTROL -- the visible frame the same run must also
     *    sample. Pixels in the texture and in guest RAM. If a session cannot
     *    produce this on a frame the player can see, its GPU-BLACK lines are
     *    not evidence of anything. */
    e[1].guest_nonzero = 123456;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_PICTURE_HELD, "picture in both");

    /* A texture with pixels whose guest count was never read is still the
     * weaker PICTURE_HELD claim, not OWED: -1 is "unread", never "zero". */
    e[1].guest_nonzero = -1; e[2].gpu_nonzero = 0; e[2].guest_nonzero = 0;
    expect(nv2a_surface_census_verdict(e, 3),
           NV2A_SURFACE_CENSUS_PICTURE_HELD, "unread guest count is not zero");

    /* Every verdict names itself, so a log line can be read without the enum
     * to hand -- and so an unknown value cannot print as one of the four. */
    for (i = 0; i <= NV2A_SURFACE_CENSUS_PICTURE_HELD; ++i)
        if (!nv2a_surface_census_verdict_text((int)i)) {
            fprintf(stderr, "FAIL verdict %u has no text\n", i);
            ++failures;
        }

    if (failures) {
        fprintf(stderr, "surface_census_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("surface_census_test: ok\n");
    return 0;
}
