/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "irq_latency.h"
#include "kernel.h"
#include "../recomp_switch.h"
#include "xbox_memory_layout.h"
#include "recomp_icall_feedback.h"
#include <stdio.h>
#include <string.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>
#include <stddef.h>
#if !defined(_WIN32)
#include <time.h>
#include <errno.h>
#include <sched.h>
#define recomp_yield() ((void)sched_yield())
#else
/* mingw provides no <sched.h> and no sched_yield, so the two spin loops below
 * called an undeclared function and the Windows cross-build stopped here.
 * SwitchToThread is the Windows equivalent -- give up the rest of this
 * thread's slice to another ready thread -- and it is already what the rest of
 * this tree spins on: xbox_memory_layout.c's aperture waits and apu_core.c's
 * APU_LOCK_YIELD both use it. The Win32 declaration arrives with <windows.h>,
 * which kernel.h resolves to on this target. */
#define recomp_yield() ((void)SwitchToThread())
#endif

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS uint32_t g_fs_base;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject (ordinal 357) - IDE channel object. Opaque; only ever
     * passed back to Io* routines we stub, so a recognisable non-null is enough. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL) = XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding.
 *
 * SIXTY-FOUR BITS, AND THE WIDTH IS THE WHOLE POINT. This was `int`, and it
 * overflowed: the counter is bumped once per kernel dispatch, and the two
 * things that read it both invert when it goes negative.
 *
 *   - KERNEL_LOG_ON() is `count <= budget` (budget 200), so a negative count
 *     turns per-call tracing PERMANENTLY ON;
 *   - the 2-second summary is gated on `count > 200`, so the `[KERNEL]
 *     summary` line and the ordinal histogram beneath it go PERMANENTLY
 *     SILENT -- and the histogram is the single most useful diagnostic this
 *     runtime prints.
 *
 * Nothing announces either flip. Measured, in
 * build-macos/jsrf-first-fault/render-investigation/a1-exec-walkers-tutorial-2
 * /stderr.log: the last summary reads 2,141,517,829 total calls, the next
 * trace line (that file's line 45355) is `[KERNEL] #-2147483647`, and
 * 10,632,107 negative-ordinal trace lines follow it to the end of the run --
 * a 1.1 GB log of the trace that was supposed to stop at 200, with no summary
 * in any of it. Current builds dispatch ~6.28M calls/s, which reaches INT_MAX
 * at t~342 s; a play session is longer than that, so this is not a corner.
 *
 * WHAT IT MAKES ATOMIC: NOTHING, deliberately. The increment stays a plain
 * read-modify-write from every guest thread, exactly like g_ordinal_calls
 * below and for the same stated reason -- an atomic RMW on the hottest path
 * in the runtime costs more than the counter is worth, and the worst a racing
 * worker can do is lose a count. What the width buys is that no interleaving
 * can make the value NEGATIVE, because there is no sign bit to reach: 2^64 at
 * 6.28M/s is ~93,000 years. (A 32-bit host could tear the 64-bit store across
 * its halves; both supported hosts are 64-bit, where the aligned load/store is
 * single-copy atomic.) */
static unsigned long long g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

/* kernel_log_budget() is clamped >= 0 above, so the usual-arithmetic
 * conversion of the long to unsigned long long here cannot produce a huge
 * positive bound out of a negative budget. */
#define KERNEL_LOG_ON()      (g_kernel_call_count <= (unsigned long long)kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= (unsigned long long)(kernel_log_budget() / 2))

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* Is a guest VA backed by a mapped page, for a `bytes`-wide access?
 *
 * A bridge turns a guest VA into a host pointer by adding an offset and hands
 * it to the kernel implementation, so a wild VA does not fail the call -- it
 * faults inside the implementation, with a host stack that has no recompiled
 * frame in it and a fault address that means nothing on its own. That is
 * exactly how the first fault after the title screen advanced presented: a
 * SIGSEGV at host 0x3FE000110, inside xbox_ExQueryNonVolatileSetting's
 * `if (Type) *Type = 4`, naming neither the ordinal nor the guest caller. */
static int bridge_va_mapped(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    uint64_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /* XBOX_TIB_MAIN, not XBOX_FS_BASE. Both named 0x1000 until upstream made
     * the TIB per-thread, at which point XBOX_FS_BASE became g_fs_base -- and
     * this is a LOWER BOUND on the mapped guest range, not a question about
     * the calling thread. On a thread that allocated its own TIB, g_fs_base is
     * that allocation, so the test rejected every address below it: the whole
     * image, the heap, and every buffer a guest thread passed to NtReadFile,
     * each of which came back STATUS_ACCESS_VIOLATION from bridge_buf_ok.
     * The title stalled in WaitEndTitle with its reads failing. */
    if (va < XBOX_TIB_MAIN)     /* page zero is deliberately unmapped */
        return 0;
    if (end <= mapped)
        return 1;
    return va >= XBOX_CONTIG_BASE
        && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
}

/* Convert an optional out-pointer argument, rejecting unmapped VAs.
 *
 * Returns 0 (the "caller passed NULL" case every kernel implementation here
 * already handles) for a VA that cannot be dereferenced, and reports it once
 * per export/argument pair with the guest return address, which is the one
 * fact that identifies the call site. g_esp has had the dummy return address
 * popped by kernel_thunk_dispatch, so it sits just below.
 */
/* Per-thread, for the same reason g_kernel_dispatch_slot is: guest threads
 * dispatch concurrently, and a process-global would report another thread's
 * ordinal beside this thread's stack. */
static RECOMP_TLS ULONG g_bridge_current_ordinal;
static RECOMP_TLS int   g_bridge_current_slot = -1;
static RECOMP_TLS uint32_t g_bridge_current_target;
extern uint32_t g_thunk_table_base;
extern uint32_t g_thunk_table_count;

static uint32_t bridge_checked_out_va(uint32_t va, uint32_t bytes,
                                      const char *export_name,
                                      const char *arg_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!va || bridge_va_mapped(va, bytes))
        return va;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == arg_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = arg_name;

    fprintf(stderr,
            "  [KERNEL] %s: %s = 0x%08X is not mapped guest memory; "
            "passing NULL. ordinal=%lu slot=%d target=0x%08X "
            "guest caller=0x%08X esp=0x%08X\n",
            export_name, arg_name, va,
            (unsigned long)g_bridge_current_ordinal, g_bridge_current_slot,
            g_bridge_current_target,
            g_esp ? (uint32_t)BRIDGE_MEM32(g_esp - 4) : 0, g_esp);
    /* The whole frame, because "which argument is wrong" is much less useful
     * than "is this frame an argument list for this export at all". A stack
     * that is shifted by one dword, or that belongs to a different export
     * entirely, is visible here and nowhere else. */
    fprintf(stderr, "  [KERNEL]   guest stack:");
    for (i = -1; i < 8; ++i)
        fprintf(stderr, " %s%08X", i < 0 ? "ret=" : "",
                (uint32_t)BRIDGE_MEM32(g_esp + i * 4));
    fprintf(stderr, "\n");

    /* The thunk entries around the one that produced this dispatch, read back
     * now through the same view the guest reads them through. A slot whose
     * entry no longer holds its own synthetic VA is a corrupted import table,
     * and that is a different defect from a guest passing a bad pointer. */
    if (g_bridge_current_slot >= 0) {
        uint32_t entry = g_thunk_table_base + (uint32_t)g_bridge_current_slot * 4;
        fprintf(stderr, "  [KERNEL]   thunk table base=0x%08X entry=0x%08X:",
                g_thunk_table_base, entry);
        for (i = -2; i <= 2; ++i)
            fprintf(stderr, " [%+d]=%08X", i,
                    (uint32_t)BRIDGE_MEM32(entry + i * 4));
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    return 0;
}

/* A GUEST BUFFER THE HOST IS ABOUT TO READ OR WRITE THROUGH.
 *
 * bridge_checked_out_va covers the small fixed-size out-parameter: a handle
 * slot, an IO_STATUS_BLOCK, a LARGE_INTEGER. This covers the other shape, and
 * it is the dangerous one -- a base and a LENGTH that both come from the
 * guest, handed to a host call that will read or write that many bytes.
 *
 * NtReadFile is the worst of them: the host WRITES `length` bytes through the
 * pointer, so a length the guest got wrong, or a buffer near the top of the
 * mapping, walks the host past the end of guest memory writing file contents.
 * Nothing above this layer can catch it -- xbox_NtReadFile receives a host
 * pointer and a count and has no way to know where the mapping ends.
 *
 * STATUS_ACCESS_VIOLATION is what NT answers for a user buffer it cannot
 * touch, and it is a great deal more useful to a title than a host crash: the
 * call fails, the guest sees a status it has a branch for, and the log names
 * the export, the buffer and the length.
 */
static int bridge_buf_ok(uint32_t va, uint32_t bytes, const char *export_name)
{
    static const char *seen[16];
    static int distinct;
    int i;

    if (!bytes)                                   /* nothing is accessed */
        return 1;
    if (va && bridge_va_mapped(va, bytes))
        return 1;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == export_name)
            return 0;
    if (distinct < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[distinct++] = export_name;

    fprintf(stderr,
            "  [KERNEL] %s: buffer 0x%08X length %u is not mapped guest "
            "memory; returning STATUS_ACCESS_VIOLATION. guest caller=0x%08X\n",
            export_name, va, (unsigned)bytes,
            g_esp ? (uint32_t)BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);
    return 0;
}


/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top;
    uint32_t tib_va, tls_context_va, tls_data_va, tls_data_size;
};

static void bridge_write_handle(uint32_t handle_va, HANDLE h);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
}

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2;

    /* Own register set (RECOMP_TLS), own simulated stack. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    xbox_SetupCurrentThreadTib(
        s->tib_va, s->tls_context_va, s->tls_data_va, s->tls_data_size,
        s->stack_top, s->stack_top + 16 - XBOX_THREAD_STACK_SIZE);
    g_thread_stack_top = s->stack_top;
    free(s);

    /* EXPERIMENT, opt-in: do not run the spin-wait workers at all.
     *
     * JSRF creates workers through XapiThreadStartup whose bodies at
     * 0x0013B180 and 0x0013B1C0 are counting spin loops on flags in .data that
     * nothing observed ever sets -- measured over a whole run, the flag reads
     * zero throughout while the spin counter passes 1.1 billion, and one such
     * thread holds a core at 100%.
     *
     * Whether that COSTS us anything is a separate question from whether it is
     * wrong, and it is the cheaper one to answer. Two renderer optimisations
     * today each removed real work from the pusher thread and moved the frame
     * rate by nothing, so the assumption that a busy core must be the limiter
     * has to be tested before anything is built on it. Returning here leaves
     * the thread object and its handle intact -- the guest's NtResumeThread
     * still succeeds -- and simply lets the thread exit instead of spinning.
     *
     * RECOMP_SKIP_SPIN_THREADS=1. Diagnostic only: if the title ever does set
     * those flags, this silently removes whatever the workers would have gone
     * on to do, so it is not a fix and must never become the default. */
    {
        static int skip = -1;
        if (skip < 0) skip = getenv("RECOMP_SKIP_SPIN_THREADS") != NULL;
        if (skip && (ctx1 == 0x0013B180u || ctx1 == 0x0013B1C0u)) {
            fprintf(stderr, "  [KERNEL] SKIPPING spin worker ctx=0x%08X "
                            "(RECOMP_SKIP_SPIN_THREADS)\n", ctx1);
            fflush(stderr);
            xbox_FreeThreadStack(g_thread_stack_top);
            g_thread_stack_top = 0;
            return 0;
        }
    }

    bridge_run_thread_inline(fn, ctx1, ctx2);

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    /* The routine returned instead of calling PsTerminateSystemThread; the
     * stack is still ours to give back. */
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    return 0;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t tib_va, uint32_t tls_context_va,
                                  uint32_t tls_data_va, uint32_t tls_data_size)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->tib_va = tib_va;
    s->tls_context_va = tls_context_va;
    s->tls_data_va = tls_data_va;
    s->tls_data_size = tls_data_size;

    /* Always created suspended. The caller writes the guest's handle slot and
     * only then releases the thread, which closes an ordering hole: the worker
     * used to be running before bridge_write_handle had filled the slot, so a
     * thread that called NtResumeThread on itself early read a handle of 0.
     * It is also what makes honouring CreateSuspended a one-line decision at
     * the call site rather than a second code path. */
    th = CreateThread(NULL, 0, bridge_thread_main, s, CREATE_SUSPENDED, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else xbox_set_game_thread(th);
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t kernel_stack_sz = STACK_ARG(2);  /* KernelStackSize, ignored */
    uint32_t tls_data_size   = STACK_ARG(3);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    /* PsCreateSystemThreadEx(ThreadHandle, ThreadExtraSize, KernelStackSize,
     * TlsDataSize, ThreadId, StartContext1, StartContext2, CreateSuspended,
     * DebugStack, StartRoutine) -- see docs/formats/kernel-exports.md. Arg 7
     * was being ignored, so a title that created a thread suspended and
     * resumed it later got one that had already run. */
    uint32_t create_suspended = STACK_ARG(7);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X"
            " ctx1=0x%08X ctx2=0x%08X tls=%u stack_requested=%u given=%u\n",
            g_thread_call_count, start_routine, start_context1, start_context2,
            tls_data_size, kernel_stack_sz, (unsigned)XBOX_THREAD_STACK_SIZE);
    fflush(stderr);

    /* Placeholder so a caller that only null-checks its handle sees success.
     * The spawn path below replaces it with a real tagged token; the inline
     * paths leave it, and it is untagged, so bridge_resolve_handle returns
     * NULL for it and any later operation fails rather than dereferencing it. */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                xbox_SetupCurrentThreadTib(
                    XBOX_PRIMARY_TIB_VA, XBOX_PRIMARY_TLS_CONTEXT_VA,
                    XBOX_PRIMARY_TLS_DATA_VA, tls_data_size,
                    XBOX_STACK_TOP, XBOX_STACK_BASE);
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                /* RECOMP_WORKERS=inline runs a title's worker routines on the
                 * calling thread instead of spawning one.
                 *
                 * Not a mode to ship a title in -- a worker that blocks
                 * waiting for requests never returns, and the caller never
                 * gets control back. It is a bisecting tool: when something
                 * only goes wrong with two guest threads running, this says so
                 * in one run, and separates a concurrency bug from everything
                 * else it might have been. Restored from upstream v0.8.0,
                 * which merge 36b4076 dropped along with the lock tracing that
                 * answers the same question from the other end. */
                const char *inline_workers = getenv("RECOMP_WORKERS");
                uint32_t stack_top;

                if (inline_workers && !strcmp(inline_workers, "inline")) {
                    fprintf(stderr, "  [KERNEL] RECOMP_WORKERS=inline: running "
                            "worker 0x%08X on this thread\n", start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                    return;
                }

                stack_top = xbox_AllocThreadStack(kernel_stack_sz);

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    uint32_t tib_va = xbox_HeapAlloc(0x30, 16);
                    uint32_t tls_context_va = xbox_HeapAlloc(0x2C, 16);
                    uint32_t tls_data_va = xbox_HeapAlloc(
                        tls_data_size ? tls_data_size : 4, 16);
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top,
                                                    tib_va, tls_context_va,
                                                    tls_data_va,
                                                    tls_data_size);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)%s\n",
                            start_routine, start_context1, stack_top,
                            create_suspended ? " suspended" : "");
                    fflush(stderr);
                    /* Handle first, then release: see bridge_spawn_thread. */
                    if (xbox_handle_ptr && th) {
                        bridge_write_handle(xbox_handle_ptr, th);
                    }
                    if (th && !create_suspended) {
                        ResumeThread(th);
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    /* Resolve table-backed handles through the kernel HLE so special device
     * handles get their own close semantics. Ordinary handles still reach the
     * platform CloseHandle implementation through xbox_NtClose. */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            g_eax = (uint32_t)xbox_NtClose(h);
        else
            g_eax = (uint32_t)STATUS_INVALID_HANDLE;
        return;
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* The allocator selects backing compatible with the host's GPU path;
     * its Windows arena supports the driver's high-VA/physical round trip. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContiguousAlloc(size, align);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
/* How much of what a title reserves does it ever commit?
 *
 * A pure MEM_RESERVE costs no RAM on hardware; ours charges the arena for all
 * of it. JSRF's guest heap reserves 1+2+4+8 MB through ra=0x0014903F and holds
 * 15,728,640 bytes of a 58 MB arena that way, which is what the stage loader
 * runs out of. Separating reserve from commit is the fix, but only if the title
 * commits substantially less than it reserves -- if it commits nearly all of
 * it, the pages are owed either way and the work buys nothing.
 *
 * So: record each pure reservation, and attribute every later MEM_COMMIT that
 * lands inside one. Read-only bookkeeping; nothing here changes what is
 * returned to the guest. */
#define BRIDGE_RESERVE_MAX 32
static struct { uint32_t base, size, committed, commits; } g_reserves[BRIDGE_RESERVE_MAX];
static unsigned g_reserve_count, g_reserve_overflow;

static void bridge_reserve_note(uint32_t base, uint32_t size)
{
    if (g_reserve_count >= BRIDGE_RESERVE_MAX) { ++g_reserve_overflow; return; }
    g_reserves[g_reserve_count].base = base;
    g_reserves[g_reserve_count].size = size;
    g_reserves[g_reserve_count].committed = 0;
    g_reserves[g_reserve_count].commits = 0;
    ++g_reserve_count;
    fprintf(stderr, "  [RESERVE] #%u reserve base=0x%08X size=%u\n",
            g_reserve_count, base, size);
    fflush(stderr);
}

static void bridge_reserve_commit(uint32_t base, uint32_t size)
{
    static unsigned seen;
    for (unsigned i = 0; i < g_reserve_count; ++i) {
        if (base < g_reserves[i].base
                || (uint64_t)base >= (uint64_t)g_reserves[i].base + g_reserves[i].size)
            continue;
        g_reserves[i].committed += size;
        ++g_reserves[i].commits;
        if ((++seen % 64) == 0) {
            uint64_t r = 0, c = 0;
            for (unsigned k = 0; k < g_reserve_count; ++k) {
                r += g_reserves[k].size;
                c += g_reserves[k].committed;
            }
            fprintf(stderr, "  [RESERVE] %u regions, reserved %llu, committed"
                            " %llu (%llu%%), commits=%u overflow=%u\n",
                    g_reserve_count, (unsigned long long)r,
                    (unsigned long long)c, r ? (unsigned long long)(c * 100 / r) : 0ull,
                    seen, g_reserve_overflow);
            fflush(stderr);
        }
        return;
    }
}

static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    /* ZeroBits (arg 1) constrains how many high bits of the returned address
     * must be zero. This allocator cannot honour it -- the guest heap hands
     * back whatever is next -- and it has never been read. Named and ignored
     * on purpose: a caller that passes a real constraint gets an address that
     * may violate it, which is worth knowing when one eventually does. */
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    static unsigned vm_said;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    /* `mapped` is named once rather than re-evaluated three times, and it is
     * checked: with both sizes still zero -- a bridge call before the memory
     * layout is up -- the test below reduces to `base_hint >= 0`, which is
     * always true, and the modulo in the message then divides by zero. The
     * warning is about aliasing against a mapping, so with no mapping there is
     * nothing to warn about. */
    uint32_t mapped = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;
    if (mapped && base_hint >= mapped) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)(mapped / (1024 * 1024)),
                    (uint32_t)(base_hint % mapped));
        fflush(stderr);
    }

    /* Report before the commit-only fast path. This used to sit below it, so
     * the log claimed to distinguish reserve from commit while omitting every
     * commit into an existing reservation. */
    if (vm_said++ < 64)
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory #%u size=%u"
                        " type=0x%X (%s%s) base_hint=0x%08X ra=0x%08X\n",
                vm_said, size, alloc_type,
                (alloc_type & 0x2000) ? "RESERVE" : "",
                (alloc_type & 0x1000) ? "|COMMIT" : "",
                base_hint, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        bridge_reserve_commit(base_hint, size);
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    /* Reserve and commit are charged the same here, which is the known gap
     * described above. Say which one each call was, rate-limited, so the
     * question "is this title paying RAM for address space it never commits?"
     * can be answered from a log rather than guessed at. */
    uint32_t xbox_va = 0;
    if ((alloc_type & 0x2000) && !(alloc_type & 0x1000)
            && xbox_SeparateReserveSpaceEnabled()) {
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X outside physical RAM\n",
                    size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va)
        xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Returns 0 unless the title asked for a mapping larger than
         * RAM, so nothing changes for titles that did not. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    if ((alloc_type & 0x2000) && !(alloc_type & 0x1000))
        bridge_reserve_note(xbox_va, size);

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* -- NtQueryVirtualMemory (ordinal 217, 2 args = 8 bytes) --------------
 *
 * Xbox takes two arguments, not NT's four:
 *
 *     NTSTATUS NtQueryVirtualMemory(PVOID BaseAddress,
 *                                   PMEMORY_BASIC_INFORMATION Info);
 *
 * This has to be a guest-side answer. The existing xbox_NtQueryVirtualMemory
 * in kernel_memory.c calls the host VirtualQuery and memcpy's a host
 * MEMORY_BASIC_INFORMATION into guest memory, whose pointer fields are 64-bit
 * on an x64 build -- so every field after BaseAddress lands in the wrong place.
 *
 * It also has to exist at all. Without a bridge entry the thunk is left
 * unbridged, and a title's CRT heap creation calls this to probe its heap
 * region: RtlCreateHeap does
 *
 *     call NtQueryVirtualMemory ; test eax,eax ; jl fail
 *     cmp  mbi.BaseAddress, requested ; jne fail
 *     cmp  mbi.State, MEM_FREE        ; je  fail
 *
 * and returns 0 on any of those. On Half-Life 2 that null heap propagated
 * silently through the rest of CRT init.
 *
 * Guest MEMORY_BASIC_INFORMATION, 32-bit, 28 bytes:
 *     +0x00 BaseAddress   +0x04 AllocationBase  +0x08 AllocationProtect
 *     +0x0C RegionSize    +0x10 State           +0x14 Protect
 *     +0x18 Type
 *
 * The guest is one flat committed mapping, so that is what we report: any
 * address inside it is MEM_COMMIT / PAGE_READWRITE / MEM_PRIVATE, and anything
 * outside is MEM_FREE rather than an error, which is the honest answer and the
 * one that lets a caller distinguish the two.
 */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_va = STACK_ARG(0);
    uint32_t info_va = STACK_ARG(1);
    uint32_t page_base = base_va & ~0xFFFu;
    uint32_t reserve_base = 0, reserve_size = 0;
    BOOL is_reserve = xbox_QueryReserveAddress(page_base, &reserve_base,
                                                &reserve_size);

    if (!info_va) {
        g_eax = 0xC000000Du;               /* STATUS_INVALID_PARAMETER */
        return;
    }

    BRIDGE_MEM32(info_va + 0x00) = page_base;          /* BaseAddress */
    BRIDGE_MEM32(info_va + 0x04) = page_base;          /* AllocationBase */
    BRIDGE_MEM32(info_va + 0x08) = 0x04;               /* PAGE_READWRITE */
    BRIDGE_MEM32(info_va + 0x14) = 0x04;               /* Protect */
    BRIDGE_MEM32(info_va + 0x18) = 0x20000;            /* MEM_PRIVATE */

    if (is_reserve) {
        BRIDGE_MEM32(info_va + 0x04) = reserve_base;       /* AllocationBase */
        BRIDGE_MEM32(info_va + 0x0C) = reserve_base + reserve_size - page_base;
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;             /* MEM_COMMIT */
    } else if (page_base >= g_xbox_code_lo && page_base < g_xbox_total_ram) {
        BRIDGE_MEM32(info_va + 0x0C) = (uint32_t)g_xbox_total_ram - page_base;
        BRIDGE_MEM32(info_va + 0x10) = 0x1000;         /* MEM_COMMIT */
    } else {
        BRIDGE_MEM32(info_va + 0x0C) = 0x1000;
        BRIDGE_MEM32(info_va + 0x10) = 0x10000;        /* MEM_FREE */
        BRIDGE_MEM32(info_va + 0x08) = 0;
        BRIDGE_MEM32(info_va + 0x18) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtQueryVirtualMemory: base=0x%08X -> "
                        "state=0x%X size=%u\n", base_va,
                        BRIDGE_MEM32(info_va + 0x10),
                        BRIDGE_MEM32(info_va + 0x0C));
        fflush(stderr);
    }
    g_eax = 0;                                          /* STATUS_SUCCESS */
}

/* The mirror image of bridge_NtAllocateVirtualMemory, and it has to be.
 *
 * The reserve came from xbox_HeapAlloc, so the release belongs to
 * xbox_HeapFree. Handing the pair to xbox_NtFreeVirtualMemory instead put the
 * host's VirtualFree on a guest address: a MEM_RELEASE was silently dropped
 * (the POSIX shim returns TRUE for size 0, so nothing was ever reclaimed and
 * the guest heap only grew), and a MEM_DECOMMIT called mprotect(PROT_NONE) on
 * live guest RAM, which would fault the next time the title touched memory it
 * still believed it owned.
 *
 * MEM_DECOMMIT is a no-op on this side for the same reason MEM_COMMIT is on
 * the allocate side: the heap commits everything it hands out, and the
 * reservation stays valid until it is released. */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t free_type = STACK_ARG(2);
    uint32_t base_va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (!base_ptr || !base_va) {
        g_eax = 0xC000000Du;                /* STATUS_INVALID_PARAMETER */
        return;
    }

    if (free_type & 0x8000u) {              /* MEM_RELEASE */
        if (!xbox_ReserveFree(base_va))
            xbox_HeapFree(base_va);
        BRIDGE_MEM32(base_ptr) = 0;
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtFreeVirtualMemory: base=0x%08X type=0x%X\n",
                base_va, free_type);
        fflush(stderr);
    }

    g_eax = 0;                              /* STATUS_SUCCESS */
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st;

    /* Every one of these is dereferenced by the implementation, and an
     * unhandled index memsets Value for ValueLength bytes. */
    type_va   = bridge_checked_out_va(type_va, 4, "ExQueryNonVolatileSetting", "Type");
    value_va  = bridge_checked_out_va(value_va, value_length,
                                      "ExQueryNonVolatileSetting", "Value");
    result_va = bridge_checked_out_va(result_va, 4, "ExQueryNonVolatileSetting",
                                      "ResultLength");

    st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    xbox_PeekSample("exit peek");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in ExitProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161, fastcall: irql in ecx)
 * Not STACK_ARG(0), for the same reason as bridge_ObfDereferenceObject: the
 * leading "Kf" marks these __fastcall, so the argument arrives in ecx and
 * never reaches the stack -- which is why their arg-size entries are 0.
 * Reading STACK_ARG(0) against a 0-byte frame returns whatever the caller
 * left at the top of the guest stack.
 *
 * Observed: JSRF logged "KfLowerIrql: attempt to raise IRQL from 2 to 144"
 * every ~24ms. 144 is not a valid IRQL (HIGH_LEVEL is 31); it was stack
 * residue. Each one silently drove g_current_irql to garbage. */
static void bridge_KfRaiseIrql(void)
{
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)g_ecx);
}

