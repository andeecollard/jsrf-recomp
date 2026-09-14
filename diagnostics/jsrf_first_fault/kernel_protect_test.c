/*
 * kernel_protect_test.c - NtProtectVirtualMemory (ordinal 204) bookkeeping
 *
 * 4,089 calls in a JSRF session, the hottest unbridged export by forty times,
 * all of them from XAPI's VirtualProtect -- and unbridged every one fell to
 * the generic path in kernel_thunk_dispatch, which sets g_eax = 0. For an
 * NTSTATUS export that is STATUS_SUCCESS. The arguments were popped correctly
 * (the arg size was known), so the damage was not to the stack: it was that
 * OldProtect, the fourth argument and an OUT parameter, was never written.
 * The guest's wrapper returned whatever was in that stack slot as "previous
 * protection", and the entire idiom this API exists for is to save that value
 * and put it back afterwards.
 *
 * What the bridge does now is a ledger, not an mprotect -- see its comment in
 * kernel_bridge.c, and the rule in CLAUDE.md about guarded MMIO pages that
 * says why it must stay one. So what is checkable, and what is checked here,
 * is that the guest gets a CONSISTENT story: the protection it sets is the
 * protection it is told about next time, through either of the two exports
 * that report one, with NT's page rounding and NT's argument validation.
 *
 * Driven through the real kernel_thunk_dispatch for the same reason the
 * KeSynchronizeExecution test is: the dispatch owns the dummy return address
 * and the stdcall cleanup, and a test that called the bridge directly would
 * skip the half that has gone wrong before.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xbox_memory_layout.h"

typedef void (*recomp_func_t)(void);

recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count);
void xbox_kernel_bridge_init(void);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

extern RECOMP_TLS uint32_t g_eax, g_esp;

#define KERNEL_VA_BASE 0xFE000000u

#define SLOT_PROTECT 0   /* ordinal 204, NtProtectVirtualMemory  */
#define SLOT_QUERY   1   /* ordinal 179, MmQueryAddressProtect   */
#define SLOT_MMSET   2   /* ordinal 182, MmSetAddressProtect     */

#define P_NOACCESS   0x01u
#define P_READONLY   0x02u
#define P_READWRITE  0x04u
#define P_WRITECOPY  0x08u
#define P_EXEC_READ  0x20u
#define P_NOCACHE    0x200u

#define STATUS_SUCCESS                  0x00000000u
#define STATUS_INVALID_PARAMETER        0xC000000Du
#define STATUS_INVALID_PAGE_PROTECTION  0xC0000045u

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

static uint32_t g_stack_top;

/* Push `n` stdcall arguments right-to-left plus the dummy return address, the
 * way translated code does, and enter the kernel dispatch for a thunk slot. */
static uint32_t call_slot(int slot, int n, const uint32_t *args)
{
    recomp_func_t fn;
    int i;

    g_esp = g_stack_top;
    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        *gmem(g_esp) = args[i];
    }
    g_esp -= 4;
    *gmem(g_esp) = 0;             /* the dummy return address dispatch pops */

    fn = recomp_lookup_kernel(KERNEL_VA_BASE + (uint32_t)slot * 4u);
    if (!fn) {
        fprintf(stderr, "FAIL: no kernel dispatch for slot %d\n", slot);
        failures++;
        g_esp = g_stack_top;
        return 0;
    }
    fn();
    check(g_esp == g_stack_top, "stdcall arguments were cleaned from the stack");
    return g_eax;
}

/* The four cells NtProtectVirtualMemory reads and writes, in guest memory. */
static uint32_t g_cells;
#define CELL_BASE (g_cells + 0)
#define CELL_SIZE (g_cells + 4)
#define CELL_OLD  (g_cells + 8)

static uint32_t protect_call(uint32_t base_ptr, uint32_t size_ptr,
                             uint32_t new_protect, uint32_t old_ptr)
{
    uint32_t args[4];
    args[0] = base_ptr;
    args[1] = size_ptr;
    args[2] = new_protect;
    args[3] = old_ptr;
    return call_slot(SLOT_PROTECT, 4, args);
}

/* The ordinary shape: fill the IN/OUT cells, poison OldProtect so that "never
 * written" is distinguishable from "written with the right answer", and go. */
