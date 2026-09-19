/* A VOICE IN NO LIST MUST NOT BE REPORTED AS AN IDLE VOICE IN A LIST.
 *
 * The idle trap means exactly one thing: this voice is LINKED INTO A LIST and
 * is not active, so DirectSound should take it out. `link(v) = v` is the
 * driver's own marker for "not in any list" -- SetupVoiceProcessor writes it
 * for all 256 voices at boot, RemoveIdleVoice writes it for every voice it
 * removes. So a self-linked voice fails the trap's own precondition.
 *
 * RECOMP_APU_SELFLINK_END already honours the marker, but only for the NEXT
 * pointer: it rewrites nxt to 0xFFFF so the walk stops AFTER the voice. The
 * voice itself is still processed and still raises. At a list HEAD that is
 * every subframe for ever, because nothing downstream can move a head.
 *
 * Player session 17 Sep 2026: v1 sat at the top of the 3D list, retired,
 * inactive and self-linked, and collected 13,651 raises against every other
 * voice's three or fewer, while the guest acknowledged each one and never
 * moved the head.
 *
 * THE HALF THAT MUST NOT BREAK. VOICE_ON prepends a voice that regs[top]
 * already names, so link(selected) = selected: a legitimate single-voice list
 * is self-linked AND ACTIVE. Suppressing the walk rather than the raise would
 * silence it. So this test drives all three states, and the third is the one
 * that would make the fix worse than the bug:
 *
 *   self-linked + INACTIVE  + switch on   -> no raise   (the fix)
 *   self-linked + INACTIVE  + switch off  -> raise      (negative control)
 *   LINKED      + inactive  + switch on   -> raise      (not over-broad)
 *
 * Driven twice by ctest, with and without the variable, because the accessor
 * caches its getenv in a static.
 *
 * BOTH ARMS SET RECOMP_APU_SELFLINK_END=1, which is NOT its default (that is
 * off) but IS the player's configuration and the only one in which this fix
 * means anything: without it a self-linked voice rings the walk to its
 * 256-iteration cap instead of terminating, which is a different bug with a
 * different fix. Holding it on in both arms keeps this test about one switch.
 * It is set EXPLICITLY and must stay that way even if the default ever moves:
 * a test that leans on a default is measuring the default.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_memory_layout.h"
#include "apu.h"
#include "apu_state.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

extern unsigned long g_idle_trap_raises;
extern unsigned long g_idle_trap_selflink_suppressed;
extern unsigned long g_idle_trap_selflink_encounters;
extern int mcpx_apu_idle_trap_selflink(void);

static MCPXAPUState *apu;
#define VOICE_BASE 0x20000u
#define HEAD   5u
#define BEHIND 6u

static void set_link(unsigned v, uint32_t next)
{
    stl_le_phys(address_space_memory,
                VOICE_BASE + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,
                next);
}
static void set_active(unsigned v, int on)
{
    stl_le_phys(address_space_memory,
                VOICE_BASE + v * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE,
                on ? NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE : 0u);
}

int main(void)
{
#if !defined(_WIN32) && defined(__aarch64__)
    const char *want = getenv("JSRF_EXPECT_SELFLINK");
    int expect_on = want ? atoi(want) : 0;      /* default OFF, for now */
    uint8_t xbe[0x400] = {0};
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    unsigned long before;
    volatile uint32_t *r;

    apu = calloc(1, sizeof(*apu));
    if (!apu) { fprintf(stderr, "alloc failed\n"); return 1; }
    *(uint32_t *)(xbe + 0x108) = sizeof(xbe);
    *(uint32_t *)(xbe + 0x120) = 0x10000;
    if (!xbox_MemoryLayoutInit(xbe, sizeof(xbe))) {
        fprintf(stderr, "memory layout init failed\n"); return 1; }
    g_apu_ram_ptr = xbox_GetMemoryBase();
    r = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + 0xFE800000u);

    CHECK(mcpx_apu_idle_trap_selflink() == expect_on,
          "switch reads %d, expected %d -- the two ctest arms are testing the "
          "same thing", mcpx_apu_idle_trap_selflink(), expect_on);

    apu->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    apu->regs[NV_PAPU_TVL2D] = HEAD;
    apu->regs[NV_PAPU_TVL3D] = 0xFFFF;
    apu->regs[NV_PAPU_TVLMP] = 0xFFFF;
    r[NV_PAPU_FETFORCE1 / 4] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;

    /* 1. THE DEFECT: a self-linked, inactive voice at the head of the list.
     *    This is v1 in the player's session. */
    set_link(HEAD, HEAD);
    set_active(HEAD, 0);
    before = g_idle_trap_raises;
    mcpx_apu_vp_frame(apu, mixbins);
    CHECK(g_idle_trap_selflink_encounters >= 1,
          "the encounter counter did not move on a self-linked head, so a "
          "zero reading from it in a run would mean nothing");
    if (expect_on) {
        CHECK(g_idle_trap_raises == before,
              "a self-linked voice -- one the driver's own convention says is "
              "in NO list -- still raised the idle trap. This is the 13,651 "
              "raises of 17 Sep");
        CHECK(g_idle_trap_selflink_suppressed >= 1,
              "the raise was withheld but not counted, so a run cannot say "
              "how much of its storm this removed");
    } else {
        CHECK(g_idle_trap_raises == before + 1,
              "the switch is OFF and the raise did not happen anyway -- the "
              "off arm is not a control, and every A/B against it compares "
              "two identical arms");
    }

    /* 2. THE HALF THAT MUST NOT BREAK: self-linked but ACTIVE is a legitimate
     *    single-voice list, because VOICE_ON prepends a voice regs[top]
     *    already names. It must not raise in EITHER arm -- an active voice is
     *    not idle -- and the walk must still have processed it. */
    r[NV_PAPU_FECTL / 4] &= ~NV_PAPU_FECTL_FEMETHMODE;   /* resume the front end */
    set_active(HEAD, 1);
    before = g_idle_trap_raises;
    mcpx_apu_vp_frame(apu, mixbins);
    CHECK(g_idle_trap_raises == before,
          "an ACTIVE voice raised the idle trap. The trap is for INACTIVE "
          "voices; firing here means it fires on everything");

    /* 3. NOT OVER-BROAD: a genuinely linked, inactive voice must still raise
     *    in BOTH arms. This is how DirectSound learns to recycle a voice, and
     *    suppressing it is the failure the whole re-raise machinery exists to
     *    prevent.
     *
     *    BEHIND does the raising, not HEAD. The edge rule latches a voice once
     *    it has been reported and will not report it again until it is re-ONd,
     *    so reusing HEAD here tests the latch rather than this switch -- which
     *    is exactly what the first version of this test did, and it failed for
     *    that reason rather than for the one its message claimed. */
    r[NV_PAPU_FECTL / 4] &= ~NV_PAPU_FECTL_FEMETHMODE;
    set_link(HEAD, BEHIND);          /* really in a list now */
    set_link(BEHIND, 0xFFFF);
    set_active(HEAD, 1);             /* active: walks past without raising */
    set_active(BEHIND, 0);           /* linked AND inactive: must raise */
    before = g_idle_trap_raises;
    mcpx_apu_vp_frame(apu, mixbins);
    CHECK(g_idle_trap_raises == before + 1,
          "a genuinely LINKED inactive voice did not raise. The fix is "
          "over-broad: it has suppressed the retirement path DirectSound "
          "needs, which is worse than the storm it was meant to stop");

    free(apu);
    if (failures) {
        fprintf(stderr, "apu_idle_trap_selflink_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("apu_idle_trap_selflink_test: ok (switch=%d)\n", expect_on);
    return 0;
#else
    printf("apu_idle_trap_selflink_test: skipped (needs macOS/aarch64)\n");
    return 0;
#endif
}
