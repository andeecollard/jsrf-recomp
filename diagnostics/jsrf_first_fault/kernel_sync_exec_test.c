/*
 * kernel_sync_exec_test.c - KeSynchronizeExecution (ordinal 153) bridge
 *
 * Ordinal 153 was in stdcall_args_for_ordinal but not in bridge_for_ordinal,
 * so every call fell to the generic path in kernel_thunk_dispatch: warn once
 * per slot, set g_eax = 0, pop the arguments. The stack stayed balanced --
 * the arg size was known -- but the guest's SynchronizeRoutine never ran and
 * the guest was handed a fabricated FALSE as if it had. JSRF makes 102-229 of
 * these a session, both call sites in DSOUND, on the two interrupt objects it
 * connects for audio, and one of them loops retesting a flag.
 *
 * The routine is a GUEST function pointer, so the bridge cannot hand it to
 * xbox_KeSynchronizeExecution (kernel_sync.c:553), which casts its PVOID to a
 * host function pointer. It has to call through the dispatch table and manage
 * the simulated stack itself, and that stack discipline is the thing most
 * likely to be wrong in a way nothing notices until a caller comes back with
 * its registers rotated -- which is what this checks.
 *
 * Driven through the real kernel_thunk_dispatch, against a real memory layout
 * and a real thunk table, rather than by calling the bridge directly: the
 * dispatch is where the dummy return address is popped and the stdcall
 * arguments are cleaned, so a test that skipped it would not be testing the
 * half that has gone wrong before.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);

/* The kernel library reaches for these; nothing here plays a video, and the
 * manual dispatch table is empty because this test supplies its own guest
 * routine through recomp_lookup below. */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count);
void xbox_kernel_bridge_init(void);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

#define KERNEL_VA_BASE 0xFE000000u

/* Slots in the thunk table this test builds. */
#define SLOT_SYNC_EXEC 0

