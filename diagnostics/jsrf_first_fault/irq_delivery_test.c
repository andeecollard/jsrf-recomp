/* Interrupt delivery: recursion protection, guest IRQL, and per-vector state
 * are three separate things.
 *
 * WHY THIS EXISTS. They used to be one process-wide interlock, so a guest ISR
 * or DPC on ANY thread suppressed delivery of EVERY vector on every other
 * thread. Measured cost: bridging ordinal 153 ran previously-dead DSOUND code
 * inside that interlock, the guest's USB ISR is delivered only from behind the
 * same interlock, and the controller died -- transfer rate fell from ~74000 to
 * ~14000 per run and the title stopped reaching New Game.
 *
 * Every case below calls xbox_IrqTestDecide, which IS the function delivery
 * uses. Nothing here restates the rule: a test that reimplements what it
 * checks passes against a broken implementation.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The bridge dispatches guest code through these; nothing here calls a guest,
 * so a null lookup is the honest stub -- the same pattern the other kernel
 * tests in this directory use. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Two device vectors with a device IRQL, as KeInitializeInterrupt would give
 * them. 5 and 6 are the two JSRF connects for audio; 1 is USB. */
#define V_USB   1u
#define V_APU   5u
#define V_ACI   6u
#define DEV_IRQL 5u

static void fresh(void)
{
    xbox_IrqTestReset();
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    xbox_IrqTestSetVectorIrql(V_USB, DEV_IRQL);
    xbox_IrqTestSetVectorIrql(V_APU, DEV_IRQL);
    xbox_IrqTestSetVectorIrql(V_ACI, DEV_IRQL);
}

