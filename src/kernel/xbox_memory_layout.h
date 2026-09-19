/**
 * Xbox Memory Layout Compatibility
 *
 * The Xbox has 64MB of unified memory shared between CPU and GPU.
 * Memory is identity-mapped (physical == virtual for most of it).
 * Game code and data are linked to specific address ranges which vary
 * per game. Section addresses are parsed dynamically from the XBE header
 * at runtime, so this module works with ANY Xbox game.
 *
 * On Windows, we:
 * 1. Create a 64MB file mapping (CreateFileMapping)
 * 2. Map the base view + 28 mirror views at 64MB intervals
 * 3. Parse the XBE section table and copy sections to their Xbox VAs
 * 4. Set up simulated stack, heap, TIB, and kernel data area
 *
 * The mirror views ensure Xbox RAM wrapping works correctly: the Xbox
 * memory controller uses a 26-bit address bus, so ALL addresses wrap
 * modulo 64MB. File mapping views backed by the same section give us
 * true aliases where writes at one address are visible at all mirrors.
 */

#ifndef XBOX_MEMORY_LAYOUT_H
#define XBOX_MEMORY_LAYOUT_H

#include "platform/xbox_winnt.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox memory map constants
 * ================================================================ */

/* Base address of all XBE files in Xbox memory */
#define XBOX_BASE_ADDRESS       0x00010000

/* Start of mapped region - includes low memory (KPCR at 0x0) because
 * game code reads from addresses like 0x20 and 0x28 (Xbox kernel structures). */
#define XBOX_MAP_START          0x00000000

/* Xbox physical memory. 64 MB is the retail default; debug/beta builds ship for
 * 128 MB devkits and allocate accordingly (Halo's cachebeta pre-allocates ~57 MB
 * plus its debug arrays, which only fits on a devkit). Runtime-overridable via
 * xbox_SetTotalRam() before xbox_MemoryLayoutInit(); see g_xbox_total_ram. */
#define XBOX_TOTAL_RAM          (64 * 1024 * 1024)  /* 64 MB (default) */
#define XBOX_DEVKIT_RAM         (128 * 1024 * 1024) /* 128 MB (debug kit) */
#define XBOX_GPU_RESERVED       (4 * 1024 * 1024)   /* ~4 MB for GPU */

/* Actual mapped RAM for this run. Defaults to XBOX_TOTAL_RAM; a title with a
 * devkit build calls xbox_SetTotalRam(XBOX_DEVKIT_RAM) before init. Heap top and
 * mirror stride derive from this, not from the compile-time constant. */
extern size_t g_xbox_total_ram;

/* How much guest address space to map, when that must exceed RAM.
 *
 * These are not the same quantity and conflating them is a bug. RAM is what
 * the console has and what the heap is carved out of; the mapped range is how
 * much guest address space is backed by distinct host pages. The runtime
 * mirrors RAM at intervals of the mapped size, because a real Xbox wraps
 * addresses on a 26-bit bus -- so anything a title allocates above the mapped
 * range silently shares storage with low memory.
 *
 * Half-Life 2 needs this: its allocator sub-allocates past the top of RAM, and
 * at 64 MB its first commit past the boundary (0x04F80000) aliases the base of
 * the live heap (0x00F80000). A CUtlRBTree element array landed at 0x0CB80000,
 * aliasing 0x00B80000, and its links were overwritten between one insert and
 * the next search.
 *
 * Raising g_xbox_total_ram instead does not work: the heap top and anything
 * the guest is told about memory derive from that, so the title sizes itself
 * differently and faults during CRT init. Growing only the mapping leaves both
 * alone.
 *
 * Zero means "same as RAM", which is the existing behaviour for every title
 * that does not ask. Set before xbox_MemoryLayoutInit(). */
extern size_t g_xbox_map_size;
void xbox_SetMapSize(size_t bytes);
size_t xbox_GetMappedSize(void);

