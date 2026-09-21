/* VOICE_ON MUST NOT LEAVE A CYCLE IN THE VOICE LIST.
 *
 * THE STATE THIS DRIVES IS THE PLAYER'S OWN, not an invented one. It is
 * reconstructed from the [VOICE-LINK] / [VOICE-TOP] stream of
 * ~/jsrf-build/preserved-logs/last-run_2026-09-21_ROBOY-BLACKSCREEN-FIRST-
 * ITAIL-KEEP.log at audio_frames 8695296 (t = 181.152 s), the moment the 3D
 * effect voice 15 stopped being retired:
 *
 *     TVL3D   = 0x0005          the guest's 3D list head
 *     link(5) = 0x000F          v15 is the SECOND entry, not the head
 *     link(15)= 0x000F          ...and carries the driver's "in no list"
 *                               marker, which RemoveIdleVoice had just written
 *
 * and then the guest issues SET_ANTECEDENT_VOICE(0x0002FFFF) + VOICE_ON(15),
 * which the log records as `link=000F->0005`.
 *
 * The old unconditional splice does link(15) = TVL3D = 5; TVL3D = 15, so the
 * list reads 15 -> 5 -> 15: a two-entry ring. Everything the list used to
 * reach is gone, because the only pointer to it was the field just
 * overwritten, and the frame walk then renders both members once per lap for
 * the rest of the run.
 *
 * WHAT EACH ARM PINS:
 *
 *   1  deep   v deeper in the list. Must be unlinked before the prepend, in
 *             EVERY switch arm: RECOMP_APU_LIST_DEEP_UNLINK is default ON and
 *             the state it repairs is one the hardware cannot be in. This is
 *             the case that fails without the fix.
 *   2  marker after any insert, link(v) must never still equal v. That is the
 *             driver's "not in any list" marker and VOICE_ON has just made it
 *             false; leaving it is a one-entry ring whose tail is unreachable.
 *   3  fresh  the ordinary insert -- a voice in no list -- must still prepend
 *             exactly as it always did. Without this the test would pass on a
 *             VOICE_ON that had been broken into a no-op.
 *
 * THE ASSERTION IS THE SHAPE OF THE LIST, read back out of guest RAM by a
 * bounded walk, not a counter. A counter here would be testing our own
 * bookkeeping; the defect is in what the guest's register file says afterwards,
 * which is what both the model's walk and the guest's ISR actually read.
 *
 * Registered four times by ctest, across the two switches that reach this
 * code, because both accessors cache their getenv in a static and a process is
 * therefore in one arm for its whole life. The `off` arm is the negative
 * control: it sets RECOMP_APU_LIST_DEEP_UNLINK=0 and REQUIRES the cycle, so
 * that a fix which silently stopped doing anything would fail here rather than
 * pass everywhere.
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

extern void mcpx_apu_vp_write(void *opaque, uint64_t addr, uint64_t val,
                              unsigned int size);
extern int mcpx_apu_list_deep_unlink(void);
extern int mcpx_apu_list_move_to_front(void);
extern unsigned long g_apu_voice_on_relink_count;
extern unsigned long g_apu_list_mtf_deep;
extern unsigned long g_apu_voice_on_self_link_normalised;
extern uint32_t mcpx_apu_idle_handoff_method(void *opaque, uint32_t method);
extern unsigned long g_idle_handoff_active;
extern unsigned long g_idle_handoff_active_held;

static MCPXAPUState *apu;
#define VOICE_BASE 0x20000u

static void set_link(unsigned v, uint32_t next)
{
    stl_le_phys(address_space_memory,
                VOICE_BASE + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,
                next);
}
static uint16_t get_link(unsigned v)
{
    return (uint16_t)(ldl_le_phys(address_space_memory,
                VOICE_BASE + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK)
            & NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
}

/* The frame walk's three exits, deliberately: 0xFFFF, an out-of-range handle,
 * and the iteration cap. Returns the handle that closed a cycle, or 0x10000
 * for an acyclic list, and prints the list it walked so a failure names the
 * shape rather than only the verdict. */