static void bridge_KfLowerIrql(void)
{
    xbox_KfLowerIrql((UCHAR)g_ecx);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryInterruptTime / PerformanceCounter / Frequency (125-127) ─ */
static void bridge_KeQueryInterruptTime(void)
{
    ULONGLONG value = xbox_KeQueryInterruptTime();
    g_eax = (uint32_t)value;
    g_edx = (uint32_t)(value >> 32);
}

static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
/* ── Guest DISPATCHER_OBJECT (KEVENT / KSEMAPHORE / KTIMER / KMUTANT) ─────
 *
 * The Ke* family takes a POINTER TO A DISPATCHER OBJECT IN GUEST MEMORY, not a
 * handle. xbox_KeSetEvent and xbox_KeWaitForSingleObject both did
 * `HANDLE h = (HANDLE)Object` and handed that to the Win32 compat layer -- the
 * same mistake the IoCreateDevice and bridge_resolve_handle notes describe,
 * pointed the other way: a guest address escaping into a host pointer.
 *
 * It cost the title everything. D3D's sub_0018CE50 waits on a KEVENT embedded
 * in the device itself:
 *
 *     [dev+0x2434] = 0                       ; SignalState
 *     KeWaitForSingleObject(&dev[0x2430], ...)
 *
 * dev+0x2430 is guest memory, never a w32_object, so the compat layer rejected
 * it and the wait returned INSTANTLY -- 68 million times in 50s. The vsync
 * pump above it (sub_0013B1C0: wait for vblank, tick, resume the ADX audio
 * server) therefore free-ran, which is the whole spin.
 *
 * DISPATCHER_HEADER, the layout every one of these objects starts with:
 *     +0x00 UCHAR Type        0 = NotificationEvent, 1 = SynchronizationEvent
 *     +0x01 UCHAR Absolute
 *     +0x02 UCHAR Size
 *     +0x03 UCHAR Inserted
 *     +0x04 LONG  SignalState
 *     +0x08 LIST_ENTRY WaitListHead
 *
 * +0x04 is confirmed by the guest itself: D3D clears SignalState by writing 0
 * to object+4 immediately before waiting.
 */
#define DISPATCHER_TYPE(va)         BRIDGE_MEM8((va) + 0)
#define DISPATCHER_SIGNALSTATE(va)  BRIDGE_MEM32((va) + 4)
#define DISPATCHER_SYNCHRONIZATION  1   /* auto-reset on acquisition */

static void bridge_vblank_poll(void);
static void bridge_device_irq_poll(void);
static void bridge_timers_poll(void);
static void bridge_run_dpc(uint32_t dpc_va, uint32_t sys1, uint32_t sys2);

/* Guest register file, saved across a nested call into guest code.
 *
 * bridge_run_dpc and bridge_run_isr execute a whole guest function from inside
 * a bridge. The register file is per-thread, and the thread they borrow is in
 * the middle of its own guest call -- bridge_KeWaitForSingleObject pumps both
 * of these while a guest function sits blocked in it, so the DPC/ISR would
 * return having clobbered the waiter's eax/ecx/edx/ebx/esi/edi. Measured: the
 * vblank ISR runs nested inside sub_0018CE50 on the waiting thread.
 *
 * On hardware an interrupt saves and restores the interrupted context. Do the
 * same. g_esp is deliberately NOT restored here: each caller balances the
 * stack it pushed itself. */
typedef struct BridgeGuestRegs {
    uint32_t eax, ecx, edx, ebx, esi, edi;
} BridgeGuestRegs;

static void bridge_save_regs(BridgeGuestRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx;
    r->ebx = g_ebx; r->esi = g_esi; r->edi = g_edi;
}

static void bridge_restore_regs(const BridgeGuestRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx;
    g_ebx = r->ebx; g_esi = r->esi; g_edi = r->edi;
}


/* kernel_sync.c keeps this file-static; the guest-object wait below needs the
 * same NT timeout conversion (NULL = infinite, negative = relative 100ns). */
/* Guest waits that passed an absolute deadline; see the return below. */
unsigned long g_sched_absolute_deadlines;

/* ...AND THE LINE THAT MAKES IT A MEASUREMENT.
 *
 * The comment at the increment says the cost of finding out whether JSRF ever
 * passes an absolute deadline "is a counter", and that zero means the
 * shortcut is free while anything else means a thread is burning a core on a
 * wait that never waits. The counter was written and printed nowhere, so the
 * question it was raised to settle stayed open -- and it is not an idle one
 * while every guest thread in the black-screen hang is sitting in a wait.
 *
 * Printed in both states on purpose: zero is the answer the comment wants,
 * and an absent line would be indistinguishable from a build without it. */
void bridge_sched_report(void)
{
    fprintf(stderr, "  [SCHED] absolute-deadline waits: %lu%s\n",
            g_sched_absolute_deadlines,
            g_sched_absolute_deadlines
                ? "   <- these return immediately instead of waiting; a thread"
                  " spinning on one reads as \"the title is slow\""
                : " (the relative-timeout shortcut is free)");
}

static DWORD bridge_nt_timeout_to_ms(uint32_t timeout_va)
{
    int64_t t;
    if (!timeout_va) return INFINITE;
    t = (int64_t)((uint64_t)BRIDGE_MEM32(timeout_va) |
                  ((uint64_t)BRIDGE_MEM32(timeout_va + 4) << 32));
    if (t < 0) return (DWORD)((-t) / 10000);   /* relative 100ns -> ms */
    /* An ABSOLUTE deadline, which this does not implement: it is reported as
     * already due, so the caller gets STATUS_TIMEOUT immediately and a guest
     * that retries in a loop spins.
     *
     * Whether JSRF ever passes one was never measured, and the cost of
     * finding out is a counter. Zero here means the shortcut is free and the
     * missing implementation can stay missing; anything else means a thread is
     * burning a core on a wait that never waits, which would read as "the
     * title is slow" and never point here. Counted rather than fixed, because
     * implementing absolute deadlines against the guest's own clock is a real
     * piece of work and should not be done on a guess. */
    ++g_sched_absolute_deadlines;
    return 0;
}

/* Which dispatcher objects are ever signalled, and which are only waited on.
 *
 * An object that a thread waits on indefinitely and nobody ever sets is a
 * deadlock the sampler shows as a sleeping thread and nothing else explains.
 * JSRF parks two worker threads in sub_0018CE50, which clears the D3D device's
 * event at device+0x2430 and waits on it with no timeout; neither KeSetEvent
 * site in D3D targets that address. Recording both sides is how to tell a
 * missing signal from a slow one.
 *
 * Addresses only, deduplicated, so the cost is a small linear scan on calls
 * that are already crossing the thunk boundary.
 */
static void event_trace_note(const char *side, uint32_t object)
{
    static int enabled = -1;
    static struct { uint32_t object; unsigned long count; char side; } seen[64];
    static unsigned distinct;
    unsigned i;

    if (enabled < 0) enabled = getenv("RECOMP_EVENT_TRACE") != NULL;
    if (!enabled || !object) return;

    for (i = 0; i < distinct; ++i) {
        if (seen[i].object == object && seen[i].side == side[0]) {
            ++seen[i].count;
            /* Report the first, then decade by decade, so a hot signal shows
             * as a rate and a wait that never returns still reports once. */
            if (seen[i].count != 10 && seen[i].count != 1000 &&
                seen[i].count != 100000)
                return;
            break;
        }
    }
    if (i == distinct) {
        if (distinct >= sizeof(seen) / sizeof(seen[0])) return;
        seen[distinct].object = object;
        seen[distinct].side = side[0];
        seen[distinct].count = 1;
        ++distinct;
    }
    /* Which guest code signals an event is the whole question when a wait
     * never returns: the object address alone cannot be traced back, because
     * a title computes it from a device pointer in a register and the literal
     * never appears in the image. g_esp has had the dummy return address
     * popped by kernel_thunk_dispatch, so the caller sits just below it. */
    fprintf(stderr, "[EVENT-%s] object=%08X count=%lu caller=%08X\n",
            side, object, seen[i].count,
            g_esp ? (uint32_t)BRIDGE_MEM32(g_esp - 4) : 0);
    fflush(stderr);
}

static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t previous;

    if (!event_ptr) {
        g_eax = 0;
        return;
    }
    event_trace_note("SET", event_ptr);
    previous = DISPATCHER_SIGNALSTATE(event_ptr);
    DISPATCHER_SIGNALSTATE(event_ptr) = 1;
    g_eax = previous;
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
/* Something for the host to run while a guest thread is blocked.
 *
 * The kernel bridge cannot call the window layer directly: it is linked into
 * tests that have no window and no SDL, and one diagnostic line is not worth
 * making them all fail to link. The owner installs it, exactly as the pusher's
 * software-method handler and the executor's recent-method dump are installed.
 * The hook is responsible for deciding which thread it is legal on. */
static void (*s_wait_poll_hook)(void);

void xbox_SetWaitPollHook(void (*fn)(void))
{
    s_wait_poll_hook = fn;
}

/* Defined with the scheduling bridges below; used here too. */
static int sched_trace_on(void);
/* These were file-scope, and shared by every guest thread in this function.
 *
 * Two threads waiting on different objects overwrote each other's pointers, so
 * one thread's wake was counted against whichever object the other had looked
 * up last -- and past the 16-object table the `if (i < 16)` guard left the
 * PREVIOUS thread's pointers in place entirely, so its counters collected
 * somebody else's increments. A counter that attributes one thread's events to
 * another object is worse than no counter, and this file's own rule is to read
 * a counter's trigger before trusting its value. They are locals now, so each
 * wait attributes to the object it actually waited on. RECOMP_SCHED_TRACE
 * only, but the traces were being read. */
static void sched_note(const char *what, uint32_t handle, uint32_t extra);
static int bridge_deliver_pending_apcs(void);

static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object      = STACK_ARG(0);
    uint32_t alertable   = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    DWORD ms, deadline;
    int infinite;
    /* Per-thread, deliberately: see the note where these used to be globals. */
    unsigned long *sched_woke = NULL, *sched_timeout = NULL;

    if (alertable && bridge_deliver_pending_apcs()) {
        g_eax = 0x000000C0u;   /* STATUS_USER_APC */
        return;
    }

    if (!object) {
        g_eax = 0;   /* STATUS_SUCCESS */
        return;
    }
    event_trace_note("WAIT", object);

    ms = bridge_nt_timeout_to_ms(timeout_ptr);
    infinite = (ms == INFINITE);
    deadline = GetTickCount() + ms;
    /* Outcome, not just entry. A thread that re-enters the same wait forever
     * is either being woken and finding nothing to do, or never being woken at
     * all, and those have opposite fixes. Counted per object. */
    if (sched_trace_on()) {
        static struct { uint32_t obj; unsigned long waits, ready, woke, timeout; } w[16];
        static unsigned wn;
        unsigned i;
        for (i = 0; i < wn; ++i) if (w[i].obj == object) break;
        if (i == wn && wn < 16) { w[wn].obj = object; ++wn; }
        if (i < 16) {
            ++w[i].waits;
            if (DISPATCHER_SIGNALSTATE(object)) ++w[i].ready;
            if ((w[i].waits % 2000) == 0) {
                fprintf(stderr, "  [SCHED] wait object=0x%08X type=%u entries=%lu"
                                " already-signalled=%lu woken=%lu timed-out=%lu\n",
                        object, (unsigned)DISPATCHER_TYPE(object), w[i].waits,
                        w[i].ready, w[i].woke, w[i].timeout);
                fflush(stderr);
            }
            sched_woke = &w[i].woke;
            sched_timeout = &w[i].timeout;
        }
    }

    for (;;) {
        if (DISPATCHER_SIGNALSTATE(object)) {
            /* A synchronization event is auto-reset: the waiter consumes it. */
            if (DISPATCHER_TYPE(object) == DISPATCHER_SYNCHRONIZATION) {
                DISPATCHER_SIGNALSTATE(object) = 0;
            }
            if (sched_woke) ++*sched_woke;
            g_eax = 0;   /* STATUS_SUCCESS */
            return;
        }
        if (!infinite && (int32_t)(GetTickCount() - deadline) >= 0) {
            if (sched_timeout) ++*sched_timeout;
            g_eax = 0x00000102u;   /* STATUS_TIMEOUT */
            return;
        }
        /* A blocked thread is not dispatching thunks, so the time-driven
         * sources that would signal this object have to be pumped from here
         * too -- otherwise a title whose threads all wait would never see a
         * vblank or a timer again. Both are re-entrancy guarded. */
        bridge_timers_poll();
        bridge_vblank_poll();
        bridge_device_irq_poll();
        /* And the host window's event loop, for the same reason.
         *
         * The guest owns the process's main thread -- main() calls the XBE
         * entry point and never returns -- so there is no thread left for the
         * platform to be pumped from, and on macOS SDL_PollEvent must run on
         * that very thread. The result was a window that drew correctly and
         * was marked unresponsive by the OS, complete with a spinning
         * beachball, which is indistinguishable from a hang to anyone
         * watching. A blocking wait on the main thread is the one moment the
         * guest is idle and the pump is legal. */
        if (s_wait_poll_hook)
            s_wait_poll_hook();
#if !defined(_WIN32)
        w32_thread_suspend_point();
#endif
        /* How long to wait before looking again.
         *
         * This was Sleep(1), and Sleep(1) is nanosleep(1 ms), which on macOS
         * returns late under load. That is a large granularity to impose on a
         * 16.67 ms budget, and one of the threads waiting here is JSRF's sound
         * server: CRI's renderer moves at most 1024 samples per pass (guest
         * 0x0013E965), so at 44100 Hz it needs >= 43.1 passes a second, and it
         * was MEASURED at 33.4 -- pinned there at every frame rate, with CRI's
         * own underrun counter at zero, so it is not short of data, it is short
         * of passes. A cycle that overruns one vblank period catches the next
         * one instead, which halves the rate; the poll granularity is enough on
         * its own to push it over.
         *
         * MEASURED, intro, 70 s each, same scene (live=137 vs 134):
         *
         *              server passes/s   guest writes   ring stale   frame rate
         *   1000 us         36.8            71-75%       25-29%       11.6/11.7/14.1
         *    100 us         44.9              86%        14%          11.6/11.9/14.2
         *
         * 43.1 passes/s is break-even, so 1000 us is below it and 100 us is
         * above it. The cost side came out empty: frame times are identical
         * window for window, the scene gets as far, and vblank delivery is
         * slightly BETTER (59.9 Hz against 59.6, max_gap 17 ms against 18).
         * So the default is 100 us. RECOMP_WAIT_POLL_US overrides it, and
         * RECOMP_WAIT_POLL_US=1000 restores the old behaviour exactly, which is
         * how the table above was produced and how a regression would be
         * bisected.
         *
         * REVERTED TO 1000 -- 100 us BROKE GAMEPLAY AUDIO COMPLETELY.
         *
         * The table above is real and was taken at the intro, where the APU
         * front end is never trapped. In actual gameplay with a controller it
         * put the front end into FEMETHMODE_TRAPPED for 203628 of 269896 APU
         * frames, against 0 and 70 in the two gameplay runs on the previous
         * default. se_frame was then skipped whenever the front end was trapped,
         * so voice processing stopped -- `processed` froze across four
         * consecutive reports -- and the game went SILENT. Polling ten times as
         * often lets
         * the guest push front-end methods ten times as often, and the trap
         * window scales with that traffic.
         *
         * So the intro measurement was real and the conclusion drawn from it
         * was not: it was taken in one scene and generalised, which is the same
         * error made three times already in this investigation. A scene where
         * the front end is never trapped cannot say what happens in one where
         * it is.
         *
         * The switch stays, because the underlying finding stands -- CRI's
         * sound server really is starved by this poll. Whatever replaces it
         * must not increase front-end method traffic: a real wakeup on event
         * set, not a faster poll. Any future change here must be measured at
         * GAMEPLAY with [APU-FRAME] trapped= read, not only at the intro.
         *
         * ONE HALF OF THE MECHANISM ABOVE IS NO LONGER TRUE, and it makes this
         * worth re-testing rather than re-reading. Since 15 Sep 2026
         * RECOMP_APU_SE_WHILE_TRAPPED is the default, so a trapped front end no
         * longer stops the sound engine: trapped= 203628 of 269896 would now
         * cost interrupts and front-end latency rather than silence. The
         * revert stands until someone measures it again at gameplay -- the
         * finding that killed it was silence, and the reason for the silence
         * has been removed. */
        {
            static long poll_us = -1;
            if (poll_us < 0) {
                const char *e = getenv("RECOMP_WAIT_POLL_US");
                poll_us = e ? strtol(e, NULL, 10) : 1000;
                if (poll_us < 0) poll_us = 0;
                if (poll_us > 1000000) poll_us = 1000000;
            }
            if (poll_us >= 1000) {
                Sleep((DWORD)(poll_us / 1000));
            } else if (poll_us > 0) {
#if defined(_WIN32)
                Sleep(0);
#else
                struct timespec ts = { 0, poll_us * 1000L };
                while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
#endif
            } else {
                recomp_yield();
            }
        }
    }
}

static HANDLE bridge_resolve_handle(uint32_t token);

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    if (alertable && bridge_deliver_pending_apcs()) {
        g_eax = 0x000000C0u;   /* STATUS_USER_APC */
        return;
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObject(
        handle, (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    if (alertable && bridge_deliver_pending_apcs()) {
        g_eax = 0x000000C0u;   /* STATUS_USER_APC */
        return;
    }

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    g_eax = (uint32_t)xbox_NtWaitForSingleObjectEx(
        handle, (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 5 args = 20 bytes)
 *
 * NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *                                     ULONG WaitType, BOOLEAN Alertable,
 *                                     PLARGE_INTEGER Timeout);
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t alertable   = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;

    if (alertable && bridge_deliver_pending_apcs()) {
        g_eax = 0x000000C0u;   /* STATUS_USER_APC */
        return;
    }

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    /* count is bounded above, so this reads at most 256 bytes -- but it still
     * reads them from a guest address nothing has checked. */
    if (!bridge_buf_ok(handles_va, count * 4u, "NtWaitForMultipleObjectsEx")) {
        g_eax = 0xC0000005u;             /* STATUS_ACCESS_VIOLATION */
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    g_eax = (uint32_t)xbox_NtWaitForMultipleObjectsEx(
        count, handles, wait_type, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(timeout_ptr));
}

/* ── Guest page protection bookkeeping ────────────────────
 *
 * Three exports argue about page protection -- NtProtectVirtualMemory (204),
 * MmSetAddressProtect (182) and MmQueryAddressProtect (179) -- and nothing in
 * this runtime remembered what any of them was ever told. 204 was not even
 * routed: 4,089 calls in a JSRF session, all of them XAPI's VirtualProtect,
 * each returning STATUS_SUCCESS having changed nothing and, worse, having left
 * the caller's lpflOldProtect untouched, so the title's wrapper handed back a
 * stack slot as "previous protection".
 *
 * This is the store those three now share: one 16-bit Xbox protection value
 * per 4 KB guest page. A zero entry means "nobody has said", which reads back
 * as PAGE_READWRITE -- the same answer the host VirtualQuery shim fabricates
 * for every address it is ever handed (win32_compat.c:1489), so a page this
 * runtime has not been told about gives exactly the answer it always did.
 *
 * IT IS BOOKKEEPING ONLY, AND DELIBERATELY SO. Nothing here calls host
 * mprotect. The rule is in CLAUDE.md: a guarded MMIO page loses writes if
 * anything else unprotects it. The APU aperture is trapped read-only precisely
 * so guest stores reach the model; host pages on macOS are 16 KB; and a
 * title-driven protection change over a guest range that shares one of those
 * pages with a trapped aperture would open the same window that once swallowed
 * every VOICE_ON in a 45-second run. Honouring PAGE_READONLY for real would
 * also mean routing every resulting fault back through the MMIO handler, which
 * is a much larger change than the lie being fixed.
 *
 * So the guest gets a consistent ANSWER about protection and no enforcement of
 * it: a page it marks PAGE_READONLY stays writable, a PAGE_NOACCESS page stays
 * readable, and a title that depends on a protection fault actually firing
 * will not get one. Nothing observed in JSRF does; a title that did would need
 * the enforcement question reopened with the aperture overlap measured first.
 */
#define BRIDGE_PROT_PAGE_SHIFT 12u
#define BRIDGE_PROT_PAGES      ((128u * 1024u * 1024u) >> BRIDGE_PROT_PAGE_SHIFT)
static uint16_t g_guest_page_protect[BRIDGE_PROT_PAGES];

/* Page index for a guest VA, or -1 for an address this table does not cover.
 *
 * The contiguous mirror folds onto the RAM it aliases: a contiguous allocation
 * is 0x80000000 | (address & 0x03FFFFFF) -- the DMA_GET round trip
 * heap_alloc_test checks -- and it is the same physical page, so it must not
 * carry a second, independent protection. Everything else is deliberately
 * uncovered, the NV2A and APU apertures above 0xFD000000 above all: this table
 * describes guest RAM, and a device window's protection is the host's
 * business, not the title's.
 */
static long bridge_prot_page_index(uint32_t va)
{
    uint32_t page;

    if (va >= XBOX_CONTIG_BASE && va < XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        va &= 0x03FFFFFFu;
    page = va >> BRIDGE_PROT_PAGE_SHIFT;
    return (page < BRIDGE_PROT_PAGES) ? (long)page : -1;
}

static uint32_t bridge_prot_get(uint32_t va)
{
    long page = bridge_prot_page_index(va);
    uint32_t p = (page >= 0) ? (uint32_t)g_guest_page_protect[page] : 0u;

    return p ? p : (uint32_t)PAGE_READWRITE;
}

static void bridge_prot_set(uint32_t va, uint32_t bytes, uint32_t protect)
{
    uint64_t first, last, p;

    if (!bytes || !protect)
        return;
    first = (uint64_t)va & ~(uint64_t)0xFFFu;
    last  = ((uint64_t)va + bytes + 0xFFFu) & ~(uint64_t)0xFFFu;
    /* A range wider than the table cannot describe anything the table covers
     * beyond its first 128 MB, and walking it page by page would be a long
     * loop for nothing. */
    if (last - first > (uint64_t)BRIDGE_PROT_PAGES * 4096u)
        last = first + (uint64_t)BRIDGE_PROT_PAGES * 4096u;
    for (p = first; p < last; p += 4096u) {
        long page = bridge_prot_page_index((uint32_t)p);
        if (page >= 0)
            g_guest_page_protect[page] = (uint16_t)protect;
    }
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);
    long page;

    if (!address) {
        g_eax = 0;
        return;
    }

    /* Answer from the store whenever the title has told this runtime something
     * about the page, so a protection it set through 204 or 182 reads back as
     * itself instead of as whatever the host thinks. Falling through to the
     * host query otherwise keeps the existing behaviour exactly: on macOS that
     * shim answers PAGE_READWRITE for every address, which is also the store's
     * default, so no answer that used to be given changes. */
    page = bridge_prot_page_index(address);
    if (page >= 0 && g_guest_page_protect[page]) {
        g_eax = (uint32_t)g_guest_page_protect[page];
        return;
    }

    g_eax = (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address));
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);

    if (alertable && bridge_deliver_pending_apcs()) {
        g_eax = 0x000000C0u;   /* STATUS_USER_APC */
        return;
    }

    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode, (BOOLEAN)alertable,
        XBOX_TO_NATIVE(interval_ptr));
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 */
static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    g_eax = 0;
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Calls the same xbox_* the thunk table exposes, so there is one
     * implementation rather than two that have to be kept in agreement.
     * Note this bridge itself is not covered by any test: bridge functions
     * are static and driven by guest CPU state, and nothing in tests/ can
     * reach them. */
    g_eax = (uint32_t)xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)addr);
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    /* Record it as well as forwarding it, so MmQueryAddressProtect and
     * NtProtectVirtualMemory answer with what this call set rather than with
     * the host shim's fixed PAGE_READWRITE. The forwarding is left alone: this
     * one has always reached the host's VirtualProtect and changing that is a
     * separate question from the ledger. */
    bridge_prot_set(addr, size, prot);
    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── NtProtectVirtualMemory (ordinal 204) ───────────────────
 *
 * NTSTATUS NtProtectVirtualMemory(PVOID *BaseAddress, PSIZE_T RegionSize,
 *                                 ULONG NewProtect, PULONG OldProtect)
 *
 * The hottest unbridged export in a JSRF session by a factor of forty -- 4,089
 * calls, one guest call site, XAPI's VirtualProtect. Unbridged it told two
 * lies. The first is the one the bookkeeping note above explains and does not
 * fix: no protection changes. The second is the one that can corrupt a caller,
 * and it is fixed here. OldProtect is an OUT parameter and nothing wrote it,
 * so VirtualProtect returned an uninitialised stack slot as "previous
 * protection" -- and the whole idiom this API exists for is to save that value
 * and restore it afterwards, which means restoring garbage.
 *
 * Both IN/OUT parameters are written back the way NT writes them: BaseAddress
 * rounded down to its page, RegionSize rounded up to cover the request from
 * there. OldProtect is the protection of the FIRST page of the region, which
 * is what NT reports for a range that spans several with different values.
 *
 * How far this goes: it is a ledger, not an mprotect. The host mapping is not
 * touched -- see the bookkeeping note above for the APU-aperture reason -- so
 * the value the guest sets is the value it reads back from here and from
 * MmQueryAddressProtect, and nothing else about the page changes.
 */
static void bridge_NtProtectVirtualMemory(void)
{
    uint32_t base_ptr = bridge_checked_out_va(STACK_ARG(0), 4,
                                              "NtProtectVirtualMemory",
                                              "BaseAddress");
    uint32_t size_ptr = bridge_checked_out_va(STACK_ARG(1), 4,
                                              "NtProtectVirtualMemory",
                                              "RegionSize");
    uint32_t new_prot = STACK_ARG(2);
    uint32_t old_ptr  = bridge_checked_out_va(STACK_ARG(3), 4,
                                              "NtProtectVirtualMemory",
                                              "OldProtect");
    uint32_t base, size, aligned_base, aligned_size, old_prot;
    uint64_t end;

    /* Both of these are IN as well as OUT: the call cannot be described
     * without them. NULL or unmapped is a caller error, not something to
     * paper over with success. */
    if (!base_ptr || !size_ptr) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }

    base = BRIDGE_MEM32(base_ptr);
    size = BRIDGE_MEM32(size_ptr);
    if (!size || (uint64_t)base + size > 0x100000000ull) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }

    /* Exactly one of the eight access values, optionally with the GUARD,
     * NOCACHE and WRITECOMBINE modifiers. NT rejects anything else and so does
     * this: a caller passing a bad protection wants to be told, and this is
     * the export whose whole job is to have an opinion about the value. */
    {
        uint32_t access    = new_prot & 0xFFu;
        uint32_t modifiers = new_prot & ~0xFFu;

        if (!access || (access & (access - 1u)) || (modifiers & ~0x700u)) {
            g_eax = 0xC0000045u;             /* STATUS_INVALID_PAGE_PROTECTION */
            return;
        }
    }

    aligned_base = base & ~0xFFFu;
    end          = ((uint64_t)base + size + 0xFFFu) & ~(uint64_t)0xFFFu;
    aligned_size = (uint32_t)(end - aligned_base);

    old_prot = bridge_prot_get(aligned_base);
    bridge_prot_set(aligned_base, aligned_size, new_prot);

    BRIDGE_MEM32(base_ptr) = aligned_base;
    BRIDGE_MEM32(size_ptr) = aligned_size;
    if (old_ptr)
        BRIDGE_MEM32(old_ptr) = old_prot;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtProtectVirtualMemory: 0x%08X+%u -> 0x%X"
                        " (was 0x%X); ledger only, host mapping untouched\n",
                aligned_base, aligned_size, new_prot, old_prot);
        fflush(stderr);
    }

    g_eax = 0;                               /* STATUS_SUCCESS */
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    xbox_SetDisplayFramebuffer(fb, pitch);
    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        xbox_FramebufferWindowSet(fb, pitch);
        xbox_FramebufferWindowStart();
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* The normal exit for a worker, and therefore the one that has to
         * return the stack -- ExitThread never comes back to bridge_thread_main
         * to do it. */
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
/* THE THREE INITIALISERS TAKE A GUEST POINTER AND ZERO IT BEFORE USING IT.
 *
 * Each of these memsets the caller's object before filling it in, and each
 * built the destination as XBOX_TO_NATIVE(STACK_ARG(0)) with nothing checking
 * the argument first. That macro maps a guest 0 to NULL, so a title passing a
 * null object pointer -- or a pointer into unmapped guest space -- crashed the
 * HOST inside memset, in a frame belonging to the kernel bridge rather than to
 * the guest code that supplied the pointer.
 *
 * bridge_checked_out_va reports the bad argument with the guest's own stack
 * frame and return address, which is the difference between "the host died in
 * memset" and "this export was handed 0x%08X by this caller". It passes a null
 * through unchanged, so a null check still follows it.
 *
 * All three exports return void, so refusing is doing nothing -- which is what
 * the real kernel does with an object it cannot write. */

static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = bridge_checked_out_va(STACK_ARG(0), 32,
                                            "KeInitializeDpc", "Dpc");
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    if (!dpc_va) {
        g_eax = 0;
        return;
    }

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here and nothing ever raises
 * that interrupt, so these exist to let initialisation complete rather than to
 * deliver anything.
 *
 * KeConnectInterrupt reports success: reporting failure sends Halo's
 * rasterizer down an error path during preinitialize, and the goal is to get
 * past setup, not to pretend the hardware is broken.
 *
 * ponytail: no interrupt is ever delivered. Code that *waits* on the ISR
 * rather than polling will hang here, and the fix for that is to bridge the
 * D3D8 entry point that owns the wait, not to synthesise NV2A interrupts.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {
        /* IRQL is conventionally the vector for device interrupts. */
        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
/* Defined with the interrupt-delivery machinery below, which is where the
 * per-vector IRQL table lives. */
static void bridge_note_vector_irql(uint32_t vector, uint32_t irql);

static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = bridge_checked_out_va(STACK_ARG(0), 44,
                                                  "KeInitializeInterrupt",
                                                  "Interrupt");
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    if (!interrupt_va) {
        g_eax = 0;
        return;
    }

    /* Xbox KINTERRUPT is 44 bytes. */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;

    /* Arg 4 is the vector's IRQL and it used to be dropped on the floor, which
     * is why delivery had nothing to test eligibility against and fell back on
     * a process-wide interlock. Xbox KeInitializeInterrupt is
     * (Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, Mode, Shared)
     * -- 7 args, which is what stdcall_args_for_ordinal has always said. */
    bridge_note_vector_irql(vector, STACK_ARG(4));
    g_eax = 0;
}

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt) */
/* Defined with the DPC machinery further down; the vblank ISR path below
 * shares its re-entrancy guard. */

static RECOMP_TLS int g_in_dpc;

/* An ISR is not a DPC, and conflating the two breaks the one thing an ISR is
 * for. KeInsertQueueDpc refuses to run a DPC while g_in_dpc is set (correctly:
 * a DPC must not recurse into another). Guarding the ISR with that same flag
 * made the guest's ISR queue its DPC and have it silently REFUSED -- so JSRF's
 * GPU ISR masked the NV2A interrupt (NV_PMC_INTR_EN_0 = 0) on its way out and
 * the DPC that re-enables it never ran. Measured: the first ISR returned TRUE,
 * every one after it returned FALSE at that gate.
 *
 * A deferred procedure call is deferred: queue it, let the ISR finish, then
 * run it. */
static RECOMP_TLS int g_in_isr;
static RECOMP_TLS uint32_t g_pending_dpc;
static RECOMP_TLS uint32_t g_pending_dpc_sys1;
static RECOMP_TLS uint32_t g_pending_dpc_sys2;
static volatile LONG g_isr_handoff_seq;
/* DEFINED in xbox_memory_layout.c, not here, and deliberately.
 *
 * The OHCI stall snapshot lives down there and gates on the returned count.
 * Defining these in this file made every target that links the memory layout
 * pull in this object too, and this object needs recomp_lookup from the
 * generated tree -- which broke the link of four unrelated unit tests. The
 * counters belong to the bridge conceptually; the storage belongs where it
 * costs nothing. */
extern volatile LONG g_bridge_isr_entered, g_bridge_isr_returned;
static RECOMP_TLS LONG g_current_isr_handoff;

/* Connected interrupts.
 *
 * KeInitializeInterrupt already records the guest's ISR, its context and its
 * vector into the guest KINTERRUPT; KeConnectInterrupt used to throw away
 * WHICH interrupt was being connected and just answer "yes". So the runtime
 * knew how to call a guest ISR and never called one.
 *
 * That is what left JSRF spinning. D3D's sub_0018CE50 clears the SignalState
 * of a KEVENT embedded in the device and waits on it forever:
 *
 *     [dev+0x2434] = 0
 *     KeWaitForSingleObject(&dev[0x2430], ...)   ; the vblank event
 *
 * and its vsync pump (sub_0013B1C0) is "wait for vblank; tick; resume the ADX
 * audio server; repeat". On hardware the GPU raises its interrupt once a
 * frame and D3D's own ISR signals that event. Nothing raised it here, so the
 * wait returned instantly and the whole title free-ran: 72M waits and 77M
 * thread resumes in 50s, the audio server ticking at ~73k/s instead of 60/s,
 * and the thread-priority churn that rides along with every resume.
 *
 * Delivering the interrupt lets the guest's OWN D3D code signal its own event,
 * so no address inside the title is needed here. */
#define BRIDGE_MAX_INTERRUPTS 8

/* Xbox IRQ 3 is the NV2A. HalGetInterruptVector is identity here, so the
 * vector the guest registers for the GPU is 3. */
#define BRIDGE_NV2A_VECTOR 3

/* The vblank period, in microseconds, because it is not a whole number of
 * milliseconds and pretending otherwise cost us 4%.
 *
 * This was `#define BRIDGE_VBLANK_PERIOD_MS 16` under a comment that said
 * 60 Hz. 1000/16 is 62.5, and the guest's clock ran that fast: the archived
 * measurement in CLAUDE_HANDOVER_2026-09-11_WINDOWS_ORACLE.txt reads
 * "delivered=2797 over 44910 ms = 62.3 Hz", and 44910/16 is 2807, so delivery
 * tracked the nominal constant to within 0.4%. The poll jitter is the small
 * term; the constant was the big one.
 *
 * NTSC progressive is 59.94 Hz -- 60000/1001 exactly -- which is 16683.3 us.
 * That cannot be a millisecond literal, and making the macro fractional does
 * NOT work: `next_vblank` and `now` are both DWORD, so `now + 16.683` promotes
 * to double and truncates straight back on every single call, leaving the
 * behaviour bit-identical to 16. The fraction has to be carried between
 * periods, which is what carry_us below does -- the step alternates between 16
 * and 17 ms in the ratio that averages 16683 us.
 *
 * Deliberately still reset from `now` rather than advanced from the previous
 * deadline. A late poll drops the periods it missed instead of delivering them
 * in a burst, which is the existing contract (see the comment above
 * g_vblank_deadlines) and the safer half of the tradeoff: a title that misses a
 * frame should see one late vblank, not five at once. */
#define BRIDGE_VBLANK_PERIOD_US 16683u

/* NV_PMC_INTR_0 is a READ-ONLY SUMMARY of the engine interrupts: bit 24 is
 * asserted for as long as the display engine (PCRTC) has one pending, and it
 * clears when the driver acknowledges at the SOURCE, never by writing PMC.
 *
 * JSRF's vblank handler depends on exactly that, and spins on it:
 *
 *     mov  [ebx+0x600100], ecx        ; ack at NV_PCRTC_INTR_0
 *     test [ebx+0x100], 0x1000000     ; re-read the summary
 *     jne  back                       ; spin until it clears
 *     call KeSetEvent(ctx+0x1C8)      ; only then signal the vblank event
 *
 * so raising bit 24 in plain memory without also modelling the clear leaves it
 * spinning forever and the event never signalled. Raise the SOURCE and mirror
 * it into the summary. */
#define NV_PMC_INTR_0          0x100u
#define NV_PMC_INTR_0_PCRTC    0x01000000u   /* bit 24: display engine */
#define NV_PMC_INTR_0_PGRAPH   0x00001000u   /* bit 12: graphics engine */
#define NV_PMC_INTR_EN_0       0x140u
#define NV_PGRAPH_INTR         0x400100u
#define NV_PGRAPH_INTR_ERROR   0x00100000u
#define NV_PGRAPH_TRAPPED_ADDR 0x400704u
#define NV_PGRAPH_TRAPPED_DATA 0x400708u
#define NV_PGRAPH_FIFO_ACCESS  0x400720u
#define NV_PCRTC_INTR_0        0x600100u
#define NV_PCRTC_INTR_0_VBLANK 0x00000001u

static volatile uint32_t g_interrupts[BRIDGE_MAX_INTERRUPTS];

/* Entries skipped by the validator below. Non-zero means the race was real and
 * was caught; it stays at zero on a host where the publish is never observed
 * part-built, which is what macOS has always been. */
unsigned long g_interrupt_not_ready;

/* Is this slot safe to dispatch FROM ANOTHER THREAD?
 *
 * The slot is published on the guest's own thread the moment the title calls
 * KeConnectInterrupt, and the KINTERRUPT it points at is the guest's memory.
 * The pumps below read the routine, the vector and the ServiceContext out of
 * it and dereference the context. While the only caller ran on the guest's own
 * thread inside a blocking wait, no reader could observe that structure
 * part-built. RECOMP_IRQ_THREAD added a reader that can: a Windows run
 * dispatched a routine of 0xFFFFFF00 moments after the title connected vector
 * 5, and the guest died shortly after with the same value in ECX.
 *
 * The routine is the one field whose correctness can be CHECKED rather than
 * assumed -- either the dispatch table knows that address or it does not. The
 * ServiceContext deliberately is not range-checked: contiguous allocations
 * live in the 0x80000000 physical mirror, so every bound naive enough to write
 * here would reject a legitimate context.
 *
 * A failing entry is skipped, never cleared. The title is mid-way through
 * filling it in and the next pass, 16 ms later, will find it complete. */
static int bridge_interrupt_ready(uint32_t iv)
{
    uint32_t routine;

    if (!iv) return 0;
    routine = BRIDGE_MEM32(iv + 0);
    if (!routine) return 0;
    if (!recomp_lookup(routine) && !recomp_lookup_manual(routine)) {
        g_interrupt_not_ready++;
        return 0;
    }
    return 1;
}

static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    int i;

    if (interrupt_va) {
        for (i = 0; i < BRIDGE_MAX_INTERRUPTS; i++) {
            if (g_interrupts[i] == interrupt_va) break;
            if (g_interrupts[i] == 0) {
                /* Release: everything KeInitializeInterrupt wrote into this
                 * KINTERRUPT must be visible to the IRQ thread before the slot
                 * that points at it is. */
                __atomic_store_n(&g_interrupts[i], interrupt_va,
                                 __ATOMIC_RELEASE);
                fprintf(stderr,
                        "  [KERNEL] KeConnectInterrupt: kinterrupt=0x%08X "
                        "routine=0x%08X context=0x%08X vector=%u\n",
                        interrupt_va,
                        BRIDGE_MEM32(interrupt_va + 0),
                        BRIDGE_MEM32(interrupt_va + 4),
                        BRIDGE_MEM32(interrupt_va + 8));
                fflush(stderr);
                break;
            }
        }
    }
    g_eax = 1;  /* connected -- see the note above */
}

/* Call a guest ISR: BOOLEAN (__stdcall *)(PKINTERRUPT, PVOID ServiceContext).
 * Same mechanism as bridge_run_dpc, and it reuses the same g_in_dpc guard so
 * an ISR cannot be entered from inside a DPC or another ISR. */
/* The KINTERRUPT a title connected on a vector, or 0.
 *
 * src/usb/ohci.c has declared and called this since it was written, and
 * NOTHING IN THIS TREE DEFINED IT. Upstream does, at kernel_bridge.c:2087 over
 * its own g_connected_isr table; we diverged, kept our own g_interrupts, and
 * lost the accessor. Nobody noticed because the only caller is ohci_call_isr,
 * which -O2 drops as unreachable along with ohci_thread -- so the OHCI service
 * path has not been linked into either host's binary, and the link only broke
 * when something finally referenced enough of ohci.c to keep those functions.
 *
 * Same contract as upstream's: the routine and its context are at +0 and +4 of
 * the returned KINTERRUPT. */
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    int i;

    /* SCAN AND MATCH, because g_interrupts is not indexed by vector.
     *
     * This used to `return g_interrupts[vector]`. bridge_KeConnectInterrupt
     * fills the FIRST FREE SLOT, so the table is dense and insertion-ordered,
     * and the vector lives inside the KINTERRUPT at +8 -- which is how every
     * other reader in this file gets it (:2977, :3566, :3609, :3752, all
     * BRIDGE_MEM32(iv + 8)). Indexing by vector only agrees with that when the
     * title happens to connect its vectors in ascending order from 0.
     *
     * JSRF does not. Measured, across all 784 KeConnectInterrupt lines in the
     * archived runs under build-macos, the order is always 3, 1, 6, 5 -- so
     * slot 0 holds vector 3, slot 1 vector 1, slot 2 vector 6, slot 3 vector
     * 5, and the old expression answered vector 1 correctly BY COINCIDENCE
     * while returning the vector-5 KINTERRUPT for vector 3 and 0 for vectors 5
     * and 6.
     *
     * NOT A LIVE macOS BUG, and the reason is worth writing down rather than
     * rediscovering: the only caller is ohci_call_isr (src/usb/ohci.c:499),
     * which asks for OHCI_VECTOR == 1 -- the one vector the coincidence got
     * right -- and its own only caller is ohci_raise, reached only from
     * ohci_thread, which is created inside `#if defined(_WIN32)`
     * (src/usb/ohci.c:877). So on this host the function is unreachable and on
     * Windows it was accidentally correct. This is correctness for the next
     * vector anyone asks for, not a fix for anything now failing.
     *
     * Acquire, and stop at the first hole, for the same reasons the other
     * readers do: the slot is published with __ATOMIC_RELEASE after the
     * KINTERRUPT is filled in, and the table is dense so a zero ends it. */
    for (i = 0; i < BRIDGE_MAX_INTERRUPTS; i++) {
        uint32_t iv = __atomic_load_n(&g_interrupts[i], __ATOMIC_ACQUIRE);
        if (!iv) break;
        if (BRIDGE_MEM32(iv + 8) == vector) return iv;
    }
    return 0;
}