/* Give pure MEM_RESERVE calls a distinct virtual-address arena above RAM.
 * `bytes` is the size of that arena, not the total mapping. Ordinary heap and
 * physical allocations remain capped at g_xbox_total_ram. This is opt-in
 * because older users of xbox_SetMapSize intentionally let the heap consume
 * the enlarged mapping. Call before xbox_MemoryLayoutInit(). */
void xbox_EnableSeparateReserveSpace(size_t bytes);
BOOL xbox_SeparateReserveSpaceEnabled(void);

/* Carve a pure address-space reservation from the mapped range above RAM.
 * Returns 0 if the mapping is no larger than RAM, or if it is exhausted.
 * See the implementation for why reservations must not come from the heap. */
uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align);
BOOL xbox_ReserveFree(uint32_t address);
BOOL xbox_QueryReserveAddress(uint32_t address, uint32_t *base, uint32_t *size);

/* Bounds of the guest's executable sections, derived from the XBE at load.
 *
 * recomp_types.h declares these too, for RECOMP_ICALL_IS_CODE. They are
 * repeated here so hand-written host code -- a fault handler wanting to tell a
 * guest return address on the stack from ordinary data, say -- can use them
 * without including the generated-code header, which redefines `eax` and
 * friends as macros.
 */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
void xbox_SetTotalRam(size_t bytes);

/* NOTE: Section addresses (.text, .rdata, .data, etc.) are NOT hardcoded.
 * They are parsed from the XBE header at runtime in xbox_MemoryLayoutInit().
 * This allows the toolkit to work with ANY Xbox game without modification. */

/* ================================================================
 * Memory initialization
 * ================================================================ */

/**
 * Initialize the Xbox memory layout.
 *
 * Reserves the virtual address range 0x00010000 through 0x0076F000
 * and maps the XBE sections to their expected addresses:
 * - .rdata: copied from XBE, read-only
 * - .data: initialized portion copied from XBE, BSS zeroed
 *
 * Note: .text is NOT mapped here - the recompiled code is native
 * Windows code and doesn't need to be at the original address.
 * The data sections DO need to be at their original addresses
 * because the recompiled code references globals by absolute address.
 *
 * @param xbe_data  Pointer to the loaded XBE file contents.
 * @param xbe_size  Size of the XBE file.
 * @return TRUE on success, FALSE on failure.
 */
BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);

/* Apply the command/W1C semantics of an OHCI root-hub port-status write.
 * Exposed so the register model's ordering can be regression-tested without
 * requiring a trapped guest store. */
uint32_t xbox_OhciPortWrite(uint32_t current, uint32_t value);

/**
 * Release the reserved Xbox memory layout.
 */
/**
 * Mirror a GPU completion fence the title spins on.
 *
 * The NV2A tables in xbox_memory_layout.c acknowledge handshakes that live at
 * fixed aperture offsets. Some titles instead wait on a semaphore the GPU
 * writes into contiguous memory: D3D seeds it, submits work, then spins until
 * it reaches the submitted count. Wreckless does this at guest 0x000FE920,
 * waiting on the 96-byte MmAllocateContiguousMemoryEx block its device struct
 * points at.
 *
 * Nothing executes the push buffer -- the D3D11 layer draws -- so everything
 * submitted is complete, and advancing the fence is the same honest
 * acknowledgement the register tables make.
 *
 * The fence has no fixed address; it is reached through the title's device
 * struct, so it is registered as the chain of indirections to follow:
 *
 *     device = MEM32(device_ptr_va)
 *     fence  = MEM32(device + get_ptr_off)
 *     MEM32(fence) = MEM32(device + put_off)
 *
 * Every step is bounds-checked each poll, so registering a chain that is not
 * yet initialised (or never becomes valid) is harmless.
 *
 * Returns 0 on success, -1 if the table is full.
 */
/* Advance a frame/swap counter inside the D3D device at ~60 Hz.
 *
 * A title that waits a frame reads the device's swap count and spins until it
 * moves. Nothing here presents, so without this the count never changes and
 * the wait never ends. Followed through the device pointer, like the fence,
 * because the device is allocated at runtime.
 *
 * Returns 0 on success, -1 if the table is full. */