static uint32_t walk_3d(const char *what)
{
    uint8_t seen[MCPX_HW_MAX_VOICES] = {0};
    uint16_t cur = (uint16_t)apu->regs[NV_PAPU_TVL3D];
    uint32_t cycle = 0x10000;
    int i;
    fprintf(stderr, "    %-8s TVL3D=%04X:", what, cur);
    for (i = 0; i < MCPX_HW_MAX_VOICES && cur != 0xFFFF; i++) {
        if (cur >= MCPX_HW_MAX_VOICES) { fprintf(stderr, " BAD(%04X)", cur); break; }
        if (seen[cur]) { fprintf(stderr, " ->%u(AGAIN)", cur); cycle = cur; break; }
        seen[cur] = 1;
        fprintf(stderr, " ->%u", cur);
        cur = get_link(cur);
    }
    if (cur == 0xFFFF) fprintf(stderr, " ->end");
    fprintf(stderr, "\n");
    return cycle;
}

/* SET_ANTECEDENT_VOICE then VOICE_ON, through the real front-end dispatch --
 * the same two stores the guest makes. 0x0002FFFF is LST=3D_TOP with the
 * FFFF antecedent every one of this title's 1,379 inserts carries. */
static void voice_on_3d(unsigned h)
{
    mcpx_apu_vp_write(apu, NV1BA0_PIO_SET_ANTECEDENT_VOICE, 0x0002FFFFu, 4);
    mcpx_apu_vp_write(apu, NV1BA0_PIO_VOICE_ON, h, 4);
}