/* xbox_AllocThreadTib IS STILL NOT DEFINED HERE, AND NOW THERE IS A MEASUREMENT.
 *
 * src/usb/ohci.c calls it; this tree does not define it; upstream does, from
 * g_tls_total / g_tls_template_va / g_tls_thread_size, none of which exist
 * here because our loader lays the per-thread block out differently. The
 * loader's size is now published as g_image_tls_total, so the obvious version
 * became writable:
 *
 *     stack = xbox_AllocThreadStack(0);
 *     tib   = xbox_HeapAlloc(0x30, 16);
 *     ctx   = xbox_HeapAlloc(0x2C, 16);
 *     data  = xbox_HeapAlloc(g_image_tls_total, 16);
 *     xbox_SetupCurrentThreadTib(tib, ctx, data, g_image_tls_total, ...);
 *
 * It was written, built on both hosts, and it WEDGES WINDOWS. With
 * RECOMP_USB=1 and a pad attached the guest hangs with
 * "[ADX] tick STUCK at 1 (flag=0)" and never advances; the same run on the
 * binary without it is fine, and USB-on runs before it showed no STUCK line at
 * all. So defining this is what breaks it, not RECOMP_USB.
 *
 * Prime suspect, unproven: those allocators. ohci_thread starts during init
 * and calls xbox_AllocThreadStack and xbox_HeapAlloc from a HOST thread while
 * the guest is allocating on its own; if the guest heap is not thread-safe --
 * and nothing here says it is -- the two corrupt each other and everything
 * downstream stalls. That is checkable: pre-allocate the TIB, stack and TLS
 * block on the main thread before ohci_thread is created, and hand them over.
 *
 * Do not re-add the naive version. It builds, it links, it looks right, and it
 * stops the title. */


/* RECOMP_PGRAPH_ISR_TRACE, read once.
 *
 * This was two unconditional getenv calls on two of the hottest paths in the
 * bridge: bridge_vblank_poll runs every vblank poll and bridge_run_isr on
 * every interrupt handoff. getenv takes a lock and walks the environment
 * linearly on macOS, and a 12 s profile of the Corn tutorial put __findenv
 * above nv2a_metal_draw. The switch cannot change during a run. */
static int bridge_isr_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PGRAPH_ISR_TRACE") != NULL;
    return on;
}

/* `entered` is set only if the guest's ISR actually ran.
 *
 * The return value cannot carry this: a guest ISR legitimately returns FALSE
 * to mean "not mine, I declined", and that is indistinguishable from the two
 * early exits below where no guest code ran at all. Inferring entry from a
 * counter delta is worse still -- g_irq_delivered is process-wide, so another
 * thread completing a USB or audio delivery moves it while this vector was
 * refused. Pass the fact out explicitly. */
static uint32_t bridge_run_isr_ex(uint32_t interrupt_va, int *entered)
{
    uint32_t routine, context;
    recomp_func_t fn;
    uint32_t isr_result = 0;
    int handoff_trace = 0;

    if (entered) *entered = 0;
    routine = BRIDGE_MEM32(interrupt_va + 0);
    context = BRIDGE_MEM32(interrupt_va + 4);
    if (!routine) return 0;

    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        static uint32_t warned_routine = 0;
        if (warned_routine != routine) {
            warned_routine = routine;
            fprintf(stderr, "  [KERNEL] ISR 0x%08X not in dispatch\n", routine);
            fflush(stderr);
        }
        return 0;
    }

    {
        BridgeGuestRegs saved;
        if (bridge_isr_trace() &&
            BRIDGE_MEM32(interrupt_va + 8) == BRIDGE_NV2A_VECTOR) {
            g_current_isr_handoff = InterlockedIncrement(&g_isr_handoff_seq);
            /* A healthy render loop executes this thousands of times. Keep
             * the first handoffs and occasional progress markers without
             * turning a short diagnostic run into hundreds of megabytes. */
            handoff_trace = g_current_isr_handoff <= 16 ||
                            (g_current_isr_handoff % 1000) == 0;
        }
        if (handoff_trace) {
            fprintf(stderr, "  [ISR-HANDOFF] #%d guest-enter routine=%08X esp=%08X\n",
                    (int)g_current_isr_handoff, routine, g_esp);
            fflush(stderr);
        }
        bridge_save_regs(&saved);
        g_in_isr = 1;
        g_pending_dpc = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = interrupt_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        if (entered) *entered = 1;
        /* Entered and returned as two counters, not one.
         *
         * An ISR count that is bumped once around the call cannot tell "the
         * handler ran 37,000 times" from "the handler was entered 37,000 times
         * and the 37,000th never came back" -- and an ISR that does not return
         * is exactly the shape a dead USB driver would have. The increment
         * before fn() and the one after it are the whole difference, and the
         * USB stall snapshot gates on the SECOND one: a machine still
         * returning from ISRs is alive, whatever else has stopped. */
        InterlockedIncrement(&g_bridge_isr_entered);
        fn();
        InterlockedIncrement(&g_bridge_isr_returned);
        g_in_isr = 0;
        isr_result = g_eax;
        if (handoff_trace) {
            fprintf(stderr,
                    "  [ISR-HANDOFF] #%d guest-return result=%02X pending-dpc=%08X\n",
                    (int)g_current_isr_handoff, isr_result & 0xFF, g_pending_dpc);
            fflush(stderr);
        }
        bridge_restore_regs(&saved);
    }

    {
        /* An ISR returns BOOLEAN "I handled it" in al. A run of FALSE means the
         * routine is bailing at one of its gates, which is worth seeing rather
         * than guessing at. JSRF's GPU ISR gates on its ServiceContext[0xA0]
         * and on the NV2A interrupt-enable register at base+0x140. */
        static int logged = 0;
        if (logged < 10) {
            uint32_t ctx_base = BRIDGE_MEM32(context);
            logged++;
            fprintf(stderr,
                    "  [KERNEL] ISR 0x%08X -> %s   ctx[0xA0]=0x%08X "
                    "ctx[0xB0]=0x%08X regbase=0x%08X en(+0x140)=0x%08X "
                    "intr(+0x100)=0x%08X\n",
                    routine, (isr_result & 0xFF) ? "TRUE" : "FALSE",
                    BRIDGE_MEM32(context + 0xA0),
                    BRIDGE_MEM32(context + 0xB0), ctx_base,
                    ctx_base ? BRIDGE_MEM32(ctx_base + 0x140) : 0,
                    ctx_base ? BRIDGE_MEM32(ctx_base + 0x100) : 0);
            fflush(stderr);
        }
    }

    /* Now run whatever the ISR queued, outside the ISR itself. */
    if (g_pending_dpc) {
        uint32_t dpc = g_pending_dpc;
        uint32_t s1 = g_pending_dpc_sys1, s2 = g_pending_dpc_sys2;
        g_pending_dpc = 0;
        if (handoff_trace) {
            fprintf(stderr, "  [ISR-HANDOFF] #%d dpc-enter dpc=%08X\n",
                    (int)g_current_isr_handoff, dpc);
            fflush(stderr);
        }
        bridge_run_dpc(dpc, s1, s2);
        if (handoff_trace) {
            fprintf(stderr, "  [ISR-HANDOFF] #%d dpc-return\n",
                    (int)g_current_isr_handoff);
            fflush(stderr);
        }
    }

    if (handoff_trace) g_current_isr_handoff = 0;

    return isr_result;
}

/* Raise the GPU interrupt at the display refresh rate. */
/* Set once the GPU interrupt is first delivered; the mirror needs the register
 * base and nothing else knows it. */
static uint32_t g_nv2a_base;
/* Refusals of a ServiceContext register base that is not the NV2A aperture,
 * and the last value refused. Printed on the [VBLANK-REG] line beside base=,
 * which is where anyone reading that base is already looking. */
static volatile LONG g_nv2a_base_refused;
static volatile LONG g_nv2a_base_last_bad;

/* Is this guest VA the NV2A register aperture?
 *
 * g_nv2a_base is read out of the guest's KINTERRUPT ServiceContext -- guest
 * memory, written by guest code -- and then used as the base of ATOMIC
 * READ-MODIFY-WRITES in bridge_nv2a_mirror_intr (the PMC summary fetch_and /
 * fetch_or, and the PGRAPH fetch_or). BRIDGE_MEM32 is an unchecked offset add
 * (see :65), so a wrong base there is not a failed call: it is a wild read AND
 * a wild WRITE into mapped guest RAM, on the interrupt pump, thousands of
 * times a second, with nothing counting it. Everything else this runtime does
 * with guest pointers is bounds-checked; this was not.
 *
 * Validated against the mapping the memory layout actually made rather than
 * against a second copy of 0xFD000000 in this file: xbox_Nv2aRegisterMemory()
 * returns the host base of the NV2A aperture view, so comparing the
 * translation of `base` against it checks the one thing that matters -- that
 * the guest handed us the aperture -- and cannot drift from the layout.
 *
 * HARDENING, NOT A LIVE BUG. All 218 [VBLANK-REG] lines in the archived runs
 * under build-macos read base=FD000000, so the guest has never yet published
 * anything else. A refusal counter is the honest way to leave it: if the value
 * ever does change, this says so instead of writing wherever it points. */
static int bridge_nv2a_base_valid(uint32_t base)
{
    const uint8_t *regs = xbox_Nv2aRegisterMemory();
    if (!base || !regs) return 0;
    return (const uint8_t *)((uintptr_t)base + g_xbox_mem_offset) == regs;
}

/* bridge_KeWaitForSingleObject pumps vblank from every blocked guest thread.
 * The TLS ISR/DPC guards only prevent recursion on one host thread; without a
 * process-wide guard, several waiters can pass the pending check together and
 * run the same interrupt and DPC concurrently.  The NV2A delivers one IRQ at a
 * time, and D3D's DPC mutates shared pending state under that assumption. */
static uint32_t bridge_run_isr(uint32_t interrupt_va)
{
    /* The guest is about to service it: close the outstanding raise. */
    recomp_irq_latency_deliver();

    return bridge_run_isr_ex(interrupt_va, NULL);
}

/* ── Interrupt delivery: three concepts that used to be one ──────────────
 *
 * g_vblank_delivery_active was a single process-wide interlock doing three
 * unrelated jobs at once, and conflating them is what made an ISR or DPC on
 * ONE guest thread stop interrupt delivery for EVERY thread and EVERY vector.
 * Measured consequence: bridging ordinal 153 ran previously-dead DSOUND code
 * inside that interlock, and the guest's USB driver -- whose ISR is delivered
 * only by bridge_device_irq_poll, behind the same interlock -- starved and
 * died. Transfer rate fell from ~74000 to ~14000 per run and the title stopped
 * reaching New Game. The interlock was never an IRQL model; it just happened
 * to suppress enough to look like one.
 *
 * Separated here into what each job actually needs:
 *
 *   1. RECURSIVE-ENTRY PROTECTION, and it is per thread, not per process.
 *      A thread already running a guest ISR or DPC must not be handed another
 *      interrupt on top of it -- that is a genuine stack hazard. A DIFFERENT
 *      thread is not endangered by it at all. g_dispatch_depth.
 *
 *   2. PER-VECTOR NON-REENTRANCY, process wide but only for the one vector.
 *      A guest ISR is not re-entrant for its own vector, so the same vector
 *      must not be in service twice concurrently. Two different vectors may
 *      be, and on real hardware routinely are. g_vector_in_service[].
 *
 *   3. GUEST IRQL AND MASKING, which is what "can this be delivered" actually
 *      means. The guest already tracks IRQL through KfRaiseIrql/KfLowerIrql
 *      and KeInitializeInterrupt already tells us each vector's IRQL -- we
 *      simply threw that argument away. An interrupt is eligible when the
 *      delivering context's effective IRQL is BELOW the vector's IRQL.
 *      Otherwise it stays pending and is delivered when IRQL drops.
 *
 * ISR/DPC execution state (g_in_isr, g_in_dpc) is concept 3's input for THIS
 * thread -- it raises this thread's effective IRQL -- and concept 1's trigger.
 * It is no longer a global veto.
 *
 * RECOMP_LEGACY_IRQ_INTERLOCK=1 restores the old process-wide behaviour, so
 * the two can be compared on one binary. */
static volatile LONG g_vblank_delivery_active;

/* 1. Per-thread recursion depth. Non-zero means this thread is inside a guest
 *    ISR or DPC and must not start another. */
static RECOMP_TLS int g_dispatch_depth;

/* 2. Per-vector in-service flags, and 3. per-vector pending + IRQL. */
static volatile LONG g_vector_in_service[BRIDGE_MAX_INTERRUPTS];
static volatile LONG g_vector_pending[BRIDGE_MAX_INTERRUPTS];
static uint8_t       g_vector_irql[BRIDGE_MAX_INTERRUPTS];

/* Counters. Every one of these is read by xbox_ReportIrqDelivery below, and
 * each is incremented at exactly one site so its meaning cannot drift -- this
 * tree has been misled more than once by a counter whose trigger nobody had
 * read. */
static volatile LONG g_irq_delivered;        /* ISR actually entered */
/* Per vector, because the process-wide total cannot answer "is the guest's USB
 * ISR still being entered" -- which is the question the bimodal investigation
 * turns on. Vector 1 is USB, 5 and 6 are the audio pair JSRF connects. */
static volatile LONG g_irq_delivered_vec[BRIDGE_MAX_INTERRUPTS];
static volatile LONG g_irq_defer_irql;       /* blocked: effective IRQL too high */
static volatile LONG g_irq_defer_reentry;    /* blocked: this thread already dispatching */
static volatile LONG g_irq_defer_vector;     /* blocked: same vector already in service */
static volatile LONG g_irq_pending_peak;     /* most vectors pending at once */
static volatile LONG g_irq_legacy_hits;      /* old global interlock actually blocked someone */

static int bridge_legacy_irq_interlock(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_LEGACY_IRQ_INTERLOCK");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on;
}

/* This context's effective IRQL.
 *
 * Inside a guest ISR the processor is at the device's IRQL; inside a DPC it is
 * at DISPATCH_LEVEL. Outside both it is whatever the guest last set. Modelling
 * ISR/DPC state as an IRQL contribution rather than as a separate veto is the
 * whole point: it keeps the suppression scoped to the context that is actually
 * raised, instead of the entire process. */
static KIRQL bridge_effective_irql(void)
{
    if (g_in_isr) return (KIRQL)0xFF;          /* device level: nothing preempts */
    if (g_in_dpc) return (KIRQL)DISPATCH_LEVEL;
    return xbox_KeGetCurrentIrql();
}

static void bridge_note_vector_irql(uint32_t vector, uint32_t irql)
{
    if (vector < BRIDGE_MAX_INTERRUPTS)
        g_vector_irql[vector] = (uint8_t)irql;
}

static KIRQL bridge_vector_irql(uint32_t vector)
{
    /* DISPATCH_LEVEL + 1 is the floor for a device interrupt: it must be able
     * to preempt DPC-level work, which is what a device IRQL means. A vector
     * the guest never gave an IRQL for gets that floor rather than 0, which
     * would make it permanently ineligible. */
    KIRQL irql = (vector < BRIDGE_MAX_INTERRUPTS) ? g_vector_irql[vector] : 0;
    return irql > (KIRQL)DISPATCH_LEVEL ? irql : (KIRQL)(DISPATCH_LEVEL + 1);
}

/* Deliver one connected interrupt, if this context is allowed to.
 *
 * Returns the ISR's result, or 0 when nothing was delivered. A refusal is
 * counted by reason and, when the reason is IRQL, the vector is left pending
 * so the next poll picks it up once the guest drops back down. Re-delivery is
 * driven by the existing poll rather than by a separate queue: the pumps
 * already revisit every connected vector on a timer, so a pending vector is
 * retried within one period, and a second mechanism would be a second thing to
 * keep correct. */
/* The eligibility decision, separated from the act of delivering.
 *
 * Separate so the tests can exercise THIS function rather than a copy of its
 * reasoning -- a test that reimplements the rule it is checking passes against
 * a broken implementation, which this tree has already been bitten by once
 * today. bridge_deliver_isr below is the only production caller. */
/* RECOMP_IRQ_RETRY_PENDING, default OFF until measured.
 *
 * A VBLANK RAISED WHILE ITS OWN ISR IS STILL RUNNING IS DROPPED FOREVER, and
 * the guest advances its simulation one step per vblank, so each one is a
 * step the game never takes. Measured in a player session 16 Sep 2026:
 *
 *     [VBLANK] delivered=9724 (raised=10170) over 170097 ms
 *     deadlines 59.75/s, delivered 57.20/s, 446 raises lost = 4.4%
 *     defer_irql=0 defer_reentry=0 not_ready=0
 *
 * so essentially all of them are XBOX_IRQ_DEFER_VECTOR: same vector already
 * in service. That is ~96% game speed before frame time is counted at all,
 * and it is independent of the frame-locked loss the earlier handovers
 * describe. On hardware the PCRTC interrupt stays ASSERTED until the driver
 * acknowledges it at the source; it is taken when the handler returns, it
 * does not evaporate because the CPU is busy.
 *
 * AND THE NEIGHBOURING BRANCH ALREADY CLAIMED TO HANDLE THIS. The IRQL defer
 * sets g_vector_pending under the comment "Pending, not dropped. The pumps
 * revisit every connected vector on a timer" -- but that flag had NO
 * production reader: set, cleared on success, counted for a peak, and
 * otherwise read only by xbox_IrqTestPending. Nothing ever re-delivered it.
 * This switch gives the flag the consumer the comment always promised, and
 * points the vector deferral at it too.
 *
 * BOUNDED BY CONSTRUCTION: one bit per vector, so a storm coalesces into a
 * single retry. It cannot deliver a burst, which is the failure mode
 * BRIDGE_VBLANK_PERIOD_US's own comment is written to avoid ("one late
 * vblank, not five at once").
 *
 * THE PREDICTION, so the A/B can refute it: delivered/elapsed moves from
 * 57.2 Hz toward the 59.75 Hz deadline rate and the game runs ~4% faster
 * with NO change in frame time. If delivered rises and nothing else moves,
 * the guest was not the bottleneck and this is not where the speed went. */
static int irq_retry_pending(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_IRQ_RETRY_PENDING");
    return on;
}
unsigned long g_irq_retry_queued, g_irq_retry_delivered;
extern unsigned long g_vblank_delivered;  /* defined below; the retry path
                                           * counts its own vblank deliveries */

static int bridge_irq_decide(uint32_t vector)
{
    int slot = (vector < BRIDGE_MAX_INTERRUPTS) ? (int)vector : -1;

    /* 1. Recursion, and only for THIS thread. */
    if (g_dispatch_depth) return XBOX_IRQ_DEFER_REENTRY;

    /* 3. Eligibility by IRQL. */
    if (bridge_effective_irql() >= bridge_vector_irql(vector))
        return XBOX_IRQ_DEFER_IRQL;

    /* 2. Same vector already in service, anywhere in the process. A different
     *    vector is deliberately NOT consulted here. */
    if (slot >= 0 && g_vector_in_service[slot]) return XBOX_IRQ_DEFER_VECTOR;

    return XBOX_IRQ_ELIGIBLE;
}

static uint32_t bridge_deliver_isr_ex(uint32_t iv, int *entered)
{
    uint32_t vector = BRIDGE_MEM32(iv + 8);
    uint32_t result;
    int slot = (vector < BRIDGE_MAX_INTERRUPTS) ? (int)vector : -1;
    int decision = bridge_irq_decide(vector);

    if (entered) *entered = 0;
    if (decision == XBOX_IRQ_DEFER_REENTRY) {
        InterlockedIncrement(&g_irq_defer_reentry);
        return 0;
    }
    if (decision == XBOX_IRQ_DEFER_IRQL) {
        /* Pending, not dropped. The pumps revisit every connected vector on a
         * timer, so this is retried within one period once IRQL drops. */
        InterlockedIncrement(&g_irq_defer_irql);
        if (slot >= 0) {
            LONG n = 0; int k;
            InterlockedExchange(&g_vector_pending[slot], 1);
            for (k = 0; k < BRIDGE_MAX_INTERRUPTS; k++)
                if (g_vector_pending[k]) n++;
            if (n > g_irq_pending_peak) InterlockedExchange(&g_irq_pending_peak, n);
        }
        return 0;
    }
    if (decision == XBOX_IRQ_DEFER_VECTOR) {
        InterlockedIncrement(&g_irq_defer_vector);
        if (irq_retry_pending() && slot >= 0
            && InterlockedExchange(&g_vector_pending[slot], 1) == 0)
            g_irq_retry_queued++;
        return 0;
    }

    /* Claim the vector. The decision above read the flag; this is what makes
     * the claim atomic against another thread deciding the same thing. */
    if (slot >= 0 &&
        InterlockedCompareExchange(&g_vector_in_service[slot], 1, 0) != 0) {
        InterlockedIncrement(&g_irq_defer_vector);
        if (irq_retry_pending()
            && InterlockedExchange(&g_vector_pending[slot], 1) == 0)
            g_irq_retry_queued++;
        return 0;
    }

    int ran_any = 0;
    {
        int ran = 0;
        g_dispatch_depth++;
        result = bridge_run_isr_ex(iv, &ran);
        g_dispatch_depth--;
        ran_any = ran;
        if (entered) *entered = ran;
        /* Count only what actually entered guest code. bridge_run_isr_ex bails
         * without running when the KINTERRUPT has no routine, or when that
         * routine is not in the dispatch table -- neither is a delivery, and
         * counting them inflated both [IRQ] delivered= and the vblank Hz. */
        if (ran) {
            InterlockedIncrement(&g_irq_delivered);
            if (slot >= 0) InterlockedIncrement(&g_irq_delivered_vec[slot]);
        }
    }

    /* Acknowledge exactly once: pending cleared and the vector released, in
     * that order, so a concurrent decide() cannot see "free but still pending"
     * and count a spurious deferral. */
    if (slot >= 0) {
        LONG queued = InterlockedExchange(&g_vector_pending[slot], 0);
        InterlockedExchange(&g_vector_in_service[slot], 0);
        /* THE CONSUMER THE PENDING FLAG NEVER HAD. The vector is free as of
         * the line above, so this is a fresh delivery and not a nested one --
         * g_dispatch_depth has already been decremented, so bridge_irq_decide
         * will not read it as re-entry. One retry per completion: the flag is
         * a single bit and was cleared before this call, so a deferral that
         * arrives DURING the retry queues the next one rather than recursing
         * without bound. */
        if (queued && irq_retry_pending() && ran_any) {
            int again = 0;
            bridge_deliver_isr_ex(iv, &again);
            if (again) {
                g_irq_retry_delivered++;
                /* AND COUNT IT AS A VBLANK DELIVERY, because it is one.
                 * g_vblank_delivered is incremented at bridge_vblank_poll's
                 * own call site only, so the first A/B of this switch read
                 * "5862 retries delivered" beside a delivered= that had not
                 * moved -- 55.7 Hz against 56.0 -- and the frame rate rose 5%
                 * with nothing in the vblank line to explain it. The retries
                 * WERE reaching the guest; the counter could not see them.
                 * Exactly the instrument failure this tree keeps retiring,
                 * introduced by me in the same hour I wrote up two others. */
                if (vector == BRIDGE_NV2A_VECTOR) g_vblank_delivered++;
            }
        }
    }
    return result;
}

static uint32_t bridge_deliver_isr(uint32_t iv)
{
    return bridge_deliver_isr_ex(iv, NULL);
}

/* ── Test seam ────────────────────────────────────────────────────────────
 * Deliberately thin, and every entry point drives the REAL state the real
 * decision reads. Nothing here duplicates the rule. */
int xbox_IrqTestDecide(uint32_t vector) { return bridge_irq_decide(vector); }
/* Drives the REAL delivery path, so a test can prove refuse-then-retry rather
 * than only that the decision function answers correctly. Codex's review of
 * the first version of this test made the point: it set pending state by hand
 * and never showed a refused interrupt was subsequently delivered -- which is
 * exactly the bug that was live in the vblank pump at the time. Returns 1 if
 * the guest ISR was entered. */
int xbox_IrqTestDeliver(uint32_t interrupt_va)
{
    int entered = 0;
    bridge_deliver_isr_ex(interrupt_va, &entered);
    return entered;
}
LONG xbox_IrqTestDeliveredCount(void) { return g_irq_delivered; }
void xbox_IrqTestSetVectorIrql(uint32_t v, uint32_t irql) { bridge_note_vector_irql(v, irql); }
void xbox_IrqTestSetInService(uint32_t v, int on)
{
    if (v < BRIDGE_MAX_INTERRUPTS) InterlockedExchange(&g_vector_in_service[v], on ? 1 : 0);
}
int xbox_IrqTestPending(uint32_t v)
{
    return (v < BRIDGE_MAX_INTERRUPTS) ? (int)g_vector_pending[v] : 0;
}
void xbox_IrqTestSetPending(uint32_t v, int on)
{
    if (v < BRIDGE_MAX_INTERRUPTS) InterlockedExchange(&g_vector_pending[v], on ? 1 : 0);
}
void xbox_IrqTestEnterIsr(int on) { g_in_isr = on ? 1 : 0; g_dispatch_depth = on ? 1 : 0; }
void xbox_IrqTestEnterDpc(int on) { g_in_dpc = on ? 1 : 0; g_dispatch_depth = on ? 1 : 0; }
void xbox_IrqTestReset(void)
{
    int k;
    g_in_isr = g_in_dpc = g_dispatch_depth = 0;
    for (k = 0; k < BRIDGE_MAX_INTERRUPTS; k++) {
        InterlockedExchange(&g_vector_in_service[k], 0);
        InterlockedExchange(&g_vector_pending[k], 0);
        g_vector_irql[k] = 0;
    }
}

void xbox_ReportIrqDelivery(void)
{
    int k; LONG pend = 0;
    for (k = 0; k < BRIDGE_MAX_INTERRUPTS; k++) if (g_vector_pending[k]) pend++;
    {
        /* Per-vector deliveries, so a stalled USB ISR is visible while audio
         * keeps running. A single total hides exactly that. */
        char per[128]; int n = 0; int v;
        per[0] = 0;
        for (v = 0; v < BRIDGE_MAX_INTERRUPTS && n < (int)sizeof per - 16; v++)
            if (g_irq_delivered_vec[v])
                n += snprintf(per + n, sizeof per - n, " v%d=%d",
                              v, g_irq_delivered_vec[v]);
        fprintf(stderr, "  [IRQ-VEC]%s\n", per[0] ? per : " (none delivered)");
    }
    fprintf(stderr,
            "  [IRQ] delivered=%d deferred: irql=%d reentry=%d vector=%d"
            " | pending now=%d peak=%d | legacy-interlock-blocks=%d%s\n",
            (int)g_irq_delivered, (int)g_irq_defer_irql,
            (int)g_irq_defer_reentry, (int)g_irq_defer_vector, (int)pend,
            (int)g_irq_pending_peak, (int)g_irq_legacy_hits,
            bridge_legacy_irq_interlock() ? " (LEGACY MODE)" : "");
    fflush(stderr);
}


/* THE SUMMARY BIT OBSERVED LOST: PCRTC bit 0 set while PMC bit 24 is clear.
 *
 * Counted on every mirror pass whether or not the repair below is armed, and
 * that is the entire point of it. With RECOMP_NV2A_PMC_UPMIRROR off this build
 * behaves exactly as the one before it and still answers the question the
 * repair is built on, so one run decides whether the repair is needed at all.
 * A repair armed by default would have erased the evidence for its own
 * necessity -- and a guard defaulted on with a good argument behind it has
 * already made a crash worse in this tree once.
 *
 * The [VBLANK] counters cannot answer it. A hung 725 s session reported
 * delivered=5355 (raised=5651 retried=33312) against deadlines=43263 with
 * unacked_skips=37571, the skips still climbing at 60/s at the end. That says
 * only that xbox_Nv2aVblankPending stayed true for the rest of the run: the
 * source is asserted and the guest never acknowledged it. It does not say why,
 * and a lost summary is only one of the candidates.
 *
 * FOUR NUMBERS, BECAUSE A RAW COUNT CANNOT DECIDE. The signature has a benign
 * producer. xbox_Nv2aRaiseVblank (xbox_memory_layout.c:2170) sets PCRTC under
 * mcpx_lock, releases the lock, and only then ORs the summary, so for a few
 * instructions every single raise looks exactly like a loss. This mirror is
 * pumped from every blocked waiter and from every kernel thunk -- thousands of
 * passes a second -- so some of them WILL land inside that window, and a
 * nonzero `lost` on its own proves nothing whatsoever.
 *
 *   lost        passes that saw the signature. Rate-like: once the summary is
 *               latched lost this climbs at the pump rate for the rest of the
 *               run, so its size measures how long the latch lasted, not how
 *               often the summary went missing.
 *   episodes    distinct runs of consecutive such passes, i.e. the 0->1
 *               transitions. THIS is how many times it went missing.
 *   max_run     the longest run, in passes.
 *   max_run_ms  the longest run, in wall time.
 *
 * The raise window closes within a microsecond and the very next pass sees an
 * agreeing pair, so benign sampling reads as episodes in the thousands with
 * max_run near 1 and max_run_ms 0. A latch reads as episodes=1 (or a handful)
 * with max_run in the millions and max_run_ms most of the run. Those two
 * cannot be mistaken for each other, which is what buys four counters instead
 * of one.
 *
 * The run bookkeeping is deliberately NOT serialised. This is the hottest path
 * in the bridge and a lock here would cost more than the bug. The races that
 * leaves can only SHORTEN a run -- an agreeing pass on any thread resets it --
 * so the race cannot manufacture a long max_run_ms, which is the only
 * direction the conclusion turns on. The start stamp is a plain DWORD for the
 * same reason; the worst it can cost is one pump interval of the reported
 * milliseconds. */
static volatile LONG g_nv2a_pmc_lost;          /* passes that saw the loss   */
static volatile LONG g_nv2a_pmc_lost_episodes; /* distinct runs of them      */
static volatile LONG g_nv2a_pmc_lost_run;      /* consecutive passes, now    */
static volatile LONG g_nv2a_pmc_lost_run_max;
static volatile LONG g_nv2a_pmc_lost_ms_max;
static volatile LONG g_nv2a_pmc_repaired;      /* up-mirrors actually done   */
static DWORD g_nv2a_pmc_lost_since;            /* start of the current run   */

/* Arm the repair? DEFAULT ON since 19 Sep 2026; it shipped OFF on 16 Sep so
 * that the detector above could prove the loss with the repair disarmed, and
 * that job is done. Its state is printed beside the counters on the
 * [VBLANK-REG] line, because src/recomp_switch.h is right that a switch which
 * does not name itself in a report cannot be compared across arms.
 *
 * THE A/B THAT BUILT IT (4111a6b), two scene-matched 180 s runs that both
 * reached a mission and played:
 *
 *   upmirror=off  delivered=1738 raised=1910 unacked_skips=8266  10.2 Hz
 *                 summary lost=302477 episodes=7 max_run_ms=138028
 *                 ADX worker frozen
 *   upmirror=on   delivered=9859 raised=10133 unacked_skips=2     58.0 Hz
 *                 summary lost=12 episodes=12 max_run=1 max_run_ms=0
 *                 repaired=12, ADX worker never froze
 *
 * AND THE CORPUS AGREES AT SCALE, which is what moved the default rather than
 * the argument. Completed runs under build-macos/jsrf-first-fault/
 * render-investigation, classified by each run's own switches.txt and
 * RESTRICTED TO 16-19 SEP, which is the window in which both arms exist --
 * this switch was not in any binary before 16 Sep, so a whole-corpus sweep
 * silently compares different software and the first version of this note
 * did exactly that:
 *
 *                     clean    "[ADX] tick STUCK"     NULL storm     n
 *   upmirror set        86              0                  1         87
 *   not set            117             70                 13        200
 *
 * 0 of 87 against 70 of 200, under the date control.
 *
 * THE THIRD COLUMN IS A SECOND BUG WEARING THE SAME WATCHDOG LINE, and
 * separating it is what makes the second column mean anything. Some frozen
 * runs are an unresolved indirect call spinning on NULL at 0x00114B66, up to
 * 2.3e10 calls; those are 17 runs and this repair does not touch them (1 of
 * 87 armed still storms). The other 70 are a vblank-delivery failure and
 * this repair removes them. Two bugs, one symptom, two fixes: do not score a
 * freeze without checking which one it is.
 *
 * That is observational and not a controlled A/B -- the arms still differ in
 * other switches -- but it is the same direction as the controlled pair
 * above, it survives the confound that mattered most, and the ADX freeze is
 * what caps every unattended measurement this harness takes. Defaulting it
 * on is how the harness stops measuring a configuration nobody plays.
 *
 * RECOMP_NV2A_PMC_UPMIRROR=0 restores the 16 Sep behaviour, and the detector
 * keeps counting either way, so the control arm is still one token. The
 * grammar is recomp_switch_on_default's: unset or empty keeps the default,
 * because `VAR= cmd` is how a shell unsets a variable for one command and a
 * harness that did that must not silently disarm the repair. */
static int bridge_nv2a_pmc_upmirror(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on_default("RECOMP_NV2A_PMC_UPMIRROR", 1);
    return on;
}

/* One pump pass that saw the source and the summary disagree, in the direction
 * that latches. Read the comment above before trusting any of these. */
static void bridge_nv2a_summary_lost(void)
{
    LONG run = InterlockedIncrement(&g_nv2a_pmc_lost_run);
    DWORD now = GetTickCount();

    InterlockedIncrement(&g_nv2a_pmc_lost);
    if (run == 1) {
        InterlockedIncrement(&g_nv2a_pmc_lost_episodes);
        g_nv2a_pmc_lost_since = now;
    } else {
        LONG ms = (LONG)(DWORD)(now - g_nv2a_pmc_lost_since);
        if (ms > g_nv2a_pmc_lost_ms_max)
            InterlockedExchange(&g_nv2a_pmc_lost_ms_max, ms);
    }
    if (run > g_nv2a_pmc_lost_run_max)
        InterlockedExchange(&g_nv2a_pmc_lost_run_max, run);
}

/* ...and one that saw them agree, which closes whatever run was open. Both
 * halves of the mirror call this, because either one leaving the pair
 * consistent ends the episode. */
static void bridge_nv2a_summary_agrees(void)
{
    if (g_nv2a_pmc_lost_run) InterlockedExchange(&g_nv2a_pmc_lost_run, 0);
}

/* Make the summary bit follow its source, which is what the hardware does.
 *
 * This must run far more often than the 60Hz raise, because the guest spins on
 * the summary in a tight loop that makes no kernel calls of its own. It gets
 * that: bridge_KeWaitForSingleObject pumps this from every blocked waiter, and
 * kernel_thunk_dispatch pumps it from every other thread. */