int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off);
/* Advance those counters now, because a swap really completed. Called by the
 * pushbuffer executor on FLIP_STALL; while these arrive the 60 Hz fallback
 * stands down, so the count follows what was actually drawn. */
void xbox_Nv2aFrameCounterFlip(void);

/* Tell the runtime where the display framebuffer is (from AvSetDisplayMode). */
void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch);

/* Allocate GPU-compatible contiguous storage. Windows uses the high physical
 * window; POSIX retains low guest-RAM backing. Returns a guest VA, or 0. */
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off);

void xbox_MemoryLayoutShutdown(void);

/**
 * Check if an address falls within the Xbox memory map.
 */
BOOL xbox_IsXboxAddress(uintptr_t address);

/**
 * Get the base pointer for direct memory access.
 * Returns NULL if memory layout is not initialized.
 */
void *xbox_GetMemoryBase(void);

/**
 * Get the offset from Xbox VA to actual mapped address.
 * actual_address = xbox_va + offset
 * Returns 0 if memory is mapped at original Xbox addresses (ideal case).
 */
ptrdiff_t xbox_GetMemoryOffset(void);

/** Opt in to a shared CPU physical view of the low guest heap on POSIX.
 * Call after MemoryLayoutInit and before running guest code. Only the heap
 * above XBOX_HEAP_BASE is aliased; low pinned pools / the fake kernel retain
 * separate storage. Requires a <=64 MB layout without pinned pools overlapping
 * the heap. Other titles and the Windows contiguous allocator are unchanged. */
BOOL xbox_EnablePhysicalHeapAlias(void);
/* Checked GPU surface access, plus the separately mapped NV2A registers.
 * NULL indicates an unavailable mapping or an out-of-bounds range. */
void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
const uint8_t *xbox_Nv2aRegisterMemory(void);
/**
 * Convert a host fault address back to a guest VA only when it lies in one of
 * the memory layout's mappings. Intended for crash diagnostics; unlike raw
 * offset subtraction, this rejects unrelated host addresses such as NULL.
 */
BOOL xbox_HostAddressToGuest(uintptr_t host_address, uint32_t *guest_address);
/* Hand ownership of the NV2A DMA_GET register to a push-buffer executor.
 * While set, the periodic acknowledgement stops copying PUT into GET, so GET
 * reflects what has actually been consumed rather than what was submitted --
 * which is what stops the producer lapping the consumer. */
extern int g_nv2a_pusher_owns_dma_get;
/* Nonzero NV097_NO_OPERATION software trap, delivered through the guest IRQ.
 * Pending includes the driver's FIFO-disable interval after it acks status. */
BOOL xbox_Nv2aRaiseSoftwareMethod(uint32_t subchannel, uint32_t parameter);
int xbox_Nv2aSoftwareMethodPending(void);

void xbox_ProtectMirrorsForDebug(void);

/* Dump the guest call stack and abort if the title has not exited within
 * RECOMP_WATCHDOG_SECS seconds. Call from the thread that runs guest code;
 * does nothing unless that variable is set. */
void xbox_WatchdogStart(void);

/* ================================================================
 * Xbox stack for recompiled code
 * ================================================================ */

/* ================================================================
 * Kernel data export area
 * ================================================================ */

/** Base VA for kernel data exports (XboxHardwareInfo, XboxKrnlVersion, etc.)
 *  These are kernel exports that are DATA, not functions. The game reads
 *  their thunk entries and dereferences them to access the data. */
#define XBOX_KERNEL_DATA_BASE   (g_xbox_low_base + 0x40000u)
#define XBOX_KERNEL_DATA_SIZE   4096   /* 4 KB - plenty for all data exports */

