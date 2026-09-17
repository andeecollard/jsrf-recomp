/* DOES THE DECODE-PAIR GUARD ACTUALLY HOLD THE PAIR?
 *
 * RECOMP_APU_FEDEC_HOLD stops a guest method overwriting the FEDECMETH/
 * FEDECPARAM pair while the front end is TRAPPED and the guest's ISR has not
 * read it yet. Without it the ISR gets our SE2FE_IDLE_VOICE method 0x8000
 * paired with somebody else's argument, and since SET_ANTECEDENT_VOICE's
 * argument is a voice handle it passes the ISR's `h >= 0x100` check and is
 * dereferenced. On 17 Sep 2026 that crashed the title inside the guest ISR
 * shortly after New Game.
 *
 * The guard was off by default from the day it was written until that crash,
 * and the comment on it said "default on" while the accessor said otherwise.
 * So this test exists to make the DEFAULT itself a tested property, not a
 * comment: a silent flip back to off would be a crash in a player's build.
 *
 * G10's shape, both halves, because a guard that holds unconditionally is as
 * wrong as one that never holds -- it would freeze the pair during normal
 * free-running operation and starve the ISR of real methods:
 *
 *   TRAPPED     -> the pair must be HELD and the counter must move
 *   FREE_RUNNING-> the pair must be WRITTEN and the counter must not move
 *
 * Driven by ctest twice, with and without the environment variable, because
 * the accessor caches its getenv in a static on first use and one process
 * therefore only ever sees one value.
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

extern unsigned long g_apu_fedec_held;
extern void mcpx_apu_vp_write(void *opaque, uint32_t addr, uint64_t val,
                              unsigned int size);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

/* A method number outside every range fe_method dispatches, so the pair is
 * exercised without any voice state being touched. The write to the pair
 * happens before the dispatch switch, which is the whole point. */
#define HARMLESS_METHOD 0x00001FFCu

static void set_methmode(MCPXAPUState *d, uint32_t mode)
{
    d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
    d->regs[NV_PAPU_FECTL] |= mode;
}

int main(void)
{
    /* The expected default, passed in by ctest, so the two registrations
     * differ in exactly one thing and neither can silently test the other. */
    const char *want = getenv("JSRF_EXPECT_HOLD");
    int expect_hold = want ? atoi(want) : 1;
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    unsigned long before;

    if (!d) { fprintf(stderr, "alloc failed\n"); return 1; }

    /* THE GUARD'S OWN CONDITION: front end TRAPPED, pair outstanding. */
    set_methmode(d, NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    d->regs[NV_PAPU_FEDECMETH]  = 0x8000u;   /* SE2FE_IDLE_VOICE, as raised */
    d->regs[NV_PAPU_FEDECPARAM] = 0x00C8u;   /* the handle the ISR must read */
    before = g_apu_fedec_held;
    mcpx_apu_vp_write(d, HARMLESS_METHOD, 0xDEADBEEFu, 4);

    if (expect_hold) {
        CHECK(g_apu_fedec_held == before + 1,
              "TRAPPED: the guard did not hold -- held stayed at %lu. The "
              "default has flipped off, which is the crash of 17 Sep 2026 "
              "back in a player's build", before);
        CHECK(d->regs[NV_PAPU_FEDECMETH] == 0x8000u,
              "TRAPPED: FEDECMETH was overwritten with 0x%08X -- the ISR will "
              "read a method we did not send",
              (unsigned)d->regs[NV_PAPU_FEDECMETH]);
        CHECK(d->regs[NV_PAPU_FEDECPARAM] == 0x00C8u,
              "TRAPPED: FEDECPARAM was overwritten with 0x%08X -- this is the "
              "exact clobber that hands the ISR a bogus voice handle to "
              "dereference", (unsigned)d->regs[NV_PAPU_FEDECPARAM]);
    } else {
        CHECK(g_apu_fedec_held == before,
              "the guard held with RECOMP_APU_FEDEC_HOLD=0 -- the off switch "
              "does nothing, so every A/B against it was two identical arms");
        CHECK(d->regs[NV_PAPU_FEDECPARAM] == 0xDEADBEEFu,
              "guard off: FEDECPARAM should have been clobbered to 0xDEADBEEF "
              "and reads 0x%08X. This is the NEGATIVE control -- if the pair "
              "survives with the guard off, the test proves nothing when it "
              "survives with the guard on",
              (unsigned)d->regs[NV_PAPU_FEDECPARAM]);
    }

    /* THE HEALTHY STATE. A free-running front end has no unread pair, so the
     * guard must stand aside whatever its setting -- holding here would starve
     * the ISR of the methods it is waiting for. */
    set_methmode(d, NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING);
    d->regs[NV_PAPU_FEDECMETH]  = 0x8000u;
    d->regs[NV_PAPU_FEDECPARAM] = 0x00C8u;
    before = g_apu_fedec_held;
    mcpx_apu_vp_write(d, HARMLESS_METHOD, 0x12345678u, 4);
    CHECK(g_apu_fedec_held == before,
          "FREE_RUNNING: the guard held anyway (held %lu -> %lu). It fires on "
          "everything, so a non-zero held= in a run says nothing about the "
          "window it was added to measure", before, g_apu_fedec_held);
    CHECK(d->regs[NV_PAPU_FEDECPARAM] == 0x12345678u,
          "FREE_RUNNING: the pair was not written (0x%08X). A front end that "
          "is not trapped must pass methods straight through",
          (unsigned)d->regs[NV_PAPU_FEDECPARAM]);

    free(d);
    if (failures) {
        fprintf(stderr, "apu_fedec_hold_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("apu_fedec_hold_test: ok (expect_hold=%d)\n", expect_hold);
    return 0;
}