int main(int argc, char **argv)
{
#if !defined(_WIN32) && defined(__aarch64__)
    const char *arm = argc > 1 ? argv[1] : "on";
    int expect_unlink = strcmp(arm, "off") != 0;
    uint8_t xbe[0x400] = {0};
    uint32_t cycle;
    unsigned long deep_before;

    apu = calloc(1, sizeof(*apu));
    if (!apu) { fprintf(stderr, "alloc failed\n"); return 1; }
    qemu_mutex_init(&apu->lock);
    qemu_cond_init(&apu->cond);
    memcpy(xbe, "XBEH", 4);
    *(uint32_t *)(xbe + 0x104) = 0x10000;
    *(uint32_t *)(xbe + 0x108) = sizeof(xbe);
    *(uint32_t *)(xbe + 0x120) = 0x10000;
    if (!xbox_MemoryLayoutInit(xbe, sizeof(xbe))) {
        fprintf(stderr, "memory layout init failed\n"); return 1; }
    g_apu_ram_ptr = xbox_GetMemoryBase();

    /* EITHER switch repairs the deep case -- the split is about the head
     * no-op, and move_to_front must not lose a behaviour it had. So the arm is
     * checked against the disjunction, which is what the code branches on. */
    CHECK((mcpx_apu_list_deep_unlink() || mcpx_apu_list_move_to_front())
              == expect_unlink,
          "deep_unlink=%d move_to_front=%d, so the repair is %s, but this arm "
          "expects %s -- the ctest arms are testing the same thing",
          mcpx_apu_list_deep_unlink(), mcpx_apu_list_move_to_front(),
          (mcpx_apu_list_deep_unlink() || mcpx_apu_list_move_to_front())
              ? "on" : "off",
          expect_unlink ? "on" : "off");

    apu->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    apu->regs[NV_PAPU_TVL2D] = 0xFFFF;
    apu->regs[NV_PAPU_TVLMP] = 0xFFFF;

    /* ---- 1. THE DEEP RE-INSERT: the player's 15 -> 5 -> 15 ------------- */
    apu->regs[NV_PAPU_TVL3D] = 5;
    set_link(5, 15);
    set_link(15, 15);            /* RemoveIdleVoice's "in no list" marker */
    deep_before = g_apu_list_mtf_deep;
    walk_3d("before");
    voice_on_3d(15);
    cycle = walk_3d("after");

    CHECK(g_apu_voice_on_relink_count >= 1,
          "relink did not move on a VOICE_ON for a handle that WAS in the "
          "list -- the setup never built the state, so the verdict below is "
          "about nothing");
    CHECK(apu->regs[NV_PAPU_TVL3D] == 15,
          "TVL3D is %04X, not 000F: the insert did not make the voice the "
          "head, so CMcpxVoiceClient's dwTVL assertion would fire",
          (unsigned)apu->regs[NV_PAPU_TVL3D]);

    if (expect_unlink) {
        CHECK(cycle == 0x10000,
              "VOICE_ON(15) closed a cycle at voice %u. This is the defect: "
              "the splice wrote link(15) = TVL3D while the list still reached "
              "15, so the list rings and everything behind the splice point "
              "is unreachable", cycle);
        CHECK(get_link(5) == 0xFFFF,
              "link(5) reads %04X. The unlink copies link(15) into it, and "
              "link(15) was the self marker, which voice_list_unlink_at reads "
              "as end-of-list", get_link(5));
        CHECK(get_link(15) == 5,
              "link(15) reads %04X, not 0005 -- the prepend did not run, so "
              "v5 has been dropped from the list rather than moved behind v15",
              get_link(15));
        CHECK(g_apu_list_mtf_deep == deep_before + 1,
              "the deep case was repaired but not counted, so a run cannot "
              "say how often it happened");
    } else {
        /* THE NEGATIVE CONTROL. Without the unlink this MUST ring: if it does
         * not, the arms are not different and every comparison against this
         * one is comparing two identical things. */
        CHECK(cycle == 15,
              "the unlink is OFF and VOICE_ON(15) did NOT ring (cycle=%u). "
              "The off arm is not a control and this test proves nothing",
              cycle);
    }

    /* ---- 2. THE MARKER MUST NOT SURVIVE AN INSERT ---------------------- */
    /* v is already the head AND carries the marker: TVL3D -> v -> v. This is
     * mtf_head, where move_to_front deliberately writes nothing, and it is
     * what 107 of the player's 1,379 inserts produced. */
    apu->regs[NV_PAPU_TVL3D] = 9;
    set_link(9, 9);
    voice_on_3d(9);
    cycle = walk_3d("marker");
    CHECK(get_link(9) != 9,
          "link(9) still reads 0009 after VOICE_ON(9). That is the driver's "
          "'not in any list' marker on a voice this insert has just put IN a "
          "list, and it is a one-entry ring for every reader of the field");
    CHECK(cycle == 0x10000,
          "the list rings at voice %u after a head re-insert", cycle);
    CHECK(apu->regs[NV_PAPU_TVL3D] == 9,
          "TVL3D is %04X, not 0009", (unsigned)apu->regs[NV_PAPU_TVL3D]);
    CHECK(g_apu_voice_on_self_link_normalised >= 1,
          "the marker was cleared but not counted, so an A/B on it cannot "
          "see the event it prevents");

    /* ---- 3. THE ORDINARY INSERT MUST BE UNCHANGED ---------------------- */
    apu->regs[NV_PAPU_TVL3D] = 20;
    set_link(20, 0xFFFF);
    set_link(21, 21);            /* v21 is in no list: the marker, truthfully */
    voice_on_3d(21);
    cycle = walk_3d("fresh");
    CHECK(apu->regs[NV_PAPU_TVL3D] == 21 && get_link(21) == 20
          && get_link(20) == 0xFFFF,
          "a plain prepend of a voice that was in NO list came out as "
          "TVL3D=%04X link(21)=%04X link(20)=%04X, expected 0015/0014/FFFF. "
          "The repair has broken the ordinary path",
          (unsigned)apu->regs[NV_PAPU_TVL3D], get_link(21), get_link(20));
    CHECK(cycle == 0x10000, "the list rings at voice %u after a plain insert",
          cycle);

    /* ---- 4. THE STALE IDLE HAND-OVER --------------------------------- */
    /* THE OTHER HALF OF THE SAME SESSION, and the one the player hears.
     *
     * At af=8695040 the walk raised SE2FE_IDLE_VOICE for v15; at af=8695296,
     * 5.3 ms later and with FECTL still TRAPPED, VOICE_ON(15) arrived and made
     * it ACTIVE; and in the same audio frame the guest's RemoveIdleVoice read
     * the handle and wrote TVL3D 000F -> 0005. The VOICE-TOP-RING entry
     * carries A (the removed head was still ACTIVE) and T (cvl named exactly
     * that voice): the guest retired a voice that was playing, and v15's
     * lifecycle ends there while seven other voices keep cycling for 894 s.
     *
     * SE2FE_IDLE_VOICE means "this voice is in a list and is NOT ACTIVE". At
     * the hand-over -- the guest's FEDECMETH load, which is the last instant
     * the model owns the decision -- that is now false, so the method must be
     * withheld. Both halves are driven, because a guard that withholds
     * unconditionally would starve the ISR of every real retirement.
     *
     * Independent of the two list switches, so it runs in all four arms. */
    apu->regs[NV_PAPU_FEDECPARAM] = 15;
    stl_le_phys(address_space_memory,
                VOICE_BASE + 15 * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE,
                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    {
        unsigned long active_before = g_idle_handoff_active;
        uint32_t got = mcpx_apu_idle_handoff_method(apu, SE2FE_IDLE_VOICE);
        CHECK(g_idle_handoff_active == active_before + 1,
              "the hand-over did not notice that v15 was ACTIVE again, so a "
              "zero on that counter in a run would mean the instrument is "
              "dead rather than that the race did not happen");
        CHECK(got == 0,
              "the guest was handed method %04X for a voice that is PLAYING. "
              "RemoveIdleVoice retires it, DirectSound stops owning it, and "
              "nothing will ever issue VOICE_OFF for it again -- which is the "
              "effect that never stops", got);
        CHECK(g_idle_handoff_active_held >= 1,
              "the method was withheld but not counted");
    }
    /* THE HALF THAT MUST NOT BREAK: a genuinely inactive voice must still be
     * handed over, or DirectSound never recycles a voice again. */
    stl_le_phys(address_space_memory,
                VOICE_BASE + 15 * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE, 0);
    {
        unsigned long active_before = g_idle_handoff_active;
        uint32_t got = mcpx_apu_idle_handoff_method(apu, SE2FE_IDLE_VOICE);
        CHECK(g_idle_handoff_active == active_before,
              "an INACTIVE voice was counted as active at the hand-over: the "
              "guard fires on everything");
        CHECK(got == SE2FE_IDLE_VOICE,
              "an inactive voice's idle method was withheld (%04X). The guard "
              "is over-broad and has cut off the retirement path, which is "
              "worse than the strand it was written for", got);
    }
    /* ...and a method that is not the idle trap must pass untouched. */
    CHECK(mcpx_apu_idle_handoff_method(apu, 0x1234u) == 0x1234u,
          "an unrelated method was rewritten at the hand-over");

    free(apu);
    if (failures) {
        fprintf(stderr, "apu_list_cycle_test[%s]: %d failure(s)\n",
                arm, failures);
        return 1;
    }
    printf("apu_list_cycle_test: ok (arm=%s deep_unlink=%d move_to_front=%d)\n",
           arm, mcpx_apu_list_deep_unlink(), mcpx_apu_list_move_to_front());
    return 0;
#else
    (void)argc; (void)argv;
    printf("apu_list_cycle_test: skipped (needs macOS/aarch64)\n");
    return 0;
#endif
}