/* Offsets within the kernel data area */
#define KDATA_HARDWARE_INFO     0x000  /* XBOX_HARDWARE_INFO (8 bytes) */
#define KDATA_KRNL_VERSION      0x010  /* XBOX_KRNL_VERSION (8 bytes) */
#define KDATA_TICK_COUNT        0x020  /* KeTickCount (4 bytes) */
#define KDATA_LAUNCH_DATA_PAGE  0x030  /* LaunchDataPage (4 bytes, pointer) */
#define KDATA_THREAD_OBJ_TYPE   0x040  /* PsThreadObjectType (4 bytes) */
#define KDATA_EVENT_OBJ_TYPE    0x050  /* ExEventObjectType (4 bytes) */
#define KDATA_XE_IMAGE_FILENAME 0x060  /* XeImageFileName (ANSI_STRING) */
#define KDATA_IO_COMPLETION_TYPE 0x070 /* IoCompletionObjectType (4 bytes) */
#define KDATA_IO_DEVICE_TYPE    0x080  /* IoDeviceObjectType (4 bytes) */
/* Object-type exports a title may compare against each other, so each needs a
 * distinct non-zero value rather than a shared placeholder. */
#define KDATA_MUTANT_OBJ_TYPE   0x090  /* ExMutantObjectType (4 bytes) */
#define KDATA_SEMAPHORE_OBJ_TYPE 0x0A0 /* ExSemaphoreObjectType (4 bytes) */
#define KDATA_TIMER_OBJ_TYPE    0x0B0  /* ExTimerObjectType (4 bytes) */
#define KDATA_FILE_OBJ_TYPE     0x0C0  /* IoFileObjectType (4 bytes) */
#define KDATA_TIME_INCREMENT    0x0D0  /* KeTimeIncrement (4 bytes) */
#define KDATA_BOOT_SMC_VIDEO    0x0E0  /* HalBootSMCVideoMode (4 bytes) */
#define KDATA_IDEX_CHANNEL      0x500  /* IDE_CHANNEL_OBJECT (512-byte reserved region) */
#define KDATA_HD_KEY            0x100  /* XboxHDKey (16 bytes) */
#define KDATA_SIGNATURE_KEY     0x110  /* XboxSignatureKey (16 bytes) */
#define KDATA_LAN_KEY           0x120  /* XboxLANKey (16 bytes) */
#define KDATA_ALT_SIGNATURE_KEYS 0x130 /* XboxAlternateSignatureKeys (256 bytes) */
#define KDATA_XE_PUBLIC_KEY     0x300  /* XePublicKeyData (284 bytes) */
/* HAL disk identity strings (ordinals 41/42). Each is an XBOX_ANSI_STRING
 * (Length, MaximumLength, Buffer VA) followed by the string bytes it points to,
 * because HalRandGather dereferences Buffer to read the text as entropy. */
#define KDATA_DISK_MODEL_STR    0x420  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_MODEL_BUF    0x430  /* model text (up to 48 bytes) */
#define KDATA_DISK_SERIAL_STR   0x460  /* XBOX_ANSI_STRING (8 bytes) */
#define KDATA_DISK_SERIAL_BUF   0x470  /* serial text (up to 32 bytes) */
#define KDATA_DISK_CACHE_PARTS  0x4A0  /* HalDiskCachePartitionCount (4 bytes) */
/* XeImageFileName's text. The exported symbol at KDATA_XE_IMAGE_FILENAME is
 * an XBOX_ANSI_STRING, and a title dereferences its Buffer -- the CRT reads
 * it to work out the running image's path. The struct was declared without
 * anything to point at, so Buffer held whatever was in that page. */
#define KDATA_XE_IMAGE_BUF      0x4B0  /* image path text (up to 64 bytes) */

/** Size of the simulated Xbox stack (8 MB).
 *  Increased from 1 MB because failed RECOMP_ICALL indirect calls
 *  can leak stdcall args onto the stack each frame. An 8 MB stack
 *  provides enough headroom for extended gameplay sessions. */