static void bridge_nv2a_mirror_intr(void)
{
    uint32_t base = g_nv2a_base;
    uint32_t *pmc;
    if (!base) return;
    /* PMC_INTR_0 is at 0xFD000100 and is NOT on a guarded page: only the PCRTC
     * page and the PGRAPH page are (g_nv2a_guard_page and g_nv2a_pgraph_page,
     * xbox_memory_layout.c:2105 and :2122). So this pointer is ordinary
     * writable RAM, an atomic RMW on it never faults into mcpx_trap_handler,
     * and it needs neither the NV2A alias view nor a VirtualProtect. That is
     * not new: it is the same pointer the down-mirror below has always used,
     * written out once so the up-mirror's use of it is visibly the same case.
     * The PCRTC read a line later IS on the guarded page, and a read is what
     * PAGE_READONLY permits -- see xbox_Nv2aVblankPending's own comment at
     * xbox_memory_layout.c:2217. */
    pmc = (uint32_t *)((uintptr_t)base + NV_PMC_INTR_0 + g_xbox_mem_offset);
    if (!(BRIDGE_MEM32(base + NV_PCRTC_INTR_0) & NV_PCRTC_INTR_0_VBLANK)) {
        __atomic_fetch_and(pmc, ~NV_PMC_INTR_0_PCRTC, __ATOMIC_SEQ_CST);
        bridge_nv2a_summary_agrees();
    } else if (!(__atomic_load_n(pmc, __ATOMIC_SEQ_CST) & NV_PMC_INTR_0_PCRTC)) {
        /* THE SUMMARY FOLLOWS ITS SOURCE UP AS WELL AS DOWN.
         *
         * PMC_INTR_0 is not state, it is a function of the engine status
         * registers: on hardware bit 24 IS "PCRTC has an interrupt pending",
         * recomputed from the source on every read, and it cannot be out of
         * step with the source for any length of time. This file models half
         * of that. The branch above follows the source down; nothing follows
         * it up except xbox_Nv2aRaiseVblank's one-shot OR at the instant of
         * the raise. So any loss of bit 24 after that OR is PERMANENT:
         * bridge_vblank_poll will not raise again while PCRTC is still
         * pending (the "do not re-raise while unacknowledged" gate below), so
         * there is never another OR, and the guest ISR -- which reads the
         * summary out of its ServiceContext base and only does the
         * swap/signal work when bit 24 is set -- runs and returns without
         * acknowledging, for ever.
         *
         * WHY AN UP-MIRROR RATHER THAN A WIDER LOCK. The known way the bit is
         * lost is a race between this function and the raise: the mirror
         * reads PCRTC clear, the pump then sets PCRTC and ORs the summary,
         * and the mirror's fetch_and clears a summary bit it never saw
         * asserted. Taking mcpx_lock across the read-and-clear here, and
         * extending it over xbox_Nv2aRaiseVblank's OR, would close that one
         * interleaving -- at the price of putting the bridge's hottest path
         * behind the same mutex the guest's MMIO fault handler holds, on
         * every thunk dispatch. And it would close only that one. The summary
         * can also be lost to a plain guest store, because PMC is unguarded
         * RAM here while on hardware PMC_INTR_0 is read-only, and no lock in
         * this process can stop a store the recompiled code makes directly.
         * Rebuilding the summary from its source repairs every cause,
         * including the ones nobody has thought of, and it is what the
         * hardware does rather than a workaround for what this code does.
         * The same argument is already written down for the PGRAPH half a few
         * lines below, which has rebuilt its summary bit this way since the
         * anti-graffiti-screen hang; this is the missing PCRTC half of it.
         *
         * WHAT IT DOES AND DOES NOT MAKE ATOMIC: nothing. The PCRTC read and
         * the PMC fetch_or are still two operations and another thread can
         * still slip a raise or an acknowledge between them. The up-mirror is
         * CONVERGENT, not exclusive. What it buys is that no interleaving can
         * leave the pair disagreeing for longer than one pump interval,
         * because whichever way the pair ends up wrong the next pass restores
         * it: a lost OR (PCRTC=1, PMC=0) is put back here, a lost AND
         * (PCRTC=0, PMC=1) is taken away by the branch above. The worst a
         * badly-timed pass can now do is re-assert a summary bit for one
         * interval after an acknowledge that has already cleared PCRTC --
         * and it cannot, because it reads PCRTC first and that case takes the
         * other branch.
         *
         * One interleaving still costs a frame and is NOT changed by this: a
         * guest acknowledge computes its write-1-to-clear result from a read
         * taken outside mcpx_lock (xbox_memory_layout.c:1883) and stores it
         * under the lock, so an acknowledge that straddles a raise can store
         * PCRTC=0 over the fresh raise. That drops one vblank; it does not
         * latch, because the pending gate then sees a clear source and the
         * next deadline raises again. Exclusion there would need the raise
         * and the acknowledge to share a lock, and the acknowledge arrives as
         * a page fault on a guest thread. Convergence is the property a
         * mirror should have; exclusion is the property a source needs. */
        bridge_nv2a_summary_lost();
        if (bridge_nv2a_pmc_upmirror()) {
            __atomic_fetch_or(pmc, NV_PMC_INTR_0_PCRTC, __ATOMIC_SEQ_CST);
            InterlockedIncrement(&g_nv2a_pmc_repaired);
        }
    } else {
        bridge_nv2a_summary_agrees();
    }

    /* PMC_INTR_0 is a read-only summary of the engine sources.  The Windows
     * aperture is ordinary RAM outside the two guarded device pages, so a
     * guest store or a concurrent acknowledgement can erase the PGRAPH bit
     * after xbox_Nv2aRaiseSoftwareMethod asserted it.  The source remains
     * pending in PGRAPH_INTR, but the delivery gate below then sees pmc=0 and
     * the pusher waits forever -- JSRF visibly stops on the anti-graffiti
     * screen.  Rebuild the summary from its source on every interrupt pump,
     * just as the hardware does.  Atomic updates also avoid losing a PGRAPH
     * raise while the PCRTC half of the summary is being retired. */
    /* Restrict the repair to a software-method trap created by
     * xbox_Nv2aRaiseSoftwareMethod.  PGRAPH_INTR can contain unrelated
     * initialization-time errors before the pusher installs that handshake;
     * promoting those to PMC sends the guest ISR into a legitimate but
     * unserviceable error loop. */
    if ((BRIDGE_MEM32(base + NV_PGRAPH_INTR) & NV_PGRAPH_INTR_ERROR)
            && (BRIDGE_MEM32(base + NV_PGRAPH_TRAPPED_ADDR) & 0x1FFFu) == 0x100u
            && BRIDGE_MEM32(base + NV_PGRAPH_TRAPPED_DATA) != 0
            && !(BRIDGE_MEM32(base + NV_PGRAPH_FIFO_ACCESS) & 1u)) {
        __atomic_fetch_or((uint32_t *)((uintptr_t)base + NV_PMC_INTR_0
                                      + g_xbox_mem_offset),
                          NV_PMC_INTR_0_PGRAPH, __ATOMIC_SEQ_CST);
    }
}

/* Does the guest see a 60 Hz clock, or does it see its own call pattern?
 *
 * Vblank here is delivered from exactly one place -- inside
 * bridge_KeWaitForSingleObject -- because the guest owns the process's main
 * thread and there is no other thread to pump from. So a vblank arrives when a
 * guest thread happens to block, not when 16 ms of wall time have passed. The
 * deadline below is reset to now+PERIOD rather than advanced by PERIOD, so a
 * late poll does not deliver the periods it missed; they are dropped.
 *
 * Whether that costs anything is a question about rates, not about structure,
 * and it needs the wall clock beside the count to answer. delivered/elapsed is
 * the number to compare against 60: if it tracks scene load rather than time,
 * the guest's clock runs on our frame rate. max_gap is the same question asked
 * about the worst case, because a mean of 60 built from a long stall and a
 * burst is not a 60 Hz clock either. */
unsigned long g_vblank_delivered;   /* guest ISR actually entered */
unsigned long g_vblank_raised;      /* source asserted, ISR may have been refused */
unsigned long g_vblank_retry_delivered; /* serviced on a later poll after a refusal */
unsigned long g_vblank_deadlines;   /* periods that elapsed and were acted on */
unsigned long g_vblank_skipped_ack; /* deadline reached, previous still unacked */
unsigned long g_vblank_max_gap_ms;
static DWORD  g_vblank_last_ms;
static DWORD  g_vblank_first_ms;

/* Does a pending PGRAPH software method still reach the guest ISR?
 *
 * jsrf_software_method deliberately blocks the pusher until the guest
 * acknowledges the notify.  If submission stops there, totals alone cannot
 * distinguish a guest handler that keeps running from a delivery path that
 * has stopped being pumped or is stuck behind one of bridge_vblank_poll's
 * gates.  Keep one counter for each gate and for the resulting handshake.
 *
 * RECOMP_PGRAPH_ISR_TRACE is diagnostic only.  xbox_PgraphIrqReport is called
 * by the harness's existing two-second "waiting" report, so a completely
 * silent poll path is observable too. */
static volatile LONG g_pgraph_poll_calls;
static volatile LONG g_pgraph_pending_polls;
static volatile LONG g_pgraph_tls_skips;
static volatile LONG g_pgraph_interlock_skips;
static volatile LONG g_pgraph_no_vector;
static volatile LONG g_pgraph_pmc_blocked;
static volatile LONG g_pgraph_ctx_disabled;
static volatile LONG g_pgraph_en_disabled;
static volatile LONG g_pgraph_isr_dispatches;
static volatile LONG g_pgraph_isr_handled;
static volatile LONG g_pgraph_notify_acked;
static volatile LONG g_pgraph_last_ctx_a0;
static volatile LONG g_pgraph_last_ctx_b0;
static volatile LONG g_pgraph_last_intr_en;

void xbox_PgraphIrqReport(void)
{
    static LONG prev_poll, prev_pending, prev_tls, prev_interlock;
    static LONG prev_no_vector, prev_pmc_blocked, prev_ctx_disabled;
    static LONG prev_en_disabled, prev_dispatch;
    static LONG prev_handled, prev_acked;
    LONG poll, pending, tls, interlock, no_vector, pmc_blocked;
    LONG ctx_disabled, en_disabled, dispatch, handled, acked;

    if (!getenv("RECOMP_PGRAPH_ISR_TRACE")) return;

    poll        = InterlockedCompareExchange(&g_pgraph_poll_calls, 0, 0);
    pending     = InterlockedCompareExchange(&g_pgraph_pending_polls, 0, 0);
    tls         = InterlockedCompareExchange(&g_pgraph_tls_skips, 0, 0);
    interlock   = InterlockedCompareExchange(&g_pgraph_interlock_skips, 0, 0);
    no_vector   = InterlockedCompareExchange(&g_pgraph_no_vector, 0, 0);
    pmc_blocked = InterlockedCompareExchange(&g_pgraph_pmc_blocked, 0, 0);
    ctx_disabled = InterlockedCompareExchange(&g_pgraph_ctx_disabled, 0, 0);
    en_disabled = InterlockedCompareExchange(&g_pgraph_en_disabled, 0, 0);
    dispatch    = InterlockedCompareExchange(&g_pgraph_isr_dispatches, 0, 0);
    handled     = InterlockedCompareExchange(&g_pgraph_isr_handled, 0, 0);
    acked       = InterlockedCompareExchange(&g_pgraph_notify_acked, 0, 0);

    fprintf(stderr,
            "  [PGRAPH-ISR] +poll=%d +pending=%d +tls-skip=%d "
            "+interlock-skip=%d +no-vector=%d +pmc-blocked=%d "
            "+ctx-off=%d +en-off=%d +dispatch=%d +handled=%d +acked=%d "
            "(last ctx-a0=%08X ctx-b0=%08X intr-en=%08X; totals "
            "dispatch=%d acked=%d)\n",
            (int)(poll - prev_poll), (int)(pending - prev_pending),
            (int)(tls - prev_tls), (int)(interlock - prev_interlock),
            (int)(no_vector - prev_no_vector),
            (int)(pmc_blocked - prev_pmc_blocked),
            (int)(ctx_disabled - prev_ctx_disabled),
            (int)(en_disabled - prev_en_disabled),
            (int)(dispatch - prev_dispatch), (int)(handled - prev_handled),
            (int)(acked - prev_acked),
            InterlockedCompareExchange(&g_pgraph_last_ctx_a0, 0, 0),
            InterlockedCompareExchange(&g_pgraph_last_ctx_b0, 0, 0),
            InterlockedCompareExchange(&g_pgraph_last_intr_en, 0, 0),
            dispatch, acked);
    fflush(stderr);

    prev_poll = poll;
    prev_pending = pending;
    prev_tls = tls;
    prev_interlock = interlock;
    prev_no_vector = no_vector;
    prev_pmc_blocked = pmc_blocked;
    prev_ctx_disabled = ctx_disabled;
    prev_en_disabled = en_disabled;
    prev_dispatch = dispatch;
    prev_handled = handled;
    prev_acked = acked;
}

void xbox_VblankReport(void)
{
    DWORD now = GetTickCount();
    unsigned long ms = g_vblank_first_ms ? (unsigned long)(now - g_vblank_first_ms) : 0;
    /* Hz from DELIVERED, not raised: the guest's clock is how often its ISR
     * ran, not how often we asserted the line. Both are printed so a gap
     * between them is visible rather than hidden -- raised > delivered means
     * interrupts are being refused, which is a real condition worth seeing. */
    double hz = ms ? (double)g_vblank_delivered * 1000.0 / (double)ms : 0.0;
    fprintf(stderr, "  [VBLANK] delivered=%lu (raised=%lu retried=%lu) over %lu ms = %.1f Hz"
            " (target %d) deadlines=%lu unacked_skips=%lu max_gap=%lu ms"
            " not_ready=%lu retry_queued=%lu retry_delivered=%lu"
            " (irq_retry_pending %s)\n",
            g_vblank_delivered, g_vblank_raised, g_vblank_retry_delivered, ms, hz,
            1000000 / BRIDGE_VBLANK_PERIOD_US,
            g_vblank_deadlines, g_vblank_skipped_ack, g_vblank_max_gap_ms,
            g_interrupt_not_ready, g_irq_retry_queued, g_irq_retry_delivered,
            irq_retry_pending() ? "on" : "OFF");

    /* THE REGISTERS THEMSELVES, because the counters above cannot tell three
     * different faults apart and every one of them prints the same line.
     *
     * The hung session's last report read delivered=5355 (raised=5651
     * retried=33312) over 725405 ms, deadlines=43263, unacked_skips=37571,
     * not_ready=0 -- 43263 deadlines came due and 37571 were skipped because
     * xbox_Nv2aVblankPending was still true. The instruments were alive
     * through it: deadlines climbed at 60/s, retried at ~58/s, [IRQ-VEC] v3
     * went 16235 -> 140753 and KeInsertQueueDpc kept climbing at ~290/s, so
     * the poll was running and the guest ISR was still being entered. What
     * none of that says is what the guest ISR SAW when it ran. This line says
     * it, and the three readings do not overlap:
     *
     *   pcrtc=00000001 pmc=00000000   the PMC summary was LOST. The ISR runs
     *                                 every poll, reads bit 24 clear, does no
     *                                 work and acknowledges nothing, and
     *                                 nothing will ever re-OR the bit because
     *                                 the raise is gated on the source being
     *                                 clear. Latched for the rest of the run.
     *   pcrtc=00000001 pmc=01000000   the summary is INTACT and the fault is
     *                                 further along the guest's acknowledge
     *                                 path -- the ISR's own spin, its DPC, or
     *                                 the event it signals. The up-mirror
     *                                 below is then irrelevant and the
     *                                 counters beside it will say so.
     *   pcrtc=00000000                the reading of the retry branch is
     *                                 WRONG: unacked_skips cannot be climbing
     *                                 at 60/s with the source clear, so the
     *                                 freeze is somewhere else entirely.
     *
     * base= is this line's own positive control, and it is load-bearing.
     * g_nv2a_base is published by bridge_vblank_poll out of the guest's
     * KINTERRUPT ServiceContext and is zero until the title connects vector 3,
     * so base=00000000 means the two register fields were NOT READ and are
     * printed as zero by construction -- not that the registers are zero.
     * Expect FD000000; anything else means the ServiceContext base the ISR
     * uses is not the aperture, which would be a finding of its own.
     *
     * Read-only by construction: two BRIDGE_MEM32 loads and
     * xbox_Nv2aVblankPending, which is itself a single load
     * (xbox_memory_layout.c:2218). Nothing here assigns to a register, takes a
     * lock, or calls VirtualProtect, so it is safe on the guarded PCRTC page
     * and safe to leave in unconditionally. */
    {
        uint32_t reg_base  = g_nv2a_base;
        uint32_t reg_pcrtc = reg_base ? (uint32_t)BRIDGE_MEM32(reg_base + NV_PCRTC_INTR_0) : 0u;
        uint32_t reg_pmc   = reg_base ? (uint32_t)BRIDGE_MEM32(reg_base + NV_PMC_INTR_0)   : 0u;
        /* base_refused belongs on THIS line and nowhere else: base= is this
         * line's positive control, and a nonzero refusal count is the only
         * thing that distinguishes "the guest published the aperture" from
         * "the guest published something else and we declined to write
         * through it", both of which otherwise print base=00000000. */
        fprintf(stderr,
                "  [VBLANK-REG] base=%08X (refused=%ld last_bad=%08lX)"
                " pcrtc=%08X pmc=%08X pending=%d"
                " | summary lost=%ld episodes=%ld run=%ld max_run=%ld"
                " max_run_ms=%ld repaired=%ld upmirror=%s\n",
                reg_base,
                (long)InterlockedCompareExchange(&g_nv2a_base_refused, 0, 0),
                (unsigned long)(uint32_t)InterlockedCompareExchange(
                                             &g_nv2a_base_last_bad, 0, 0),
                reg_pcrtc, reg_pmc, xbox_Nv2aVblankPending(),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_lost, 0, 0),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_lost_episodes, 0, 0),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_lost_run, 0, 0),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_lost_run_max, 0, 0),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_lost_ms_max, 0, 0),
                (long)InterlockedCompareExchange(&g_nv2a_pmc_repaired, 0, 0),
                bridge_nv2a_pmc_upmirror() ? "on" : "off");
    }
    fflush(stderr);
}

/* One vblank period in whole milliseconds, carrying the remainder.
 *
 * Returns 17, 17, 17, 16, 17, 17, 17, 16 ... -- whatever sequence makes the
 * running average 16683 us. The carry is what makes the average right; any
 * single step is wrong by up to 317 us, which is far inside the poll jitter
 * this is delivered against anyway. */
static DWORD bridge_vblank_step_ms(void)
{
    static uint32_t carry_us;
    uint32_t step_us = carry_us + BRIDGE_VBLANK_PERIOD_US;
    carry_us = step_us % 1000u;
    return (DWORD)(step_us / 1000u);
}

static void bridge_vblank_poll(void)
{
    static DWORD next_vblank = 0;
    DWORD now;
    int i;
    int trace = bridge_isr_trace();
    int pending_at_entry = xbox_Nv2aSoftwareMethodPending();

    if (trace) {
        InterlockedIncrement(&g_pgraph_poll_calls);
        if (pending_at_entry) InterlockedIncrement(&g_pgraph_pending_polls);
    }

    /* Per-thread only. A thread inside a guest ISR or DPC must not start
     * another; other threads are unaffected, which is the change. Eligibility
     * for each individual vector is decided in bridge_deliver_isr. */
    if (g_dispatch_depth) {
        if (trace && pending_at_entry) InterlockedIncrement(&g_pgraph_tls_skips);
        return;
    }

    if (bridge_legacy_irq_interlock()) {
        if (InterlockedCompareExchange(&g_vblank_delivery_active, 1, 0) != 0) {
            InterlockedIncrement(&g_irq_legacy_hits);
            if (trace && pending_at_entry)
                InterlockedIncrement(&g_pgraph_interlock_skips);
            return;
        }
    }

    bridge_nv2a_mirror_intr();

    /* PGRAPH software methods share the GPU interrupt vector but are not
     * display refresh events. Deliver them on the guest thread that pumps
     * interrupts, using its saved register/stack context. */
    if (xbox_Nv2aSoftwareMethodPending()) {
        int vector_found = 0;
        int pmc_ready = 0;
        for (i = 0; i < BRIDGE_MAX_INTERRUPTS; ++i) {
            uint32_t iv = __atomic_load_n(&g_interrupts[i], __ATOMIC_ACQUIRE);
            if (!iv) break;
            if (!bridge_interrupt_ready(iv)) continue;
            if (BRIDGE_MEM32(iv + 8) != BRIDGE_NV2A_VECTOR) continue;
            vector_found = 1;
            uint32_t ctx = BRIDGE_MEM32(iv + 4);
            uint32_t base = ctx ? BRIDGE_MEM32(ctx) : 0;
            if (base && (BRIDGE_MEM32(base + NV_PMC_INTR_0) & 0x1000u)) {
                uint32_t handled;
                pmc_ready = 1;
                if (trace) {
                    LONG ctx_a0 = (LONG)BRIDGE_MEM32(ctx + 0xA0);
                    LONG ctx_b0 = (LONG)BRIDGE_MEM32(ctx + 0xB0);
                    LONG intr_en = (LONG)BRIDGE_MEM32(base + NV_PMC_INTR_EN_0);
                    InterlockedExchange(&g_pgraph_last_ctx_a0, ctx_a0);
                    InterlockedExchange(&g_pgraph_last_ctx_b0, ctx_b0);
                    InterlockedExchange(&g_pgraph_last_intr_en, intr_en);
                    if (!ctx_a0) InterlockedIncrement(&g_pgraph_ctx_disabled);
                    if (!intr_en) InterlockedIncrement(&g_pgraph_en_disabled);
                    InterlockedIncrement(&g_pgraph_isr_dispatches);
                }
                handled = bridge_deliver_isr(iv);
                if (trace && (handled & 0xFF))
                    InterlockedIncrement(&g_pgraph_isr_handled);
                if (trace && !xbox_Nv2aSoftwareMethodPending())
                    InterlockedIncrement(&g_pgraph_notify_acked);
            }
        }
        if (trace && !vector_found) InterlockedIncrement(&g_pgraph_no_vector);
        if (trace && vector_found && !pmc_ready)
            InterlockedIncrement(&g_pgraph_pmc_blocked);
    }

    now = GetTickCount();
    if (next_vblank == 0) {
        next_vblank = now + bridge_vblank_step_ms();
        goto done;
    }
    if ((int32_t)(now - next_vblank) < 0) goto done;
    next_vblank = now + bridge_vblank_step_ms();
    g_vblank_deadlines++;

    for (i = 0; i < BRIDGE_MAX_INTERRUPTS; i++) {
        uint32_t iv = __atomic_load_n(&g_interrupts[i], __ATOMIC_ACQUIRE);
        if (!iv) break;
        if (!bridge_interrupt_ready(iv)) continue;
        if (BRIDGE_MEM32(iv + 8) == BRIDGE_NV2A_VECTOR) {
            /* Raise the pending bit the way the GPU would before asserting the
             * line. JSRF's ISR reads its ServiceContext's register base and
             * only does the swap/signal work when NV_PMC_INTR_0 bit 24 is set;
             * with the register left at 0 it acknowledged the interrupt and
             * queued its DPC every frame but never completed a frame. The ISR
             * clears the bit itself as part of acknowledging. */
            uint32_t ctx  = BRIDGE_MEM32(iv + 4);
            uint32_t base = ctx ? BRIDGE_MEM32(ctx) : 0;
            if (!base) continue;
            /* The ONLY assignment to g_nv2a_base in this file, which is why
             * validating here is enough and bridge_nv2a_mirror_intr does not
             * repeat the check on its hot path: everything downstream reads a
             * value that either passed this test or is still zero, and zero is
             * already handled there. Refuse and count rather than proceed. */
            if (!bridge_nv2a_base_valid(base)) {
                InterlockedIncrement(&g_nv2a_base_refused);
                InterlockedExchange(&g_nv2a_base_last_bad, (LONG)base);
                continue;
            }
            g_nv2a_base = base;

            /* Do not re-raise while the previous vblank is still unacknowledged.
             *
             * The source stays asserted until the driver acks it, and the
             * hardware does not assert it again underneath a handler that is
             * still spinning waiting for the summary to clear. Raising every
             * 16ms regardless fought the acknowledge and cut delivery from
             * 3380 per 50s to 8.
             *
             * Gating on NV_PMC_INTR_EN_0 instead looks equally principled and
             * is WRONG here: the ISR masks it and its DPC restores it from
             * [ctx+0xb0], which this runtime never establishes, so that gate
             * latches shut after the first delivery and nothing is delivered
             * at all. Measured both ways. */
            /* RAISE and SERVICE are different actions and this used to
             * `continue` past both.
             *
             * The raise is gated because raising underneath an unacknowledged
             * vblank fights the acknowledge -- measured, it cut delivery from
             * 3380 per 50 s to 8. But the SERVICE was gated by the same test,
             * and once bridge_deliver_isr gained the ability to refuse (IRQL,
             * reentry, vector in service) that became a trap: a refused
             * delivery leaves the vblank pending, and every later poll took
             * this branch and skipped the retry. The interrupt was stranded
             * until something else happened to acknowledge it, and the
             * per-vector pending array did not rescue it -- nothing consumed
             * that array here.
             *
             * A level-triggered source that is still asserted is exactly what
             * SHOULD be re-offered to the handler, so: skip the raise, keep
             * the service. */
            if (xbox_Nv2aVblankPending()) {
                int retried = 0;
                g_vblank_skipped_ack++;
                bridge_deliver_isr_ex(iv, &retried);
                if (retried) g_vblank_retry_delivered++;
                continue;
            }

            {
                DWORD t = GetTickCount();
                if (g_vblank_first_ms == 0) g_vblank_first_ms = t;
                if (g_vblank_last_ms) {
                    unsigned long gap = (unsigned long)(t - g_vblank_last_ms);
                    if (gap > g_vblank_max_gap_ms) g_vblank_max_gap_ms = gap;
                }
                g_vblank_last_ms = t;
                g_vblank_raised++;
            }
            xbox_Nv2aRaiseVblank();
            /* Raising the source and running the guest's ISR are two different
             * events and this used to count only the first while calling it
             * "delivered". That was always loose -- the ISR could decline --
             * and it became wrong when bridge_deliver_isr gained the ability to
             * REFUSE outright (reentry, IRQL, vector already in service), at
             * which point a refused interrupt still incremented the counter and
             * still fed the Hz figure. Every "62.3 Hz, the guest's clock is
             * fine" claim in the handovers came from this number.
             *
             * g_irq_delivered is incremented in exactly one place, after the
             * guest ISR returns, so differencing it across the call is the
             * honest test of whether anything actually ran. */
            {
                int ran = 0;
                bridge_deliver_isr_ex(iv, &ran);
                if (ran) g_vblank_delivered++;
            }
        }
    }

done:
    if (bridge_legacy_irq_interlock())
        InterlockedExchange(&g_vblank_delivery_active, 0);
}

/* Device interrupts other than the GPU.
 *
 * KeConnectInterrupt records every interrupt a title connects, and JSRF
 * connects four: vector 3 the NV2A, vector 1 USB (routine 0x001C288F, inside
 * the XPP peripheral library), vector 6 ACI and vector 5 the APU. Only the
 * NV2A was ever delivered -- bridge_vblank_poll filters on its vector -- so
 * the other three were registered and starved.
 *
 * That is a plausible reason a title boots, initialises its GPU and then ticks
 * forever without drawing: with no USB interrupt the controller is never
 * enumerated, and the ordinal histogram shows JSRF making no input-related
 * kernel calls at all. A device that never announces itself is
 * indistinguishable from one that was never plugged in.
 *
 * These are delivered on a plain timer rather than off a modelled device.
 * Nothing here models an OHCI controller, so an ISR that polls its status
 * registers should find nothing pending and decline -- which is the honest
 * outcome, and is visible in the log rather than being a fabricated event.
 * Finding out which of those two happens is the point of raising them.
 */
#define BRIDGE_DEVICE_IRQ_PERIOD_MS 8

static void bridge_device_irq_poll(void)
{
    static DWORD next_irq = 0;
    DWORD now;
    int i;

    /* Per-thread only. This pump used to share the NV2A global interlock on the
     * reasoning that "a guest ISR is not re-entrant" -- true, but that is a
     * property of ONE vector, not of the process, and paying for it globally is
     * what let an audio DPC stop USB interrupts. Per-vector non-reentrancy is
     * enforced in bridge_deliver_isr instead. */
    if (g_dispatch_depth) return;

    if (bridge_legacy_irq_interlock()) {
        if (InterlockedCompareExchange(&g_vblank_delivery_active, 1, 0) != 0) {
            InterlockedIncrement(&g_irq_legacy_hits);
            return;
        }
    }

    now = GetTickCount();
    if (next_irq == 0) {
        next_irq = now + BRIDGE_DEVICE_IRQ_PERIOD_MS;
        goto done;
    }
    if ((int32_t)(now - next_irq) < 0) goto done;
    next_irq = now + BRIDGE_DEVICE_IRQ_PERIOD_MS;

    for (i = 0; i < BRIDGE_MAX_INTERRUPTS; i++) {
        uint32_t iv = __atomic_load_n(&g_interrupts[i], __ATOMIC_ACQUIRE);
        uint32_t vector;
        uint32_t result;

        if (!iv) break;
        if (!bridge_interrupt_ready(iv)) continue;
        vector = BRIDGE_MEM32(iv + 8);
        if (vector == BRIDGE_NV2A_VECTOR) {
            continue;   /* the GPU has its own handshake; see bridge_vblank_poll */
        }

        result = bridge_deliver_isr(iv);

        /* Logged per vector, because one chatty device would otherwise spend a
         * shared budget and hide the others. A run of FALSE means the routine
         * declines at one of its gates, which is what to expect while nothing
         * models the device -- worth seeing rather than assuming. */
        {
            static int logged[BRIDGE_MAX_INTERRUPTS];
            if (logged[i] < 3) {
                logged[i]++;
                fprintf(stderr,
                        "  [KERNEL] device ISR vector %u routine 0x%08X -> %s\n",
                        vector, BRIDGE_MEM32(iv + 0),
                        (result & 0xFF) ? "TRUE (handled)" : "FALSE (declined)");
                fflush(stderr);
            }
        }
    }

done:
    if (bridge_legacy_irq_interlock())
        InterlockedExchange(&g_vblank_delivery_active, 0);
}

/* RECOMP_IRQ_THREAD -- deliver device interrupts from a host thread.
 *
 * The three pumps above have exactly one caller: the guest's blocking wait, in
 * bridge_wait_for_object. That is sufficient for a guest that WAITS, and
 * nothing whatsoever for a guest that SPINS -- and JSRF's audio bring-up
 * spins. Measured on the Windows build: the title writes its APU configuration,
 * connects the DSOUND ISR on vector 6, then polls in user code with no kernel
 * call and no register access for as long as it is left running. No wait, no
 * pump, no interrupt, and the completion it is polling for can never arrive.
 *
 * Hardware does not work that way -- an interrupt arrives when the device
 * decides, not when the driver next blocks. This runs the timer and non-GPU
 * device pumps on their own thread, which is exactly what ohci_thread already
 * does for the USB controller, and for the same stated reason: the guest
 * register file is thread-local, so anything calling recompiled code needs a
 * thread of its own.
 *
 * The GPU is deliberately different. Its ISR masks NV_PMC_INTR_EN_0 and queues
 * a DPC which restores it. On Windows, running that translated pair on this
 * synthetic guest stack eventually lost the restore and left the pusher
 * blocked forever on a software-method notify. The macOS path already avoids
 * that failure: bridge_KeWaitForSingleObject pumps bridge_vblank_poll on the
 * real waiting guest thread, preserving the interrupted guest's scheduling
 * context. Windows completed thousands of the same ISR/DPC handoffs when run
 * that way. Keep RECOMP_IRQ_THREAD_GPU as a diagnostic escape hatch, not the
 * normal RECOMP_IRQ_THREAD behaviour.
 *
 * The pumps are re-entrancy guarded already (g_vblank_delivery_active is a
 * process-wide interlock shared by all three), so this thread and a blocked
 * guest thread cannot deliver at once; whichever loses the CAS simply returns.
 *
 * IT RACED KeConnectInterrupt, and now does not. The slot is published with a
 * release store and every pump acquire-loads it and puts it through
 * bridge_interrupt_ready before reading any other field of the KINTERRUPT --
 * see the validator's own comment for the mechanism. g_interrupt_not_ready
 * counts what that rejects and is reported as not_ready= on the [VBLANK] line;
 * on macOS, where no reader ever observed a part-built entry, it should stay at
 * zero, which is the control that says the counter is measuring the race and
 * not something ordinary.
 *
 * That crash is STILL NOT EXPLAINED, and this fix should not be read as
 * explaining it. The Windows push-buffer ring bounds (0x1000-0x81000) overlap
 * the title's own .text (0x11000-0x18CB30) by 448 KB, and a guest scribbling
 * command words over its own code yields garbage routine pointers just as well
 * -- the validator would reject those too, and silently. Checksum a .text page
 * before believing either account.
 *
 * OPT-IN, DEFAULT OFF, deliberately. It changes interrupt timing on a build
 * that currently reaches gameplay, and the re-entrancy here has been got wrong
 * twice before -- see the two rejected gates documented in bridge_vblank_poll,
 * one of which cut delivery from 3380 per 50s to 8. Prove it on the host that
 * needs it before it becomes the default anywhere.
 */
static DWORD WINAPI bridge_irq_thread(LPVOID unused)
{
    int slot;

    (void)unused;
    /* bridge_run_isr pushes the ISR's arguments onto g_esp -- it does not
     * establish a stack, because every existing caller already had the guest's
     * own. A host thread has none, so it borrows a worker slice.
     *
     * The pool is XBOX_WORKER_STACK_COUNT slices and that is 0 by default: a
     * main-loop title never needed one, and reserving 256 KB apiece for a pool
     * nothing draws from is pure waste. So this thread needs a build with the
     * pool compiled in, and says so rather than silently delivering nothing.
     *
     * g_fs_base is left at its TLS initialiser (XBOX_PRIMARY_TIB_VA), which is
     * enough for fs:[0]/fs:[4] to resolve. Sharing the primary TIB with the
     * main thread is a real hazard if a guest ISR and the main thread use SEH
     * at the same time; it is acceptable only because this is opt-in and the
     * delivery interlock already serialises the ISRs themselves. */
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [IRQ-THREAD] no worker stack slice"
                        " (XBOX_WORKER_STACK_COUNT=%d); not delivering\n",
                (int)XBOX_WORKER_STACK_COUNT);
        fflush(stderr);
        return 0;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);

    fprintf(stderr, "  [IRQ-THREAD] live; device interrupts no longer depend"
                    " on the guest blocking\n");
    /* Resolved once. Every other switch in this file caches with a
     * `static int on = -1`; these two were calling getenv twice per
     * millisecond for the life of the process, which walks the environment
     * each time -- on the thread whose whole job is to be punctual. */
    static int irq_thread_gpu = -1;
    if (irq_thread_gpu < 0)
        irq_thread_gpu = recomp_switch_on("RECOMP_IRQ_THREAD_GPU");
    if (irq_thread_gpu) {
        fprintf(stderr, "  [IRQ-THREAD] synthetic GPU delivery enabled"
                        " (diagnostic; may break the ISR/DPC handoff)\n");
    } else {
        fprintf(stderr, "  [IRQ-THREAD] GPU delivery remains on waiting guest"
                        " threads\n");
    }
    fflush(stderr);

    for (;;) {
        Sleep(1);
        bridge_timers_poll();
        if (irq_thread_gpu)
            bridge_vblank_poll();
        bridge_device_irq_poll();
    }
}