static uint32_t protect(uint32_t base, uint32_t size, uint32_t new_protect)
{
    *gmem(CELL_BASE) = base;
    *gmem(CELL_SIZE) = size;
    *gmem(CELL_OLD)  = 0xDEADBEEFu;
    return protect_call(CELL_BASE, CELL_SIZE, new_protect, CELL_OLD);
}

static uint32_t query(uint32_t address)
{
    uint32_t args[1];
    args[0] = address;
    return call_slot(SLOT_QUERY, 1, args);
}

static uint32_t mm_set(uint32_t address, uint32_t size, uint32_t protection)
{
    uint32_t args[3];
    args[0] = address;
    args[1] = size;
    args[2] = protection;
    return call_slot(SLOT_MMSET, 3, args);
}

static uint8_t g_xbe[0x400];

static void build_xbe(void)
{
    memset(g_xbe, 0, sizeof g_xbe);
    memcpy(g_xbe, "XBEH", 4);
    *(uint32_t *)(g_xbe + 0x0104) = 0x00010000u;
    *(uint32_t *)(g_xbe + 0x0108) = sizeof g_xbe;
    *(uint32_t *)(g_xbe + 0x011C) = 0;
    *(uint32_t *)(g_xbe + 0x0120) = 0x00010000u;
}

