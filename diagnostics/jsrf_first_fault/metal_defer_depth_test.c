/* THE DEPTH REFUSAL, AND THE ONE CONDITION THAT MAKES IT POINTLESS.
 *
 * RECOMP_METAL_DEFER_SWAP refuses to defer a surface swap whenever depth is
 * dirty, because surface_slot_writeback carries COLOUR only: defer with depth
 * outstanding and a later cache miss re-uploads stale depth from guest RAM.
 *
 * That refusal is why A2 was inert. Measured over a 240 s gameplay run it
 * fired 5,747-12,656 times against ONE successful deferral, so the swap kept
 * paying its full drain and read-back and [SYNC] kept owning the median frame.
 *
 * The argument holds exactly as long as the depth write-back happens at all.
 * With RECOMP_METAL_NO_DEPTH_SYNC on it does not: the write-back is skipped
 * and depth_dirty is cleared without depth ever reaching guest RAM. Refusing
 * to defer in order to protect a value nobody writes is not a safety property.
 *
 * SO THIS TEST PINS THE NARROWING, AND BOTH ARMS ARE LOAD-BEARING:
 *
 *   depth dirty, no_depth_sync OFF -> MUST NOT defer. This is the control. If
 *        it deferred, the narrowing would have removed a real guard and a
 *        cache miss could re-upload stale depth over a live frame.
 *   depth dirty, no_depth_sync ON  -> MUST defer. If it did not, the narrowing
 *        did nothing and A2 is still inert, which is the whole point.
 *
 * A test with only the second arm would pass against code that always defers.
 *
 * Driven twice by ctest because both accessors cache their getenv in a static.
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
#define SENTINEL 0x5Au

static uint8_t bufA[BYTES], bufB[BYTES], depth[BYTES * 2], tex[512];

/* Same shape as metal_defer_swap_test's draw, with depth test AND write on --
 * which is the pair nv2a_metal_draw reads to set depth_dirty. Without both,
 * this test would exercise the ordinary deferral path and prove nothing. */
static int draw_into(uint8_t *target)
{
    NV2ATextureCopy s = {0};
    float v[3][16][4] = {{{0}}};
    s.width = s.height = s.clip_w = s.clip_h = W;
    s.pitch = s.target_pitch = PITCH; s.target_bpp = 2; s.levels = 1;
    s.texture_mask = 1;
    /* depth_pitch must describe a real D24S8 surface or the backend
     * rejects the draw and this test reports "skipped", which proves
     * nothing. 16x16 at 4 bytes is the 1024 bytes `depth` provides. */
    s.depth_test = 1; s.depth_write = 1; s.depth_pitch = W * 4;
    for (unsigned i = 0; i < 3; ++i) { v[i][0][3] = 1; v[i][9][3] = 1; }
    v[1][0][0] = v[1][9][0] = (float)W;
    v[2][0][1] = v[2][9][1] = (float)H;
    return nv2a_metal_draw(&s, tex, sizeof tex, target, BYTES,
                           depth, sizeof depth, v, 3, 5);
}

static int all_sentinel(const uint8_t *p)
{
    for (size_t i = 0; i < BYTES; ++i) if (p[i] != SENTINEL) return 0;
    return 1;
}

int main(void)
{
    const char *want = getenv("JSRF_EXPECT_DEFER");
    int expect_defer = want ? atoi(want) : 0;

    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73 + 11);
    memset(bufA, 0xCC, BYTES);
    memset(bufB, 0x33, BYTES);
    memset(depth, 0, sizeof depth);

    if (draw_into(bufA) < 0 || draw_into(bufB) < 0 || draw_into(bufA) < 0) {
        /* A rejection here is not a failure: this path needs a backend that
         * accepts a depth-writing draw, and saying "skipped" is honest where
         * asserting would be a false green. */
        printf("metal_defer_depth_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }

    /* Guest RAM for A is made wrong AFTER the draw, so whatever the swap does
     * next is visible: leave it (deferred) or overwrite it (paid eagerly). */
    memset(bufA, SENTINEL, BYTES);

    if (draw_into(bufB) < 0) {
        printf("metal_defer_depth_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }

    if (expect_defer) {
        CHECK(all_sentinel(bufA),
              "depth was dirty and RECOMP_METAL_NO_DEPTH_SYNC was on, so the "
              "depth refusal is protecting a value nobody writes -- the swap "
              "should have deferred and did not. A2 is still inert.");
    } else {
        CHECK(!all_sentinel(bufA),
              "depth was dirty and the write-back was ENABLED, so deferring "
              "would risk a later cache miss re-uploading stale depth. The "
              "swap deferred anyway: the narrowing removed a real guard.");
    }

    if (failures) {
        fprintf(stderr, "metal_defer_depth_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS: depth refusal %s, as expected\n",
           expect_defer ? "correctly narrowed (deferred)" : "held (did not defer)");
    return 0;
}