static void bridge_irq_thread_start(void)
{
    static int started;
    HANDLE th;

    if (started || !getenv("RECOMP_IRQ_THREAD"))
        return;
    started = 1;
    th = CreateThread(NULL, 0, bridge_irq_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        fprintf(stderr, "  [IRQ-THREAD] CreateThread failed\n");
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 *
 * Initializes a timer object. Xbox KTIMER is 40 bytes.
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = bridge_checked_out_va(STACK_ARG(0), 40,
                                              "KeInitializeTimerEx", "Timer");
    uint32_t type = STACK_ARG(1);

    if (!timer_va) {
        g_eax = 0;
        return;
    }

    /* Zero the structure (40 bytes) */
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Set Type (0x08 = TimerNotificationObject, 0x09 = TimerSynchronizationObject) */
    BRIDGE_MEM16(timer_va + 0) = (uint16_t)(0x08 + (type & 1));
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 *
 * Sets a timer. We don't actually start timers - just record the state.
 * Returns FALSE (timer was not already set).
 */
/* ── Timer / DPC delivery ─────────────────────────────────
 *
 * KeSetTimer used to be a stub that recorded nothing and scheduled nothing,
 * which is why gap-analysis.md's "Timers and DPCs | DONE" was wrong.
 *
 * It matters because a driver's periodic work runs from a timer DPC. JSRF's
 * DSOUND arms one (KeSetTimer(Timer=obj+0x720, DueTime, Dpc=obj+0x748)) to
 * pump its APU command ring: the ring's doorbell at +0x810 is written by the
 * caller and cleared by that pump. With the timer inert the pump never ran,
 * the doorbell never cleared, and the title's main thread spun on it forever
 * while four worker threads spun waiting on the main thread.
 *
 * Delivery model: a DPC runs at DISPATCH_LEVEL in whatever thread context the
 * scheduler happens to be in, so running it from the kernel thunk dispatch --
 * the choke point every guest kernel call already passes through, and where
 * the suspend safe point already lives -- is a fair model and needs no thread
 * of its own. A guest thread that makes no kernel calls will not deliver, the
 * same caveat the suspend safe point carries.
 *
 * A DPC routine can itself call the kernel, so delivery is guarded against
 * re-entering itself on the same thread.
 */
#define BRIDGE_MAX_TIMERS 32

/* THE TABLE IS POLLED LOCK-FREE BY EVERY THREAD IN THE PROCESS, so it is
 * published as a seqlock, not as four plain stores.
 *
 * Who reads it: kernel_thunk_dispatch polls it on EVERY kernel dispatch
 * (~6.28M/s across all guest threads), bridge_KeWaitForSingleObject polls it
 * from every blocked waiter, and bridge_irq_thread polls it once a
 * millisecond. Who writes it: any guest thread calling KeSetTimer.
 *
 * WHAT WAS WRONG. bridge_arm_timer wrote timer_va -- the ARMED FLAG, the one
 * field every poller tests first -- BEFORE the deadline, with a GetTickCount()
 * call between them, so the window was a whole function call wide and visible
 * in the compiled binary rather than a theoretical store reordering. A poller
 * landing in it saw a slot marked armed carrying the PREVIOUS occupant's
 * deadline, which is in the past, and ran the DPC immediately instead of after
 * the requested delay. Two more races rode along: the free-slot scan was an
 * unsynchronised read-then-write, so two threads could pick the same slot; and
 * two pollers could both pass the due test on one slot and run the same DPC
 * concurrently, which is exactly what DPC delivery is supposed to serialise.
 *
 * WHAT THIS MAKES ATOMIC, PRECISELY:
 *   - `owner` is the slot allocation, and it only ever changes by CAS. Two
 *     threads cannot claim one slot.
 *   - `gen` is a seqlock generation: ODD while bridge_arm_timer is rewriting
 *     the payload, EVEN when it is stable. A poller reads it before and after
 *     the payload and discards the pass if it was odd or if it moved, so a
 *     poller can no longer act on half an arming. This is what closes the
 *     publish-order window, and it closes it in BOTH directions -- the
 *     release/acquire pair also stops the compiler and the machine reordering
 *     the payload past the flag.
 *   - `armed` is the firing claim: the poller takes it with an atomic
 *     exchange to 0, and EXACTLY ONE thread can observe the old value 1. That
 *     thread runs the DPC; the rest count a loss and move on.
 *
 * WHAT IT DOES NOT MAKE ATOMIC. The DPC body itself is not serialised against
 * anything but a second firing of the SAME slot -- two different timers still
 * run their DPCs concurrently, as they did before, and bridge_run_dpc's own
 * g_in_dpc guard is what keeps them off one thread. A KeCancelTimer that lands
 * after a poller has won the exchange but before the DPC returns does not
 * unrun it; that hole predates this and cannot be closed without a lock on the
 * hot path. The hot path stays lock-free: a poll of an idle slot is one
 * relaxed load of `armed`, unchanged from before.
 *
 * MEASUREMENT, and why the periodic branch is the quiet one: across 765
 * archived run logs under build-macos carrying a `[KERNEL] ordinals used`
 * histogram, ordinal 149 (KeSetTimer) appears in every one of them, peaking at
 * 4410 in a run, and ordinal 150 (KeSetTimerEx) appears in NONE. Every timer
 * JSRF arms is therefore a one-shot -- period_ms 0, the branch that releases
 * the slot -- so the periodic re-arm below is correct-by-construction code
 * this title never executes, and no reading of these counters should be
 * attributed to it. */
static struct {
    volatile uint32_t owner;     /* slot allocation: guest KTIMER VA, 0 free */
    volatile uint32_t gen;       /* seqlock: odd = payload in flux           */
    volatile uint32_t armed;     /* 1 = due-testable; the firing claim       */
    volatile uint32_t dpc_va;    /* guest KDPC, 0 = no DPC to run            */
    volatile uint32_t period_ms; /* 0 = one-shot                             */
    volatile uint32_t deadline;  /* GetTickCount() value it becomes due at   */
    /* The two fields below exist ONLY to instrument the race this seqlock
     * closes, and they are deliberately redundant with `deadline`: a
     * consistent snapshot always has deadline == armed_at + delay_ms, so a
     * firing whose measured age is less than the delay the guest asked for is
     * a snapshot that mixed one arming's deadline with another's. That is a
     * different witness to the same fault as `gen` moving, taken from fields
     * the poller does not otherwise need -- so it still fires if a later
     * change writes a payload field outside the gen bracket. */
    volatile uint32_t armed_at;  /* GetTickCount() at the arming             */
    volatile uint32_t delay_ms;  /* what the guest asked for                 */
} g_timers[BRIDGE_MAX_TIMERS];

/* Timer-table counters, printed beside the ordinal histogram by
 * xbox_bridge_dump_ordinal_histogram so the positive control is on the
 * adjacent line rather than in a different report.
 *
 * THE POSITIVE CONTROL IS AN IDENTITY, NOT A VIBE: bridge_KeSetTimer and
 * bridge_KeSetTimerEx call bridge_arm_timer unconditionally, and the only way
 * out of it without arming is the table-full refusal, so
 *
 *     armed + full  ==  (histogram 149=) + (histogram 150=)
 *
 * and 150 never appears, so `armed + full` must equal the 149= entry on the
 * line above. If it does not, these counters are lying and nothing read off
 * them counts. That matters most for `early`, `torn` and `lost`, which are
 * ABSENCE measurements: `early=0` is only evidence that the race is gone if
 * `armed` matches the histogram and `fired` is nonzero in the same block.
 * Otherwise `early=0` means the instrument never ran. */
static volatile LONG g_timers_armed;      /* arms published                  */
static volatile LONG g_timers_full;       /* arms refused, table full        */
static volatile LONG g_timers_fired;      /* firings claimed (DPCs run)      */
static volatile LONG g_timers_lost;       /* pollers that lost the exchange  */
static volatile LONG g_timers_torn;       /* passes discarded mid-publish    */
static volatile LONG g_timers_claim_lost; /* CAS collisions claiming a slot  */
static volatile LONG g_timers_early;      /* fired before its own delay      */

/* g_in_dpc is defined near the vblank ISR path above; a second TLS
 * definition here is a redefinition under GCC. */

/* Call a guest KDPC's DeferredRoutine:
 *   VOID Routine(PKDPC Dpc, PVOID Ctx, PVOID Sys1, PVOID Sys2)  __stdcall
 * bridge_KeInitializeDpc stores the routine at +12 and the context at +16.
 * Same shape as bridge_NtUserIoApcDispatcher: push right-to-left plus the
 * dummy return address the callee's `ret 16` consumes. */
static void bridge_run_dpc(uint32_t dpc_va, uint32_t sys1, uint32_t sys2)
{
    uint32_t routine, context;
    recomp_func_t fn;

    if (!dpc_va) {
        return;
    }
    routine = BRIDGE_MEM32(dpc_va + 12);
    context = BRIDGE_MEM32(dpc_va + 16);
    if (!routine) {
        return;
    }
    fn = recomp_lookup(routine);
    if (!fn) fn = recomp_lookup_manual(routine);
    if (!fn) {
        static uint32_t warned_routine = 0;
        if (warned_routine != routine) {
            warned_routine = routine;
            fprintf(stderr, "  [KERNEL] DPC routine 0x%08X not in dispatch\n",
                    routine);
            fflush(stderr);
        }
        return;
    }

    {
        static int dpc_logged = 0;
        if (dpc_logged < 6) {
            dpc_logged++;
            fprintf(stderr,
                    "  [KERNEL] DPC dpc_va=0x%08X routine=0x%08X "
                    "context=0x%08X ctx[0xB0]=0x%08X\n",
                    dpc_va, routine, context,
                    context ? BRIDGE_MEM32(context + 0xB0) : 0);
            fflush(stderr);
        }
    }

    {
        BridgeGuestRegs saved;
        bridge_save_regs(&saved);
        /* Raise the per-thread recursion depth around every DPC body, not just
         * the ones reached from an ISR. A timer DPC runs here with no ISR above
         * it, and guest code inside it can call back into the kernel and reach
         * a pump; without this that pump would see depth 0 and start a second
         * guest interrupt on a thread already running guest interrupt code. */
        g_dispatch_depth++;
        g_in_dpc = 1;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = sys2;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = sys1;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        fn();
        g_in_dpc = 0;
        g_dispatch_depth--;
        bridge_restore_regs(&saved);
    }
}

/* Run any timer whose deadline has passed. Called from the thunk dispatch. */
static void bridge_timers_poll(void)
{
    DWORD now;
    int i;

    if (g_in_dpc || g_in_isr) {
        /* A timer expires at DPC level; it cannot pre-empt an ISR.  Besides
         * being the Xbox ordering rule, this is required by the bridge's ISR
         * hand-off: JSRF masks NV_PMC_INTR_EN_0, then calls KeInsertQueueDpc
         * to arrange the callback that restores it.  kernel_thunk_dispatch
         * polls timers before entering that bridge.  Letting a timer DPC run
         * there can strand the ISR inside an unrelated callback after it has
         * masked the GPU interrupt but before KeInsertQueueDpc records the
         * restore DPC. */
        return;
    }
    now = GetTickCount();
    for (i = 0; i < BRIDGE_MAX_TIMERS; i++) {
        uint32_t dpc_va, period_ms, deadline, armed_at, delay_ms, owner;
        uint32_t gen0, gen1;

        /* The cheap reject first: an idle slot costs one relaxed load, which
         * is what it cost before this became a seqlock. Acquire, so everything
         * bridge_arm_timer published before the flag is visible if it is 1. */
        if (!__atomic_load_n(&g_timers[i].armed, __ATOMIC_ACQUIRE)) {
            continue;
        }
        /* Seqlock read. Odd means bridge_arm_timer is mid-publish; a moved
         * generation means it finished one while we were reading. Either way
         * this snapshot may pair one arming's deadline with another's flag --
         * which is precisely the bug: the old code had no way to tell, so it
         * fired the DPC against a stale, already-past deadline. Skipping costs
         * nothing, because the next poll is microseconds away on the dispatch
         * path. */
        gen0 = __atomic_load_n(&g_timers[i].gen, __ATOMIC_ACQUIRE);
        if (gen0 & 1u) {
            InterlockedIncrement(&g_timers_torn);
            continue;
        }
        /* Relaxed atomic loads rather than plain ones: the payload is written
         * by another thread, so plain accesses here would be a data race in
         * the language even though the fences make them correct on the
         * machine. Relaxed compiles to the same single load on both hosts. */
        deadline  = __atomic_load_n(&g_timers[i].deadline,  __ATOMIC_RELAXED);
        dpc_va    = __atomic_load_n(&g_timers[i].dpc_va,    __ATOMIC_RELAXED);
        period_ms = __atomic_load_n(&g_timers[i].period_ms, __ATOMIC_RELAXED);
        armed_at  = __atomic_load_n(&g_timers[i].armed_at,  __ATOMIC_RELAXED);
        delay_ms  = __atomic_load_n(&g_timers[i].delay_ms,  __ATOMIC_RELAXED);
        owner     = __atomic_load_n(&g_timers[i].owner,     __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        gen1 = __atomic_load_n(&g_timers[i].gen, __ATOMIC_ACQUIRE);
        if (gen1 != gen0) {
            InterlockedIncrement(&g_timers_torn);
            continue;
        }

        /* Wrap-safe compare: signed difference, not `now >= deadline`. */
        if ((int32_t)(now - deadline) < 0) {
            continue;
        }

        /* Claim the firing. Exactly one thread can see the old value 1, so
         * exactly one runs this DPC -- every other poller that passed the same
         * due test on the same slot lands here and counts a loss. Before this,
         * all of them ran it, concurrently. */
        if (__atomic_exchange_n(&g_timers[i].armed, 0u, __ATOMIC_ACQ_REL) == 0u) {
            InterlockedIncrement(&g_timers_lost);
            continue;
        }

        /* Did it fire before the delay the guest asked for? See the comment on
         * armed_at/delay_ms: from a consistent snapshot this cannot happen, so
         * a nonzero reading names a publish-order fault directly. */
        if ((int32_t)(now - armed_at) < (int32_t)delay_ms) {
            InterlockedIncrement(&g_timers_early);
        }

        if (period_ms) {
            /* Periodic: re-publish through the seqlock, same order as an arm.
             * JSRF never reaches this -- ordinal 150 appears in none of the
             * 765 archived histograms -- so it is correctness, not a path any
             * reading of these counters comes from.
             *
             * `gen` is re-read rather than derived from gen0, because a
             * KeSetTimer on this same slot may have moved it since. That still
             * is not mutual exclusion and is not claimed to be: gen is an
             * ORDERING and DETECTION device, not a lock, so two writers on one
             * slot can leave it at a value a reader disagrees with. The worst
             * that costs is a poller counting `torn` and skipping a pass,
             * microseconds before it polls again -- it can never produce the
             * failure this patch exists to remove, which is a poller acting on
             * a deadline from a DIFFERENT arming. */
            uint32_t g = __atomic_load_n(&g_timers[i].gen, __ATOMIC_RELAXED);
            __atomic_store_n(&g_timers[i].gen, g + 1u, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_RELEASE);
            __atomic_store_n(&g_timers[i].deadline, (uint32_t)now + period_ms,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_timers[i].armed_at, (uint32_t)now,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_timers[i].delay_ms, period_ms,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&g_timers[i].gen, g + 2u, __ATOMIC_RELEASE);
            __atomic_store_n(&g_timers[i].armed, 1u, __ATOMIC_RELEASE);
        } else {
            /* One-shot: already disarmed by the exchange above. Release the
             * slot as well, so BRIDGE_MAX_TIMERS is a working-set limit and
             * not a lifetime one -- which is what the old `timer_va = 0` did,
             * since that field was both the flag and the allocation. CAS, not
             * a store, so releasing cannot stamp on an owner some other thread
             * has already claimed. */
            uint32_t expect = owner;
            if (expect)
                __atomic_compare_exchange_n(&g_timers[i].owner, &expect, 0u,
                                            0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
        }
        InterlockedIncrement(&g_timers_fired);
        /* The guest's KTIMER is a dispatcher object a wait can be built on;
         * mark it signalled the way KeSetEvent would. */
        bridge_run_dpc(dpc_va, 0, 0);
    }
}

/* Arm a timer. due_lo/due_hi are a LARGE_INTEGER in 100ns units: negative is
 * relative to now, positive is an absolute system time. Only the relative form
 * is modelled -- an absolute deadline needs a system clock epoch this runtime
 * does not carry -- and an absolute request is armed as "due now" and said so
 * once, rather than being dropped silently. */
static void bridge_arm_timer(uint32_t timer_va, uint32_t due_lo,
                             uint32_t due_hi, uint32_t period_ms,
                             uint32_t dpc_va)
{
    int64_t due = (int64_t)(((uint64_t)due_hi << 32) | due_lo);
    uint32_t delay_ms;
    int i, slot = -1;

    if (due < 0) {
        delay_ms = (uint32_t)((-due) / 10000);   /* 100ns -> ms */
    } else {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "  [KERNEL] KeSetTimer: absolute due time is not "
                    "modelled; arming as due now\n");
            fflush(stderr);
        }
        delay_ms = 0;
    }

    /* Already ours? Re-arm in place. This scan is a read of an atomically
     * written word, so it cannot see a half-claimed slot. */
    for (i = 0; i < BRIDGE_MAX_TIMERS; i++) {
        if (__atomic_load_n(&g_timers[i].owner, __ATOMIC_ACQUIRE) == timer_va) {
            slot = i;
            break;
        }
    }
    /* Otherwise claim a free one by CAS. The old code read the slot and then
     * wrote it, so two guest threads arming at once could both see slot N free
     * and both take it -- the loser's timer then silently became the winner's.
     * A failed CAS here means another thread got in first; retry the next
     * slot, and count it, because a nonzero reading is direct evidence that
     * guest threads really do arm concurrently. */
    if (slot < 0) {
        for (i = 0; i < BRIDGE_MAX_TIMERS; i++) {
            uint32_t expect = 0;
            if (__atomic_compare_exchange_n(&g_timers[i].owner, &expect,
                                            timer_va, 0, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                slot = i;
                break;
            }
            if (expect != 0 && expect != timer_va)
                InterlockedIncrement(&g_timers_claim_lost);
        }
    }
    if (slot < 0) {
        InterlockedIncrement(&g_timers_full);
        fprintf(stderr, "  [KERNEL] KeSetTimer: timer table full (%d)\n",
                BRIDGE_MAX_TIMERS);
        fflush(stderr);
        return;
    }

    /* PUBLISH IN THE ORDER A READER CAN SURVIVE.
     *
     * This used to be four plain stores with timer_va -- the armed flag -- at
     * the TOP and the deadline at the BOTTOM, with a GetTickCount() call
     * between them. Every poller tests the flag first, so for the width of
     * that call the table advertised an armed timer carrying the previous
     * occupant's deadline, which is in the past. The DPC fired immediately
     * instead of after the requested delay.
     *
     * Now: drop `armed` first so nothing can act on the slot at all, take the
     * generation odd, write the payload, take it even with a release, and only
     * then re-publish `armed` with a release. A poller either sees armed == 0
     * and skips, or sees the whole payload that the release ordered before it.
     * The one case that is neither -- a poller already past its acquire load
     * when we begin -- is caught by the generation check on its way out.
     *
     * GetTickCount() is called ONCE and before the bracket, so the window the
     * bug lived in is not merely ordered, it is gone: nothing slow happens
     * between the odd and the even. */
    __atomic_store_n(&g_timers[slot].armed, 0u, __ATOMIC_RELAXED);
    {
        DWORD    now = GetTickCount();
        uint32_t gen = __atomic_load_n(&g_timers[slot].gen, __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].gen, gen + 1u, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&g_timers[slot].dpc_va,    dpc_va,    __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].period_ms, period_ms, __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].deadline,
                         (uint32_t)now + delay_ms, __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].armed_at,
                         (uint32_t)now, __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].delay_ms,  delay_ms,  __ATOMIC_RELAXED);
        __atomic_store_n(&g_timers[slot].gen, gen + 2u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_timers[slot].armed, 1u, __ATOMIC_RELEASE);
    }
    InterlockedIncrement(&g_timers_armed);
}

static int bridge_disarm_timer(uint32_t timer_va)
{
    int i;
    for (i = 0; i < BRIDGE_MAX_TIMERS; i++) {
        uint32_t expect;
        if (__atomic_load_n(&g_timers[i].owner, __ATOMIC_ACQUIRE) != timer_va)
            continue;
        /* Take the armed flag the same way a firing poller does, so cancelling
         * and firing cannot both happen: whoever gets the 1 owns the outcome.
         * The return value is the guest-visible BOOLEAN "it was already set",
         * which is now the truth rather than "a slot still names it". */
        {
            uint32_t was = __atomic_exchange_n(&g_timers[i].armed, 0u,
                                               __ATOMIC_ACQ_REL);
            /* Release the slot, as the one-shot path does and for the same
             * reason: the old code's single timer_va field freed the slot as a
             * side effect of clearing the flag, and 32 slots have to be
             * reusable across the ~2500 KeSetTimer calls a run makes. */
            expect = timer_va;
            __atomic_compare_exchange_n(&g_timers[i].owner, &expect, 0u, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            return was != 0u;
        }
    }
    return 0;
}

/* BOOLEAN KeSetTimer(PKTIMER, LARGE_INTEGER DueTime, PKDPC) */
static void bridge_KeSetTimer(void)
{
    uint32_t timer_va = STACK_ARG(0);
    int was_set = bridge_disarm_timer(timer_va);
    bridge_arm_timer(timer_va, STACK_ARG(1), STACK_ARG(2), 0, STACK_ARG(3));
    g_eax = (uint32_t)was_set;
}

/* BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER DueTime, LONG Period, PKDPC) */
static void bridge_KeSetTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    int was_set = bridge_disarm_timer(timer_va);
    bridge_arm_timer(timer_va, STACK_ARG(1), STACK_ARG(2),
                     STACK_ARG(3), STACK_ARG(4));
    g_eax = (uint32_t)was_set;
}

/* BOOLEAN KeCancelTimer(PKTIMER)
 *
 * Supersedes the wrapper that forwarded to xbox_KeCancelTimer: that one
 * cancelled a timer in the NATIVE kernel HLE, which is a different object from
 * the guest KTIMER this bridge arms. Cancelling has to happen in the table the
 * arming went into. */
static void bridge_KeCancelTimer(void)
{
    g_eax = (uint32_t)bridge_disarm_timer(STACK_ARG(0));
}

/* BOOLEAN KeInsertQueueDpc(PKDPC, PVOID SystemArgument1, PVOID SystemArgument2)
 *
 * Xbox queues the DPC to run later at DISPATCH_LEVEL; this runs it inline.
 * That is a real ordering difference -- on hardware an ISR finishes before its
 * DPC does -- and it is the same simplification xbox_KeInsertQueueDpc already
 * falls back to. Deferring it properly needs a queue drained at the same safe
 * point, which is worth doing if a title turns out to care about the ordering. */
static void bridge_KeInsertQueueDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    if (g_in_isr) {
        /* Queued from an ISR: defer it until the ISR returns, which is the
         * whole point of a DPC. Running it here would re-enter guest code from
         * inside the interrupt. */
        g_pending_dpc = dpc_va;
        g_pending_dpc_sys1 = STACK_ARG(1);
        g_pending_dpc_sys2 = STACK_ARG(2);
        if (getenv("RECOMP_PGRAPH_ISR_TRACE") && g_current_isr_handoff) {
            fprintf(stderr,
                    "  [ISR-HANDOFF] #%d queue-dpc dpc=%08X sys1=%08X sys2=%08X\n",
                    (int)g_current_isr_handoff, dpc_va,
                    g_pending_dpc_sys1, g_pending_dpc_sys2);
            fflush(stderr);
        }
        g_eax = 1;
        return;
    }
    if (g_in_dpc) {
        g_eax = 0;   /* already inside one; refuse rather than recurse */
        return;
    }
    bridge_run_dpc(dpc_va, STACK_ARG(1), STACK_ARG(2));
    g_eax = 1;
}

/* ── KeSynchronizeExecution (ordinal 153) ───────────────
 *
 * BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 *
 * DELIBERATELY NOT ROUTED TO xbox_KeSynchronizeExecution. That function
 * (kernel_sync.c:553) casts its PVOID straight to a host function pointer and
 * calls it. SynchronizeRoutine is a GUEST VA -- a 32-bit number naming a
 * recompiled function -- so handing it over would jump to whatever host code
 * happens to live at address 0x0013xxxx. It is the memory-model trap the
 * NOT ROUTED note by bridge_for_ordinal describes, in its function-pointer
 * form, and it is why the CALL has to happen here, through the dispatch table,
 * like every other guest callback in this file. The native version stays for a
 * native caller.
 *
 * Unbridged, this set g_eax = 0 and ran nothing. For an NTSTATUS export that
 * zero reads as STATUS_SUCCESS; this one returns BOOLEAN, so what the guest
 * actually got was a fabricated FALSE -- the routine's answer, invented,
 * without the routine. JSRF calls it 102-229 times a session from DSOUND, on
 * the two interrupt objects it connects for audio, and one of those call sites
 * loops retesting a flag. A synthesised FALSE there is not a harmless lie; it
 * is a loop whose exit condition nobody is computing.
 *
 * Stack shape: BOOLEAN (__stdcall *)(PVOID Context). Push the context, then
 * the dummy return address; the callee's `ret 4` consumes both, so g_esp needs
 * no fixup afterwards -- the same discipline as bridge_NtUserIoApcDispatcher
 * and bridge_run_dpc. The BOOLEAN comes back in al, and eax is passed through
 * to the caller unchanged, which is what the real export does: it is a call
 * and a return, and it does not normalise the byte.
 *
 * IRQL AND THE INTERRUPT LOCK -- the part that had a choice in it.
 *
 * Hardware raises to the interrupt's SynchronizeIrql and takes its spinlock so
 * the routine cannot race the ISR. The nearest equivalent this runtime has is
 * g_vblank_delivery_active, the process-wide interlock all three interrupt
 * pumps already hold while they deliver, so holding it across the routine IS
 * the exclusion the real call provides, against the only thing that can run a
 * guest ISR here.
 *
 * It is taken as a BOUNDED spin and never as a blocking wait, and the reason
 * is the rule in CLAUDE.md that a lock held across guest code has frozen this
 * title before -- the APU lock inversion, where healthy audio and a dead guest
 * turned out to be one thread holding a lock another needed. The interlock is
 * held by bridge_run_isr for as long as a guest ISR body takes, which is
 * bounded but not instant, so a short spin with a yield fits; if it does not
 * come free in that time the routine runs anyway and the miss is counted and
 * reported. Running unsynchronised is a far smaller lie than not running at
 * all, which is the behaviour being replaced, and the counter makes the
 * frequency measurable instead of assumed. As of writing NOTHING HAS RUN THIS:
 * the shape is argued from the code, not from a session.
 *
 * While the routine runs, g_in_isr marks this thread. That flag means, at all
 * four of its readers, "this thread is executing guest code at a raised level:
 * deliver no interrupts or timers into it, and defer any DPC it queues" --
 * which is exactly the state a synchronised routine is in. Without it a nested
 * kernel call from the routine would let bridge_timers_poll run a timer DPC
 * inside the synchronised region, which is the one thing the region exists to
 * prevent. Any DPC the routine queues is run afterwards, outside the region,
 * the same way bridge_run_isr does it.
 *
 * Already inside an ISR or a DPC on this thread: call straight through. We
 * hold the exclusion already, spinning for an interlock this thread may itself
 * be holding would deadlock, and g_pending_dpc belongs to the outer frame.
 */
#define BRIDGE_SYNC_SPIN_TRIES 256

static unsigned long g_sync_exec_unsynchronised;
/* Plain call count, because the warning above only fires when the interlock is
 * contended -- so a silent log could mean "never called" or "called hundreds of
 * times, cleanly", and on 14 Sep 2026 those two were indistinguishable while
 * trying to attribute a freeze. Reported by xbox_ReportSyncExec below. */
static unsigned long g_sync_exec_calls;

/* Ordinal 153 is OFF BY DEFAULT, and that is a retreat from the commit that
 * added it (b707832). Enable with RECOMP_KE_SYNC_EXEC=1.
 *
 * WHY. Bridging it was correct on its own terms -- the guest really was being
 * handed a fabricated FALSE for a routine that never ran. But it was shipped
 * without ever being exercised at gameplay, and measurement since says it
 * regressed the title:
 *
 *   scripted, 3 runs each, same schedule, same machine:
 *     binary WITHOUT this bridge   3/3 reached New Game, ord175 = 74603, 74150, 73945
 *     binary WITH it               1/3 reached New Game, ord175 = 27676, 10614, 5130
 *
 * ord175 is MmLockUnlockBufferPages, i.e. how often the guest's XPP USB driver
 * pins a transfer buffer. A third of the traffic, and degrading, while the same
 * binary rendered MORE (666k draws against 607k) -- so it is not a general
 * slowdown but something specific to interrupt-driven I/O. Three hand-played
 * sessions also froze during a rail grind with it on, and it has never once
 * been played with it off.
 *
 * THE MECHANISM, and it is not the one the bridge's own comment worried about.
 * Both guest call sites are inside DPC bodies, so the spin and g_in_isr code
 * below never executes here and g_sync_exec_unsynchronised is vacuous. What
 * the bridge actually does is switch on roughly 41 previously-dead DSOUND
 * functions: the only instruction in the title that sets a bit in the DSOUND
 * state word [this+0x6FC] lives inside the routine this now calls, so before
 * it was bridged those code paths were unreachable. They run inside
 * bridge_run_isr -> bridge_run_dpc, which holds g_vblank_delivery_active --
 * the same interlock bridge_device_irq_poll needs, and that pump is the ONLY
 * thing that runs the guest's USB ISR on vector 1. More time in the interlock
 * is fewer USB interrupts delivered. That closure contains a 10 ms busy-wait
 * (KeStallExecutionProcessor 0x2710 at 0x001A1BDA) and four unbounded guest
 * spins on APU MMIO.
 *
 * This is a retreat, not a diagnosis. The remaining work is to make the
 * interlock not double as the IRQL model, so guest ISR and DPC bodies stop
 * blocking interrupt delivery process-wide -- at which point this can be
 * turned back on and the guest can finally have its synchronize routine. */
static int bridge_sync_exec_disabled(void)
{
    static int off = -1;
    if (off < 0) {
        const char *v = getenv("RECOMP_KE_SYNC_EXEC");
        off = (v && *v && *v != '0') ? 0 : 1;   /* default: OFF */
        fprintf(stderr, off
                ? "  [KERNEL] KeSynchronizeExecution (153) off (default);"
                  " returning FALSE without running the routine, as before it"
                  " was bridged. RECOMP_KE_SYNC_EXEC=1 enables it.\n"
                : "  [KERNEL] KeSynchronizeExecution (153) ENABLED by"
                  " RECOMP_KE_SYNC_EXEC; it regressed USB transfer rate when"
                  " last measured -- watch ordinal 175.\n");
        fflush(stderr);
    }
    return off;
}

void xbox_ReportSyncExec(void)
{
    fprintf(stderr, "  [KE-SYNC] calls=%lu unsynchronised=%lu disabled=%d\n",
            g_sync_exec_calls, g_sync_exec_unsynchronised,
            bridge_sync_exec_disabled());
    fflush(stderr);
}