/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise. */
/* Thread-local storage class for the recompiled register set. Must match
 * templates/runtime/recomp_types.h -- a mismatch is a link-time surprise.
 *
 * MEASURED 12 Sep 2026, and the biggest single cost in the program on BOTH
 * hosts. The generated code is `#define eax g_eax` over this register set, so
 * every guest register read and write is one access to a thread-local.
 *
 *   macOS   Darwin has no ELF TLS models, so __thread always goes through the
 *           tlv descriptor and _tlv_get_addr in libdyld, and -ftls-model= is
 *           silently a no-op. `sample` puts _tlv_get_addr at roughly 75% of
 *           the main guest thread's samples -- more than the rasteriser, the
 *           combiner and the ADX decoder put together.
 *   Windows mingw-w64 defines __GNUC__, so it takes the __thread branch, and
 *           GCC implements __thread on this target with EMULATED TLS:
 *           __emutls_get_address(), a real call with a lookup inside it, per
 *           access. jsrf_first_fault.exe carries 145 emutls symbols.
 *
 * TRIED AND REVERTED: keying the choice on _WIN32 so mingw takes
 * __declspec(thread) (native TEB-relative, no call). It builds, and emutls
 * symbols drop 145 -> 16, but the title exits through HalReturnToFirmware
 * within 16 file opens. Native PE TLS needs its directory and per-thread
 * initialisation, and something in the harness's own thread creation or memory
 * layout does not satisfy that. Do not simply re-apply it; find out why first.
 *
 * The real fix is not a storage class. It is to stop doing a TLS lookup per
 * register access -- hand the generated functions a register-context pointer
 * once on entry and index through it. That needs a translator change or a
 * mechanical edit of the generated tree, and it is worth doing: it is the
 * single largest measured win available on either host.
 *
 * RECOMP_TLS may be predefined by the build to override the choice below;
 * -DRECOMP_TLS= builds with plain globals, which is not thread-safe and
 * crashes, but is useful for bounding experiments. */
#ifndef RECOMP_TLS
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif
#endif

/* SSE register storage, shared with the generated code. Defined in both this
 * header and templates/runtime/recomp_types.h -- a translation unit can end up
 * including both, so the guard keeps that from being a redefinition. Keep the
 * two identical: the generated code and the runtime have to agree on the
 * layout, and nothing else checks. */
#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

/* Stack region size.
 *
 * Everything above this is the heap arena -- XBOX_HEAP_BASE is literally
 * XBOX_STACK_BASE + XBOX_STACK_SIZE -- so a byte reserved here is a byte the
 * title cannot allocate. At 8 MB the arena was 50,855,936 bytes on a 64 MB
 * console, and JSRF ran out loading player assets with 49.4 MB live: it wanted
 * 827,904 contiguous bytes and the largest free block was 475,136.
 *
 * 8 MB was never measured. Painting the region and scanning for the lowest
 * word the title ever touched puts the main stack's high-water mark at 3 KB
 * across a full 70-second JSRF startup -- recompiled code keeps its working
 * set in host locals, so the guest stack stays shallow.
 *
 * 6 MB rather than something closer to the measurement, because the bottom
 * 4 MB is the worker-slice pool below and a title uses one model or the other:
 * the slices keep their 4 MB, and a main-loop title gets 2 MB of stack against
 * a measured 3 KB. The 2 MB this returns to the arena is what JSRF needed.
 * A title that really does recurse deeply will fault in its own stack rather
 * than corrupt the heap -- the arena starts above it, growing up. */
#define XBOX_WORKER_STACK_SIZE   (256 * 1024)
#define XBOX_MAIN_STACK_SIZE (2 * 1024 * 1024)

/* Worker slices are reserved only for a title that uses them.
 *
 * They exist for the tick-driven model (see below), and nothing in this tree
 * calls xbox_worker_stack_alloc -- so on a main-loop title the pool was 4 MB of
 * address space nobody could touch, sitting directly under the arena. JSRF
 * loads a 235-texture startup set and then fails allocations of a few hundred
 * bytes with about 2 MB to spare, so 4 MB is not a rounding error.
 *
 * Build a tick-driven title with -DXBOX_WORKER_STACK_COUNT=16. */
#ifndef XBOX_WORKER_STACK_COUNT
#define XBOX_WORKER_STACK_COUNT  0
#endif

