/* DOES DEFERRING THE SWAP'S WRITEBACK ACTUALLY DEFER, AND STAY CORRECT?
 *
 * 18,937 of 18,942 surface swaps in a gameplay run are rebinds at a 100% cache
 * hit rate, and every one drains the GPU and reads 614 KB back so guest RAM
 * holds pixels that are sitting safely in the slot we are about to keep. That
 * is [SYNC] p50 = 9.0 ms of an 18.5 ms median frame.
 *
 * RECOMP_METAL_DEFER_SWAP marks the slot as owing guest RAM instead of paying,
 * and lets the flip's nv2a_metal_sync_range pay when it names the range.
 *
 * TWO PROPERTIES, AND BOTH HALVES ARE LOAD-BEARING:
 *
 *   it really defers  -- guest RAM must still hold the stale bytes right after
 *                        the swap. If it does not, the switch buys nothing and
 *                        every frame-time measurement of it is measuring noise.
 *   it stays correct  -- naming the range must then produce the real pixels.
 *                        If it does not, the flip presents whatever guest RAM
 *                        happened to hold, which is a visibly wrong frame.
 *
 * The OFF arm is the control: with the switch off the swap must write back
 * eagerly, so the stale bytes are gone before any range is named. A test that
 * only ran the ON arm could not tell "deferred" from "the draw never landed".
 *
 * Driven twice by ctest because the accessor caches its getenv in a static.
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

static uint8_t bufA[BYTES], bufB[BYTES], tex[512];

static int draw_into(uint8_t *target)
{
    NV2ATextureCopy s = {0};
    float v[3][16][4] = {{{0}}};
    s.width = s.height = s.clip_w = s.clip_h = W;
    s.pitch = s.target_pitch = PITCH; s.target_bpp = 2; s.levels = 1;
    s.texture_mask = 1;
    for (unsigned i = 0; i < 3; ++i) { v[i][0][3] = 1; v[i][9][3] = 1; }
    v[1][0][0] = v[1][9][0] = (float)W;
    v[2][0][1] = v[2][9][1] = (float)H;
    return nv2a_metal_draw(&s, tex, sizeof tex, target, BYTES, NULL, 0, v, 3, 5);
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

    if (draw_into(bufA) < 0 || draw_into(bufB) < 0) {
        printf("metal_defer_swap_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }
    /* Back to A and draw again, so A is the BOUND and DIRTY surface with a
     * slot of its own -- the exact state the deferral is about. */
    if (draw_into(bufA) < 0) {
        printf("metal_defer_swap_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }

    /* Guest RAM for A is deliberately made wrong AFTER the draw. Whatever the
     * swap does next is now visible: leave it (deferred) or overwrite it. */
    memset(bufA, SENTINEL, BYTES);

    if (draw_into(bufB) < 0) {
        printf("metal_defer_swap_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }

    if (expect_defer) {
        CHECK(all_sentinel(bufA),
              "the swap wrote A back even with RECOMP_METAL_DEFER_SWAP on. "
              "Nothing is deferred, so the switch buys no time and any "
              "frame-time A/B of it is measuring noise");
        /* AND IT MUST STILL BE PAYABLE. This is the half that decides whether
         * the flip shows a correct frame. */
        CHECK(nv2a_metal_sync_range(bufA, BYTES) == 1, "sync_range(A) failed");
        CHECK(!all_sentinel(bufA),
              "the debt was never paid: naming A's range left guest RAM "
              "holding the stale bytes. The flip would present exactly this, "
              "which is a visibly wrong frame");
    } else {
        CHECK(!all_sentinel(bufA),
              "the swap did NOT write A back with the switch off. The off arm "
              "is not a control, so the on arm proves nothing -- and this is "
              "also the eager writeback the whole tree currently relies on");
    }

    if (failures) {
        fprintf(stderr, "metal_defer_swap_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("metal_defer_swap_test: ok (defer=%d)\n", expect_defer);
    return 0;
}