static void bridge_KeSynchronizeExecution(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine_va   = STACK_ARG(1);
    uint32_t context_va   = STACK_ARG(2);
    recomp_func_t fn;
    int nested = (g_in_isr || g_in_dpc);
    int locked = 0;

    g_sync_exec_calls++;
    if (bridge_sync_exec_disabled()) {
        g_eax = 0;   /* exactly what the unbridged dispatcher did */
        return;
    }

    if (!routine_va) {
        /* The real kernel would call through a null pointer and bugcheck.
         * xbox_KeSynchronizeExecution answers FALSE; match it rather than
         * inventing a fault. */
        g_eax = 0;
        return;
    }

    fn = recomp_lookup(routine_va);
    if (!fn) fn = recomp_lookup_manual(routine_va);
    if (!fn) {
        static uint32_t warned_routine = 0;
        if (warned_routine != routine_va) {
            warned_routine = routine_va;
            fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: routine "
                    "0x%08X not in dispatch (kinterrupt=0x%08X); returning "
                    "FALSE without running it\n", routine_va, interrupt_va);
            fflush(stderr);
        }
        g_eax = 0;
        return;
    }

    if (!nested) {
        int tries;
        for (tries = 0; tries < BRIDGE_SYNC_SPIN_TRIES; tries++) {
            if (InterlockedCompareExchange(&g_vblank_delivery_active, 1, 0) == 0) {
                locked = 1;
                break;
            }
#if defined(_WIN32)
            Sleep(0);
#else
            recomp_yield();
#endif
        }
        if (!locked && ++g_sync_exec_unsynchronised <= 8) {
            fprintf(stderr, "  [KERNEL] KeSynchronizeExecution: interrupt "
                    "delivery interlock still held after %d yields; running "
                    "routine 0x%08X UNSYNCHRONISED (%lu so far)\n",
                    BRIDGE_SYNC_SPIN_TRIES, routine_va,
                    g_sync_exec_unsynchronised);
            fflush(stderr);
        }
    }

    {
        BridgeGuestRegs saved;
        uint32_t result;
        uint32_t outer_pending = 0, outer_sys1 = 0, outer_sys2 = 0;

        bridge_save_regs(&saved);
        if (!nested) {
            outer_pending = g_pending_dpc;
            outer_sys1 = g_pending_dpc_sys1;
            outer_sys2 = g_pending_dpc_sys2;
            g_pending_dpc = 0;
            g_in_isr = 1;
        }

        g_esp -= 4; BRIDGE_MEM32(g_esp) = context_va;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        fn();
        result = g_eax;

        if (!nested)
            g_in_isr = 0;
        /* Callee-saved registers only in spirit: restoring all six is stricter
         * than x86 requires, since ecx and edx are the caller's to lose, but a
         * caller cannot tell the difference and this tree has a measured
         * history of recompiled functions not giving ebx/esi/edi back
         * (129 of them). eax is the return value and is put back after. */
        bridge_restore_regs(&saved);
        g_eax = result;

        if (!nested) {
            uint32_t queued = g_pending_dpc;
            uint32_t s1 = g_pending_dpc_sys1, s2 = g_pending_dpc_sys2;

            g_pending_dpc = outer_pending;
            g_pending_dpc_sys1 = outer_sys1;
            g_pending_dpc_sys2 = outer_sys2;
            if (locked)
                InterlockedExchange(&g_vblank_delivery_active, 0);
            /* Outside the synchronised region and outside the interlock, which
             * is where a deferred call belongs. */
            if (queued)
                bridge_run_dpc(queued, s1, s2);
        }
    }
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    /* The informational and warning codes, which are not failures and must
     * not fall through to the generic answer.
     *
     * 317 is ERROR_MR_MID_NOT_FOUND -- "there is no message text for this
     * number" -- and as a default for a status nobody has mapped yet it is
     * honest. As an answer to "is this request still in flight?" it is not:
     * a caller comparing against ERROR_IO_PENDING gets "no" and takes the
     * branch for a request that never started.
     *
     * Shin Megami Tensei: Nine does exactly that. Its resource loader issues
     * a read, sees the call fail, asks for the error, and marks the object as
     * loading only when the answer is ERROR_IO_PENDING. With 317 the object
     * stayed idle, the poll that finishes the load returned "not started" on
     * every frame, and the title sat in its first boot state forever with
     * everything else working. */
    case 0x00000103: g_eax = 997; break;        /* STATUS_PENDING → ERROR_IO_PENDING */
    case 0x00000102: g_eax = 1460; break;       /* STATUS_TIMEOUT → ERROR_TIMEOUT */
    case 0x00000104: g_eax = 0; break;          /* STATUS_REPARSE → ERROR_SUCCESS */
    case 0x80000005: g_eax = 234; break;        /* STATUS_BUFFER_OVERFLOW → ERROR_MORE_DATA */
    case 0x80000006: g_eax = 18; break;         /* STATUS_NO_MORE_FILES → ERROR_NO_MORE_FILES */
    case 0xC0000011: g_eax = 38; break;         /* STATUS_END_OF_FILE → ERROR_HANDLE_EOF */
    case 0xC0000023: g_eax = 122; break;        /* STATUS_BUFFER_TOO_SMALL → ERROR_INSUFFICIENT_BUFFER */
    case 0xC0000035: g_eax = 183; break;        /* STATUS_OBJECT_NAME_COLLISION → ERROR_ALREADY_EXISTS */
    case 0xC00000BB: g_eax = 50; break;         /* STATUS_NOT_SUPPORTED → ERROR_NOT_SUPPORTED */

    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Snapshot the counted ANSI path without modifying guest memory. XAPI's
 * FindFirstFile splits one buffer into parent and basename by shortening
 * Length, not by inserting a NUL. strlen() reopened the entire marker file
 * as a directory, so a completed JSRF cache was never recognized.
 * The snapshot is thread-local and valid until the next path conversion on
 * this thread; all current consumers finish before callbacks/another path. */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    static RECOMP_TLS char path[UINT16_MAX+1u];
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    uint16_t length=BRIDGE_MEM16(ansi_str_va);
    /* TWO CHECKS, AND THEY ANSWER DIFFERENT QUESTIONS.
     *
     * This one -- Length must not exceed MaximumLength -- came from upstream's
     * 0472a59 and is an ancestor of this tree, but the line is not in it: a
     * merge resolved this hunk in favour of our side and dropped it silently.
     * Nothing in the history explains the removal, and the function's own log
     * shows no commit that took it out, which is what a merge resolution looks
     * like after the fact. Restored rather than reinvented.
     *
     * It says the ANSI_STRING is SELF-CONSISTENT: a Length longer than the
     * buffer the string itself claims is malformed, and cheap to reject before
     * anything is dereferenced. It does not say the buffer is real. */
    if (length > BRIDGE_MEM16(ansi_str_va + 2)) return NULL;
    /* `path` is sized UINT16_MAX+1 so a uint16 Length can never overrun the
     * DESTINATION. The SOURCE is the side that needed checking: Length and
     * Buffer both come from a guest ANSI_STRING, and nothing here established
     * that Buffer+Length is inside the mapping. A string near the top of guest
     * memory, or one whose Length does not match the buffer it names, walked
     * the host off the end of the mapping for up to 64 KB -- a read fault in
     * the bridge with no recompiled frame on the stack, which is precisely the
     * diagnosis problem bridge_va_mapped exists to prevent.
     *
     * Refusing the path is the right failure: every caller treats NULL as "no
     * object name", which is an error the guest gets told about, rather than a
     * host crash it cannot be told about. */
    if (length && !bridge_va_mapped(buf_va, length)) {
        static int warned;
        if (warned++ < 4) {
            fprintf(stderr,
                    "  [KERNEL] object name: ANSI_STRING at 0x%08X names "
                    "buffer 0x%08X length %u, which is not mapped guest "
                    "memory; refusing the path\n",
                    ansi_str_va, buf_va, (unsigned)length);
            fflush(stderr);
        }
        return NULL;
    }
    memcpy(path,XBOX_TO_NATIVE(buf_va),length);
    path[length]='\0';
    return path;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: not a handle this bridge issued. See
     * bridge_resolve_handle for why this is NULL and not the raw value. */
    return NULL;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /*
     * Untagged: not a handle this bridge ever issued.
     *
     * This used to return (HANDLE)token, which is the mirror of the mistake
     * the IoCreateDevice note above describes: there a host pointer must not
     * escape into the guest ABI, and here a guest value must not escape into a
     * host pointer. The consumers dereference what they are handed --
     * ResumeThread does `((w32_object *)h)->kind` after only a NULL check --
     * so a guest address arriving here faulted the host inside
     * xbox_NtResumeThread rather than failing the call.
     *
     * The synthetic handles that are deliberately untagged (0xDEAD0001 from
     * NtOpenSymbolicLinkObject, 0xBEEF0010 from NtCreateDirectoryObject,
     * 0xBEEF0001 from a thread that ran inline) are only ever compared, never
     * operated on: bridge_NtClose tests the raw token before resolving. NULL
     * is what they should resolve to, and it makes an operation on one fail
     * honestly -- ResumeThread(NULL) returns -1, so NtResumeThread reports
     * STATUS_UNSUCCESSFUL -- instead of taking the process down.
     */
    return NULL;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = name->Length==UINT16_MAX ? UINT16_MAX : (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va+8) : 0;
}

/*
 * The DVD drive as a device, not as a directory.
 *
 * A title that checks its media opens "\\Device\\CdRom0" itself -- the bare
 * device, with nothing after it -- and then issues IOCTLs on the handle. The
 * path table in kernel_path.c only carries the "\\Device\\CdRom0\\" form with
 * the separator, which is the prefix for reading a *file* off the disc, so the
 * bare open matched no rule, was reported as "Unrecognized Xbox path", and came
 * back STATUS_OBJECT_PATH_NOT_FOUND. DDS9 reads that as "no disc" and exits
 * through HalReturnToFirmware before it draws a frame.
 *
 * There is nothing on the host to open here: the game directory is a
 * directory, and a directory handle would not answer the IOCTLs that follow.
 * So the open returns a synthetic handle, in the same style as the ones
 * NtCreateDirectoryObject and the partition devices already hand out. It is
 * deliberately untagged, which bridge_resolve_handle passes through unchanged,
 * and distinct so bridge_NtDeviceIoControlFile can recognise it by value.
 *
 * Accepts the "\??\" prefix, since titles reach the device both ways.
 */
#define BRIDGE_CDROM_HANDLE 0xDECD0001u

static int bridge_is_cdrom_device(const char *path)
{
    if (!path) return 0;
    if (_strnicmp(path, "\\??\\", 4) == 0) path += 4;
    return _stricmp(path, "\\Device\\CdRom0") == 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (bridge_is_cdrom_device(name.Buffer)) {
        fprintf(stderr, "  [FILE] %s -> synthetic DVD device handle\n",
                name.Buffer);
        if (handle_va)
            BRIDGE_MEM32(handle_va) = BRIDGE_CDROM_HANDLE;
        bridge_write_iostatus(iostatus_va, 0, 1 /* FILE_OPENED */);
        return 0;
    }

    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
/* Every live message-box object, and what it says.
 *
 * The disc-error dialog was found because it wrote a file; a dialog that only
 * sits on screen writes nothing and is invisible to every instrument here. But
 * the class is known -- vtable 0x001CC660, message string at +0xA0 -- so the
 * objects can be found directly by scanning guest RAM for the vtable pointer.
 *
 * RECOMP_DIALOG_SCAN=1, once, a few seconds in. A title parked on a healthy
 * frame loop that draws one full-screen quad and loads nothing looks identical
 * whether it is idling or showing a message, and this is the difference.
 */
#define DIALOG_VTABLE 0x001CC660u

/* The OHCI root hub, as the title left it.
 *
 * XPP programs the USB host controller at 0xFED00000 directly: HcRhDescriptorA
 * at +0x48, HcRhStatus at +0x50, a HostControllerReset through HcCommandStatus
 * at +0x08, then HcControl at +0x04. The MCPX aperture behind those addresses
 * is plain RAM with no register semantics, so every write sticks and every
 * unwritten register reads zero.
 *
 * That matters most for HcRhDescriptorA: its low byte, NDP, is the number of
 * downstream ports and is READ-ONLY on real hardware. On RAM the title's own
 * write defines it. RECOMP_OHCI_DUMP=1 prints the block so the value it ends
 * up with is a measurement rather than an inference.
 */
static void bridge_dump_ohci(void)
{
    static int done;
    const uint32_t base = 0xFED00000u;
    uint32_t i;

    /* The gate the USB init opens with.
     *
     * sub_001BD108 maps 0xFED00000 and builds the OHCI device, but only after
     *     eax = MEM32(0x1C40BC); if (MEM8(eax + 5) == 0xA1) return;
     * so if that byte reads 0xA1 the controller is never created at all and
     * everything downstream -- the port, the HCCA, the ISR's register base --
     * is moot. Printed beside the register block because the two answer the
     * same question from opposite ends. */
    {
        uint32_t obj = BRIDGE_MEM32(0x1C40BC);
        fprintf(stderr, "  [OHCI] usb-init gate: [0x1C40BC]=0x%08X",
                obj);
        if (obj >= 0x10000u && obj < 0x04000000u)
            fprintf(stderr, " byte[+5]=0x%02X %s", BRIDGE_MEM8(obj + 5),
                    BRIDGE_MEM8(obj + 5) == 0xA1 ? "-> init SKIPPED" : "-> init runs");
        fputc('\n', stderr);
        fflush(stderr);
    }

    /* RECOMP_OHCI_DUMP=n dumps n times, so a response to the attach probe is
     * visible as a change rather than only as a first reading. */
    if (!getenv("RECOMP_OHCI_DUMP"))
        return;
    if (done >= atoi(getenv("RECOMP_OHCI_DUMP")))
        return;
    done++;

    fprintf(stderr, "  [OHCI] root hub at 0x%08X\n", base);
    for (i = 0; i <= 0x5C; i += 4)
        fprintf(stderr, "  [OHCI]   +0x%02X = 0x%08X%s\n", i,
                BRIDGE_MEM32(base + i),
                i == 0x48 ? "   HcRhDescriptorA (low byte = NDP, port count)"
              : i == 0x50 ? "   HcRhStatus"
              : i == 0x54 ? "   HcRhPortStatus[0]"
              : i == 0x58 ? "   HcRhPortStatus[1]"
              : i == 0x04 ? "   HcControl"
              : i == 0x08 ? "   HcCommandStatus" : "");
    fflush(stderr);
}

static void bridge_scan_dialogs(void)
{
    static int done;
    uint32_t va, hits = 0;

    if (done || !getenv("RECOMP_DIALOG_SCAN"))
        return;
    done = 1;

    fprintf(stderr, "  [DIALOG] scanning guest RAM for vtable 0x%08X\n",
            DIALOG_VTABLE);
    for (va = 0x00010000u; va < 0x04000000u && hits < 12; va += 4) {
        char     msg[192];
        uint32_t sp;
        int      k;

        if (BRIDGE_MEM32(va) != DIALOG_VTABLE)
            continue;
        hits++;
        sp = BRIDGE_MEM32(va + 0xA0);
        msg[0] = 0;
        if (sp >= 0x00010000u && sp < 0x04000000u) {
            for (k = 0; k < (int)sizeof(msg) - 1; k++) {
                char c = (char)(BRIDGE_MEM32((sp + (uint32_t)k) & ~3u)
                                >> (8 * ((sp + (uint32_t)k) & 3)));
                if (!c) break;
                msg[k] = (c >= 32 && c < 127) ? c : '.';
            }
            msg[k] = 0;
        }
        fprintf(stderr, "  [DIALOG] object 0x%08X [+0x98]=0x%08X \"%s\"\n",
                va, BRIDGE_MEM32(va + 0x98), msg);
    }
    fprintf(stderr, "  [DIALOG] %u object(s)\n", hits);
    fflush(stderr);
}

/* Guest backtrace for one file open, selected by path.
 *
 * A title that declares a fatal error writes a file and keeps running, so none
 * of the usual signals fire -- no fault, no exit -- and the ordinal histogram
 * only says that NtCreateFile was called. RECOMP_FILE_BACKTRACE=<substring>
 * scans the guest stack at the call and prints everything that looks like a
 * code address, which turns "something wrote JSRF_FATAL.ERR" into a list of
 * guest functions to disassemble.
 *
 * A scan, not a frame walk: recompiled frames do not share a host frame
 * layout, so following a saved EBP would be wrong more often than right. Some
 * hits are stale stack garbage. The ones that repeat across runs are real.
 */
static void bridge_file_backtrace(const char *path)
{
    const char *want = getenv("RECOMP_FILE_BACKTRACE");
    uint32_t   sp, limit;
    int        printed = 0;

    if (!want || !*want || !path || !strstr(path, want) || !g_esp)
        return;

    fprintf(stderr, "  [FILE] backtrace for %s (esp=0x%08X ret=0x%08X)\n",
            path, g_esp, BRIDGE_MEM32(g_esp - 4));
    limit = g_esp + 0x400;
    for (sp = g_esp; sp < limit && printed < 40; sp += 4) {
        uint32_t v = BRIDGE_MEM32(sp);
        /* The XBE's code sits below .data at 0x001EB760. */
        if (v >= 0x00011000u && v < 0x001EB760u) {
            fprintf(stderr, "      esp+0x%03X = 0x%08X\n", sp - g_esp, v);
            printed++;
        }
    }

    /* The object whose flags caused this.
     *
     * JSRF decides to write the marker in sub_0006EC80 with
     * "test [esi+0x98], 0x400000", and esi is a this-pointer several frames
     * up. Rather than guess at frame layout, scan the same window for a value
     * that looks like a heap object and actually has the bit set. The flag
     * word it prints says which other bits are up alongside it, which is what
     * names the condition. */
    for (sp = g_esp; sp < limit; sp += 4) {
        uint32_t v = BRIDGE_MEM32(sp);
        uint32_t flags;
        if (v < 0x00200000u || v >= 0x04000000u || (v & 3))
            continue;
        flags = BRIDGE_MEM32(v + 0x98);
        if (flags & 0x400000u) {
            int w;
            fprintf(stderr,
                    "      candidate this=0x%08X [+0x98]=0x%08X "
                    "[+0x24]=0x%08X vtbl=0x%08X\n",
                    v, flags, BRIDGE_MEM32(v + 0x24), BRIDGE_MEM32(v));
            /* The header names the class: slot 0 is the vtable, and the few
             * words after it are what distinguishes one instance from the
             * next. Printed rather than guessed at, because the heap address
             * itself moves between runs. */
            for (w = 0; w < 8; w++)
                fprintf(stderr, "        +0x%02X = 0x%08X\n",
                        w * 4, BRIDGE_MEM32(v + (uint32_t)w * 4));
            /* The constructor copies a string into +0xA0, so the object can
             * say in its own words what it is. */
            {
                uint32_t sp2 = BRIDGE_MEM32(v + 0xA0);
                if (sp2 >= 0x00010000u && sp2 < 0x04000000u) {
                    char msg[160];
                    int  k;
                    for (k = 0; k < (int)sizeof(msg) - 1; k++) {
                        char c = (char)(BRIDGE_MEM32((sp2 + (uint32_t)k) & ~3u)
                                        >> (8 * ((sp2 + (uint32_t)k) & 3)));
                        if (!c) break;
                        msg[k] = (c >= 32 && c < 127) ? c : '.';
                    }
                    msg[k] = 0;
                    fprintf(stderr, "        +0xA0 -> \"%s\"\n", msg);
                }
            }
        }
    }
    fflush(stderr);
}

static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    bridge_file_backtrace(bridge_get_xbox_path(obj_attrs));

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
#if defined(_WIN32)
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

#endif

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    fprintf(stderr, "  [FILE] -> 0x%08X%s\n", g_eax, g_eax ? " FAILED" : "");
    fflush(stderr);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */
    const char *path   = bridge_get_xbox_path(obj_attrs);

    fprintf(stderr,
            "  [FILE] NtOpenFile handle_va=0x%08X oa=0x%08X "
            "{root=0x%08X name=0x%08X attributes=0x%08X} "
            "ios=0x%08X access=0x%08X share=0x%08X "
            "disposition=0x%08X options=0x%08X file_attributes=0x%08X "
            "caller=0x%08X path=%s\n",
            handle_va, obj_attrs,
            obj_attrs ? BRIDGE_MEM32(obj_attrs + 0) : 0,
            obj_attrs ? BRIDGE_MEM32(obj_attrs + 4) : 0,
            obj_attrs ? BRIDGE_MEM32(obj_attrs + 8) : 0,
            iostatus, access, share, 1u, options, 0u,
            /* kernel_thunk_dispatch has popped the dummy return address, so
             * the guest's own return address sits just below g_esp -- the same
             * idiom the unimplemented-ordinal report uses. Without it a [FILE]
             * line says what was opened and never who asked, and "which code
             * opens this path" has been answered by correlating against
             * neighbouring log lines, which is guesswork. */
            g_esp ? (uint32_t)BRIDGE_MEM32(g_esp - 4) : 0,
            path ? path : "<null>");

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);

    /* Same result line NtCreateFile prints. Without it an NtOpenFile showed
     * what was asked for and never whether it opened, which reads as a failed
     * call whenever the next line is the title doing something drastic. */
    fprintf(stderr, "  [FILE] -> 0x%08X%s\n", g_eax, g_eax ? " FAILED" : "");
    fflush(stderr);
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * The native backends complete synchronously, but the APC must not run inline.
 * Xbox software can inspect its in-flight flag immediately after NtReadFile
 * returns and only publish completion after an alertable wait dispatches the
 * APC. JSRF's WXCI reader does exactly that: inline delivery clears the flag
 * too early, sends the request server down its early-exit branch, and leaves
 * the request status at 2 forever even though all bytes arrived.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

typedef struct bridge_pending_apc {
    uint32_t routine;
    uint32_t context;
    uint32_t iostatus;
} bridge_pending_apc_t;

#define BRIDGE_PENDING_APC_MAX 256u
static RECOMP_TLS bridge_pending_apc_t
    s_pending_apcs[BRIDGE_PENDING_APC_MAX];
static RECOMP_TLS unsigned s_pending_apc_head;
static RECOMP_TLS unsigned s_pending_apc_count;

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    uint32_t caller_esp = g_esp;
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG) */
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        fn();
        /* A normal guest APC ends in ret 12 and restores this itself. Keep the
         * bridge boundary exact even for a manual override with a mismatched
         * declaration: the synthetic callback frame must never leak into the
         * interrupted alertable wait. */
        g_esp = caller_esp;
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

static void bridge_queue_file_apc(uint32_t routine, uint32_t context,
                                  uint32_t iostatus)
{
    unsigned tail;

    if (s_pending_apc_count == BRIDGE_PENDING_APC_MAX) {
        /* Losing an I/O completion deadlocks its owner. This should never be
         * reachable with synchronous host I/O, but inline delivery is the
         * only recoverable fallback if a title queues hundreds without an
         * alertable wait. */
        fprintf(stderr, "  [KERNEL] file I/O APC queue full; delivering inline\n");
        fflush(stderr);
        deliver_one_apc(routine, context, iostatus);
        return;
    }

    tail = (s_pending_apc_head + s_pending_apc_count) %
           BRIDGE_PENDING_APC_MAX;
    s_pending_apcs[tail].routine = routine;
    s_pending_apcs[tail].context = context;
    s_pending_apcs[tail].iostatus = iostatus;
    ++s_pending_apc_count;
}

/* Deliver every APC already queued for this guest thread. Removing an item
 * before invoking guest code makes re-entrant file I/O safe. */
static int bridge_deliver_pending_apcs(void)
{
    int delivered = 0;

    while (s_pending_apc_count) {
        bridge_pending_apc_t apc = s_pending_apcs[s_pending_apc_head];
        s_pending_apc_head = (s_pending_apc_head + 1) %
                             BRIDGE_PENDING_APC_MAX;
        --s_pending_apc_count;
        deliver_one_apc(apc.routine, apc.context, apc.iostatus);
        ++delivered;
    }
    return delivered;
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        bridge_queue_file_apc(apc_routine, apc_context, iostatus);
    }
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is paged in from disc on demand,
 * and a title that keeps its video decoder in one -- Wreckless keeps WMVDEC
 * there -- calls this before touching it. Every section is already resident
 * here, so the work is the bookkeeping: hand back success and keep the
 * reference count the title can read.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;              /* STATUS_INVALID_PARAMETER */
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (load)
        count++;
    else if (count)
        count--;
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* -- RtlUnwind (ordinal 312, 4 args = 16 bytes) -------------------------
 *
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
 *
 * Discards the SEH registration frames between the current one and
 * TargetFrame, letting each handler run its __finally blocks on the way past,
 * and leaves fs:[0] pointing at TargetFrame. fs:[0] is guest address 0 here,
 * because the runtime models the TIB at the bottom of guest memory.
 *
 * Left unbridged this returned 0 without touching anything, which is not a
 * harmless stub: MSVC's _global_unwind2 calls it and then carries on as if the
 * frames were gone, so the chain kept pointing into stack that had already
 * been reused and the next dispatch walked records built out of live locals.
 *
 * The walk is bounded and checked rather than trusting the chain, since it
 * lives in guest stack memory that a title can corrupt: records must climb
 * toward the stack top, stay inside the stack, and stay 4-byte aligned. A
 * chain that breaks any of those is truncated instead of followed.
 */
#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
#define XBOX_SEH_MAX_FRAMES 64

static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t reg          = BRIDGE_MEM32(g_fs_base);
    uint32_t prev_reg     = 0;
    uint32_t scratch      = 0;
    int      guard;

    /* An unwind with no record of its own still has to tell the handlers it
     * is an unwind, so synthesise one below the stack pointer. */
    if (!exc_record) {
        g_esp -= 0x50;
        scratch = g_esp;
        /* XBOX_TO_NATIVE maps a guest 0 to NULL, and a synthesised record is
         * built at whatever g_esp happens to be -- so this memset is one bad
         * stack pointer away from writing through NULL inside the bridge. */
        if (!scratch || !bridge_va_mapped(scratch, 0x50)) {
            fprintf(stderr, "  [KERNEL] RtlUnwind: cannot synthesise an "
                    "exception record at esp 0x%08X\n", scratch);
            fflush(stderr);
            g_eax = 0;
            return;
        }
        memset((uint8_t *)XBOX_TO_NATIVE(scratch), 0, 0x50);
        BRIDGE_MEM32(scratch) = 0xC0000027u;   /* STATUS_UNWIND */
        exc_record = scratch;
    }
    BRIDGE_MEM32(exc_record + 4) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == 0 || reg == target_frame)
            break;
        if ((reg & 3u) || reg < XBOX_STACK_BASE || reg >= XBOX_STACK_TOP)
            break;                       /* not a stack frame: chain is broken */
        if (prev_reg && reg <= prev_reg)
            break;                       /* not climbing: cycle or corruption */

        next    = BRIDGE_MEM32(reg);
        handler = BRIDGE_MEM32(reg + 4);

        /* Pop before dispatching. The handler may raise, and it must not see
         * its own frame still on the chain. */
        BRIDGE_MEM32(g_fs_base) = next;

        if (handler) {
            recomp_func_t fn = recomp_lookup(handler);
            if (!fn) fn = recomp_lookup_manual(handler);
            if (!fn) fn = recomp_lookup_kernel(handler);
            if (fn) {
                /* EXCEPTION_DISPOSITION handler(record, frame, context,
                 * dispatcher) -- cdecl, so the caller pops. */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = exc_record;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* return address */
                fn();
                /* 16, not 20: the handler's own `ret` has already taken the
                 * return address off, leaving just the four arguments for the
                 * caller to drop. Cleaning 20 leaves esp four bytes high, and
                 * every argument the unwound-into frame reads after that comes
                 * from one slot over. */
                g_esp += 16;
            }
        }

        prev_reg = reg;
        reg      = next;
    }

    /* Land on the target even if the walk stopped early: leaving fs:[0] on a
     * discarded frame is worse than losing a __finally. */
    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(g_fs_base) = target_frame;

    if (scratch)
        g_esp += 0x50;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtReadFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);

    /* How much asset data actually crosses the boundary.
     *
     * Opens and closes are traced and reads were not, so counting "NtReadFile"
     * in a log measured the logging rather than the title -- and 1341 opens
     * beside an apparent single read is a conclusion that shape invites. This
     * is the number itself: calls, bytes asked, bytes delivered. */
    {
        static int enabled = -1;
        static unsigned long calls;
        static unsigned long long asked, got;

        if (enabled < 0) enabled = getenv("RECOMP_READ_COUNT") != NULL;
        if (enabled) {
            ++calls;
            asked += length;
            got += ios.Information;
            /* Decades, so a healthy stream reports a rate and a starved one
             * still reports its first read. */
            if (calls <= 5 || calls % 500 == 0)
                fprintf(stderr,
                        "[READ] calls=%lu asked=%llu got=%llu last=%u/%u"
                        " status=%08X\n",
                        calls, asked, got, (unsigned)ios.Information,
                        length, (unsigned)g_eax);
        }
    }

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        /* A zero-length read passes the buffer check without the buffer having
         * to be anything, so p can be NULL here while got is not trusted to be
         * zero. Only the pointer makes the dereferences below safe. */
        if (!p) got = 0;
        fprintf(stderr, "  [READ] want=%u got=%u st=0x%08X %02X %02X %02X %02X\n",
                length, got, (uint32_t)ios.Status,
                got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(buffer_va, length, "NtWriteFile")) {
        bridge_write_iostatus(iostatus, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtSetInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryVolumeInformationFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) ─ */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t infoclass   = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (!bridge_buf_ok(info_va, length, "NtQueryDirectoryFile")) {
        bridge_write_iostatus(ios_va, (NTSTATUS)0xC0000005, 0);
        g_eax = 0xC0000005u;                 /* STATUS_ACCESS_VIOLATION */
        return;
    }
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        /* The pattern is a guest string too, and the match walks Length bytes
         * of it. Dropping an unmapped one to NULL means "no pattern", which is
         * a restart-scan with no filter -- a wrong answer the caller can see,
         * rather than a host read off the end of the mapping. */
        fn.Buffer        = (fn_buf && bridge_va_mapped(fn_buf, fn.Length))
                             ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, (XBOX_FILE_INFORMATION_CLASS)infoclass,
                pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    /* Say so when the buffer could not be filled.
     *
     * Reporting STATUS_SUCCESS with an untouched output buffer is the same
     * defect that left ordinal 215 unrouted: the caller believes it has a
     * device path and parses whatever was already in that memory. Half-Life 2
     * does exactly that -- it walked uninitialised bytes and dereferenced
     * 0x68737572, the ASCII "rush", as a pointer. That only looked survivable
     * because the RAM mirrors happened to back the address; with a mapping
     * that does not alias, it faults immediately.
     *
     * STATUS_BUFFER_TOO_SMALL is the honest answer, and it is one the caller
     * already has to handle -- it is what a real kernel returns when the
     * ANSI_STRING it was handed has no room. */
    if (!target_va) {
        g_eax = 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        if (!buf_va || len >= max_len) {
            static unsigned warned;
            if (warned++ < 4) {
                fprintf(stderr,
                        "  [KERNEL] NtQuerySymbolicLinkObject: buffer 0x%08X "
                        "max=%u cannot hold %u bytes; returning "
                        "STATUS_BUFFER_TOO_SMALL\n",
                        buf_va, (unsigned)max_len, (unsigned)len + 1);
                fflush(stderr);
            }
            g_eax = 0xC0000023u;         /* STATUS_BUFFER_TOO_SMALL */
            return;
        }
        memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
        BRIDGE_MEM16(target_va) = len;
    }
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 67, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t handle_id = STACK_ARG(0);
    uint32_t ios_va    = STACK_ARG(4);
    uint32_t ioctl     = STACK_ARG(5);
    uint32_t out_va    = STACK_ARG(8);
    uint32_t out_len   = STACK_ARG(9);

    /* IOCTLs aimed at the DVD device (see bridge_is_cdrom_device).
     *
     * These are the media check: the title asks the drive to confirm a disc is
     * present and that it is the one it expects. There is no drive here and no
     * disc to describe, so the honest answer is the one that lets the title
     * proceed -- the alternative is STATUS_NOT_SUPPORTED, which it reads as a
     * failed check and answers with HalReturnToFirmware.
     *
     * Reported rather than silent: which codes a title sends is the useful
     * fact when the check still fails, and guessing at them from documentation
     * is how this layer accumulates handlers for IOCTLs nothing ever sends. The
     * output buffer is zeroed, so a title that reads a result field back sees a
     * defined value instead of whatever was on its heap. */
    if (handle_id == BRIDGE_CDROM_HANDLE) {
        uint32_t in_va  = STACK_ARG(6);
        uint32_t in_len = STACK_ARG(7);

        /* The media check arrives as a SCSI pass-through, so the answer the
         * title reads is not the IOCTL's output buffer -- that is NULL here,
         * with length zero -- but the DataBuffer the request points at.
         * Returning STATUS_SUCCESS alone leaves that buffer as the title
         * zeroed it, which it reads as a failed check; DDS9 retries five
         * times and then exits through HalReturnToFirmware.
         *
         * SCSI_PASS_THROUGH_DIRECT, 32-bit layout, 44 bytes:
         *   0 Length(USHORT)  2 ScsiStatus  3 PathId  4 TargetId  5 Lun
         *   6 CdbLength  7 SenseInfoLength  8 DataIn
         *   12 DataTransferLength  16 TimeOutValue  20 DataBuffer
         *   24 SenseInfoOffset  28 Cdb[16] */
        if (in_va && in_len >= 44 && BRIDGE_MEM8(in_va + 28) == 0x5A) {
            uint32_t data_va  = BRIDGE_MEM32(in_va + 20);
            uint32_t data_len = BRIDGE_MEM32(in_va + 12);
            uint32_t page     = BRIDGE_MEM8(in_va + 30) & 0x3F;

            fprintf(stderr, "  [FILE] DVD MODE SENSE(10) page 0x%02X, "
                            "%u bytes -> authentication page\n", page, data_len);

            if (data_va && data_len) {
                uint32_t i;
                for (i = 0; i < data_len; i++)
                    BRIDGE_MEM8(data_va + i) = 0;

                /* An 8-byte MODE SENSE(10) parameter header, then the page.
                 * The three bytes that matter are named by the title's own
                 * validation at guest 0x0021EA56-0x0021EA6D, which is the only
                 * specification of this page there is: byte 11 must be exactly
                 * 1, and bytes 10 and 12 must both be non-zero. Anything else
                 * is read as "not the expected disc". */
                if (data_len >= 2) {
                    BRIDGE_MEM8(data_va + 0) = 0;
                    BRIDGE_MEM8(data_va + 1) = 26;   /* mode data length */
                }
                if (data_len >= 10) {
                    BRIDGE_MEM8(data_va + 8) = 0x3E; /* page code */
                    BRIDGE_MEM8(data_va + 9) = 18;   /* page length */
                }
                if (data_len >= 13) {
                    BRIDGE_MEM8(data_va + 10) = 1;   /* non-zero */
                    BRIDGE_MEM8(data_va + 11) = 1;   /* exactly 1 */
                    BRIDGE_MEM8(data_va + 12) = 1;   /* non-zero */
                }
            }

            BRIDGE_MEM8(in_va + 2) = 0;              /* ScsiStatus = GOOD */
            bridge_write_iostatus(ios_va, 0, in_len);
            g_eax = 0;
            return;
        }

        fprintf(stderr, "  [FILE] DVD device IOCTL 0x%X (in=%u out=%u) "
                        "-> STATUS_SUCCESS\n", ioctl, in_len, out_len);
        if (out_va && out_len) {
            uint32_t i;
            for (i = 0; i < out_len; i++)
                BRIDGE_MEM8(out_va + i) = 0;
        }
        bridge_write_iostatus(ios_va, 0, out_len);
        g_eax = 0;
        return;
    }

    /* Everything else belongs to the kernel implementation, which already
     * owns the raw-disk geometry and partition queries AND Partition5's
     * sparse-cache semantics -- upstream answers the first two here instead,
     * and repeating them in the bridge would shadow the Partition5 case with
     * a plain fixed-disk answer. The result marshals back through the common
     * completion path so an asynchronous caller is signalled.
     *
     * The DVD branch above cannot come through here: BRIDGE_CDROM_HANDLE is a
     * pseudo-handle with no host file behind it, so bridge_resolve_handle has
     * nothing to hand to xbox_NtDeviceIoControlFile. */
    {
        HANDLE handle = bridge_resolve_handle(handle_id);
        uint32_t input_va = STACK_ARG(6);
        XBOX_IO_STATUS_BLOCK ios;

        memset(&ios, 0, sizeof(ios));
        g_eax = (uint32_t)xbox_NtDeviceIoControlFile(
            handle, NULL, NULL, NULL, &ios, ioctl,
            input_va ? XBOX_TO_NATIVE(input_va) : NULL, STACK_ARG(7),
            out_va ? XBOX_TO_NATIVE(out_va) : NULL, out_len);
        bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
        bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), ios_va);
    }
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(4);
    uint32_t input_va = STACK_ARG(6);
    uint32_t output_va = STACK_ARG(8);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFsControlFile(
        handle, NULL, NULL, NULL, &ios, STACK_ARG(5),
        input_va ? XBOX_TO_NATIVE(input_va) : NULL, STACK_ARG(7),
        output_va ? XBOX_TO_NATIVE(output_va) : NULL, STACK_ARG(9));
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3), ios_va);
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
/* ── Guest-visible kernel objects ────────────────────────────────────────
 *
 * ObReferenceObjectByHandle hands the guest a POINTER to a kernel object, and
 * the guest then passes that pointer to the object-taking exports
 * (KeSetBasePriorityThread, KeQueryBasePriorityThread, ObfDereferenceObject).
 * So the object has to live in GUEST memory and be identified by a guest VA --
 * the same rule as the guest DEVICE_OBJECT above, and for the same reason: no
 * host pointer may be written into a guest slot.
 *
 * This used to write 0 into *Object and return STATUS_SUCCESS. That is a fake
 * success, and it cost the title everything downstream: JSRF's scheduler runs
 *
 *     ObReferenceObjectByHandle(hThread, type, &obj)
 *     KeQueryBasePriorityThread(obj)
 *     KeSetBasePriorityThread(obj, n)
 *     ObfDereferenceObject(obj)
 *
 * and with obj always NULL and the three Ke/Ob calls unrouted, the priority it
 * read back never matched the one it had just written, so the loop could not
 * converge. Measured over ~50s: 16,666,400 reference calls, 11,110,930 sets,
 * 5,555,465 queries -- an exact 3:2:1, i.e. one non-terminating iteration
 * repeated five and a half million times.
 *
 * The object is opaque to the guest: it only ever passes the pointer back to
 * us. So the layout is ours to choose, and nothing here is a guess about
 * bytes the title inspects. Only fields the bridge itself needs are defined.
 */