int main(void)
{
    uint32_t thunk_page, thunk_base, stack, region, status;

    build_xbe();
    if (!xbox_MemoryLayoutInit(g_xbe, sizeof g_xbe)) {
        fprintf(stderr, "FAIL: memory layout would not initialise\n");
        return 1;
    }

    thunk_page = xbox_HeapAlloc(0x1000, 4096);
    check(thunk_page != 0, "thunk page allocated");
    thunk_base = thunk_page + 0x80;
    *gmem(thunk_base + 0) = 0x80000000u | 204u;
    *gmem(thunk_base + 4) = 0x80000000u | 179u;
    *gmem(thunk_base + 8) = 0x80000000u | 182u;
    xbox_kernel_set_thunk_address(thunk_base, 3);
    xbox_kernel_bridge_init();

    stack = xbox_HeapAlloc(0x1000, 4096);
    check(stack != 0, "guest stack allocated");
    g_stack_top = stack + 0x800;

    g_cells = xbox_HeapAlloc(0x1000, 4096);
    check(g_cells != 0, "argument cells allocated");

    /* Four pages to protect. Page-aligned so the rounding below is about the
     * offsets this test adds and nothing else. */
    region = xbox_HeapAlloc(0x4000, 4096);
    check(region != 0 && (region & 0xFFFu) == 0, "page-aligned test region");
    check(region < 0x04000000u,
          "the region is inside the window the contiguous mirror aliases");

    /* A first protection change, from an address part-way into a page and for
     * a size well under one. Both IN/OUT parameters come back page-rounded the
     * way NT rounds them, OldProtect is written, and what it says is
     * PAGE_READWRITE -- the answer for a page nobody has said anything about,
     * which is also what the host query shim has always fabricated. */
    status = protect(region + 0x40, 0x10, P_READONLY);
    check(status == STATUS_SUCCESS, "a well-formed request succeeds");
    check(*gmem(CELL_BASE) == region, "BaseAddress is rounded down to its page");
    check(*gmem(CELL_SIZE) == 0x1000u, "RegionSize is rounded up to cover it");
    check(*gmem(CELL_OLD) != 0xDEADBEEFu, "OldProtect is actually written");
    check(*gmem(CELL_OLD) == P_READWRITE,
          "an untouched page reports PAGE_READWRITE as its previous protection");

    check(query(region) == P_READONLY,
          "MmQueryAddressProtect reports what 204 just set");
    check(query(region + 0x100) == P_READONLY,
          "and reports it for the whole page, not just the first byte");
    check(query(region + 0x1000) == P_READWRITE,
          "a page outside the request is untouched");

    /* Changing it back reports the real previous value, which is the whole
     * point of the parameter. A multi-page region reports the FIRST page. */
    status = protect(region, 0x2000, P_READWRITE);
    check(status == STATUS_SUCCESS, "a second change succeeds");
    check(*gmem(CELL_OLD) == P_READONLY,
          "OldProtect is the previous protection, not a constant");
    check(*gmem(CELL_SIZE) == 0x2000u, "a two-page request rounds to two pages");
    check(query(region) == P_READWRITE && query(region + 0x1000) == P_READWRITE,
          "both pages took the new protection");

    /* Modifier bits ride along with the access value and are preserved. */
    status = protect(region, 0x10, P_READWRITE | P_NOCACHE);
    check(status == STATUS_SUCCESS, "an access value with a modifier succeeds");
    check(query(region) == (P_READWRITE | P_NOCACHE),
          "the modifier bits are kept");

    /* NT validates NewProtect, and a caller that passes rubbish wants to be
     * told rather than to get a success it cannot act on. Two access bits at
     * once, no access bits at all, and an unknown modifier are all rejected --
     * and rejection must change nothing. */
    status = protect(region, 0x10, P_READONLY | P_READWRITE);
    check(status == STATUS_INVALID_PAGE_PROTECTION,
          "two access bits at once is rejected");
    check(query(region) == (P_READWRITE | P_NOCACHE),
          "a rejected request leaves the protection alone");

    check(protect(region, 0x10, 0) == STATUS_INVALID_PAGE_PROTECTION,
          "no access bits at all is rejected");
    check(protect(region, 0x10, P_READWRITE | 0x8000u)
              == STATUS_INVALID_PAGE_PROTECTION,
          "an unknown modifier bit is rejected");

    /* The two IN/OUT pointers are not optional: the call cannot be described
     * without them, and answering STATUS_SUCCESS to a caller that passed
     * nothing is the class of lie this whole bridge exists to stop. */
    check(protect_call(0, CELL_SIZE, P_READWRITE, CELL_OLD)
              == STATUS_INVALID_PARAMETER,
          "a null BaseAddress is rejected");
    check(protect_call(CELL_BASE, 0, P_READWRITE, CELL_OLD)
              == STATUS_INVALID_PARAMETER,
          "a null RegionSize is rejected");
    check(protect_call(0x7F000000u, CELL_SIZE, P_READWRITE, CELL_OLD)
              == STATUS_INVALID_PARAMETER,
          "an unmapped BaseAddress is rejected");
    *gmem(CELL_BASE) = region;
    *gmem(CELL_SIZE) = 0;
    check(protect_call(CELL_BASE, CELL_SIZE, P_READWRITE, CELL_OLD)
              == STATUS_INVALID_PARAMETER,
          "a zero-length region is rejected");

    /* OldProtect IS optional -- NT accepts a null one -- and the change must
     * still happen. */
    *gmem(CELL_BASE) = region + 0x2000;
    *gmem(CELL_SIZE) = 0x1000;
    check(protect_call(CELL_BASE, CELL_SIZE, P_NOACCESS, 0) == STATUS_SUCCESS,
          "a null OldProtect is accepted");
    check(query(region + 0x2000) == P_NOACCESS,
          "and the protection was still recorded");

    /* The contiguous mirror is the same physical page and must not carry a
     * second, independent protection: 0x80000000 | (address & 0x03FFFFFF) is
     * the address a contiguous allocation is handed back as. */
    status = protect(region | 0x80000000u, 0x1000, P_EXEC_READ);
    check(status == STATUS_SUCCESS, "a request through the physical mirror succeeds");
    check(query(region) == P_EXEC_READ,
          "the mirror and the low address share one protection");

    /* MmSetAddressProtect writes into the same ledger, so a title that sets a
     * protection one way and reads it back the other is not told two different
     * things. PAGE_WRITECOPY is used because the host shim maps everything it
     * does not recognise to read/write, so the forwarded call cannot make the
     * test's own memory unreadable. */
    check(mm_set(region, 0x1000, P_WRITECOPY) == STATUS_SUCCESS,
          "MmSetAddressProtect returns success");
    check(query(region) == P_WRITECOPY,
          "and its value is what 179 reports afterwards");
    status = protect(region, 0x10, P_READWRITE);
    check(status == STATUS_SUCCESS, "204 accepts the page 182 last set");
    check(*gmem(CELL_OLD) == P_WRITECOPY,
          "and reports 182's value as the previous protection");

    if (failures) {
        fprintf(stderr, "%d NtProtectVirtualMemory check(s) failed\n", failures);
        return 1;
    }
    printf("NtProtectVirtualMemory bookkeeping: all checks passed\n");
    return 0;
}
