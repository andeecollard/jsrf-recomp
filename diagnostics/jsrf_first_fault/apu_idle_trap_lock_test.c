/* THE PUBLISH-BEFORE-ACTIVATE WINDOW IN VOICE_ON, AND THE GUARD FOR IT.
 *
 * Forty crash dumps under build-macos/jsrf-first-fault/render-investigation
 * end the same way: HOST PC sub_001A2E2E +0x670, fault address guest
 * 0xFFFFFFBE, and a guest stack carrying 001A25D9 / 001A24D1 / 001A2450 /
 * 001A2031 -- the return addresses of DirectSound's front-end-trap ISR chain.
 * The six guest registers in those dumps are reproduced by exactly one path
 * through the generated C for that function, the one where its `this` is NULL:
 * the ISR is handed an idle-voice handle, looks up its own owner table for
 * that handle (+0x2C4 + h*4, no NULL check), and dereferences what it finds.
 * In every dump that carries the ring, the last handle we raised is voice 0.
 *
 * Our VOICE_ON publishes a voice into a list before it finishes setting it up:
 *
 *     d->regs[top_reg] = selected_handle;      <- reachable by the walk now
 *     ... ~80 lines of descriptor setup ...
 *     voice_set_mask(..., ACTIVE_VOICE, 1);    <- only now is it active
 *
 * and that body runs on a guest thread while the voice-list walk runs on the
 * frame thread, with no mutual exclusion between them. A walk landing inside
 * the gap sees an inactive voice at the head of a list and reports it idle --
 * a handle DirectSound has published to the hardware but has not yet given an
 * owner object. That is the shape of the crash.
 *
 * The one thing the model already has for this is the per-voice lock:
 * NV1BA0_PIO_VOICE_LOCK, which VOICE_ON and VOICE_RELEASE take across their
 * own bodies, and which the walk has never read.
 *
 * This test drives mcpx_apu_vp_frame directly -- no guest, no device, no mixer
 * thread -- and pins both arms:
 *
 *   guard OFF (default)  a locked, inactive head IS reported, and the ring
 *                        records it as locked, so one run of the title can say
 *                        whether the fatal raise was one of these
 *   guard ON             it is NOT reported, AND an unlocked inactive voice
 *                        behind it still is
 *
 * The second half of the ON arm is the positive control: without it, a guard
 * that had simply stopped the instrument working would pass just as well.
 *
 * Run as two ctest cases over one binary because the switch caches its getenv
 * in a static, so a process can only be in one arm.
 *
 * THE FIRST TWO ARMS RUN WITH RECOMP_APU_IDLE_TRAP_EDGE=0, set in
 * CMakeLists.txt. They require a raise from each of three back-to-back frames
 * for the same voice, and the edge latch -- on by default since 19 Sep 2026 --
 * reports a transition once and withholds the repeats. Holding it off keeps
 * those two arms about the lock guard and nothing else.
 *
 * THE THIRD ARM, `shipping`, IS THE COMBINATION A PLAYER ACTUALLY RUNS: lock
 * guard on AND edge on, both at the values the tree now ships, and it exists
 * because for one commit the edge was a default that no test drove. It was
 * nearly not written, on the belief that the latch waits on a clock a test
 * cannot advance. THAT WAS WRONG, and the wrongness is worth recording: the
 * re-raise counts SUBFRAMES, not milliseconds. RECOMP_APU_IDLE_TRAP_REARM_MS
 * is converted to a subframe count ONCE, at
 * mcpx_apu_idle_trap_rearm_subframes, and thereafter the latch only ever
 * compares g_idle_trap_edge_hold[v] -- incremented once per encounter -- with
 * that count. There is no clock anywhere on the path, so a test advances it
 * by calling mcpx_apu_vp_frame, which is the only thing this file does
 * anyway. jsrf_apu_idle_trap_edge had been doing exactly that since 16 Sep.
 *
 * So the third arm changes NOTHING to be testable: no switch is loosened, no
 * seam is added, and RECOMP_APU_IDLE_TRAP_REARM_MS keeps its shipping default
 * rather than being shortened to suit the loop. The arm simply pumps
 * subframes until the latch re-raises, and asserts it was withheld before it
 * did. See step 3.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "xbox_memory_layout.h"
#include "apu.h"
#include "apu_state.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

extern unsigned long g_idle_trap_raises;
extern unsigned long g_idle_trap_locked_raises;
extern unsigned long g_idle_trap_locked_encounters;
extern unsigned long g_idle_trap_lock_suppressed;
extern unsigned long g_idle_trap_never_on_raises;
extern unsigned long g_idle_trap_ring;
extern uint16_t g_idle_trap_last[];
extern uint8_t  g_idle_trap_why[];
extern uint16_t g_idle_trap_from[];
extern unsigned long g_idle_trap_edge_suppressed;
extern unsigned long g_idle_trap_edge_reraise;
extern int mcpx_apu_idle_trap_lock_guard(void);
extern int mcpx_apu_idle_trap_edge(void);
extern void mcpx_apu_idle_trap_report(int crash);

#define WHY_LOCKED   (1u << 0)
#define WHY_NEVER_ON (1u << 1)

static MCPXAPUState *apu;
static uint32_t read_apu(uint32_t off, unsigned width)
{ return (uint32_t)mcpx_apu_mmio_read(apu, off, width); }
static void write_apu(uint32_t off, uint32_t value, unsigned width)
{ mcpx_apu_mmio_write(apu, off, value, width); }

#define VOICE_BASE 0x20000u
#define HEAD  5u    /* the voice the guest is in the middle of introducing */
#define BEHIND 6u   /* the one behind it, never locked: the positive control */