#define XBOX_STACK_SIZE     (XBOX_MAIN_STACK_SIZE \
                             + XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Base VA of the stack area (above last XBE section). */
/* Where the fake TIB lives -- the linear address fs: is based at.
 *
 * Deliberately not 0. The TIB used to sit on page zero, because the lifter
 * dropped the fs prefix and fs:[N] became linear [N]. That made a null
 * dereference read or write the TIB instead of faulting: a null check of the
 * form `cmp byte [ecx], 0` saw the exception-chain head's 0xFF and passed, and
 * a store through a null pointer quietly overwrote that head. Both then
 * surfaced somewhere else entirely. With the TIB up here, page zero is left
 * unmapped and either mistake faults where it happens.
 *
 * Sits below every XBE's image base (0x00010000), so it displaces nothing. */
/* Base of the fixed low block: primary TLS data, the kernel data exports, the
 * TLS/PRCB stand-ins, and above them the stack and then the heap arena.
 *
 * This was 0x00700000, chosen to clear any XBE. Nothing below the heap can be
 * allocated by the title, and XBOX_HEAP_BASE is XBOX_STACK_BASE plus the stack
 * size, so the distance between the top of the loaded image and this base is
 * dead space charged to the arena. JSRF's image is 11 sections ending at
 * 0x00288620 -- 2.6 MB -- so 0x00700000 threw away 4.5 MB on a console with 64.
 *
 * Set from the actual section extents during xbox_MemoryLayoutInit, rounded up
 * to 64 KB, and left at the old default until then so anything reading these
 * before init sees what it always did. A title with a larger image pushes it
 * up; nothing pushes it below the image, and init fails loudly if the sections
 * would overlap it. */
#define XBOX_LOW_BASE_DEFAULT 0x00700000u
extern uint32_t g_xbox_low_base;

#define XBOX_FS_BASE        0x00001000

#define XBOX_STACK_BASE     (g_xbox_low_base + 0x80000u)

/** Initial ESP value (top of stack, 16-byte aligned). */
#define XBOX_STACK_TOP      (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)
#define XBOX_THREAD_STACK_SIZE (512 * 1024)   /* cap, and the default */
#define XBOX_THREAD_STACK_MIN  (64 * 1024)    /* floor for a stated size */

/* Primary-thread storage used by the title's own Xbox TLS bootstrap. */
#define XBOX_PRIMARY_TIB_VA          XBOX_FS_BASE
#define XBOX_PRIMARY_TLS_CONTEXT_VA  (g_xbox_low_base + 0x60000u)
#define XBOX_PRIMARY_TLS_DATA_VA     (g_xbox_low_base)

/* ================================================================
 * Worker stack slices (host-tick-driven titles)
 * ================================================================
 *
 * A second way to drive a recompiled title, ported from the Burnout 3 fork as
 * the runtimes reunite (see docs/technical/burnout3-reunification.md).
 *
 * The default model (Halo, Crimson Skies) runs the game's entry routine inline
 * and it drives its own main loop. Some titles instead return from their entry
 * after spawning an init thread, and expect the *host* to drive the per-frame
 * tick -- Burnout 3 is tick-driven, not main-loop-driven. To call recompiled
 * code from the host's own message-loop thread, that thread needs a guest stack
 * (its g_esp starts at 0), which is what a worker slice provides.
 *
 * These slices carve the low end of the same 8 MB stack region xbox_AllocThreadStack
 * uses, and a title uses one model or the other -- never both -- so they do not
 * coexist at runtime. For a title that never calls xbox_worker_stack_alloc
 * (every default-model title), this is unused address space and dead code, so
 * adding it changes nothing for them.
 */
#define XBOX_WORKER_STACK_BASE   XBOX_STACK_BASE
/* XBOX_WORKER_STACK_SIZE and _COUNT are defined with XBOX_STACK_SIZE above,
 * because the region's size depends on how many slices are reserved. */
#define XBOX_WORKER_STACK_END    (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * XBOX_WORKER_STACK_COUNT)

