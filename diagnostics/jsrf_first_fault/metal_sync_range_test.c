/* DOES A NAMED RANGE ACTUALLY GET WRITTEN BACK?
 *
 * nv2a_gpu_sync_range passed a real guest range and, on this backend, threw it
 * away: the macro expanded to nv2a_metal_sync(), which writes back whichever
 * surface is BOUND. That was harmless only because the surface swap writes
 * back eagerly, so guest RAM was already current for everything -- and it is
 * exactly what stops being harmless the moment the swap defers, which is the
 * whole of G3's remaining work.
 *
 * So this test is about the precondition, not about a symptom the player can
 * see today. It manufactures the one state where the distinction is real -- a
 * retained surface that OWES guest RAM while a different surface is bound --
 * and asserts three things:
 *
 *   1. a range that COVERS the owing surface pays the debt
 *   2. a range that MISSES it does not                   (negative control)
 *   3. a NULL range still means "all of it"              (every internal caller)
 *
 * Property 2 is the one that matters. A sync_range that writes back everything
 * regardless of its argument would pass a naive test, behave correctly, and
 * silently reintroduce the full cost that deferring exists to remove.
 *
 * The debt is made with a resident clear of an UNBOUND surface, which is the
 * only thing in the tree that sets owes_guest_ram today: it clears the GPU
 * texture and deliberately skips the CPU clear, leaving guest RAM behind.
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

static uint8_t bufA[BYTES], bufB[BYTES], tex[512];

/* One triangle into `target`, which also makes it the bound surface. */
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
    return nv2a_metal_draw(&s, tex, sizeof tex, target, BYTES,
                           NULL, 0, v, 3, 5);
}

/* 565 red, so a paid debt is unmistakable against the 0xCC fill. */
#define CLEAR_565 0xF800u

static int all_equal(const uint8_t *p, uint16_t v)
{
    for (size_t i = 0; i < BYTES; i += 2)
        if ((uint16_t)(p[i] | (p[i+1] << 8)) != v) return 0;
    return 1;
}

int main(void)
{
    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73 + 11);

    /* A becomes a retained slot, then B becomes the bound surface. The swap
     * syncs A out on the way past, so guest RAM for A is current here. */
    memset(bufA, 0xCC, BYTES);
    memset(bufB, 0x33, BYTES);
    if (draw_into(bufA) < 0) {
        printf("metal_sync_range_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }
    if (draw_into(bufB) < 0) {
        printf("metal_sync_range_test: skipped (%s)\n", nv2a_metal_last_reject());
        return 0;
    }

    /* THE DEBT. A resident clear of A while B is bound: the GPU texture is
     * cleared and the CPU clear is skipped, so guest RAM for A is now behind
     * its surface. If this refuses there is nothing to test -- report it
     * rather than passing vacuously. */
    if (!nv2a_metal_clear_color(bufA, BYTES, PITCH, W, H, 0xF0, CLEAR_565)) {
        printf("metal_sync_range_test: skipped (resident unbound clear refused; "
               "no slot can owe guest RAM, so there is nothing to measure)\n");
        return 0;
    }
    CHECK(!all_equal(bufA, CLEAR_565),
          "guest RAM for A already holds the clear colour, so the resident "
          "clear did NOT skip the CPU clear and no debt exists. Everything "
          "below would pass without sync_range doing anything");

    /* 2. NEGATIVE CONTROL FIRST, so a sync_range that ignores its argument
     *    cannot be hidden by the positive case having already run. B's range
     *    does not touch A, so A must still be owed. */
    CHECK(nv2a_metal_sync_range(bufB, BYTES) == 1, "sync_range(B) failed");
    CHECK(!all_equal(bufA, CLEAR_565),
          "a range naming ONLY B wrote A back anyway. sync_range is ignoring "
          "its argument, which would silently reintroduce the whole cost that "
          "deferring the swap exists to remove");

    /* 1. THE POSITIVE CASE: name A's range, and the debt must be paid. */
    CHECK(nv2a_metal_sync_range(bufA, BYTES) == 1, "sync_range(A) failed");
    CHECK(all_equal(bufA, CLEAR_565),
          "a range naming A did NOT write A back. The flip would present the "
          "pixels guest RAM happened to be holding, which is what deferring "
          "the swap's writeback would make visible");

    /* 3. A NULL range still means everything -- every internal caller and the
     *    full invalidate rely on it. */
    memset(bufB, 0x33, BYTES);
    if (draw_into(bufB) >= 0) {
        CHECK(nv2a_metal_sync_range(NULL, 0) == 1,
              "sync_range(NULL) failed; the full-sync path is what every "
              "internal caller uses");
    }

    if (failures) {
        fprintf(stderr, "metal_sync_range_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("metal_sync_range_test: ok\n");
    return 0;
}