/* Slot of the most recent raise. The ring index is the count, so the last one
 * written is at (ring - 1) & 15. */
static unsigned last_slot(void) { return (unsigned)((g_idle_trap_ring - 1) & 15u); }

int main(int argc, char **argv)
{
#if !defined(_WIN32) && defined(__aarch64__)
    /* `shipping` implies the guard: it is the guard-on arm with the edge left
     * at its default instead of pinned off. */
    int shipping = (argc > 1 && strcmp(argv[1], "shipping") == 0);
    int guard = shipping || (argc > 1 && strcmp(argv[1], "guard") == 0);
    /* THE DEFAULT ITSELF, FIRST, IN A CHILD, AND BEFORE ANYTHING READS THE
     * SWITCH. The switch caches its getenv in a function-static, and fork()
     * copies that cache -- so a child forked after the parent has already
     * asked gets the parent's answer and the check silently passes or fails
     * for the wrong reason. It has to happen before the first call in this
     * process, which is why it is the first thing in main.
     *
     * The guard defaults OFF. It was ON for part of 16 Sep 2026 and was
     * reverted the same day: with it on, four of four runs faulted at t=24.03
     * where the fault it was meant to prevent had been probabilistic and ten
     * seconds later. The window it closes is real -- 13,294 of 23,933 raises
     * happen while the guest holds that voice's lock -- but suppressing the
     * raise is not a shown-safe way to close it. This line pins the default so
     * the next attempt has to move it deliberately. */
    {
        int st = 0;
        pid_t pid = fork();
        if (pid == 0) {
            unsetenv("RECOMP_APU_IDLE_TRAP_LOCK_GUARD");
            _exit(mcpx_apu_idle_trap_lock_guard() ? 3 : 0);
        }
        CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* PIN THE ARM EXPLICITLY, NOT THE DEFAULT.
     *
     * This used to set the variable only for the guard arm and let the other
     * arm inherit whatever the default happened to be -- so when the default
     * flipped to ON on 16 Sep 2026 the "default" arm asserted guard==0 against
     * a guard that was now on, and failed. The test was right to fail; it was
     * pinning the wrong thing. An arm that means "the window is open" has to
     * SAY so, or it is measuring a default rather than a behaviour.
     *
     * The default is still pinned, separately and deliberately, below. */
    setenv("RECOMP_APU_IDLE_TRAP_LOCK_GUARD", guard ? "1" : "0", 1);
    /* The edge is pinned OFF for the first two arms by CMakeLists.txt. The
     * shipping arm deliberately does NOT set it: the whole point is to run
     * the tree's default, so setting it here -- even to the value the default
     * already has -- would mean the arm still passed if the default were
     * quietly taken back off. It is ASSERTED instead, once, below. Everything
     * in this file after that line runs in the player's configuration. */
    if (shipping) {
        unsetenv("RECOMP_APU_IDLE_TRAP_EDGE");
        CHECK(mcpx_apu_idle_trap_edge() == 1);
    }

    uint8_t xbe[0x400] = {0};
    apu = calloc(1, sizeof(*apu));
    CHECK(apu);
    qemu_mutex_init(&apu->lock);
    qemu_cond_init(&apu->cond);
    xbox_SetApuMmioReadHook(read_apu);
    xbox_SetApuMmioWriteHook(write_apu);
    memcpy(xbe, "XBEH", 4);
    *(uint32_t *)(xbe + 0x104) = 0x10000;
    *(uint32_t *)(xbe + 0x108) = sizeof(xbe);
    *(uint32_t *)(xbe + 0x120) = 0x10000;
    CHECK(xbox_MemoryLayoutInit(xbe, sizeof(xbe)));
    g_apu_ram_ptr = xbox_GetMemoryBase();
    volatile uint32_t *r = (volatile uint32_t *)
        ((uintptr_t)xbox_GetMemoryOffset() + 0xFE800000u);

    CHECK(mcpx_apu_idle_trap_lock_guard() == guard);

    /* A two-entry 2D list: HEAD -> BEHIND -> end. Both voices' descriptors are
     * zero, so both read as inactive and as CFG_FMT 0 -- which is what an
     * unconfigured voice looks like, and what the crashing handle looked like. */
    apu->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    apu->regs[NV_PAPU_TVL2D] = HEAD;
    apu->regs[NV_PAPU_TVL3D] = 0xFFFF;
    apu->regs[NV_PAPU_TVLMP] = 0xFFFF;
    stl_le_phys(address_space_memory,
                VOICE_BASE + HEAD * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,
                BEHIND);
    stl_le_phys(address_space_memory,
                VOICE_BASE + BEHIND * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,
                0xFFFF);
    r[NV_PAPU_FETFORCE1 / 4] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;

    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};

    /* --- 1. Unlocked. Both arms must report it: this is the instrument's own
     * positive control, and it is also the behaviour nothing here may change,
     * because a genuinely retired voice is how DirectSound learns to recycle
     * one. The handle has never been VOICE_ON, so the ring must say so -- that
     * flag is the other half of what a crash dump has to be able to tell us. */
    unsigned long raises0 = g_idle_trap_raises;
    mcpx_apu_vp_frame(apu, mixbins);
    CHECK(g_idle_trap_raises == raises0 + 1);
    CHECK(r[NV_PAPU_FEDECMETH / 4] == SE2FE_IDLE_VOICE);
    CHECK(r[NV_PAPU_FEDECPARAM / 4] == HEAD);
    CHECK(g_idle_trap_last[last_slot()] == HEAD);
    CHECK(g_idle_trap_why[last_slot()] & WHY_NEVER_ON);
    CHECK(!(g_idle_trap_why[last_slot()] & WHY_LOCKED));
    CHECK(g_idle_trap_from[last_slot()] == 0xFFFF);  /* straight off TVL */
    CHECK(g_idle_trap_never_on_raises >= 1);
    CHECK(g_idle_trap_locked_raises == 0);
    CHECK(g_idle_trap_locked_encounters == 0);
    CHECK(g_idle_trap_lock_suppressed == 0);

    /* Resume the front end, the way the guest's ISR does, so the next frame is
     * not merely coalesced against the outstanding trap. */
    r[NV_PAPU_FECTL / 4] = 0;
    r[NV_PAPU_ISTS / 4] = NV_PAPU_ISTS_FETINTSTS;

    /* --- 2. The window itself. VOICE_LOCK on the head is the state the guest
     * is in between publishing the voice and activating it. */
    r[NV_PAPU_FECV / 4] = HEAD;
    mcpx_apu_mmio_write(apu, 0x20000 + NV1BA0_PIO_VOICE_LOCK, 1, 4);

    unsigned long raises1 = g_idle_trap_raises;
    unsigned long locked1 = g_idle_trap_locked_raises;
    unsigned long enc1 = g_idle_trap_locked_encounters;
    unsigned long suppressed1 = g_idle_trap_lock_suppressed;
    mcpx_apu_vp_frame(apu, mixbins);

    /* ENCOUNTERS are counted in both arms -- that is what lets a single run
     * with the guard off say whether the guard would have mattered to the
     * raise that killed it. RAISES are not, and must not be: the whole point
     * of the guard is that it does not raise. Asserting both here is what
     * keeps the two from being conflated again, which is exactly how the
     * guard's justification came to quote a ratio of unlike things. */
    CHECK(g_idle_trap_locked_encounters == enc1 + 1);

    if (!guard) {
        CHECK(g_idle_trap_locked_raises == locked1 + 1);
        /* Default behaviour, unchanged: the locked voice is still reported,
         * and the ring now says it was locked when we reported it. */
        CHECK(g_idle_trap_raises == raises1 + 1);
        CHECK(g_idle_trap_lock_suppressed == suppressed1);
        CHECK(r[NV_PAPU_FEDECPARAM / 4] == HEAD);
        CHECK(g_idle_trap_last[last_slot()] == HEAD);
        CHECK(g_idle_trap_why[last_slot()] & WHY_LOCKED);
        CHECK((r[NV_PAPU_FECTL / 4] & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    } else {
        /* The guard withholds the raise for the locked voice -- and the walk
         * carries on, so BEHIND is reported instead. Both halves matter: the
         * first is the fix, the second is the proof that the fix did not just
         * switch the idle-voice machinery off. */
        CHECK(g_idle_trap_locked_raises == locked1);   /* withheld */
        CHECK(g_idle_trap_lock_suppressed == suppressed1 + 1);
        CHECK(g_idle_trap_raises == raises1 + 1);
        CHECK(r[NV_PAPU_FEDECPARAM / 4] == BEHIND);
        CHECK(g_idle_trap_last[last_slot()] == BEHIND);
        CHECK(g_idle_trap_from[last_slot()] == HEAD);
        CHECK(!(g_idle_trap_why[last_slot()] & WHY_LOCKED));
        CHECK((r[NV_PAPU_FECTL / 4] & NV_PAPU_FECTL_FEMETHMODE)
              == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    }

    /* --- 3. Releasing the lock restores the raise in the guard arm. Nothing
     * is lost by holding it: the voice traps on the next frame instead, which
     * is 1/1500 s of audio later. Without this the guard could be hiding a
     * retirement permanently and the test above would not notice. */
    r[NV_PAPU_FECTL / 4] = 0;
    r[NV_PAPU_ISTS / 4] = NV_PAPU_ISTS_FETINTSTS;
    mcpx_apu_mmio_write(apu, 0x20000 + NV1BA0_PIO_VOICE_LOCK, 0, 4);

    unsigned long raises2 = g_idle_trap_raises;
    if (!shipping) {
        mcpx_apu_vp_frame(apu, mixbins);
        CHECK(g_idle_trap_raises == raises2 + 1);
        CHECK(r[NV_PAPU_FEDECPARAM / 4] == HEAD);
        CHECK(g_idle_trap_last[last_slot()] == HEAD);
        CHECK(!(g_idle_trap_why[last_slot()] & WHY_LOCKED));
    } else {
        /* THE SHIPPING PAIR, AND THE ONE QUESTION ONLY IT CAN ANSWER: when
         * the lock guard and the edge latch are both on, does a voice that
         * was withheld while locked still get told once the lock is gone?
         *
         * Each arm alone says nothing about this. With the edge off, the
         * frame above answers immediately and the latch is never exercised.
         * With the guard off, HEAD is raised at step 2 and never reaches the
         * guard's suppression path at all. Only together does a voice go
         * guard-suppressed -> unlocked -> latch-suppressed -> re-raised, and
         * that is the sequence the player's music rides on: a voice the guest
         * stopped, not reclaimed on the first telling, and told again.
         *
         * NOTE WHICH SUPPRESSION RAN AT STEP 2. The guard's `suppress = 1`
         * is set BEFORE the edge block, and the edge block is gated on
         * !suppress, so a guard-suppressed voice does not advance its own
         * re-raise counter. HEAD therefore arrives here with hold[] at zero
         * and owes the full wait -- which is the conservative direction, and
         * is why this loop cannot pass by accident on a leftover count.
         *
         * NO VALUE IS SHORTENED TO FIT. RECOMP_APU_IDLE_TRAP_REARM_MS keeps
         * its shipping default and the loop simply runs as long as the latch
         * asks. The bound is a runaway guard, not the expected count: at the
         * default 16 ms the wait is 24 subframes, and 256 is far enough above
         * that to survive a change to the default while still failing in a
         * second rather than hanging ctest.
         *
         * The assertions are on the SHAPE, not on the number, for the same
         * reason: withheld at least once (the edge is doing its job, so this
         * is not just the level trap in disguise), then raised, on HEAD, with
         * the re-raise counter moving -- that last one because
         * [APU-IDLE-EDGE] reraise is the line the handover tells the next
         * person to read first if the music dies, and a counter nobody has
         * forced is not evidence. */
        unsigned long sup0 = g_idle_trap_edge_suppressed;
        unsigned long rer0 = g_idle_trap_edge_reraise;
        unsigned n = 0, raised = 0;
        while (n < 256 && !raised) {
            r[NV_PAPU_FECTL / 4] = 0;
            r[NV_PAPU_ISTS / 4] = NV_PAPU_ISTS_FETINTSTS;
            r[NV_PAPU_FETFORCE1 / 4] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
            mcpx_apu_vp_frame(apu, mixbins);
            ++n;
            raised = (g_idle_trap_raises != raises2);
        }
        CHECK(raised);                                   /* it is told again */
        CHECK(n > 1);                        /* and it was withheld first -- */
        CHECK(g_idle_trap_edge_suppressed > sup0);    /* by the edge, not by
                                                       * something else */
        CHECK(g_idle_trap_edge_reraise == rer0 + 1);
        CHECK(g_idle_trap_raises == raises2 + 1);
        CHECK(r[NV_PAPU_FEDECPARAM / 4] == HEAD);
        CHECK(g_idle_trap_last[last_slot()] == HEAD);
        CHECK(!(g_idle_trap_why[last_slot()] & WHY_LOCKED));
    }

    /* The report has to survive being called -- the crash handler calls it on
     * the faulting thread and a format bug there costs a whole run. */
    mcpx_apu_idle_trap_report(1);
    mcpx_apu_idle_trap_report(0);
#else
    (void)argc; (void)argv;
#endif
    puts("APU idle-voice trap: lock provenance recorded,"
         " lock guard withholds only the locked raise, walk continues");
    return 0;
}
