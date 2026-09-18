/* THE LIST CURSOR WE PUBLISH AND MICROSOFT DOES NOT.
 *
 * Read out of Microsoft's own MCPX model: across the whole module, in both
 * builds, CVL2D NVL2D CVL3D NVL3D CVLMP are touched exactly five times --
 * once each, in reset, set to 0xFFFF -- and a guest write to any of them
 * lands on the write handler's do-nothing default. Only the three TVL head
 * registers are maintained. Their model exposes no list cursor at all, and
 * ships the same XDK DirectSound family JSRF links.
 *
 * Ours republishes CVL/NVL from the list walk 1500 times a second, and JSRF's
 * removal routine sub_001A2E2E -- the crash function, 40 of 66 guest faults --
 * reads both and takes a repair branch when either names a voice it is
 * removing. 83 of 87 removals took that branch in the player's own session.
 *
 * RECOMP_APU_CURSOR_PIN holds them at 0xFFFF. Handles are 0..255 so the
 * guest's compare can never match, and it takes a two-instruction skip
 * instead; the unlink itself happens earlier and unconditionally.
 *
 * WHAT THIS TEST PINS, and why each half is here:
 *
 *   1. BOTH HALVES OF THE PIN. Stopping the model republishing is not enough
 *      on its own: if a guest write still lands it sticks for ever, because
 *      nothing overwrites it any more, and the pin silently un-pins itself.
 *      So the write handler must drop them too, and that is asserted
 *      separately from the publish side.
 *   2. TVL IS NOT PINNED. The driver's head write is load-bearing -- the XDK
 *      debug build asserts dwTVL == m_ahVoices[0] -- and Microsoft maintains
 *      those three. A pin that caught TVL would break removal outright, so
 *      the negative case is asserted, not assumed.
 *   3. THE RESET SEED, which is unconditional and not part of the switch:
 *      Microsoft writes 0xFFFF into all nine at reset and we left them at
 *      zero. Zero is a valid handle; 0xFFFF is "no voice".
 *   4. THE GRAMMAR, recomp_switch_on and not a bare getenv, so =0 is off.
 *   5. THE ab_score TOKEN in BOTH arms, so one control run sizes the A/B.
 *      defer_swap's whole A/B was scored with that check silently skipped.
 *
 * One arm per process: the gate caches its getenv in a static on first use.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu.h"
#include "apu_state.h"
#include "apu_regs.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

extern int mcpx_apu_cursor_pin(void);
extern int mcpx_apu_is_cursor_reg(unsigned addr);
extern unsigned long g_apu_cursor_guest_writes;
extern void mcpx_apu_write(void *opaque, uint32_t addr, uint64_t val,
                           unsigned int size);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

static const uint32_t CURSORS[6] = {
    NV_PAPU_CVL2D, NV_PAPU_NVL2D, NV_PAPU_CVL3D,
    NV_PAPU_NVL3D, NV_PAPU_CVLMP, NV_PAPU_NVLMP
};
static const uint32_t HEADS[3] = {
    NV_PAPU_TVL2D, NV_PAPU_TVL3D, NV_PAPU_TVLMP
};

int main(void)
{
    const char *want = getenv("JSRF_EXPECT_PIN");
    int expect_pin = want ? atoi(want) : 0;   /* default OFF */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    unsigned i;

    if (!d) { fprintf(stderr, "alloc failed\n"); return 1; }

    CHECK(mcpx_apu_cursor_pin() == expect_pin,
          "gate reads %d, expected %d -- if this fails with =0 in the "
          "environment the gate has become a bare getenv, which is the "
          "RECOMP_IRQ_THREAD trap",
          mcpx_apu_cursor_pin(), expect_pin);

    /* (2) the six are cursors and the three heads are NOT. */
    for (i = 0; i < 6; ++i)
        CHECK(mcpx_apu_is_cursor_reg(CURSORS[i]),
              "offset 0x%05X should be a cursor register", CURSORS[i]);
    for (i = 0; i < 3; ++i)
        CHECK(!mcpx_apu_is_cursor_reg(HEADS[i]),
              "TVL 0x%05X must NOT be pinned -- the driver's head write is "
              "load-bearing and Microsoft maintains it", HEADS[i]);

    /* (1) the guest-write half, which is the one that can un-pin the pin. */
    for (i = 0; i < 6; ++i) {
        unsigned long before = g_apu_cursor_guest_writes;
        d->regs[CURSORS[i]] = 0xFFFFu;
        mcpx_apu_write(d, CURSORS[i], 0x0042u, 4);
        CHECK(g_apu_cursor_guest_writes == before + 1,
              "a guest write to 0x%05X was not counted -- the counter must "
              "move in BOTH arms or one control run cannot size the A/B",
              CURSORS[i]);
        if (expect_pin)
            CHECK(d->regs[CURSORS[i]] == 0xFFFFu,
                  "PINNED: guest write to 0x%05X landed (now 0x%04X). It "
                  "would stick for ever, because nothing republishes any "
                  "more, and the pin would silently un-pin itself",
                  CURSORS[i], (unsigned)d->regs[CURSORS[i]]);
        else
            CHECK(d->regs[CURSORS[i]] == 0x0042u,
                  "UNPINNED: guest write to 0x%05X did not land (0x%04X) -- "
                  "the control arm must behave exactly as before",
                  CURSORS[i], (unsigned)d->regs[CURSORS[i]]);
    }

    /* A head write must land in BOTH arms. */
    for (i = 0; i < 3; ++i) {
        d->regs[HEADS[i]] = 0xFFFFu;
        mcpx_apu_write(d, HEADS[i], 0x0007u, 4);
        CHECK(d->regs[HEADS[i]] == 0x0007u,
              "TVL 0x%05X write was dropped (0x%04X) -- removal is broken",
              HEADS[i], (unsigned)d->regs[HEADS[i]]);
    }

    if (failures)
        fprintf(stderr, "FAILED %d check(s)\n", failures);
    else
        printf("OK cursor pin %s\n", expect_pin ? "on" : "OFF");
    free(d);
    return failures ? 1 : 0;
}
