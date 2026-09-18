/* DOES GUEST RAM EVER NEED THE COLOUR SURFACE WRITTEN BACK AT A SWAP?
 *
 * RECOMP_METAL_NO_COLOUR_SYNC says "assume not" and skips the write-back. It
 * is the same question RECOMP_METAL_NO_DEPTH_SYNC asked of depth and won 9.1%
 * of the frame with, aimed at the only thing the swap still writes back.
 *
 * The switch cannot be evaluated by looking at it. A skip that quietly did
 * nothing, and a skip that fired in both arms, both produce a run that boots,
 * presents and reports -- and this tree has already scored an A/B whose
 * control arm silently took the treatment. So the behaviour is pinned here,
 * against the real backend, with BOTH arms load-bearing:
 *
 *   no_colour_sync OFF -> guest RAM MUST receive the surface at the swap.
 *        The control. If it did not, the A/B's two arms are the same arm and
 *        any frame-time difference between them is noise with a story on it.
 *   no_colour_sync ON  -> guest RAM MUST be left exactly as the CPU last
 *        wrote it. If it were written anyway the switch saves nothing, and a
 *        measured win would have to be coming from somewhere else.
 *
 * A test with only the second arm would pass against code that never writes
 * colour back at all; a test with only the first would pass against code where
 * the switch is dead.
 *
 * AND THE THIRD ARM IS THE ONE THAT PROTECTS SOMETHING. Skipping the colour
 * write-back also lets the 4.9 MB getBytes that feeds it be elided -- but on
 * the software tail the depth write-back reads its depth out of THAT SAME
 * buffer's alpha channel. Elide the read while that path is live and the
 * guest's D24S8 surface is written from uninitialised heap, with no symptom:
 * the sync still returns 1, the frame still presents, and the corruption is
 * in a buffer nothing in this harness looks at. So the third arm forces the
 * software tail (RECOMP_METAL_HW=0) with colour skipped and depth NOT skipped,
 * and requires depth to arrive in guest RAM anyway.
 *
 * NEGATIVE CONTROLS, all three run before this was committed: make the skip
 * unconditional and "colour left alone" passes while the OFF arm fails; make
 * the skip never fire and the ON arm fails; drop the depth path from the
 * read-back's liveness test and the software-tail arm fails.
 *
 * Driven once per arm by ctest, because every switch accessor in the backend
 * caches its getenv in a static -- one process only ever sees one value.
 */
#include "nv2a_metal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

#define W 16u
#define H 16u
#define PITCH (W * 2u)
#define BYTES ((size_t)PITCH * H)
#define DPITCH (W * 4u)
#define DBYTES ((size_t)DPITCH * H)
#define SENTINEL 0x5Au

static uint8_t bufA[BYTES], bufB[BYTES];
static uint8_t depthA[DBYTES], depthB[DBYTES], tex[512];

/* Depth test AND write are both on, which is the pair nv2a_metal_draw reads to
 * mark depth_dirty. The software tail needs that to be true for the third arm
 * to mean anything: without a dirty depth buffer the read-back's liveness test
 * is never asked the question this test exists to ask it. */
static int draw_into(uint8_t *target, uint8_t *depth)
{
    NV2ATextureCopy s = {0};
    float v[3][16][4] = {{{0}}};
    s.width = s.height = s.clip_w = s.clip_h = W;
    s.pitch = s.target_pitch = PITCH; s.target_bpp = 2; s.levels = 1;
    s.texture_mask = 1;
    s.depth_test = 1; s.depth_write = 1; s.depth_pitch = DPITCH;
    for (unsigned i = 0; i < 3; ++i) { v[i][0][3] = 1; v[i][9][3] = 1; }
    v[1][0][0] = v[1][9][0] = (float)W;
    v[2][0][1] = v[2][9][1] = (float)H;
    return nv2a_metal_draw(&s, tex, sizeof tex, target, BYTES,
                           depth, DBYTES, v, 3, 5);
}

static int all_sentinel(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != SENTINEL) return 0;
    return 1;
}

int main(void)
{
    const char *want_colour = getenv("JSRF_EXPECT_COLOUR");
    const char *want_depth  = getenv("JSRF_EXPECT_DEPTH");
    /* Absent means "do not assert", not "expect zero". An arm that forgot to
     * say what it wanted would otherwise assert the OFF behaviour and pass for
     * the wrong reason. */
    int expect_colour = want_colour ? atoi(want_colour) : -1;
    int expect_depth  = want_depth  ? atoi(want_depth)  : -1;

    if (expect_colour < 0) {
        fprintf(stderr, "metal_colour_sync_test: JSRF_EXPECT_COLOUR must be "
                "set -- an arm that does not name its expectation is not an "
                "arm\n");
        return 1;
    }

    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73 + 11);
    memset(bufA, 0xCC, BYTES);
    memset(bufB, 0x33, BYTES);
    memset(depthA, 0, DBYTES);
    memset(depthB, 0, DBYTES);

    /* Three draws so both surfaces exist and both are in the slot cache, and
     * so A is the bound one when the poison goes in. */
    if (draw_into(bufA, depthA) < 0 || draw_into(bufB, depthB) < 0
            || draw_into(bufA, depthA) < 0) {
        /* A rejection is not a failure. This needs a backend that accepts a
         * depth-writing draw, and "skipped" is honest where asserting would be
         * a false green -- the shape metal_defer_depth_test already uses. */
        printf("metal_colour_sync_test: skipped (%s)\n",
               nv2a_metal_last_reject());
        return 0;
    }

    /* Guest RAM for A is made wrong AFTER the draws, so whatever the swap does
     * next is visible: leave it (skipped) or overwrite it (written back).
     * Both halves are poisoned, because the third arm asks about depth while
     * colour is being skipped and the two must be distinguishable. */
    memset(bufA, SENTINEL, BYTES);
    memset(depthA, SENTINEL, DBYTES);

    /* The swap. A is unbound here, and nv2a_metal_sync runs before the rebind. */
    if (draw_into(bufB, depthB) < 0) {
        printf("metal_colour_sync_test: skipped (%s)\n",
               nv2a_metal_last_reject());
        return 0;
    }

    if (expect_colour) {
        CHECK(!all_sentinel(bufA, BYTES),
              "no_colour_sync is OFF, so the surface swap had to write the "
              "colour surface back to guest RAM and it did not. The control "
              "arm is taking the treatment: any A/B against it compares one "
              "arm with itself.");
    } else {
        CHECK(all_sentinel(bufA, BYTES),
              "no_colour_sync is ON, so the swap had to leave guest RAM "
              "exactly as the CPU wrote it, and %zu byte(s) of it changed. "
              "The skip is not firing, so the switch saves nothing and a "
              "measured win is coming from somewhere else.", BYTES);
    }

    if (expect_depth == 1) {
        CHECK(!all_sentinel(depthA, DBYTES),
              "the depth write-back was ENABLED and depth reaches guest RAM "
              "through the colour buffer's alpha on this tail, so eliding the "
              "read-back because colour is skipped has silently starved it. "
              "Guest RAM keeps the CPU's bytes and the frame still presents: "
              "this is the failure that has no symptom.");
    } else if (expect_depth == 0) {
        CHECK(all_sentinel(depthA, DBYTES),
              "the depth write-back was disabled but depth reached guest RAM "
              "anyway -- the two skips are not independent.");
    }

    if (failures) {
        fprintf(stderr, "metal_colour_sync_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: colour write-back %s%s\n",
           expect_colour ? "taken, as expected" : "skipped, as expected",
           expect_depth == 1 ? "; depth still written back" : "");
    return 0;
}