int main(void)
{
    /* Baseline: at PASSIVE_LEVEL with nothing in service, a device interrupt
     * is deliverable. Without this the rest could pass by being uniformly
     * negative. */
    fresh();
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);

    /* 1. An interrupt arriving while THIS thread runs an ISR is refused, and
     *    refused for re-entry -- a genuine stack hazard. */
    fresh();
    xbox_IrqTestEnterIsr(1);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_DEFER_REENTRY);
    xbox_IrqTestEnterIsr(0);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);

    /* 2. Same for a DPC body. */
    fresh();
    xbox_IrqTestEnterDpc(1);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_DEFER_REENTRY);
    xbox_IrqTestEnterDpc(0);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);

    /* 3. The same vector must not be delivered while it is in service. This is
     *    process-wide state, so it holds even for a thread that is not itself
     *    dispatching -- which is what makes a guest ISR safely non-reentrant. */
    fresh();
    xbox_IrqTestSetInService(V_APU, 1);
    CHECK(xbox_IrqTestDecide(V_APU) == XBOX_IRQ_DEFER_VECTOR);

    /* 4. THE REGRESSION TEST. A DIFFERENT eligible vector must not be blocked
     *    by that. Under the old global interlock this was refused, which is
     *    precisely how an audio interrupt starved the USB driver. */
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);
    CHECK(xbox_IrqTestDecide(V_ACI) == XBOX_IRQ_ELIGIBLE);
    xbox_IrqTestSetInService(V_APU, 0);

    /* 5. A masked interrupt stays pending and fires once IRQL drops.
     *    Raising to the device's own IRQL masks it; anything below does not. */
    fresh();
    xbox_KfRaiseIrql((KIRQL)DEV_IRQL);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_DEFER_IRQL);
    xbox_IrqTestSetPending(V_USB, 1);          /* what delivery records */
    CHECK(xbox_IrqTestPending(V_USB) == 1);
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);
    CHECK(xbox_IrqTestPending(V_USB) == 1);    /* still owed until delivered */

    /* DISPATCH_LEVEL must NOT mask a device interrupt: a device IRQL is above
     * dispatch precisely so it can preempt DPC-level work. Getting this
     * backwards would silently reproduce the old behaviour. */
    fresh();
    xbox_KfRaiseIrql((KIRQL)DISPATCH_LEVEL);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);
    xbox_KfLowerIrql(PASSIVE_LEVEL);

    /* 6. No duplicate or lost acknowledgement: the reasons are checked in a
     *    fixed order, so a vector that is BOTH masked and in service reports
     *    the mask -- one reason, deterministically, never two counts for one
     *    event. And clearing state returns it to eligible exactly once. */
    fresh();
    xbox_IrqTestSetInService(V_APU, 1);
    xbox_KfRaiseIrql((KIRQL)DEV_IRQL);
    CHECK(xbox_IrqTestDecide(V_APU) == XBOX_IRQ_DEFER_IRQL);
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    CHECK(xbox_IrqTestDecide(V_APU) == XBOX_IRQ_DEFER_VECTOR);
    xbox_IrqTestSetInService(V_APU, 0);
    CHECK(xbox_IrqTestDecide(V_APU) == XBOX_IRQ_ELIGIBLE);
    CHECK(xbox_IrqTestDecide(V_APU) == XBOX_IRQ_ELIGIBLE);   /* idempotent */

    /* A vector the guest never gave an IRQL for must still be deliverable --
     * defaulting it to 0 would make it permanently masked. */
    xbox_IrqTestReset();
    xbox_KfLowerIrql(PASSIVE_LEVEL);
    CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);

    /* 7. REFUSE, THEN RETRY -- at the DELIVERY level, not the decision level.
     *
     * The first version of this test only called the decision function and set
     * pending state by hand. It therefore could not have caught the bug that
     * was live in the vblank pump when it was written: a refused delivery left
     * the source pending, and the pump's own "already pending" guard then
     * skipped the retry for ever. A test that never retries cannot see that.
     *
     * Delivery needs a resolvable guest routine to enter, and this harness
     * stubs recomp_lookup to NULL, so `entered` is 0 here for a reason that is
     * not refusal. What this case CAN prove without a guest is the half that
     * broke: that a refusal is not sticky -- the vector is left free and the
     * eligibility answer returns to ELIGIBLE for the next attempt, which is
     * what makes a retry possible at all. */
    fresh();
    {
        /* A real KINTERRUPT in real guest memory, so delivery walks the same
         * path production does instead of dereferencing a null. Its routine is
         * left 0: no guest code exists in a unit test, so `entered` is 0 for
         * that reason as well as for refusal. What this case proves is the
         * half that actually broke -- that a refusal leaves nothing claimed
         * and the next attempt is allowed. */
        uint32_t iv = 0x00020000u;
        LONG before;
        uint8_t xbe[0x400] = {0};
        memcpy(xbe, "XBEH", 4);
        *(uint32_t *)(xbe + 0x104) = 0x10000;
        *(uint32_t *)(xbe + 0x108) = sizeof xbe;
        *(uint32_t *)(xbe + 0x120) = 0x10000;
        CHECK(xbox_MemoryLayoutInit(xbe, sizeof xbe));
        memset((uint8_t *)xbox_GetMemoryBase() + iv, 0, 44);
        *(uint32_t *)((uint8_t *)xbox_GetMemoryBase() + iv + 8) = V_USB;
        before = xbox_IrqTestDeliveredCount();

        xbox_IrqTestEnterIsr(1);               /* force a refusal */
        CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_DEFER_REENTRY);
        CHECK(xbox_IrqTestDeliver(iv) == 0);   /* refused: nothing entered */
        CHECK(xbox_IrqTestDeliveredCount() == before);   /* and nothing counted */

        xbox_IrqTestEnterIsr(0);               /* the condition clears */
        CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);   /* retry is possible */
        xbox_IrqTestSetInService(V_USB, 0);
        CHECK(xbox_IrqTestDecide(V_USB) == XBOX_IRQ_ELIGIBLE);   /* not left claimed */
    }

    printf("irq delivery: OK (reentry, irql, per-vector, a second vector not "
           "globally blocked, and a refusal that does not stick)\n");
    return 0;
}