#define SYNC_ROUTINE_VA    0x00123456u   /* in this test's dispatch */
#define MISSING_ROUTINE_VA 0x00654321u   /* deliberately not */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static volatile uint32_t *gmem(uint32_t va)
{
    return (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

/* Push three stdcall arguments and the dummy return address exactly the way
 * translated code does, then enter the kernel dispatch for a thunk slot.
 * Works from whatever g_esp currently is, so the re-entrancy case below can
 * use it from inside a running guest routine. */
static uint32_t call_slot3(int slot, uint32_t a0, uint32_t a1, uint32_t a2)
{
    recomp_func_t fn;

    *gmem(g_esp -  4) = a2;
    *gmem(g_esp -  8) = a1;
    *gmem(g_esp - 12) = a0;
    *gmem(g_esp - 16) = 0;        /* the dummy return address dispatch pops */
    g_esp -= 16;

    fn = recomp_lookup_kernel(KERNEL_VA_BASE + (uint32_t)slot * 4u);
    if (!fn) {
        fprintf(stderr, "FAIL: no kernel dispatch for slot %d\n", slot);
        failures++;
        g_esp += 16;
        return 0;
    }
    fn();
    return g_eax;
}

/* The guest's SynchronizeRoutine: BOOLEAN (__stdcall *)(PVOID Context). */
static int      g_routine_calls;
static uint32_t g_routine_ctx;
static uint32_t g_routine_ret_slot;
static int      g_reenter_next;
static int      g_reenter_ok;
static int      g_reenter_ran;

static void sync_routine(void)
{
    g_routine_calls++;
    g_routine_ret_slot = *gmem(g_esp);       /* return address at [esp] */
    g_routine_ctx      = *gmem(g_esp + 4);   /* its one argument at [esp+4] */

    /* Re-enter the bridge from inside the synchronised region. On hardware
     * this is a routine at raised IRQL calling KeSynchronizeExecution again;
     * here it is the path that must NOT spin for an interlock this thread is
     * already holding. */
    if (g_reenter_next) {
        uint32_t esp_before = g_esp;
        uint32_t inner;

        g_reenter_next = 0;
        g_reenter_ran = 1;
        inner = call_slot3(SLOT_SYNC_EXEC, 0xAA000000u, SYNC_ROUTINE_VA,
                           0x22222222u);
        g_reenter_ok = (inner == 0xDEADBE01u) && (g_esp == esp_before);
        g_routine_ctx = 0x11111111u;   /* the outer call's context, restored */
    }

    /* A recompiled function is entitled to the volatile registers and owes the
     * others back. Clobber all six so that what the caller sees afterwards is
     * the bridge's own save/restore and not this routine's good manners. */
    g_ebx = 0xBBBBBBBBu;
    g_esi = 0x51515151u;
    g_edi = 0xD1D1D1D1u;
    g_ecx = 0xCCCCCCCCu;
    g_edx = 0xDDDDDDDDu;

    /* BOOLEAN lives in al and the high bytes are the callee's business. A
     * value with rubbish above al is the interesting one: the export is a call
     * and a return, so whatever is in eax must reach the caller unaltered. */
    g_eax = 0xDEADBE01u;
    g_esp += 8;                              /* ret 4 */
}

recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return (xbox_va == SYNC_ROUTINE_VA) ? sync_routine : NULL;
}

/* The smallest XBE the memory layout accepts: no sections, so nothing is
 * copied into guest memory and everything below comes from the heap. */
static uint8_t g_xbe[0x400];

static void build_xbe(void)
{
    memset(g_xbe, 0, sizeof g_xbe);
    memcpy(g_xbe, "XBEH", 4);
    *(uint32_t *)(g_xbe + 0x0104) = 0x00010000u;   /* base address    */
    *(uint32_t *)(g_xbe + 0x0108) = sizeof g_xbe;  /* header size     */
    *(uint32_t *)(g_xbe + 0x011C) = 0;             /* section count   */
    *(uint32_t *)(g_xbe + 0x0120) = 0x00010000u;   /* section headers */
}

int main(void)
{
    uint32_t thunk_page, thunk_base, stack, stack_top, result;

    build_xbe();
    if (!xbox_MemoryLayoutInit(g_xbe, sizeof g_xbe)) {
        fprintf(stderr, "FAIL: memory layout would not initialise\n");
        return 1;
    }

    /* A one-entry kernel import table in guest memory, carrying the unresolved
     * ordinal marker the XBE loader leaves behind. Sitting 0x80 into a zeroed
     * page keeps the bridge's look-behind scan away from anything that could
     * be mistaken for a further import run. */
    thunk_page = xbox_HeapAlloc(0x1000, 4096);
    check(thunk_page != 0, "thunk page allocated");
    thunk_base = thunk_page + 0x80;
    *gmem(thunk_base) = 0x80000000u | 153u;        /* KeSynchronizeExecution */
    xbox_kernel_set_thunk_address(thunk_base, 1);
    xbox_kernel_bridge_init();

    check(*gmem(thunk_base) == KERNEL_VA_BASE + SLOT_SYNC_EXEC * 4u,
          "the thunk entry was replaced with its synthetic VA");

    stack = xbox_HeapAlloc(0x1000, 4096);
    check(stack != 0, "guest stack allocated");
    stack_top = stack + 0x800;

    /* The ordinary call. The routine runs, sees its context, and its return
     * value reaches the caller; the simulated stack comes back balanced and
     * the callee-saved registers come back intact. */
    g_esp = stack_top;
    g_ebx = 0x0B0B0B0Bu;
    g_esi = 0x05050505u;
    g_edi = 0x0D0D0D0Du;
    result = call_slot3(SLOT_SYNC_EXEC, 0xF0000000u, SYNC_ROUTINE_VA,
                        0x11111111u);
    check(g_routine_calls == 1, "the guest SynchronizeRoutine was called");
    check(g_routine_ctx == 0x11111111u,
          "it received SynchronizeContext as its argument");
    check(g_routine_ret_slot == 0,
          "a dummy return address was pushed below the argument");
    check(result == 0xDEADBE01u,
          "the routine's eax reaches the caller unaltered");
    check(g_esp == stack_top,
          "the simulated stack is balanced across the whole call");
    check(g_ebx == 0x0B0B0B0Bu && g_esi == 0x05050505u && g_edi == 0x0D0D0D0Du,
          "callee-saved registers survive the guest callback");

    /* A routine the dispatch table does not know. Nothing can be called, so
     * FALSE is the honest answer -- but the stack must still balance, because
     * a bridge that returns early is the classic way to leave a caller's own
     * pops running low. */
    g_esp = stack_top;
    result = call_slot3(SLOT_SYNC_EXEC, 0xF0000000u, MISSING_ROUTINE_VA,
                        0x33333333u);
    check(result == 0, "an unknown routine VA returns FALSE");
    check(g_routine_calls == 1, "and calls nothing");
    check(g_esp == stack_top, "and still balances the stack");

    /* A null routine. xbox_KeSynchronizeExecution answers FALSE for this
     * rather than faulting; match it. */
    g_esp = stack_top;
    result = call_slot3(SLOT_SYNC_EXEC, 0xF0000000u, 0, 0x44444444u);
    check(result == 0, "a null routine returns FALSE");
    check(g_routine_calls == 1, "and calls nothing");
    check(g_esp == stack_top, "and still balances the stack");

    /* Re-entry from inside the synchronised region. The nested call takes the
     * already-excluded path: it must run the routine, return its value, and
     * balance its own stack without deadlocking on the interrupt-delivery
     * interlock the outer call is holding. */
    g_esp = stack_top;
    g_reenter_next = 1;
    result = call_slot3(SLOT_SYNC_EXEC, 0xF0000000u, SYNC_ROUTINE_VA,
                        0x11111111u);
    check(g_reenter_ran, "the re-entrant case actually ran");
    check(g_reenter_ok, "a nested KeSynchronizeExecution completes and balances");
    check(g_routine_calls == 3, "both the outer and the nested routine ran");
    check(result == 0xDEADBE01u, "the outer call still returns its own result");
    check(g_esp == stack_top, "and the outer stack is balanced");

    if (failures) {
        fprintf(stderr, "%d KeSynchronizeExecution check(s) failed\n", failures);
        return 1;
    }
    printf("KeSynchronizeExecution bridge: all checks passed\n");
    return 0;
}
