/* G73: THE WRITE-BACK WITHOUT THE WAIT (RECOMP_METAL_ASYNC_WRITEBACK).
 *
 * The switch changes WHEN guest RAM becomes current, so the properties that
 * make it safe are all "a reader still gets the right bytes":
 *
 *   swap   -- a swap away from a dirty surface leaves guest RAM alone (the
 *             write-back is in flight), and naming the range later produces
 *             exactly the bytes an eager read-back would have.
 *   flip   -- naming ONE range does not write back the bound surface. This is
 *             the drain the switch exists to remove; with only DEFER_SWAP the
 *             bound surface is synced whatever range is named.
 *   texture -- a draw that samples the bound, dirty surface as a texture sees
 *             what the GPU drew, not the stale guest RAM under it. This is the
 *             swap's copy quad, and the reader that broke the parked
 *             async-flip experiment.
 *
 * The OFF arm (DEFER_SWAP alone) is the control for the last two: there the
 * bound surface IS written back by the flip, and the texture read DOES see
 * guest RAM -- so a pass of the ON arm cannot be "the draw never landed".
 * Driven twice by ctest because the accessors cache their getenv. */
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

static uint8_t bufA[BYTES], bufB[BYTES], bufC[BYTES], bufD[BYTES], bufE[BYTES], tex[512];

/* One textured triangle into `target`, sampling `src` (swizzled 16-bit when
 * `linear` is 0, the linear R5G6B5 image rectangle a render target is read as
 * when it is 1). */
static int draw(uint8_t *target, const uint8_t *src, size_t src_bytes, int linear)
{
    NV2ATextureCopy s = {0};
    float v[3][16][4] = {{{0}}};
    s.width = s.height = s.clip_w = s.clip_h = W;
    s.pitch = s.target_pitch = PITCH; s.target_bpp = 2; s.levels = 1;
    s.texture_mask = 1; s.linear = (uint32_t)linear;
    for (unsigned i = 0; i < 3; ++i) { v[i][0][3] = 1; v[i][9][3] = 1; }
    v[1][0][0] = (float)W; v[2][0][1] = (float)H;
    v[1][9][0] = linear ? (float)W : 1.0f; v[2][9][1] = linear ? (float)H : 1.0f;
    return nv2a_metal_draw(&s, src, src_bytes, target, BYTES, NULL, 0, v, 3, 5);
}

static int all_sentinel(const uint8_t *p)
{
    for (size_t i = 0; i < BYTES; ++i) if (p[i] != SENTINEL) return 0;
    return 1;
}

#define STEP(x) do { if ((x) < 0) { printf("metal_async_writeback_test: skipped (%s)\n", \
                                           nv2a_metal_last_reject()); return 0; } } while (0)

int main(void)
{
    const char *want = getenv("JSRF_EXPECT_ASYNC");
    int on = want ? atoi(want) : 0;

    for (unsigned i = 0; i < sizeof tex; ++i) tex[i] = (uint8_t)(i * 73 + 11);
    memset(bufA, 0xCC, BYTES); memset(bufB, 0x33, BYTES); memset(bufC, 0xCC, BYTES);
    memset(bufD, 0x11, BYTES); memset(bufE, 0x11, BYTES);

    /* The reference: C drawn exactly as A will be, and read back eagerly
     * (C is bound, so naming it syncs it in both arms). */
    STEP(draw(bufC, tex, sizeof tex, 0));
    CHECK(nv2a_metal_sync_range(bufC, BYTES) == 1, "sync_range(C) failed");

    /* ---- swap ---- */
    STEP(draw(bufA, tex, sizeof tex, 0));
    memset(bufA, SENTINEL, BYTES);          /* guest RAM made wrong after the draw */
    STEP(draw(bufB, tex, sizeof tex, 0));   /* the swap away from A: deferred in both arms */
    CHECK(all_sentinel(bufA), "the swap wrote A back: nothing was deferred");

    /* ---- flip: name A only; B is bound and dirty ---- */
    memset(bufB, SENTINEL, BYTES);
    CHECK(nv2a_metal_sync_range(bufA, BYTES) == 1, "sync_range(A) failed");
    CHECK(memcmp(bufA, bufC, BYTES) == 0,
          "naming A did not produce the bytes an eager read-back gives: the flip would present a wrong frame");
    if (on)
        CHECK(all_sentinel(bufB),
              "naming A wrote back the bound surface B -- the flip still drains for a surface it does not show");
    else
        CHECK(!all_sentinel(bufB),
              "control: without the switch naming any range syncs the bound surface, and it did not");

    /* ---- texture: B is bound and dirty with guest RAM wrong; D samples it ---- */
    STEP(draw(bufB, tex, sizeof tex, 0));
    memset(bufB, SENTINEL, BYTES);
    STEP(draw(bufD, bufB, BYTES, 1));
    /* E samples the true bytes of B, from a copy made after paying B. */
    CHECK(nv2a_metal_sync_range(bufD, BYTES) == 1, "sync_range(D) failed");
    CHECK(nv2a_metal_sync_range(bufB, BYTES) == 1, "sync_range(B) failed");
    CHECK(!all_sentinel(bufB), "B was never written back when named");
    {
        uint8_t copyB[BYTES];
        memcpy(copyB, bufB, BYTES);
        STEP(draw(bufE, copyB, BYTES, 1));
        CHECK(nv2a_metal_sync_range(bufE, BYTES) == 1, "sync_range(E) failed");
    }
    if (on)
        CHECK(memcmp(bufD, bufE, BYTES) == 0,
              "a draw sampling the bound dirty surface did not see what the GPU drew (the swap's copy quad reads stale guest RAM)");
    else
        CHECK(memcmp(bufD, bufE, BYTES) != 0,
              "control: without the switch the sample reads guest RAM (the sentinel), and D came out as if it had not");

    if (failures) {
        fprintf(stderr, "metal_async_writeback_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("metal_async_writeback_test: ok (async=%d)\n", on);
    return 0;
}