#define XBOX_GUEST_OBJECT_SIZE 0x10u

typedef struct XboxGuestObject {
    uint32_t HandleToken;   /* +0x00 the tagged token this object stands for */
    int32_t  RefCount;      /* +0x04 */
    int32_t  BasePriority;  /* +0x08 Xbox base priority, as Set/Query see it */
    uint32_t Reserved0C;    /* +0x0C */
} XboxGuestObject;

_Static_assert(sizeof(XboxGuestObject) == XBOX_GUEST_OBJECT_SIZE,
               "guest object: padded to a size the host chose");

/* Host-side registry, deliberately not stored in the guest object: guest
 * memory is not a free list, and this is also what lets the dereference and
 * priority paths reject a pointer this bridge never issued. */
#define XBOX_MAX_GUEST_OBJECTS 256

static struct {
    uint32_t object_va;
    uint32_t handle_token;
} s_guest_objects[XBOX_MAX_GUEST_OBJECTS];

static XboxGuestObject *bridge_guest_object(uint32_t object_va)
{
    int i;
    if (!object_va) return NULL;
    for (i = 0; i < XBOX_MAX_GUEST_OBJECTS; i++) {
        if (s_guest_objects[i].object_va == object_va) {
            return (XboxGuestObject *)XBOX_TO_NATIVE(object_va);
        }
    }
    return NULL;   /* not a pointer we issued */
}

/* NT's current-thread pseudo-handle. The Xbox kernel uses the same convention,
 * and JSRF adjusts its own priority through it -- so this is by far the most
 * common argument ObReferenceObjectByHandle actually sees, not an edge case.
 * It is not a handle the bridge issued, so it needs its own resolution path. */
#define XBOX_NT_CURRENT_THREAD 0xFFFFFFFEu

/* Objects standing for "the thread that asked" are keyed by thread id rather
 * than by a handle: win32_compat only has a real per-thread object for threads
 * it spawned (t_self_obj), so the main thread has no handle to key on. The tag
 * cannot collide with BRIDGE_HANDLE_TAG. */
#define BRIDGE_TID_TOKEN_TAG  0x54000000u
#define BRIDGE_TID_TOKEN_MASK 0x00FFFFFFu

static uint32_t bridge_current_thread_token(void)
{
    return BRIDGE_TID_TOKEN_TAG |
           ((uint32_t)GetCurrentThreadId() & BRIDGE_TID_TOKEN_MASK);
}

/* Host thread HANDLE for a stored token, or NULL when it cannot be named from
 * here. A thread-id token only resolves on the thread it identifies, which is
 * the case that matters: a title references NtCurrentThread and acts on it
 * immediately, on that same thread. */
static HANDLE bridge_thread_handle_for_token(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_TID_TOKEN_TAG) {
        return (token == bridge_current_thread_token())
                   ? GetCurrentThread() : NULL;
    }
    return bridge_resolve_handle(token);
}

/* One object per handle token, so repeated references to the same thread
 * return the same pointer -- which is what makes a priority written through
 * one reference visible through the next. */
static uint32_t bridge_object_for_token(uint32_t token)
{
    int i, free_slot = -1;
    for (i = 0; i < XBOX_MAX_GUEST_OBJECTS; i++) {
        if (s_guest_objects[i].object_va &&
            s_guest_objects[i].handle_token == token) {
            return s_guest_objects[i].object_va;
        }
        if (!s_guest_objects[i].object_va && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return 0;

    {
        uint32_t va = xbox_HeapAlloc(XBOX_GUEST_OBJECT_SIZE, 16);
        XboxGuestObject *obj;
        if (!va) return 0;
        obj = (XboxGuestObject *)XBOX_TO_NATIVE(va);
        obj->HandleToken  = token;
        obj->RefCount     = 0;
        /* Seed from the real thread so the first query reports the thread's
         * actual priority rather than an invented zero. */
        obj->BasePriority = (int32_t)xbox_KeQueryBasePriorityThread(
                                bridge_thread_handle_for_token(token));
        obj->Reserved0C   = 0;
        s_guest_objects[free_slot].object_va    = va;
        s_guest_objects[free_slot].handle_token = token;
        return va;
    }
}

static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle     = STACK_ARG(0);
    uint32_t object_ptr = STACK_ARG(2);
    uint32_t token      = handle;
    uint32_t object_va  = 0;

    if (handle == XBOX_NT_CURRENT_THREAD) {
        token = bridge_current_thread_token();
    } else if (bridge_resolve_handle(handle) == NULL) {
        /* A handle this bridge never issued is not silently a success. */
        static int unknown_count = 0;
        if (++unknown_count <= 10) {
            fprintf(stderr,
                    "  [BRIDGE] ObReferenceObjectByHandle: unknown handle "
                    "0x%08X type=0x%08X (#%d)\n",
                    handle, STACK_ARG(1), unknown_count);
            fflush(stderr);
        }
        if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
        g_eax = 0xC0000008u;  /* STATUS_INVALID_HANDLE */
        return;
    }

    object_va = bridge_object_for_token(token);
    if (!object_va) {
        if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
        g_eax = 0xC000009Au;  /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }

    ((XboxGuestObject *)XBOX_TO_NATIVE(object_va))->RefCount++;
    if (object_ptr) BRIDGE_MEM32(object_ptr) = object_va;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0): Xbox uses __fastcall here, so the argument arrives in ecx
 * and never reaches the stack, which is why the arg-size entry is 0.
 *
 * Supersedes an earlier version that called xbox_ObfDereferenceObject on
 * XBOX_TO_NATIVE(g_ecx) -- treating whatever the guest passed as a host
 * object. It now only acts on a pointer this bridge actually issued.
 *
 * The object stays allocated at refcount 0: one token keeps one object for the
 * life of the process, so a later reference to the same thread returns the
 * same pointer and the priority it carries. */
static void bridge_ObfDereferenceObject(void)
{
    XboxGuestObject *obj = bridge_guest_object(g_ecx);
    if (obj && obj->RefCount > 0) obj->RefCount--;
    g_eax = 0;
}

/* ── KeQueryBasePriorityThread (ordinal 124, 1 arg)
 * ── KeSetBasePriorityThread   (ordinal 143, 2 args)
 *
 * Both take a guest OBJECT POINTER, not a handle. The pre-existing
 * xbox_Ke*BasePriorityThread take a host HANDLE and cast the argument
 * straight to one, which is why routing these mechanically was unsafe and
 * why they sat unrouted: XBOX_TO_NATIVE(guest pointer) is not a host thread
 * handle, and handing it to GetThreadPriority reads host memory at a guest
 * address. Resolve the object to the token it stands for, and let the
 * existing helpers take the real handle. */
/* WHAT THE POLL ACTUALLY READS, because the count says it is a spin.
 *
 * This is queried 82 MILLION times in a hung session against 213 sets in the
 * whole run, as one leg of an ObReferenceObjectByHandle / QueryBasePriority /
 * ObfDereferenceObject cycle. A ratio like that is not bookkeeping, it is a
 * thread waiting for a value that never arrives.
 *
 * `obj ? ... : 0` is the line to watch: a handle that does not resolve reads
 * ZERO and says nothing about it, so a poll waiting for a nonzero priority on
 * a thread this bridge cannot find would spin exactly like this and leave no
 * trace. Counting resolved against unresolved separates "the value never
 * changes" from "we never found the object", which need opposite fixes.
 *
 * Under RECOMP_SCHED_TRACE only; bounded to eight handles and printed once. */
static struct { uint32_t handle; unsigned long hits; long last; int unresolved; }
    s_qbp[8];
static unsigned s_qbp_n;
static unsigned long s_qbp_over;

void bridge_query_priority_report(void)
{
    unsigned i;
    if (!s_qbp_n) return;
    fprintf(stderr, "  [SCHED] KeQueryBasePriorityThread by handle"
                    " (overflow %lu):\n", s_qbp_over);
    for (i = 0; i < s_qbp_n; ++i)
        fprintf(stderr, "  [SCHED]   handle=0x%08X queries=%-12lu last=%ld%s\n",
                s_qbp[i].handle, s_qbp[i].hits, s_qbp[i].last,
                s_qbp[i].unresolved ? "   <-- HANDLE DID NOT RESOLVE" : "");
    fflush(stderr);
}

static void bridge_KeQueryBasePriorityThread(void)
{
    uint32_t h = STACK_ARG(0);
    XboxGuestObject *obj = bridge_guest_object(h);
    g_eax = obj ? (uint32_t)obj->BasePriority : 0;
    if (sched_trace_on()) {
        unsigned i;
        for (i = 0; i < s_qbp_n; ++i) if (s_qbp[i].handle == h) break;
        if (i == s_qbp_n) {
            if (s_qbp_n < 8) { s_qbp[s_qbp_n].handle = h; ++s_qbp_n; }
            else { ++s_qbp_over; return; }
        }
        ++s_qbp[i].hits;
        s_qbp[i].last = (long)(int32_t)g_eax;
        if (!obj) s_qbp[i].unresolved = 1;
    }
}

static void bridge_KeRestoreFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeRestoreFloatingPointState(NULL);
}

static void bridge_KeSaveFloatingPointState(void)
{
    g_eax = (uint32_t)xbox_KeSaveFloatingPointState(NULL);
}

static void bridge_KeSetBasePriorityThread(void)
{
    XboxGuestObject *obj = bridge_guest_object(STACK_ARG(0));
    LONG increment = (LONG)STACK_ARG(1);
    LONG previous;

    if (!obj) {
        g_eax = 0;
        return;
    }

    previous = obj->BasePriority;
    obj->BasePriority = (int32_t)increment;
    /* Under RECOMP_SCHED_TRACE, which thread is being given which priority.
     * This matters more than it looks: JSRF runs threads whose entire body is a
     * counting spin loop, and such a thread is only affordable if it actually
     * runs at the bottom of the scheduler. Whether the guest asks for that, and
     * for which handle, is not something to assume. */
    sched_note("KeSetBasePriority", obj->HandleToken, (uint32_t)increment);
    /* Apply to the real thread through the handle the object stands for. */
    xbox_KeSetBasePriorityThread(
        bridge_thread_handle_for_token(obj->HandleToken), increment);
    g_eax = (uint32_t)previous;
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    /* The Protect argument was read and then dropped, so a caller that mapped
     * I/O space read-only and later asked MmQueryAddressProtect what it had
     * got back the ledger's default instead of its own value. Recorded here
     * for the same reason NtProtectVirtualMemory records it. */
    if (xbox_va && protect)
        bridge_prot_set(xbox_va, num_bytes, protect);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u prot=0x%X → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, protect, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees.
 *
 * The allocation side matters here: bridge_ExAllocatePool and
 * bridge_ExAllocatePoolWithTag both take the block from xbox_HeapAlloc and
 * hand back a guest VA. So the free has to go back to the same allocator.
 * Routing it to xbox_ExFreePool -- which calls the host HeapFree on the
 * translated pointer -- hands the host allocator an address it never issued,
 * inside the guest mapping. */
static void bridge_ExFreePool(void)
{
    xbox_HeapFree(STACK_ARG(0));
    g_eax = 0;
}

/* ── IoCreateDevice / IoDeleteDevice (ordinals 65 and 68) ─────────────────
 *
 * These live here rather than routing to xbox_IoCreateDevice in kernel_io.c.
 * That implementation HeapAllocs from the host process heap and hands the
 * caller a native pointer; the ABI on this side is 32-bit guest addresses in
 * mapped guest memory, and on a 64-bit host the two cannot be reconciled by
 * address translation -- writing a host pointer through the caller's 4-byte
 * out-parameter truncates it and clobbers the neighbouring guest dword.
 *
 * What the guest ABI actually requires is proven from the call site and the
 * instructions that follow it (JSRF, XPP section):
 *
 *   push 0x1bc818       ; DriverObject
 *   push 0x170          ; DeviceExtensionSize
 *   push eax            ; DeviceName   (ANSI_STRING, guest VA)
 *   push 0x3a           ; DeviceType
 *   push 0              ; Exclusive
 *   push eax            ; DeviceObject out-slot (guest VA of one dword)
 *   call [IoCreateDevice]
 *   test eax, eax
 *   jl   <failure>                       ; negative NTSTATUS is failure
 *   mov  eax, [ebp-4]                    ; the dword we wrote back
 *   mov  edx, [eax+0x18]                 ; DeviceExtension, a guest address
 *   mov  ecx, 0x5c / rep stosd           ; clears 0x170 bytes through it
 *   mov  [edx], eax                      ; stores the DEVICE_OBJECT address
 *   mov  byte ptr [eax+0x1e], 1
 *   or   dword ptr [eax+0x14], 4
 *   and  dword ptr [eax+0x14], 0xffffffef
 *
 * So: the out-slot is four bytes wide and holds a guest address; +0x18 of the
 * object is a guest address; the extension is at least DeviceExtensionSize
 * bytes; and the object itself is written at +0x14 and +0x1e.
 */

/* Bytes of guest DEVICE_OBJECT allocated per device.
 *
 * The highest offset any observed caller touches is +0x1E. The allocation is
 * larger than that so a field this title happens not to touch reads back zero
 * from inside the object rather than reading into whatever the allocator put
 * next; the extra bytes carry no claimed meaning and stay zero. */
#define XBOX_GUEST_DEVICE_OBJECT_SIZE   0x38u

/* Guest-layout DEVICE_OBJECT. Fixed-width fields only: this structure is
 * addressed by the guest, so no member may vary with host pointer width.
 * Only the three fields named below are proven; everything else is reserved
 * padding kept at zero rather than invented semantics. */
typedef struct XboxGuestDeviceObject {
    uint8_t  Reserved00[0x14];  /* +0x00 unproven, left zero */
    uint32_t Dword14;           /* +0x14 bit field the caller ORs and ANDs */
    uint32_t DeviceExtension;   /* +0x18 GUEST address of the extension */
    uint8_t  Reserved1C[2];     /* +0x1C unproven, left zero */
    uint8_t  Byte1E;            /* +0x1E byte the caller sets to 1 */
    uint8_t  Reserved1F[XBOX_GUEST_DEVICE_OBJECT_SIZE - 0x1Fu];
} XboxGuestDeviceObject;

_Static_assert(offsetof(XboxGuestDeviceObject, DeviceExtension) == 0x18,
               "guest DEVICE_OBJECT: DeviceExtension must be at +0x18");
_Static_assert(sizeof(uint32_t) == 4,
               "guest DEVICE_OBJECT: DeviceExtension must be a 32-bit guest address");
_Static_assert(offsetof(XboxGuestDeviceObject, Dword14) == 0x14,
               "guest DEVICE_OBJECT: +0x14 field misplaced");
_Static_assert(offsetof(XboxGuestDeviceObject, Byte1E) == 0x1E,
               "guest DEVICE_OBJECT: +0x1E field misplaced");
_Static_assert(sizeof(XboxGuestDeviceObject) == XBOX_GUEST_DEVICE_OBJECT_SIZE,
               "guest DEVICE_OBJECT: padded to a size the host chose");

/* Host-side lifetime bookkeeping, deliberately NOT stored in the guest object.
 * The extension address also lives at +0x18 where the guest can see it, but
 * guest-writable memory is not a free list: a title that reuses that field
 * would have IoDeleteDevice hand the heap an address it never allocated. This
 * table is also what lets IoDeleteDevice ignore a pointer it never issued. */
#define XBOX_MAX_GUEST_DEVICES 32

static struct {
    uint32_t device_va;
    uint32_t extension_va;
} g_guest_devices[XBOX_MAX_GUEST_DEVICES];

static void bridge_device_record(uint32_t device_va, uint32_t extension_va)
{
    int i;
    for (i = 0; i < XBOX_MAX_GUEST_DEVICES; i++) {
        if (!g_guest_devices[i].device_va) {
            g_guest_devices[i].device_va = device_va;
            g_guest_devices[i].extension_va = extension_va;
            return;
        }
    }
    /* Out of slots: the device still works, only its extension leaks on
     * delete. Say so rather than silently dropping the record. */
    fprintf(stderr, "  [KERNEL] IoCreateDevice: device table full (%d), "
            "extension 0x%08X will leak on delete\n",
            XBOX_MAX_GUEST_DEVICES, extension_va);
    fflush(stderr);
}

/* ── IoCreateDevice (ordinal 65, 6 args)
 * NTSTATUS IoCreateDevice(PVOID DriverObject, ULONG DeviceExtensionSize,
 *                         PANSI_STRING DeviceName, ULONG DeviceType,
 *                         BOOLEAN Exclusive, PVOID *DeviceObject)
 */
static void bridge_IoCreateDevice(void)
{
    uint32_t driver_object   = STACK_ARG(0);
    uint32_t extension_size  = STACK_ARG(1);
    uint32_t device_name_va  = STACK_ARG(2);
    uint32_t device_type     = STACK_ARG(3);
    uint32_t exclusive       = STACK_ARG(4);
    uint32_t out_va          = STACK_ARG(5);
    uint32_t device_va;
    uint32_t extension_va = 0;
    XboxGuestDeviceObject *device;
    static int calls = 0;

    (void)exclusive;

    if (!out_va) {
        g_eax = (uint32_t)STATUS_INVALID_PARAMETER;
        return;
    }

    /* Both allocations come from the guest heap, the same allocator
     * ExAllocatePool and MmAllocateContiguousMemory use, so every address
     * handed back is a guest VA the title can dereference. xbox_HeapAlloc
     * zeroes what it returns, which is what the console does and what the
     * caller assumes of the fields it does not write itself. */
    device_va = xbox_HeapAlloc(XBOX_GUEST_DEVICE_OBJECT_SIZE, 16);
    if (!device_va) {
        BRIDGE_MEM32(out_va) = 0;
        g_eax = (uint32_t)STATUS_INSUFFICIENT_RESOURCES;
        return;
    }

    if (extension_size) {
        extension_va = xbox_HeapAlloc(extension_size, 16);
        if (!extension_va) {
            xbox_HeapFree(device_va);   /* unwind the object we just took */
            BRIDGE_MEM32(out_va) = 0;
            g_eax = (uint32_t)STATUS_INSUFFICIENT_RESOURCES;
            return;
        }
    }

    /* Host pointer used only to reach guest memory; the value stored is the
     * guest address, never the pointer. */
    device = (XboxGuestDeviceObject *)XBOX_TO_NATIVE(device_va);
    device->DeviceExtension = extension_va;
    /* Upstream fills the remaining observed Xbox header fields. Keep local
     * allocation bookkeeping so IoDeleteDevice releases both allocations. */
    BRIDGE_MEM16(device_va + 0x02) = XBOX_GUEST_DEVICE_OBJECT_SIZE;
    BRIDGE_MEM32(device_va + 0x04) = 1;
    BRIDGE_MEM32(device_va + 0x08) = driver_object;
    BRIDGE_MEM8(device_va + 0x1C) = (uint8_t)device_type;
    BRIDGE_MEM8(device_va + 0x1D) = 1;

    bridge_device_record(device_va, extension_va);

    /* Exactly four bytes, the width of the guest's out-slot. */
    BRIDGE_MEM32(out_va) = device_va;

    if (++calls <= 8) {
        uint32_t name_len = device_name_va ? BRIDGE_MEM16(device_name_va) : 0;
        uint32_t name_buf = device_name_va ? BRIDGE_MEM32(device_name_va + 4) : 0;
        fprintf(stderr,
                "  [KERNEL] IoCreateDevice #%d: name='%.*s' type=0x%X excl=%u "
                "ext_size=0x%X -> device=0x%08X ext=0x%08X "
                "(out slot 0x%08X <- 4 bytes 0x%08X, [device+0x18]=0x%08X)\n",
                calls, (int)name_len,
                name_buf ? (const char *)XBOX_TO_NATIVE(name_buf) : "",
                device_type, exclusive, extension_size,
                device_va, extension_va, out_va, BRIDGE_MEM32(out_va),
                BRIDGE_MEM32(device_va + 0x18));
        fflush(stderr);
    }

    g_eax = (uint32_t)STATUS_SUCCESS;
}

/* ── IoDeleteDevice (ordinal 68, 1 arg)
 * VOID IoDeleteDevice(PVOID DeviceObject)
 *
 * Symmetric with the above: releases the extension and then the object, both
 * back to the guest heap. A pointer this bridge never issued is ignored --
 * the guest heap frees by address, so passing it an arbitrary VA would either
 * do nothing or retire a block that belongs to something else.
 */
static void bridge_IoDeleteDevice(void)
{
    uint32_t device_va = STACK_ARG(0);
    int i;

    g_eax = 0;
    if (!device_va) {
        return;
    }

    for (i = 0; i < XBOX_MAX_GUEST_DEVICES; i++) {
        if (g_guest_devices[i].device_va != device_va) {
            continue;
        }
        if (g_guest_devices[i].extension_va) {
            xbox_HeapFree(g_guest_devices[i].extension_va);
        }
        xbox_HeapFree(device_va);
        g_guest_devices[i].device_va = 0;
        g_guest_devices[i].extension_va = 0;
        return;
    }

    fprintf(stderr, "  [KERNEL] IoDeleteDevice: 0x%08X was not created by "
            "IoCreateDevice, ignoring\n", device_va);
    fflush(stderr);
}

/* ── KeDisconnectInterrupt (ordinal 100, 1 arg) */
static void bridge_KeDisconnectInterrupt(void)
{
    g_eax = (uint32_t)xbox_KeDisconnectInterrupt(
        (PXBOX_KINTERRUPT)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
/* Who is waiting for whom.
 *
 * With the arena fixed, JSRF still stops all file I/O a quarter of the way
 * into a run and animates its loading screen for the rest. What it does
 * instead is drive its own cooperative scheduler: 39,130 NtResumeThread and
 * 37,883 NtSuspendThread calls, 42,762 KeSetBasePriorityThread, and 3.9M
 * critical-section pairs, from a handful of sites around 0x00147C7A.
 *
 * "The scheduler is spinning" is not a diagnosis. This records the calling
 * thread, the target handle and the guest return address for each scheduling
 * primitive, so the question becomes which thread is blocked on which -- and
 * whether anything is ever actually resumed. RECOMP_SCHED_TRACE only.
 */
static int sched_trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_SCHED_TRACE") != NULL;
    return on;
}

static void sched_note(const char *what, uint32_t handle, uint32_t extra)
{
    static unsigned long seen;
    unsigned long n = ++seen;
    /* Every call early, then a thinning sample: the pattern is what matters
     * and a three-minute run makes millions of these. */
    if (n > 200 && (n % 5000) != 0) return;
    fprintf(stderr, "  [SCHED] %-18s #%lu host_thread=%lu handle=0x%08X"
                    " stack_top=0x%08X ra=0x%08X extra=0x%08X\n",
            what, n, (unsigned long)GetCurrentThreadId(), handle,
            g_thread_stack_top, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0, extra);
    fflush(stderr);
}

static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    if (sched_trace_on()) sched_note("NtSuspendThread", STACK_ARG(0), 0);
    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
    if (sched_trace_on() && count_va)
        sched_note("  -> prev count", STACK_ARG(0), BRIDGE_MEM32(count_va));
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    /* PreviousSuspendCount is optional, and XBOX_TO_NATIVE(0) is not NULL --
     * it is the base of the guest mapping, so a caller passing NULL had four
     * bytes written to guest address 0 on every resume. NtSuspendThread beside
     * this one already guards; this did not. JSRF drives its own cooperative
     * scheduler and issues 39,130 resumes in a three-minute run. */
    uint32_t count_va = STACK_ARG(1);

    if (sched_trace_on()) sched_note("NtResumeThread", STACK_ARG(0), 0);
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
    if (sched_trace_on() && count_va)
        sched_note("  -> prev count", STACK_ARG(0), BRIDGE_MEM32(count_va));
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ── DbgPrint (ordinal 8) ─────────────────────────────────
 *
 * ULONG __cdecl DbgPrint(PCSTR Format, ...)
 *
 * The one varargs export in the table, and the reason it needs its own
 * formatter rather than a forward to xbox_DbgPrint: the arguments are on the
 * *guest* stack, in guest layout, and a `%s` among them is a guest VA. Handing
 * that list to the host's vsnprintf would print host memory at a guest
 * address. So the conversions are walked here and each one is rendered
 * individually, pulling exactly as many guest dwords as its type spends.
 *
 * What the width of a guest argument is, per conversion:
 *   d i u o x X c   one dword           (char/short are promoted to int)
 *   ll / I64 forms  two dwords, low first (little-endian)
 *   f e E g G a A   two dwords -- a float is promoted to double by the
 *                   default argument promotions, so 8 bytes even for %f
 *   s p             one dword, a guest VA
 *
 * %p prints the guest VA, not the host address it maps to. A pointer in this
 * title's log is only useful if it can be matched against the title's own
 * addresses.
 */
static uint32_t bridge_dbgprint_arg(int *slot)
{
    return (uint32_t)BRIDGE_MEM32(g_esp + (*slot)++ * 4);
}

/* Render one conversion into `out`. `spec` is the format the guest wrote, with
 * any length modifier already stripped -- the host's own modifier is supplied
 * here to match the C type actually being passed. */
static int bridge_dbgprint_one(char *out, size_t out_sz, const char *spec,
                               char conv, int is64, int *slot)
{
    char host[64];
    size_t n = strlen(spec);

    if (n + 8 >= sizeof(host))
        return 0;

    switch (conv) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
        if (is64) {
            uint32_t lo = bridge_dbgprint_arg(slot);
            uint32_t hi = bridge_dbgprint_arg(slot);
            uint64_t v  = ((uint64_t)hi << 32) | lo;
            /* Splice "ll" in ahead of the conversion character. */
            memcpy(host, spec, n - 1);
            host[n - 1] = 'l'; host[n] = 'l'; host[n + 1] = conv; host[n + 2] = '\0';
            if (conv == 'd' || conv == 'i')
                return snprintf(out, out_sz, host, (long long)v);
            return snprintf(out, out_sz, host, (unsigned long long)v);
        } else {
            uint32_t v = bridge_dbgprint_arg(slot);
            memcpy(host, spec, n + 1);
            if (conv == 'd' || conv == 'i')
                return snprintf(out, out_sz, host, (int)(int32_t)v);
            return snprintf(out, out_sz, host, (unsigned)v);
        }
    }
    case 'c': {
        uint32_t v = bridge_dbgprint_arg(slot);
        memcpy(host, spec, n + 1);
        return snprintf(out, out_sz, host, (int)(v & 0xFF));
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
    case 'a': case 'A': {
        uint32_t lo = bridge_dbgprint_arg(slot);
        uint32_t hi = bridge_dbgprint_arg(slot);
        uint64_t bits = ((uint64_t)hi << 32) | lo;
        double d;
        memcpy(&d, &bits, sizeof(d));
        memcpy(host, spec, n + 1);
        return snprintf(out, out_sz, host, d);
    }
    case 's': {
        uint32_t va = bridge_dbgprint_arg(slot);
        const char *p = va ? (const char *)XBOX_TO_NATIVE(va) : NULL;
        memcpy(host, spec, n + 1);
        /* A null or unmapped string is a title bug worth seeing spelled out
         * rather than a crash inside the logger. */
        return snprintf(out, out_sz, host, p ? p : "(null)");
    }
    case 'p': {
        uint32_t va = bridge_dbgprint_arg(slot);
        return snprintf(out, out_sz, "0x%08X", va);
    }
    default:
        /* An unrecognised conversion consumes nothing: guessing a width here
         * would desynchronise every argument after it, which turns one unknown
         * conversion into a whole garbled line. Show it verbatim instead. */
        return snprintf(out, out_sz, "%s", spec);
    }
}

static void bridge_DbgPrint(void)
{
    uint32_t fmt_va = STACK_ARG(0);
    const char *fmt;
    char buf[1024];
    size_t out = 0;
    int slot = 1;              /* guest dword 0 is the format pointer */
    int i = 0;

    if (!fmt_va) {
        g_eax = 0;
        return;
    }
    fmt = (const char *)XBOX_TO_NATIVE(fmt_va);

    /* The format string is guest memory and nothing guarantees it is
     * terminated. Bounding the walk keeps a corrupt one from reading its way
     * out of the mapping; no real format is anywhere near this long. */
    while (i < 4096 && fmt[i] && out + 1 < sizeof(buf)) {
        if (fmt[i] != '%') {
            buf[out++] = fmt[i++];
            continue;
        }
        if (fmt[i + 1] == '%') {
            buf[out++] = '%';
            i += 2;
            continue;
        }

        /* Copy the whole conversion through to its conversion character,
         * dropping the guest's length modifier -- the host's is decided by the
         * argument width above, not by what the guest wrote. */
        {
            char spec[48];
            size_t sn = 0;
            int is64 = 0;
            char conv = '\0';
            int n;

            spec[sn++] = fmt[i++];                       /* '%' */
            while (fmt[i] && strchr("-+ #0", fmt[i]) && sn + 1 < sizeof(spec))
                spec[sn++] = fmt[i++];                   /* flags */
            while (fmt[i] && (fmt[i] == '*' || (fmt[i] >= '0' && fmt[i] <= '9'))
                   && sn + 1 < sizeof(spec)) {
                /* `*` takes its width from an argument; consume that dword so
                 * the ones after it still line up. Rendered as a literal
                 * width so the host format needs no extra argument. */
                if (fmt[i] == '*') {
                    int w = (int)(int32_t)bridge_dbgprint_arg(&slot);
                    n = snprintf(spec + sn, sizeof(spec) - sn, "%d", w);
                    if (n < 0 || (size_t)n >= sizeof(spec) - sn) break;
                    sn += (size_t)n;
                    i++;
                } else {
                    spec[sn++] = fmt[i++];
                }
            }
            if (fmt[i] == '.' && sn + 1 < sizeof(spec)) {
                spec[sn++] = fmt[i++];                   /* precision */
                while (fmt[i] && (fmt[i] == '*' || (fmt[i] >= '0' && fmt[i] <= '9'))
                       && sn + 1 < sizeof(spec)) {
                    if (fmt[i] == '*') {
                        int w = (int)(int32_t)bridge_dbgprint_arg(&slot);
                        n = snprintf(spec + sn, sizeof(spec) - sn, "%d", w);
                        if (n < 0 || (size_t)n >= sizeof(spec) - sn) break;
                        sn += (size_t)n;
                        i++;
                    } else {
                        spec[sn++] = fmt[i++];
                    }
                }
            }
            /* Length modifiers. MSVC's I64 spelling is what an XDK-era title
             * emits; ll is accepted too. Both mean two guest dwords. */
            for (;;) {
                if (fmt[i] == 'l' && fmt[i + 1] == 'l') { is64 = 1; i += 2; continue; }
                if (fmt[i] == 'I' && fmt[i + 1] == '6' && fmt[i + 2] == '4') {
                    is64 = 1; i += 3; continue;
                }
                /* `l`, `h`, `hh`, `w`, `L`, `z`, `t` all still arrive as one
                 * promoted dword on a 32-bit guest, so they are dropped. */
                if (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'w'
                    || fmt[i] == 'L' || fmt[i] == 'z' || fmt[i] == 't') {
                    i++;
                    continue;
                }
                break;
            }
            if (!fmt[i]) break;                          /* truncated format */
            conv = fmt[i++];
            if (sn + 2 >= sizeof(spec)) break;
            spec[sn++] = conv;
            spec[sn] = '\0';

            n = bridge_dbgprint_one(buf + out, sizeof(buf) - out, spec,
                                    conv, is64, &slot);
            if (n < 0) break;
            out += (size_t)n;
            if (out >= sizeof(buf)) { out = sizeof(buf) - 1; break; }
        }
    }
    buf[out] = '\0';

    fprintf(stderr, "[GUEST] %s", buf);
    if (out == 0 || buf[out - 1] != '\n')
        fputc('\n', stderr);
    fflush(stderr);

    /* DbgPrint returns the character count. __cdecl: the caller cleans, and
     * stdcall_args_for_ordinal() already reports 0 for this ordinal. */
    g_eax = (uint32_t)out;
}

/* ── IoDismountVolumeByName (ordinal 91) ──────────────────
 *
 * The ANSI_STRING is rebuilt locally with its Buffer translated, the same way
 * bridge_RtlEqualString does it: the struct's Buffer field holds a guest VA,
 * so handing the guest struct straight to a native function would have it
 * dereference a guest address as a host pointer.
 */
static void bridge_IoDismountVolumeByName(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDismountVolumeByName(&name);
}

/* ── KeSetDisableBoostThread (ordinal 144) ────────────────
 *
 * BOOLEAN KeSetDisableBoostThread(PKTHREAD Thread, BOOLEAN Disable)
 *
 * The argument is a guest PKTHREAD, and this runtime never materialises KTHREAD
 * objects in guest memory -- host threads are reached through the handle tokens
 * bridge_handle_token issues. So there is no general guest-pointer-to-thread
 * mapping to consult, and inventing one by casting the VA to a HANDLE is the
 * mistake bridge_resolve_handle's note spells out at length.
 *
 * What the contract actually requires is narrower than "change the schedule",
 * and it is worth separating the two:
 *
 *   - The scheduling half does not exist on this host at all. POSIX has no
 *     wakeup priority boost, so there is nothing to disable; see
 *     SetThreadPriorityBoost in win32_compat.c.
 *   - The bookkeeping half does matter. The idiom is save-set-restore, and a
 *     version that always answered FALSE would have every caller restore a
 *     state the thread was never in.
 *
 * So the flag is kept per guest thread pointer. A title that passes a handle
 * token instead -- some do, the argument is a pointer either way -- is routed
 * to the real thread object, which keeps it consistent with anything else that
 * reads the flag back through the host.
 */
#define BRIDGE_BOOST_MAX 64
static struct { uint32_t va; uint8_t disabled; } s_boost_flags[BRIDGE_BOOST_MAX];

static void bridge_KeSetDisableBoostThread(void)
{
    uint32_t thread_va = STACK_ARG(0);
    uint32_t disable   = STACK_ARG(1);
    HANDLE   h         = bridge_resolve_handle(thread_va);
    int i;

    if (h) {
        BOOL previous = FALSE;
        if (!GetThreadPriorityBoost(h, &previous))
            previous = FALSE;
        SetThreadPriorityBoost(h, disable ? TRUE : FALSE);
        g_eax = previous ? 1 : 0;
        return;
    }

    if (!thread_va) {
        g_eax = 0;
        return;
    }

    for (i = 0; i < BRIDGE_BOOST_MAX; i++) {
        if (s_boost_flags[i].va == thread_va) {
            g_eax = s_boost_flags[i].disabled;
            s_boost_flags[i].disabled = (uint8_t)(disable != 0);
            return;
        }
    }
    for (i = 0; i < BRIDGE_BOOST_MAX; i++) {
        if (s_boost_flags[i].va == 0) {
            s_boost_flags[i].va = thread_va;
            s_boost_flags[i].disabled = (uint8_t)(disable != 0);
            g_eax = 0;      /* never set before, so it was not disabled */
            return;
        }
    }

    /* More distinct thread objects than the table holds. Reporting the
     * previous state as FALSE is what a never-seen thread gets anyway; say so
     * once rather than silently starting to lie about save-restore. */
    {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "  [KERNEL] KeSetDisableBoostThread: more than %d "
                    "thread objects seen; previous-state tracking stops here\n",
                    BRIDGE_BOOST_MAX);
            fflush(stderr);
        }
    }
    g_eax = 0;
}