/** Top (initial esp) of worker stack slice n, 16-byte aligned, growing down. */
#define XBOX_WORKER_STACK_TOP(n) (XBOX_WORKER_STACK_BASE + \
                                  XBOX_WORKER_STACK_SIZE * ((n) + 1) - 16)

/* ================================================================
 * Xbox dynamic heap (for MmAllocateContiguousMemory, etc.)
 * ================================================================ */

/** Base VA of the dynamic heap area (above stack). */
#define XBOX_HEAP_BASE      (XBOX_STACK_BASE + XBOX_STACK_SIZE)  /* 0x00D80000 */

/** Exclusive top of the dynamic heap: the end of RAM for this run. Runtime,
 *  not a macro, because RAM size is now configurable (retail 64 MB vs devkit
 *  128 MB). The total mapped region (data + stack + heap) equals RAM so the
 *  engine's memory probing stops at the correct boundary. */
/* The heap runs to the end of the *mapped* range, not the end of RAM.
 *
 * When a title maps more address space than it has RAM, a large
 * MEM_RESERVE has to come from somewhere. Carving it out of a separate
 * arena above the heap looked tidy and was wrong: the guest CRT's own
 * bookkeeping never learns about that region, so realloc's block lookup
 * fails for a pointer in it and the copy is skipped -- a grown buffer
 * comes back empty with the old one still intact. Half-Life 2 loses a
 * 129-node CUtlRBTree that way, and the tree then self-cycles.
 *
 * Letting the ordinary heap serve the whole mapped range keeps every
 * allocation inside one allocator the guest already understands.
 */
#define XBOX_HEAP_TOP       ((uint32_t)(xbox_SeparateReserveSpaceEnabled() \
                                      ? g_xbox_total_ram \
                                      : (g_xbox_map_size ? g_xbox_map_size \
                                                         : g_xbox_total_ram)))

/** No static mirror/guard region. RAM mirror is handled via file mapping
 *  views that alias the same physical pages as the base 64 MB region. */
#define XBOX_MIRROR_SIZE    0
#define XBOX_GUARD_SIZE     0

/** Number of 64 MB mirror views to pre-map (covers 1.75 GB of address space). */
#define XBOX_NUM_MIRRORS    28

/* Tiled / write-combined aperture. The NV2A shows physical RAM again here, and
 * titles render through it: physical page P is at XBOX_TILED_BASE + P. Aliases
 * the RAM mapping rather than getting its own storage, because a title writes a
 * surface through the tiled address and reads it back through the normal one. */
#define XBOX_TILED_BASE     0xF0000000u

/**
 * Allocate from the Xbox heap. Returns an Xbox VA, or 0 on failure.
 * Alignment must be a power of 2 (minimum 4).
 * Thread-safe: no (single-threaded recompiled code).
 */
/* Install a sink for guest writes to the APU register aperture. The kernel does
 * not link xbox_apu, so the host program wires this to mcpx_apu_mmio_write.
 * Must be called before xbox_MemoryLayoutInit, which installs the trap. */
/* Assert the NV2A display-engine vblank interrupt (PCRTC source + PMC summary),
 * bypassing the write-1-to-clear guard so the raise is not mistaken for the
 * guest's acknowledge. xbox_Nv2aVblankPending reports whether the guest has
 * acknowledged the last one yet. */
void xbox_Nv2aRaiseVblank(void);
int  xbox_Nv2aVblankPending(void);

void xbox_SetApuMmioWriteHook(void (*fn)(uint32_t offset, uint32_t value,
                                         unsigned width));
/* Pair with the write hook before initialization. Main APU register reads on
 * AArch64 then come from the device model, including interrupt/trap state. */
void xbox_SetApuMmioReadHook(uint32_t (*fn)(uint32_t offset, unsigned width));

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* Record who the next heap allocations belong to. kernel_thunk_dispatch calls
 * this before running a bridge so blocks carry the kernel ordinal and guest
 * return address that asked for them; xbox_HeapReport attributes by both. */