/* ── Wrappers over existing xbox_* implementations ────────
 *
 * Every one of these forwards to a function in src/kernel that was written for
 * a native caller. That is only safe where the function does nothing with an
 * address except read or write the bytes at it: an allocator, a free, or
 * anything returning a pointer would put a host address into the guest ABI,
 * which is the failure tools/kernel_audit/coverage.py describes. So the ones
 * that allocate (MmAllocateSystemMemory, MmFreeSystemMemory) and the ones that
 * take IRP or DEVICE_OBJECT graphs (IoStartPacket, IofCompleteRequest,
 * IoInvalidDeviceRequest, IoMarkIrpMustComplete) are deliberately still
 * unrouted -- they need a design decision about who owns the object, not a
 * wrapper.
 */

/* ── AvSetSavedDataAddress (4) ────────────────────────────
 * A stored value, not a dereferenced one: what goes in is a guest VA and the
 * same guest VA comes back out, so there is no translation to do and none to
 * get wrong. The matching getter (ordinal 1) is bridge_AvGetSavedDataAddress,
 * already defined with the Halo wrapper group above. */
static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress((ULONG)STACK_ARG(0));
    g_eax = 0;
}

/* ── HalReadWritePCISpace (46) ────────────────────────────
 * Bus/slot/register/length are scalars; only Buffer is an address, and it is
 * read or written in place. */
static void bridge_HalReadWritePCISpace(void)
{
    xbox_HalReadWritePCISpace(STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
                              XBOX_TO_NATIVE(STACK_ARG(3)),
                              STACK_ARG(4), (BOOLEAN)(STACK_ARG(5) != 0));
    g_eax = 0;
}

/* ── IoDeleteSymbolicLink (69) ────────────────────────────
 * The ANSI_STRING is rebuilt with a translated Buffer, the same reason
 * bridge_IoDismountVolumeByName does it. */
static void bridge_IoDeleteSymbolicLink(void)
{
    uint32_t name_va = STACK_ARG(0);
    XBOX_ANSI_STRING name;

    if (!name_va) {
        g_eax = 0xC000000Du;                 /* STATUS_INVALID_PARAMETER */
        return;
    }
    name.Length        = BRIDGE_MEM16(name_va + 0);
    name.MaximumLength = BRIDGE_MEM16(name_va + 2);
    name.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(name_va + 4));

    g_eax = (uint32_t)xbox_IoDeleteSymbolicLink(&name);
}

/* ── KeRemoveQueueDpc (137) ───────────────────────────────
 *
 * The one in this group that must NOT forward to its xbox_* function, and the
 * reason is invisible to a coverage tool: there are two DPC queues, and the
 * native one is not the live one.
 *
 * bridge_KeInsertQueueDpc never calls xbox_KeInsertQueueDpc. A DPC raised
 * inside an ISR is parked in g_pending_dpc and run when the ISR returns; one
 * raised outside is run immediately by bridge_run_dpc. Either way the native
 * queue stays empty, so xbox_KeRemoveQueueDpc would search a list nothing ever
 * inserts into and answer FALSE every time -- while the DPC the caller was
 * trying to cancel stayed parked in g_pending_dpc and fired anyway.
 *
 * A cancel that reports success and then fires is worse than no cancel at all,
 * so this reads the queue that actually holds the DPC.
 */
static void bridge_KeRemoveQueueDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);

    if (dpc_va && g_pending_dpc == dpc_va) {
        g_pending_dpc = 0;
        g_pending_dpc_sys1 = 0;
        g_pending_dpc_sys2 = 0;
        g_eax = 1;               /* it was queued, and now it is not */
        return;
    }
    /* Not queued: either it already ran -- bridge_run_dpc is synchronous
     * outside an ISR, so most DPCs are finished before anyone could cancel
     * them -- or it was never inserted. FALSE is correct for both. */
    g_eax = 0;
}

/* ── MmLockUnlockPhysicalPage (176) ───────────────────────
 * Both arguments are scalars. The native implementation tracks the request
 * without pinning anything, which is the honest model here: guest physical
 * memory is a host mapping that never moves. */
static void bridge_MmLockUnlockPhysicalPage(void)
{
    xbox_MmLockUnlockPhysicalPage((ULONG_PTR)STACK_ARG(0),
                                  (BOOLEAN)(STACK_ARG(1) != 0));
    g_eax = 0;
}

/* ── RtlCompareMemoryUlong (269) ──────────────────────────
 * Reads Length bytes at Source and returns how many matched. Nothing escapes. */
static void bridge_RtlCompareMemoryUlong(void)
{
    uint32_t src = STACK_ARG(0);

    if (!src) {
        g_eax = 0;
        return;
    }
    g_eax = (uint32_t)xbox_RtlCompareMemoryUlong(XBOX_TO_NATIVE(src),
                                                 STACK_ARG(1), STACK_ARG(2));
}

/* ── RtlTimeFieldsToTime (304) ────────────────────────────
 * TIME_FIELDS is eight SHORTs and LARGE_INTEGER is eight bytes -- neither
 * carries a pointer, so both translate directly. */
static void bridge_RtlTimeFieldsToTime(void)
{
    uint32_t fields_va = STACK_ARG(0);
    uint32_t time_va   = bridge_checked_out_va(STACK_ARG(1), 8,
                                               "RtlTimeFieldsToTime", "Time");

    if (!fields_va || !time_va) {
        g_eax = 0;                           /* FALSE */
        return;
    }
    g_eax = xbox_RtlTimeFieldsToTime(
                (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(fields_va),
                (PLARGE_INTEGER)XBOX_TO_NATIVE(time_va)) ? 1 : 0;
}

/* ── HalIsResetOrShutdownPending (358) / HalInitiateShutdown (360) ──
 * Both are void-in, and neither touches guest memory. */
static void bridge_HalIsResetOrShutdownPending(void)
{
    g_eax = xbox_HalIsResetOrShutdownPending() ? 1 : 0;
}

static void bridge_HalInitiateShutdown(void)
{
    xbox_HalInitiateShutdown();
    g_eax = 0;
}

/* ── MmAllocateSystemMemory (167) / MmFreeSystemMemory (172) ──
 *
 * PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 * VOID  MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
 *
 * Written against the guest memory model rather than forwarded, for the reason
 * the NOT-ROUTED note gives and this pair demonstrates exactly:
 * xbox_MmAllocateSystemMemory returns VirtualAlloc's pointer, which is a host
 * address in the host's address space. The guest receives it in eax, 32 bits
 * wide, and then dereferences it -- so forwarding would hand the title a
 * truncated host pointer for every allocation. It is the IoCreateDevice
 * failure with the truncation moved from an out-parameter into the return
 * value.
 *
 * So the allocation comes from the guest heap, which is what the title can
 * actually address, and the protection is recorded in the same ledger
 * NtProtectVirtualMemory and MmQueryAddressProtect read. A title that
 * allocates with PAGE_READWRITE and later asks what the page is now gets the
 * answer it set rather than a default.
 *
 * MmFreeSystemMemory goes to xbox_HeapFree, which is a bump allocator's no-op.
 * That is not a leak this bridge introduces -- it is the heap's existing
 * behaviour, shared with MmFreeContiguousMemory -- but it is the reason the
 * size argument is accepted and ignored rather than checked against the block.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    uint32_t size    = STACK_ARG(0);
    uint32_t protect = STACK_ARG(1);
    uint32_t xbox_va;

    if (!size) {
        g_eax = 0;
        return;
    }

    /* Page granularity: this export's callers treat the result as page-backed,
     * and MmQueryAddressProtect answers per page. */
    xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va) {
        g_eax = 0;          /* NULL, which is this export's failure return */
        return;
    }

    if (protect)
        bridge_prot_set(xbox_va, (size + 0xFFFu) & ~0xFFFu, protect);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateSystemMemory: size=%u protect=0x%X"
                " → Xbox VA 0x%08X\n", size, protect, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_MmFreeSystemMemory(void)
{
    uint32_t addr = STACK_ARG(0);

    /* NumberOfBytes (STACK_ARG(1)) is deliberately unread: the heap frees by
     * base address and a partial free is not something it can express. */
    if (addr)
        xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── The IRP group (74, 81, 83, 87, 359) ──────────────────
 *
 * These are the last of JSRF's imports, and they are routed together because
 * the same sentence covers all five: there is no IRP model here. The title
 * links the XDK's device-driver surface, the XDK references these exports, and
 * nothing in this runtime builds or dispatches an IRP -- file I/O goes through
 * the Nt* exports directly to the host filesystem, so no packet is ever
 * constructed to start, complete or mark.
 *
 * Four of them are therefore no-ops, and their xbox_* implementations already
 * were. Routing them changes no behaviour; what it changes is that "no bridge
 * for ordinal N" stops being printed for a decision that has been made. A
 * warning that fires for deliberate no-ops teaches the reader to ignore it,
 * and it is the only thing standing between a genuinely missing export and
 * being noticed.
 *
 * IoInvalidDeviceRequest is the exception and the reason this group was worth
 * touching at all. It is not a stub: it is the default dispatch entry for
 * major functions a driver does not implement, and its entire contract is to
 * REJECT. Unrouted, the generic stub answered 0 -- STATUS_SUCCESS -- so every
 * unhandled major function reported that it had worked. It now returns what it
 * is for.
 *
 * If a title ever does drive real IRPs, none of this is sufficient and the
 * no-ops become wrong rather than merely empty. That needs an IRP model and a
 * decision about which side owns the packet, which is a larger piece of work
 * than a wrapper.
 */
static void bridge_IoInvalidDeviceRequest(void)
{
    /* Neither argument is read; the answer does not depend on them. */
    g_eax = 0xC0000010u;                 /* STATUS_INVALID_DEVICE_REQUEST */
}

static void bridge_IoStartPacket(void)
{
    g_eax = 0;                           /* VOID */
}

static void bridge_IoStartNextPacket(void)
{
    g_eax = 0;                           /* VOID */
}

/* __fastcall: Irp in ecx, PriorityBoost in edx, and no stack arguments --
 * which is why stdcall_args_for_ordinal() reports 0 for this ordinal. Reading
 * STACK_ARG(0) here would pick up the caller's frame instead. */
static void bridge_IofCompleteRequest(void)
{
    (void)g_ecx;
    (void)g_edx;
    g_eax = 0;                           /* VOID */
}

static void bridge_IoMarkIrpMustComplete(void)
{
    g_eax = 0;                           /* VOID */
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  8;  /* IoStartNextPacket (2) */
    case  82: return 12;  /* IoStartNextPacketByKey (3) */
    case  83: return 16;  /* IoStartPacket (4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return 8;   /* NtQueryVirtualMemory (2) -- Xbox takes
                             BaseAddress and Info only, not NT's four */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 20;  /* NtWaitForMultipleObjectsEx (5) */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */

    /* ── Crypto ── */

    /* -1, not 0. A void function and an ordinal nobody has written down both
     * pop zero bytes, but only one of them is a bug, and the caller below has
     * to tell them apart -- the missing-bridge warning used to accuse
     * AvGetSavedDataAddress (ordinal 1, genuinely void) of corrupting the
     * caller's stack, every run, on both hosts. It cost an hour tonight. */
    default:  return -1;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;
    /* Routed, and checked against the memory-model bar in the NOT ROUTED note
     * below: this one allocates nothing, frees nothing and hands back no host
     * pointer. It writes three guest dwords at addresses the caller supplied
     * and keeps a ledger of what the title asked for; see its own comment for
     * exactly how far the emulation goes, and why it stops short of calling
     * host mprotect. */
    case 204: return bridge_NtProtectVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  17: return bridge_ExFreePool;
    case  23: return bridge_ExQueryPoolBlockSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* Floating-point state. Both implementations are no-ops -- the host
     * preserves FP state across its own context switches -- but routing them
     * matters anyway: the missing-bridge warning says a missing bridge "is
     * usually the reason a game misbehaves", and these two first appear in the
     * log at the exact tick the title screen advances, where they read as a
     * cause and are not one. */
    case 139: return bridge_KeRestoreFloatingPointState;
    case 142: return bridge_KeSaveFloatingPointState;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 125: return bridge_KeQueryInterruptTime;
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;
    case  97: return bridge_KeCancelTimer;
    case 119: return bridge_KeInsertQueueDpc;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    /* Routed at last. Not to xbox_KeSynchronizeExecution -- that takes a host
     * function pointer and this argument is a guest VA -- but to a bridge that
     * calls the routine through the dispatch table. See its comment for the
     * IRQL and locking decision, which was the part with a choice in it. */
    case 153: return bridge_KeSynchronizeExecution;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    /* Four of the wrappers the NOT-ROUTED note below left for individual
     * review, checked one at a time against the bar it sets and cleared:
     *
     *   100  KeDisconnectInterrupt  reads and clears one BOOLEAN field of the
     *        caller's KINTERRUPT and returns the previous value. One address,
     *        dereferenced in place, nothing allocated.
     *   252  PhyGetLinkState        scalar in, scalar out.
     *   253  PhyInitialize          scalars; its second argument is ignored.
     *   346  XcDESKeyParity         a no-op over a caller-supplied key buffer.
     *
     * XcRC4Key (338) and XcRC4Crypt (339) are NOT here, although they are the
     * same shape and the note names the Xc* group as one that should be fine.
     * They carry their own BISECT-OFF markers further down, and a bisect
     * result is a measurement -- outranking a judgement about the shape of the
     * code. Re-routing them needs a run that says they are safe, not an
     * argument that they ought to be. */
    case 100: return bridge_KeDisconnectInterrupt;
    case 252: return bridge_PhyGetLinkState;
    case 253: return bridge_PhyInitialize;
    case 346: return bridge_XcDESKeyParity;

    case   1: return bridge_AvGetSavedDataAddress;
    case   4: return bridge_AvSetSavedDataAddress;
    case  46: return bridge_HalReadWritePCISpace;
    case  69: return bridge_IoDeleteSymbolicLink;
    case 137: return bridge_KeRemoveQueueDpc;
    case  74: return bridge_IoInvalidDeviceRequest;
    case  81: return bridge_IoStartNextPacket;
    case  83: return bridge_IoStartPacket;
    case  87: return bridge_IofCompleteRequest;
    case 359: return bridge_IoMarkIrpMustComplete;
    case 167: return bridge_MmAllocateSystemMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 176: return bridge_MmLockUnlockPhysicalPage;
    case 269: return bridge_RtlCompareMemoryUlong;
    case 304: return bridge_RtlTimeFieldsToTime;
    case 358: return bridge_HalIsResetOrShutdownPending;
    case 360: return bridge_HalInitiateShutdown;

    case   8: return bridge_DbgPrint;
    case  91: return bridge_IoDismountVolumeByName;
    case 144: return bridge_KeSetDisableBoostThread;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   2: return bridge_AvSendTVEncoderOption;
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case  68: return bridge_IoDeleteDevice;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;


    /* BISECT-OFF case 338: bridge_XcRC4Key */
    /* BISECT-OFF case 339: bridge_XcRC4Crypt */


    /* NOT ROUTED, deliberately. The wrappers above exist and compile, and each
     * has a working xbox_* behind it, but routing them made Halo 2276 crash
     * EARLIER than leaving them stubbed -- twice, with two different faults.
     * Bisected to a memory-model mismatch, not to the wrappers' arithmetic:
     *
     *   xbox_IoCreateDevice HeapAllocs from GetProcessHeap() and writes that
     *   NATIVE pointer through its out-parameter. The bridge hands it the
     *   native address of a 4-BYTE GUEST slot, so a 64-bit pointer is written
     *   into 4 bytes: it clobbers the adjacent guest dword and leaves the title
     *   a truncated pointer it then dereferences. Crash was a write to
     *   0x90909090. (Ordinals 65 and 68 are now routed again -- not by calling
     *   the native xbox_* pair, but by a bridge written against the guest
     *   memory model: see bridge_IoCreateDevice above. The native versions
     *   stay for a native caller.)
     *
     *   xbox_ExFreePool calls HeapFree(GetProcessHeap(), P). Guest pool memory
     *   is not on the host heap, so P is a pointer HeapFree has never seen.
     *
     * These xbox_* functions were written for a NATIVE caller, where pointers
     * are host pointers and allocations are host allocations. The bridge is a
     * different world: pointers are guest VAs and memory lives in the mapped
     * guest space. XBOX_TO_NATIVE converts an address; it cannot convert an
     * allocator.
     *
     * So 'an xbox_* exists, therefore the wrapper is mechanical' is false, and
     * tools.kernel_audit.coverage no longer says it. Each of these needs its
     * memory model checked one at a time: which side owns the allocation, and
     * whether an out-pointer must carry a guest VA. Ones that only read or
     * write bytes at a caller-supplied address (the Xc* crypto group,
     * RtlTimeToTimeFields) should be fine; ones that allocate, free, or hand
     * back a pointer are not.
     *
     * Left in place rather than deleted: the wrappers are correct as argument
     * marshalling, and re-deriving them is the easy half of the work.
     */
    /* case  97: bridge_KeCancelTimer */
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    /* Routed for the same reason as 224 above, and it is the other half of the
     * same pair: without it a self-suspending worker spins at ~3M calls/sec. */
    case 231: return bridge_NtSuspendThread;
    /* Routed against the memory-model warning above, not assumed mechanical.
     * These three take a GUEST OBJECT POINTER that this bridge itself issued
     * from bridge_object_for_token, they read and write only fields of that
     * guest object, and the only host pointer involved is the thread HANDLE
     * resolved from the stored token -- which never reaches guest memory.
     * Nothing here allocates on behalf of the guest beyond the one object per
     * token, and nothing hands a host pointer back. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    case 250: return bridge_ObfDereferenceObject;
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    /* case 346: bridge_XcDESKeyParity */

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
#define XBOX_KERNEL_MAX_ORDINAL 400
static unsigned g_ordinal_calls[XBOX_KERNEL_MAX_ORDINAL];

/* Distinct guest call sites per ordinal.
 *
 * The histogram says WHICH kernel calls a title makes; it does not say who is
 * making them, and the existing per-call ret= line stops after 200 calls --
 * long before a spin gets going. Recording the first few distinct return
 * addresses per ordinal turns "something calls this 4 million times" into a
 * guest address to disassemble, which is the difference between reading the
 * loop and guessing at its semantics. */
#define XBOX_ORDINAL_SITES 4
static uint32_t g_ordinal_sites[XBOX_KERNEL_MAX_ORDINAL][XBOX_ORDINAL_SITES];

/* ── RECOMP_KERNEL_THREADS: which guest thread is still calling ───────────
 *
 * WHY THIS EXISTS. A player session on 18 Sep 2026 went black six minutes in
 * with the process still alive and presenting. The ordinal histogram is a
 * COMPLETE census and says exactly what stopped: every allocator, every free,
 * object create/close and timer arming went to zero across the 165,000 calls
 * that followed, while the per-frame present path kept running. What it cannot
 * say is WHICH THREAD stopped, because nothing in this tree records one.
 *
 * The only thread fingerprint that log carried was the `esp=` printed once per
 * periodic summary. Bucketed by stack region it does separate three guest
 * threads -- but at roughly one sample a second it cannot tell "stopped" from
 * "slowed down fourfold", and in that session it did not: the quiet thread's
 * last sample lands 26 s AFTER the freeze. Suggestive, and nothing more. That
 * is the gap this closes.
 *
 * g_fs_base is the guest TIB pointer and is RECOMP_TLS, so it already names the
 * calling guest thread with no bookkeeping at all. Bucket on it, keep the last
 * ordinal and the tick it arrived, and a freeze reads straight off the report:
 * thread T, N calls, last ordinal O, silent for M ms.
 *
 * The gate is at the CALL SITE, not in here, for two reasons: the hot path then
 * costs one cached int, and this function stays directly testable without the
 * environment. Opt-in via recomp_switch_on(), per the convention adopted after
 * a comment that said "defaults on" sat thirty lines above a gate that never
 * did.
 *
 * A thread with no TIB is still a thread, so fs_base 0 is folded to a sentinel
 * rather than dropped -- dropping it would silently under-count exactly the
 * unusual thread most likely to be interesting.
 */
#define KTHREAD_MAX 16u

/* WHICH ordinals, not just the most recent one.
 *
 * `last_ordinal` names whatever this thread happened to call last, which is
 * a sample of one. On 21 Sep 2026 two guest threads read last_ordinal=250
 * (ObfDereferenceObject) with 302 and 214 MILLION calls during a cutscene
 * hang, against 765k and 352k on ordinal 159 (KeWaitForSingleObject) in a
 * healthy run of the same build. That says the threads stopped waiting and
 * started spinning -- but NOT what they are spinning ON, because a loop of
 * five kernel calls reports only its last one, and a dereference is what
 * most handle-based idioms end with.
 *
 * A histogram names the loop. KORD_HIST is a cap, not the ordinal count: the
 * table is per thread and this is a diagnostic. It matches
 * XBOX_KERNEL_MAX_ORDINAL rather than being a round number: 384 would drop
 * the top sixteen ordinals silently, which is the kind of cap that makes an
 * instrument read zero for the one thing it was armed to find. */
#define KORD_HIST 400u

typedef struct {
    uint32_t fs_base;        /* guest TIB VA; 0 means the slot is free */
    unsigned long calls;
    unsigned int  last_ordinal;
    unsigned long last_tick;
    unsigned long first_tick;
    unsigned long ord[KORD_HIST];
} KThreadSlot;

static KThreadSlot g_kthreads[KTHREAD_MAX];
unsigned long g_kthread_distinct;
unsigned long g_kthread_over;    /* calls that found no free slot */

void xbox_bridge_note_thread_call(uint32_t fs_base, unsigned int ordinal,
                                  unsigned long tick)
{
    unsigned int i;

    if (fs_base == 0) fs_base = 0xFFFFFFFFu;

    for (i = 0; i < KTHREAD_MAX; ++i) {
        uint32_t cur = __atomic_load_n(&g_kthreads[i].fs_base, __ATOMIC_ACQUIRE);
        if (cur == fs_base) break;
        if (cur == 0) {
            uint32_t expect = 0;
            if (__atomic_compare_exchange_n(&g_kthreads[i].fs_base, &expect,
                                            fs_base, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&g_kthreads[i].first_tick, tick, __ATOMIC_RELAXED);
                __atomic_fetch_add(&g_kthread_distinct, 1ul, __ATOMIC_RELAXED);
                break;
            }
            /* Lost the claim. If the winner wanted the same thread, this slot
             * is still ours to use; otherwise keep walking. */
            if (expect == fs_base) break;
        }
    }
    if (i == KTHREAD_MAX) {
        __atomic_fetch_add(&g_kthread_over, 1ul, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&g_kthreads[i].calls, 1ul, __ATOMIC_RELAXED);
    if (ordinal < KORD_HIST)
        __atomic_fetch_add(&g_kthreads[i].ord[ordinal], 1ul, __ATOMIC_RELAXED);
    __atomic_store_n(&g_kthreads[i].last_ordinal, ordinal, __ATOMIC_RELAXED);
    __atomic_store_n(&g_kthreads[i].last_tick, tick, __ATOMIC_RELAXED);
}

/* Read-only accessor rather than exposing the table: the test needs to see a
 * slot, nothing needs to write one. Returns 0 when the slot is free. */
int xbox_bridge_kthread_slot(unsigned int i, uint32_t *fs_base,
                             unsigned long *calls, unsigned int *last_ordinal,
                             unsigned long *last_tick)
{
    uint32_t fs;
    if (i >= KTHREAD_MAX) return 0;
    fs = __atomic_load_n(&g_kthreads[i].fs_base, __ATOMIC_ACQUIRE);
    if (!fs) return 0;
    if (fs_base)      *fs_base      = fs;
    if (calls)        *calls        = __atomic_load_n(&g_kthreads[i].calls, __ATOMIC_RELAXED);
    if (last_ordinal) *last_ordinal = __atomic_load_n(&g_kthreads[i].last_ordinal, __ATOMIC_RELAXED);
    if (last_tick)    *last_tick    = __atomic_load_n(&g_kthreads[i].last_tick, __ATOMIC_RELAXED);
    return 1;
}

int xbox_bridge_kthreads_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_KERNEL_THREADS");
    return on;
}

/* Silent when unarmed would read exactly like "no threads", so say so. */
void xbox_bridge_dump_thread_census(unsigned long now)
{
    unsigned int i;

    if (!xbox_bridge_kthreads_on()) {
        fprintf(stderr, "  [KERNEL-THREADS] OFF"
                        " (RECOMP_KERNEL_THREADS=1 to arm)\n");
        return;
    }
    fprintf(stderr, "  [KERNEL-THREADS] %lu distinct, %lu calls with no slot"
                    " (cap %u):\n",
            g_kthread_distinct, g_kthread_over, (unsigned)KTHREAD_MAX);
    for (i = 0; i < KTHREAD_MAX; ++i) {
        uint32_t fs = __atomic_load_n(&g_kthreads[i].fs_base, __ATOMIC_ACQUIRE);
        unsigned long last, calls;
        if (!fs) continue;
        last  = __atomic_load_n(&g_kthreads[i].last_tick, __ATOMIC_RELAXED);
        calls = __atomic_load_n(&g_kthreads[i].calls, __ATOMIC_RELAXED);
        fprintf(stderr, "  [KERNEL-THREADS]   tib=0x%08X calls=%-10lu"
                        " last_ordinal=%-4u silent_for=%lu ms\n",
                fs, calls,
                __atomic_load_n(&g_kthreads[i].last_ordinal, __ATOMIC_RELAXED),
                now >= last ? now - last : 0ul);
        /* The five ordinals this thread actually spends its calls on. A
         * spinning thread names its loop here; a blocked one shows almost
         * nothing. Selection sort over a small fixed table, printed once every
         * few seconds, is not worth a better algorithm. */
        {
            unsigned shown, o;
            unsigned long best_n; unsigned best_o;
            unsigned long seen[5]; unsigned which[5];
            unsigned used = 0;
            for (shown = 0; shown < 5; ++shown) {
                best_n = 0; best_o = 0;
                for (o = 0; o < KORD_HIST; ++o) {
                    unsigned long c = __atomic_load_n(&g_kthreads[i].ord[o],
                                                      __ATOMIC_RELAXED);
                    unsigned d, dup = 0;
                    for (d = 0; d < used; ++d) if (which[d] == o) dup = 1;
                    if (!dup && c > best_n) { best_n = c; best_o = o; }
                }
                if (!best_n) break;
                seen[used] = best_n; which[used] = best_o; ++used;
            }
            if (used) {
                fprintf(stderr, "  [KERNEL-THREADS]     top ordinals:");
                for (shown = 0; shown < used; ++shown)
                    fprintf(stderr, " %u=%lu", which[shown], seen[shown]);
                fprintf(stderr, "\n");
            }
        }
    }
}

static void bridge_note_call_site(ULONG ordinal, uint32_t ret)
{
    int i;
    if (!ret || ordinal >= XBOX_KERNEL_MAX_ORDINAL) return;
    for (i = 0; i < XBOX_ORDINAL_SITES; i++) {
        if (g_ordinal_sites[ordinal][i] == ret) return;
        if (g_ordinal_sites[ordinal][i] == 0) {
            g_ordinal_sites[ordinal][i] = ret;
            return;
        }
    }
}

/* Dump the busiest ordinals, and separately every ordinal called at least once.
 * The second list is the useful one when asking "did the title ever call X?" */
void xbox_bridge_dump_ordinal_histogram(void)
{
    unsigned idx[XBOX_KERNEL_MAX_ORDINAL];
    int n = 0;
    for (unsigned i = 0; i < XBOX_KERNEL_MAX_ORDINAL; i++) {
        if (g_ordinal_calls[i]) idx[n++] = i;
    }
    /* Insertion sort by descending count; n is small and this runs every 2s. */
    for (int i = 1; i < n; i++) {
        unsigned v = idx[i];
        int j = i - 1;
        while (j >= 0 && g_ordinal_calls[idx[j]] < g_ordinal_calls[v]) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
    /* Built into one buffer and emitted with a single write: worker threads
     * log concurrently, and a per-entry fprintf loop interleaves with them,
     * producing a line with entries repeated out of order. That is not a
     * cosmetic problem -- it makes the histogram misread. */
    {
        char line[4096];
        int off = snprintf(line, sizeof(line), "  [KERNEL] ordinals used (%d):", n);
        for (int i = 0; i < n && off > 0 && off < (int)sizeof(line) - 32; i++) {
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            " %u=%u", idx[i], g_ordinal_calls[idx[i]]);
        }
        fprintf(stderr, "%s\n", line);
    }

    /* The timer table, on the line straight after the histogram ON PURPOSE.
     *
     * armed+full is an identity against the 149= entry printed immediately
     * above (plus 150=, which has never appeared in any archived run), so a
     * reader can check the instrument against a number that was already there
     * before believing anything it says. Without that, `early=0 torn=0` is
     * indistinguishable from a dead counter -- the failure mode this tree has
     * been bitten by twice. See the declarations near BRIDGE_MAX_TIMERS. */
    fprintf(stderr,
            "  [KERNEL] timers: armed=%ld full=%ld (armed+full must equal"
            " 149= above) fired=%ld early=%ld torn=%ld lost=%ld"
            " claim_lost=%ld\n",
            (long)InterlockedCompareExchange(&g_timers_armed, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_full, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_fired, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_early, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_torn, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_lost, 0, 0),
            (long)InterlockedCompareExchange(&g_timers_claim_lost, 0, 0));

    /* Call sites for the busiest ordinals only: the point is to locate a spin,
     * and a full dump buries it. */
    for (int i = 0; i < n && i < 8; i++) {
        char line[256];
        int off = snprintf(line, sizeof(line), "  [KERNEL]   ord %u sites:",
                           idx[i]);
        for (int k = 0; k < XBOX_ORDINAL_SITES; k++) {
            uint32_t site = g_ordinal_sites[idx[i]][k];
            if (!site) break;
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            " 0x%08X", site);
        }
        fprintf(stderr, "%s\n", line);
    }
}

static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];
/* Whether stdcall_args_for_ordinal actually had an entry for this slot, as
 * opposed to returning zero because the function takes no arguments. */
static uint8_t g_slot_arg_known[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot.
 *
 * recomp_lookup_kernel records the synthetic thunk's slot and returns the
 * shared kernel_thunk_dispatch entry point.  Real guest threads can perform
 * that lookup concurrently, so the hand-off value is part of the translated
 * CPU's per-thread state just like g_esp.  A process-global selector allowed
 * one thread to replace another thread's ordinal between lookup and dispatch;
 * the wrong bridge then read a different argument layout and popped the wrong
 * number of stack bytes. */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    /* Cooperative suspend safe point. A guest thread another thread has
     * suspended parks itself here rather than being stopped from outside,
     * which POSIX cannot do safely. Costs one relaxed read when nothing is
     * pending. See w32_thread_suspend_point. */
#if !defined(_WIN32)
    w32_thread_suspend_point();
#endif
    bridge_timers_poll();

    g_kernel_call_count++;

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses. */
        fprintf(stderr,
                "  [KERNEL] #%llu: ordinal %u (slot %d) esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    /* Per-ordinal histogram.
     *
     * The "latest ordinal" in the summary below names one call out of millions,
     * which is nearly useless once a title starts spinning: the churn drowns
     * out the calls that matter. Knowing WHICH kernel calls a title makes, and
     * how often, repeatedly turned out to be the fastest way to locate a
     * blocker -- e.g. establishing that JSRF opens files but issues zero
     * NtReadFile, which is not visible from any single sample.
     *
     * Counted unconditionally (one increment, no lock: worst case a racing
     * worker loses a count, which does not change any conclusion drawn from
     * an order-of-magnitude histogram) and dumped with the periodic summary. */
    if (ordinal < XBOX_KERNEL_MAX_ORDINAL) {
        g_ordinal_calls[ordinal]++;
        bridge_note_call_site(ordinal, g_esp ? BRIDGE_MEM32(g_esp) : 0);
        if (xbox_bridge_kthreads_on())
            xbox_bridge_note_thread_call(g_fs_base, (unsigned int)ordinal,
                                         (unsigned long)GetTickCount());
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            bridge_scan_dialogs();
            bridge_dump_ohci();
            fprintf(stderr, "  [KERNEL] summary: %llu total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            xbox_bridge_dump_ordinal_histogram();
            xbox_bridge_dump_thread_census((unsigned long)now);
            recomp_irq_latency_report();
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%llu)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    /* Attribute anything this bridge allocates to the export and the guest
     * call site, so an exhausted heap can be read back to a caller instead of
     * a size. g_esp has already had the dummy return address popped, so the
     * guest return address is the dword just below it. */
    xbox_HeapSetOwner(ordinal, g_esp ? BRIDGE_MEM32(g_esp - 4) : 0);

    /* So a bridge that rejects an argument can name the export it was reached
     * through, not just the one whose implementation it is about to enter. */
    g_bridge_current_ordinal = ordinal;
    g_bridge_current_slot = slot;

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            /* "Returning 0" is the harmless half. The damaging half is the
             * stack: the Xbox kernel is stdcall, so the callee owes the caller
             * its arguments back, and an ordinal missing from
             * stdcall_args_for_ordinal() returns 0 bytes and leaves them
             * there. The caller's own `pop`s then run low by that much and it
             * returns with its callee-saved registers rotated -- silently,
             * frames away from here. Say so, because a title that dies of this
             * looks nothing like a title that is missing a kernel function. */
            if (!g_slot_arg_known[slot])
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    xbox_HeapSetOwner(0, 0);

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            g_bridge_current_target = xbox_va;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    bridge_irq_thread_start();
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            {
                int _a = stdcall_args_for_ordinal(ordinal);
                g_slot_arg_known[i] = (_a >= 0);
                g_slot_arg_bytes[i] = (_a > 0) ? _a : 0;
            }
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000)) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            {
                int _a = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
                g_slot_arg_known[slot] = (_a >= 0);
                g_slot_arg_bytes[slot] = (_a > 0) ? _a : 0;
            }
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