void xbox_HeapSetOwner(uint32_t ordinal, uint32_t guest_ra);

/* Print live/free/largest-free/retained accounting plus the top owners. Called
 * on the first allocation failure, and on demand from a diagnostic. */
void xbox_HeapReport(const char *why);

/* Describe the heap block covering this guest address (mirror aliases
 * accepted) into `buf`: extent, requested size, and the kernel ordinal and
 * guest return address that asked for it. Returns 0, and says so in `buf`,
 * when this heap never issued the address. */
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size);

/**
 * Free a block from the Xbox heap. Currently a no-op (bump allocator).
 */
void xbox_HeapFree(uint32_t xbox_va);

/**
 * Bytes remaining in the heap block containing this guest address, or 0 if the
 * heap never handed it out. Backs MmQueryAllocationSize and
 * ExQueryPoolBlockSize -- the host cannot answer either, since VirtualQuery on
 * the translated address describes the whole guest mapping.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va);

/**
 * Get the file mapping handle for the Xbox memory region.
 * Used by the VEH handler to map additional mirror views on demand.
 * Returns NULL if file mapping is not available.
 */
HANDLE xbox_GetMappingHandle(void);

/* Checksum a spread of the title's own code pages. First call baselines, every
 * later call compares and names any page that moved. Opt-in via
 * RECOMP_TEXT_CHECKSUM; see the definition for why the Windows work needs it. */
void xbox_TextChecksumReport(void);

#ifdef __cplusplus
}
#endif


/* Carve a simulated stack for a spawned thread. Returns the Xbox VA of the
 * stack top, or 0 when the pool is exhausted. */
uint32_t xbox_AllocThreadStack(uint32_t bytes);

/* The guest thread stack containing `esp`, if any. Returns 1 and fills the
 * range, else 0. For the crash reporter: g_esp is thread-local, so a fault off
 * the primary thread otherwise reads as "ESP is outside the primary stack",
 * which is true of every worker and says nothing. */
int xbox_GuestStackRangeFor(uint32_t esp, uint32_t *base_out, uint32_t *top_out);

/* Install the per-thread Xbox FS/TIB state used by generated FS_MEM accesses.
 * tls_data_size is the value passed to PsCreateSystemThreadEx. */
/* Size of the image's TLS block as the loader built it; 0 before load. */
extern uint32_t g_image_tls_total;

void xbox_SetupCurrentThreadTib(uint32_t tib_va, uint32_t tls_context_va,
                               uint32_t tls_data_va, uint32_t tls_data_size,
                               uint32_t stack_top, uint32_t stack_limit);
/**
 * Return a worker's stack when the worker ends. Takes the value
 * xbox_AllocThreadStack returned. Without this the pool counts threads ever
 * created rather than threads alive, and a title that cycles workers exhausts
 * it -- after which PsCreateSystemThreadEx runs them inline, which deadlocks
 * any caller that then waits for the worker it thought it had spawned.
 */
void xbox_FreeThreadStack(uint32_t stack_top);

/* Worker stack slices for host-tick-driven titles (see XBOX_WORKER_STACK_* and
 * docs/technical/burnout3-reunification.md). Additive; unused by default-model
 * titles. */
int  xbox_worker_stack_alloc(void);   /* slice index, or -1 if none free */
void xbox_worker_stack_free(int slot);
void   xbox_set_game_thread(void *h);  /* HANDLE, recorded for the host watchdog */
void  *xbox_thread_debug_handle(void); /* the game thread, or NULL under inline model */

/* PsCreateSystemThreadEx behaviour. Default INLINE runs the first call as the
 * game (Halo, Crimson Skies). SPAWN makes every call a real thread so a
 * host-tick-driven title's entry can return and let the host drive -- call
 * xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN) before the game starts. */
#define XBOX_THREAD_MODE_INLINE 0
#define XBOX_THREAD_MODE_SPAWN  1
void xbox_SetThreadMode(int mode);

#endif /* XBOX_MEMORY_LAYOUT_H */
