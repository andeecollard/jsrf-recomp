/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

/* ucontext_t is behind the X/Open guard on macOS, and the MCPX write trap
 * below needs the signal context to read the faulting store's registers.
 * Must precede every include, so it sits above them rather than with the
 * trap. diagnostics/jsrf_first_fault/main.c does the same for its handler. */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
/* _XOPEN_SOURCE alone hides the BSD extensions this file already uses
 * (MAP_ANONYMOUS); _DARWIN_C_SOURCE puts them back. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "xbox_memory_layout.h"
#include "kernel.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <sys/mman.h>
#endif

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */
#if defined(__APPLE__)
/* Own the complete RAM + mirror span before MapViewOfFileEx uses MAP_FIXED. */
static void *g_host_reservation = NULL;
static size_t g_host_reservation_size = 0;

static BOOL host_reservation_contains(uintptr_t target, size_t size)
{
    uintptr_t base = (uintptr_t)g_host_reservation;

    if (!base || size > g_host_reservation_size || target < base)
        return FALSE;
    return target - base <= g_host_reservation_size - size;
}
#endif

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;
static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding an UNMODELLED engine's status at zero is correct rather than
     * convenient: nothing raises those, so "none pending" is the truth.
     *
     * The DISPLAY engine is the exception, and the masks below carve it out.
     * xbox_Nv2aRaiseVblank now genuinely raises a GPU interrupt once a frame --
     * NV_PCRTC_INTR_0 bit 0 (VBLANK) and the NV_PMC_INTR_0 bit 24 summary that
     * follows it -- and the guest acknowledges it through the write trap. This
     * thread cannot tell that raise apart from a stale bit, so with the old
     * 0xFFFFFFFF masks it raced the raise and won: PMC bit 24 was back to zero
     * before D3D's DPC (sub_00194480) read it, the DPC therefore never
     * dispatched to the vblank handler (sub_00193D90), the handler never
     * acknowledged NV_PCRTC_INTR_0 and never signalled the device's vblank
     * KEVENT, and bridge_vblank_poll's "do not re-raise while unacknowledged"
     * gate then latched shut. Measured: exactly one ISR and one DPC per run,
     * then every D3D thread blocked in sub_0018CE50 forever.
     *
     * Clearing PCRTC_INTR_0 was also pure overhead: the trap models it as
     * write-1-to-clear, so this thread's store of 0 changed nothing while
     * still taking a page fault on every loop iteration. */
    { 0x000100, ~0x01000000u }, /* PMC_INTR_0, except the PCRTC summary  */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, ~0x00000001u }, /* PCRTC_INTR_0, except VBLANK          */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
static void *g_mcpx_regs = NULL;

static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

/*
 * MCPX status bits that must always read as SET.
 *
 * The MCPX counterpart of NV2A_IDLE, and the same argument: a device that is
 * present and finished resetting reports itself ready, and zeroed RAM reports
 * it forever un-ready.
 *
 * DirectSound's AC97 bring-up is the case that needs it. JSRF stops here:
 *
 *   sub_001A6C94:                      ; AC97 codec-ready handshake
 *     eax = [0xFEC0012C]               ; control
 *     if (!(al & 2)) [0xFEC0012C] = eax | 2      ; de-assert cold reset
 *     if (al & 8)    [0xFEC0012C] = eax & ~0xC   ; clear warm reset / shut off
 *     edi = 0x3E8                                ; 1000 retries
 *   L: if ([0xFEC00130] & 0x100) return 1        ; primary codec ready
 *      if (edi-- == 0) return 0
 *      KeStallExecutionProcessor(0x14)           ; 20 us
 *      goto L
 *
 * Returning 0 makes sub_001A73C7 hand back DSERR_NODRIVER (0x88780078). That
 * is the game object's construction status, stored at +0x10 by sub_00012210;
 * sub_00012C10 tests it and returns immediately when negative, so the run loop
 * never starts, main returns, and XAPI reboots to the dashboard. The title
 * exits cleanly having never drawn a frame. Measured before this table
 * existed: exactly 1000 KeStallExecutionProcessor calls from 0x001A6CD2, the
 * whole retry budget, then the reboot.
 *
 * Holding the bit set is a model of the console, not a way past the check: on
 * hardware the AC97 controller raises primary-codec-ready once the codec
 * leaves reset and it stays raised. Nothing here clears it, and the bit is
 * status, not state the title owns.
 *
 * ponytail: this says a codec is present, nothing more. Audio still goes
 * through src/apu and src/audio; no AC97 register beyond this one is modelled.
 */
static const struct { uint32_t offset; uint32_t ready_mask; } MCPX_READY[] = {
    /* AC97 GLOB_STA, 0xFEC00130. Bit 8 is primary codec ready. */
    { 0x400130, 0x00000100u },
};

/*
 * MCPX registers whose written bits do not stick.
 *
 * NV2A_ACK covers "software sets a bit, hardware clears it later, software
 * spins re-reading". This table covers the harder shape: hardware clears the
 * bit so fast that software never expects to see it set at all, so the
 * compiler is free to hoist the read out of the loop -- and MSVC did.
 *
 * DirectSound's AC97 bus-master reset, sub_001A6F52, is the case:
 *
 *     MEM8(cr) = 2;                 // RR, "reset this box's registers"
 *     cl = MEM8(cr) & 2;            // read back ONCE, outside the loop
 *   L: if (cl) goto L;              // never re-reads memory
 *
 * Against plain RAM the 2 sticks, the single read returns it, and the title
 * spins forever inside CDirectSound init. An acknowledging thread cannot help
 * here: it would have to land in the few-instruction window between the write
 * and the read, exactly once, and losing the race hangs the title permanently.
 * The semantic that is actually missing is write suppression, so that is what
 * this models -- the bit reads back clear because the reset already completed,
 * which is what the hardware reports.
 *
 * AC97 bus-master registers live at 0xFEC00100, in boxes of 0x10, with the
 * control byte at +0x0B and RR as bit 1. Every box is listed rather than the
 * two JSRF happens to reset: the layout is the AC97 spec's, not a title's.
 */
#define MCPX_AC97_NABM   0x400100u   /* 0xFEC00100, relative to XBOX_MCPX_BASE */
#define MCPX_AC97_BOX    0x10u
#define MCPX_AC97_CR     0x0Bu
#define MCPX_AC97_CR_RR  0x02u

static const struct { uint32_t offset; uint8_t write_clear; } MCPX_WRITE_CLEAR[] = {
    { MCPX_AC97_NABM + 0 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 1 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 2 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 3 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 4 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 5 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 6 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
    { MCPX_AC97_NABM + 7 * MCPX_AC97_BOX + MCPX_AC97_CR, MCPX_AC97_CR_RR },
};

/* APU register aperture, as an offset window inside the MCPX span. The guest's
 * statically linked DSOUND drives this hardware directly, so its writes have to
 * reach the emulated APU rather than landing in plain RAM.
 *
 * Writes only. Reads stay ordinary loads against the mapped page, which keeps a
 * polled register (the sample counter at +0x020010 is read in a spin loop) from
 * becoming millions of signals. The cost is that a register with read side
 * effects -- read-to-clear -- is not modelled; none is known to be needed here.
 *
 * Reached through a hook rather than a direct call so xbox_kernel keeps no link
 * dependency on xbox_apu: targets that link the kernel alone must still build. */
#define MCPX_APU_MMIO_OFFSET 0x000000u
#define MCPX_APU_MMIO_SIZE   0x080000u   /* 512 KB */

static void (*g_mcpx_apu_write)(uint32_t offset, uint32_t value,
                                unsigned width) = NULL;

void xbox_SetApuMmioWriteHook(void (*fn)(uint32_t, uint32_t, unsigned))
{
    g_mcpx_apu_write = fn;
}

/* Set while the write trap below is guarding those registers. When it is off
 * (unsupported host, or install failed) the ack thread keeps re-applying
 * MCPX_READY instead, which is all it could ever do for this aperture. */
static int g_mcpx_trap_active = 0;

/* Apply MCPX_READY to the aperture. Called once when the aperture is mapped so
 * the first reader sees the bit without having to race the ack thread -- the
 * poll above burns its 1000 retries in microseconds while
 * KeStallExecutionProcessor is a no-op -- and again from the thread so the bit
 * survives anything that clears it. */
static void xbox_McpxApplyReady(void)
{
    if (!g_mcpx_regs) {
        return;
    }
    for (size_t i = 0; i < sizeof(MCPX_READY) / sizeof(MCPX_READY[0]); i++) {
        volatile uint32_t *r =
            (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_READY[i].offset);
        if ((*r & MCPX_READY[i].ready_mask) != MCPX_READY[i].ready_mask) {
            *r |= MCPX_READY[i].ready_mask;
        }
    }
}

/* ================================================================
 * MCPX register write trap (POSIX / AArch64)
 * ================================================================
 *
 * The Windows build decodes device-register accesses in a vectored exception
 * handler (src/nv2a/nv2a_mmio_hook.c, src/apu/apu_mmio_hook.c). Both are
 * x86-64 decoders behind #if defined(_WIN32), so on this host there was no
 * mechanism at all -- and the APU one covers 0xFE800000+512K, which does not
 * reach AC97 at 0xFEC00000 even on Windows.
 *
 * This is the same idea with far less machinery. Only the pages holding a
 * MCPX_WRITE_CLEAR register are protected read-only, so reads stay ordinary
 * loads against mapped RAM and only a write to one of those pages traps.
 *
 * The decoder is small because the faulting address does not have to be
 * recovered from the instruction: the signal already carries it in si_addr.
 * That leaves just two fields to read out of the instruction -- how wide the
 * store was, and which register held the value -- and those sit in fixed bit
 * positions across every AArch64 store addressing mode, so the addressing mode
 * itself never has to be decoded.
 *
 * A store this cannot decode is reported and passed to the previous handler
 * rather than skipped: continuing past a store whose value is unknown would
 * corrupt state quietly, and a fault that is not ours belongs to whoever
 * installed before us.
 */
#if !defined(_WIN32) && defined(__aarch64__)

#include <signal.h>
#include <ucontext.h>

static struct sigaction g_mcpx_old_segv;
static struct sigaction g_mcpx_old_bus;
static uintptr_t g_mcpx_guard_page[8];

/* NV2A display-engine interrupt, trapped for WRITE-1-TO-CLEAR.
 *
 * NV_PMC_INTR_0 is a read-only SUMMARY: bit 24 stays asserted while the display
 * engine has an interrupt pending, and clears only when the driver
 * acknowledges at the SOURCE, NV_PCRTC_INTR_0. JSRF's vblank handler depends
 * on precisely that and spins on it:
 *
 *     mov  [nv2a+0x600100], ecx     ; ecx = 1  -- acknowledge
 *     test [nv2a+0x100], 0x1000000  ; re-read the summary
 *     jne  back                     ; spin until it clears
 *     call KeSetEvent(...)          ; only then signal the vblank event
 *
 * The acknowledge WRITES 1 to clear (matching pcrtc_write in the in-tree NV2A
 * model). Guest memory is ordinary RAM here, so that store leaves the bit set,
 * the summary never clears, and the handler spins forever -- the event is never
 * signalled and the frame loop never runs.
 * Polling cannot fix it: "the guest acked" and "the runtime raised" both store
 * a 1, so the write itself has to be observed. Hence the trap.
 */
#define XBOX_NV2A_PCRTC_INTR_0   (XBOX_NV2A_BASE + 0x600100u)
#define XBOX_NV2A_PMC_INTR_0     (XBOX_NV2A_BASE + 0x000100u)
#define XBOX_NV2A_PMC_INTR_PCRTC 0x01000000u

static uintptr_t g_nv2a_guard_page;
static int       g_nv2a_guarded;
static size_t g_mcpx_guard_pages = 0;
static size_t g_mcpx_page_size = 0;

static int g_mcpx_apu_guarded = 0;

/*
 * Serialises the unprotect/write/reprotect dance between the trap handler and
 * the ack thread. Both flip the same pages, and without this they interleave:
 * one reprotects while the other is mid-write, and the write faults with the
 * handler already inside itself. That crashed the ack thread about one run in
 * four.
 *
 * A spin on __atomic rather than a mutex because one side is a signal handler,
 * where taking a pthread mutex is not allowed. Deadlock is avoided by the
 * discipline that neither side touches guarded memory while holding it: the
 * ack thread has already unprotected, so its write cannot fault and re-enter.
 */
static volatile int g_mcpx_lock = 0;

static void mcpx_lock(void)
{
    while (__atomic_test_and_set(&g_mcpx_lock, __ATOMIC_ACQUIRE)) {
        /* spin: the holder only ever does two mprotects and one store */
    }
}

static void mcpx_unlock(void)
{
    __atomic_clear(&g_mcpx_lock, __ATOMIC_RELEASE);
}

static int mcpx_guarded_page(uintptr_t host_addr, uintptr_t *page_out)
{
    uintptr_t page = host_addr & ~(uintptr_t)(g_mcpx_page_size - 1);
    if (g_nv2a_guarded && page == g_nv2a_guard_page) {
        if (page_out) *page_out = page;
        return 1;
    }
    if (g_mcpx_apu_guarded && g_mcpx_regs) {
        uintptr_t apu = (uintptr_t)g_mcpx_regs + MCPX_APU_MMIO_OFFSET;
        if (host_addr >= apu && host_addr < apu + MCPX_APU_MMIO_SIZE) {
            if (page_out) *page_out = page;
            return 1;
        }
    }
    for (size_t i = 0; i < g_mcpx_guard_pages; i++) {
        if (g_mcpx_guard_page[i] == page) {
            if (page_out) *page_out = page;
            return 1;
        }
    }
    return 0;
}

/* Value the store would have written, after removing bits the hardware clears
 * before software can observe them. Applied per byte so a wide store covering
 * a control byte is handled the same as a byte store to it. */
static uint64_t mcpx_apply_write_clear(uint32_t guest_va, uint64_t value,
                                       unsigned width)
{
    for (unsigned b = 0; b < width; b++) {
        uint32_t off = (guest_va - XBOX_MCPX_BASE) + b;
        for (size_t i = 0; i < sizeof(MCPX_WRITE_CLEAR) / sizeof(MCPX_WRITE_CLEAR[0]); i++) {
            if (MCPX_WRITE_CLEAR[i].offset == off) {
                value &= ~((uint64_t)MCPX_WRITE_CLEAR[i].write_clear << (8 * b));
            }
        }
    }
    return value;
}

static void mcpx_trap_handler(int sig, siginfo_t *si, void *context)
{
    ucontext_t *uc = (ucontext_t *)context;
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
    uintptr_t page = 0;
    uint32_t insn, rt;
    unsigned width;
    uint64_t value;
    uint32_t guest_va;
    DWORD old_prot;

    if (!uc || !mcpx_guarded_page(fault, &page)) {
        goto chain;
    }

    insn = *(const uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;

    /* Load/store, non-SIMD, opc == 00 (store). Bits 29:27 = 111 select the
     * load/store group, bit 26 is the SIMD/FP flag, bit 25 separates the
     * unsigned-offset form from the unscaled/register-offset/indexed forms,
     * and bits 23:22 are the opcode. Everything the mask leaves out is the
     * addressing mode, which si_addr has already resolved for us. */
    if ((insn & 0x3EC00000u) != 0x38000000u) {
        fprintf(stderr,
                "  [MCPX] write trap: undecoded store 0x%08X at pc=0x%llX "
                "(guest 0x%08X) - passing to previous handler\n",
                insn, (unsigned long long)uc->uc_mcontext->__ss.__pc,
                (uint32_t)((uintptr_t)fault - g_memory_offset));
        fflush(stderr);
        goto chain;
    }

    width = 1u << (insn >> 30);          /* size field: B, H, W, X */
    rt = insn & 0x1Fu;                   /* source register */
    if (rt < 29) {
        value = uc->uc_mcontext->__ss.__x[rt];
    } else if (rt == 29) {
        value = uc->uc_mcontext->__ss.__fp;
    } else if (rt == 30) {
        value = uc->uc_mcontext->__ss.__lr;
    } else {
        value = 0;                       /* wzr / xzr, never SP for a store */
    }

    guest_va = (uint32_t)((uintptr_t)fault - g_memory_offset);
    {
        uint32_t off = guest_va - XBOX_MCPX_BASE;
        if (g_mcpx_apu_write && off < MCPX_APU_MMIO_OFFSET + MCPX_APU_MMIO_SIZE) {
            static unsigned long n = 0;
            g_mcpx_apu_write(off, (uint32_t)value, width);
            if (++n <= 8 || (n % 1000) == 0) {
                fprintf(stderr, "  [APU-MMIO] write #%lu +0x%06X = 0x%08X (%u)\n",
                        n, off, (uint32_t)value, width);
                fflush(stderr);
            }
        }
    }
    /* NV_PCRTC_INTR_0 is write-1-to-clear. xbox_Nv2aRaiseVblank bypasses the
     * guard, so every trapped write here is a guest acknowledge rather than a
     * runtime assertion. Apply the same `pending &= ~value` operation as the
     * in-tree NV2A model before performing the intercepted store. */
    if (guest_va == XBOX_NV2A_PCRTC_INTR_0 && width == 4) {
        value = *(volatile uint32_t *)fault & ~(uint32_t)value;
    } else {
        value = mcpx_apply_write_clear(guest_va, value, width);
    }

    /* Perform the store the faulting instruction was going to perform, then
     * re-arm the guard. */
    mcpx_lock();
    if (!VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READWRITE, &old_prot)) {
        mcpx_unlock();
        goto chain;
    }
    switch (width) {
    case 1: *(volatile uint8_t  *)fault = (uint8_t)value;  break;
    case 2: *(volatile uint16_t *)fault = (uint16_t)value; break;
    case 4: *(volatile uint32_t *)fault = (uint32_t)value; break;
    default: *(volatile uint64_t *)fault = value;          break;
    }
    if (guest_va == XBOX_NV2A_PCRTC_INTR_0) {
        /* The summary follows its source. Different page, not guarded, so this
         * is an ordinary store. */
        if ((value & 0x1u) == 0) {
            *(volatile uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0 + g_memory_offset)
                &= ~XBOX_NV2A_PMC_INTR_PCRTC;
        }
    } else {
        xbox_McpxApplyReady();   /* the ack thread cannot reach a guarded page */
    }
    VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READONLY, &old_prot);
    mcpx_unlock();

    uc->uc_mcontext->__ss.__pc += 4;   /* every AArch64 instruction is 4 bytes */
    return;

chain:
    {
        struct sigaction *old = (sig == SIGBUS) ? &g_mcpx_old_bus : &g_mcpx_old_segv;
        if (old->sa_flags & SA_SIGINFO) {
            if (old->sa_sigaction) {
                old->sa_sigaction(sig, si, context);
                return;
            }
        } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
            old->sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void xbox_McpxTrapInstall(void)
{
    struct sigaction sa;

    if (!g_mcpx_regs) {
        return;
    }
    g_mcpx_page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (g_mcpx_page_size == 0) {
        return;
    }

    /* Only the pages that actually hold a register with semantics. */
    for (size_t i = 0; i < sizeof(MCPX_WRITE_CLEAR) / sizeof(MCPX_WRITE_CLEAR[0]); i++) {
        uintptr_t addr = (uintptr_t)g_mcpx_regs + MCPX_WRITE_CLEAR[i].offset;
        uintptr_t page = addr & ~(uintptr_t)(g_mcpx_page_size - 1);
        int seen = 0;
        for (size_t j = 0; j < g_mcpx_guard_pages; j++) {
            if (g_mcpx_guard_page[j] == page) { seen = 1; break; }
        }
        if (!seen && g_mcpx_guard_pages < 8) {
            g_mcpx_guard_page[g_mcpx_guard_pages++] = page;
        }
    }
    if (g_mcpx_guard_pages == 0 && !g_mcpx_apu_write) {
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = mcpx_trap_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_mcpx_old_segv) != 0 ||
        sigaction(SIGBUS, &sa, &g_mcpx_old_bus) != 0) {
        fprintf(stderr, "  WARNING: MCPX write trap: sigaction failed\n");
        return;
    }

    for (size_t i = 0; i < g_mcpx_guard_pages; i++) {
        DWORD old_prot;
        if (!VirtualProtect((LPVOID)g_mcpx_guard_page[i], g_mcpx_page_size,
                            PAGE_READONLY, &old_prot)) {
            fprintf(stderr, "  WARNING: MCPX write trap: mprotect failed at %p\n",
                    (void *)g_mcpx_guard_page[i]);
            return;
        }
    }

    /* The NV2A PCRTC interrupt register, for write-1-to-clear. Guarded after
     * the handler is installed, for the same reason as the APU aperture. */
    {
        uintptr_t addr = (uintptr_t)(XBOX_NV2A_PCRTC_INTR_0 + g_memory_offset);
        uintptr_t page = addr & ~(uintptr_t)(g_mcpx_page_size - 1);
        DWORD old_prot;
        if (VirtualProtect((LPVOID)page, g_mcpx_page_size,
                           PAGE_READONLY, &old_prot)) {
            g_nv2a_guard_page = page;
            g_nv2a_guarded = 1;
        } else {
            fprintf(stderr, "  WARNING: NV2A write trap: mprotect failed on "
                    "PCRTC; the vblank acknowledge will not be observed\n");
        }
    }

    /* The APU aperture, as one range rather than a page list. Protected only
     * after the handler is installed: between the mprotect and the sigaction
     * there is no handler, and a write landing in that window would be fatal. */
    if (g_mcpx_apu_write) {
        uintptr_t apu = (uintptr_t)g_mcpx_regs + MCPX_APU_MMIO_OFFSET;
        DWORD old_prot;
        if (VirtualProtect((LPVOID)apu, MCPX_APU_MMIO_SIZE,
                           PAGE_READONLY, &old_prot)) {
            g_mcpx_apu_guarded = 1;
        } else {
            fprintf(stderr, "  WARNING: MCPX write trap: mprotect failed on the "
                    "APU aperture; its registers will not reach the model\n");
        }
    }

    g_mcpx_trap_active = 1;
    fprintf(stderr, "  MCPX write trap: %zu page(s) guarded, %zu register(s) "
            "with write-clear semantics; APU aperture %s\n",
            g_mcpx_guard_pages,
            sizeof(MCPX_WRITE_CLEAR) / sizeof(MCPX_WRITE_CLEAR[0]),
            g_mcpx_apu_guarded ? "routed to the model" : "NOT routed");
}


/* Assert the display-engine vblank: raise the PCRTC source and the PMC summary.
 *
 * This must NOT be an ordinary store. The PCRTC page is write-protected for
 * write-1-to-clear, so a plain `reg |= 1` from runtime code faults into
 * mcpx_trap_handler and is indistinguishable from the guest acknowledging --
 * it clears the very bit it was setting. Measured: every ack logged
 * "pending=0x00000000", because the raise had already been eaten.
 *
 * So drop the guard for the store, exactly as the trap handler does, under the
 * same lock. */
void xbox_Nv2aRaiseVblank(void)
{
    volatile uint32_t *pcrtc =
        (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_PCRTC_INTR_0 + g_memory_offset);
    volatile uint32_t *pmc =
        (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0 + g_memory_offset);

    if (!g_nv2a_guarded) {
        *pcrtc |= 0x1u;
        *pmc   |= XBOX_NV2A_PMC_INTR_PCRTC;
        return;
    }
    {
        DWORD old_prot;
        mcpx_lock();
        if (VirtualProtect((LPVOID)g_nv2a_guard_page, g_mcpx_page_size,
                           PAGE_READWRITE, &old_prot)) {
            *pcrtc |= 0x1u;
            VirtualProtect((LPVOID)g_nv2a_guard_page, g_mcpx_page_size,
                           PAGE_READONLY, &old_prot);
        }
        mcpx_unlock();
        *pmc |= XBOX_NV2A_PMC_INTR_PCRTC;   /* different page, not guarded */
    }
}

/* Is a vblank still pending, i.e. has the guest not acknowledged the last one?
 * Reads are permitted on the guarded page. */
int xbox_Nv2aVblankPending(void)
{
    if (!g_memory_offset) return 0;
    return (*(volatile uint32_t *)(uintptr_t)(XBOX_NV2A_PCRTC_INTR_0 + g_memory_offset)
            & 0x1u) != 0;
}

#else  /* Windows, or a host this decoder does not cover */

/* No write trap on this host, so there is no guard to drop and no way to
 * observe the guest's write-1-to-clear acknowledge either. The raise is a
 * plain store; the acknowledge will not be seen, which is the same limitation
 * this file already documents for the MCPX registers. */
void xbox_Nv2aRaiseVblank(void)
{
    if (!g_memory_offset) return;
    *(volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + 0x600100u + g_memory_offset) |= 0x1u;
    *(volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + 0x000100u + g_memory_offset) |= 0x01000000u;
}

int xbox_Nv2aVblankPending(void)
{
    if (!g_memory_offset) return 0;
    return (*(volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + 0x600100u + g_memory_offset)
            & 0x1u) != 0;
}


static void xbox_McpxTrapInstall(void)
{
    /* Windows routes device registers through the VEH hooks instead; other
     * hosts get the ack thread only, which cannot model write suppression. */
}

#endif

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
            if (*r & NV2A_ACK[i].busy_mask) {
                *r &= ~NV2A_ACK[i].busy_mask;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }
        {
            volatile uint32_t *put =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            volatile uint32_t *get =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET);
            if (*get != *put) {
                *get = *put;
            }
        }
        if (g_mcpx_regs) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                if (g_mcpx_apu_guarded) {
                    uintptr_t pg = (uintptr_t)c
                                   & ~(uintptr_t)(g_mcpx_page_size - 1);
                    DWORD op;
                    mcpx_lock();
                    if (VirtualProtect((LPVOID)pg, g_mcpx_page_size,
                                       PAGE_READWRITE, &op)) {
                        *c += 1;
                        VirtualProtect((LPVOID)pg, g_mcpx_page_size,
                                       PAGE_READONLY, &op);
                    }
                    mcpx_unlock();
                } else {
                    *c += 1;
                }
            }
            /* Guarded pages cannot be written from here; the trap re-applies
             * MCPX_READY after each intercepted write instead. */
            if (!g_mcpx_trap_active) {
                xbox_McpxApplyReady();
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = GetTickCount();

        Sleep(0);  /* yield; the waiter is spinning on another core */
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;
RECOMP_TLS uint32_t g_fs_base = XBOX_PRIMARY_TIB_VA;

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;

/* MMX register file. See recomp_types.h for why these exist. */
RECOMP_TLS uint64_t g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    g_memory_size = g_xbox_total_ram;

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of size (64 MB) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

#if defined(__APPLE__)
    /* macOS has no safe MAP_FIXED_NOREPLACE equivalent for replacing a
     * reservation with a file-backed view. Reserve the whole 29-view span at
     * an OS-selected address first; subsequent MAP_FIXED calls may then only
     * replace pages that this runtime already owns. */
    g_host_reservation_size =
        g_memory_size * (size_t)(XBOX_NUM_MIRRORS + 1);
    g_host_reservation = mmap(NULL, g_host_reservation_size, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_host_reservation == MAP_FAILED) {
        g_host_reservation = NULL;
        g_host_reservation_size = 0;
        fprintf(stderr, "xbox_MemoryLayoutInit: host reservation failed\n");
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }
    fprintf(stderr, "Xbox host reservation:\n");
    fprintf(stderr, "base = %p\n", g_host_reservation);
    fprintf(stderr, "size = %zu\n", g_host_reservation_size);
#endif

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
#if defined(__APPLE__)
        uintptr_t target = (uintptr_t)g_host_reservation;
        if (!host_reservation_contains(target, g_memory_size)) {
            fprintf(stderr, "xbox_MemoryLayoutInit: base RAM target outside host reservation\n");
        } else {
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle, FILE_MAP_ALL_ACCESS, 0, 0,
                g_memory_size, (LPVOID)target);
            if (g_memory_base != (void *)target) {
                fprintf(stderr, "xbox_MemoryLayoutInit: base RAM mapping failed inside reservation\n");
                g_memory_base = NULL;
            }
        }
#else
        static const uintptr_t try_bases[] = {
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        /* macOS portability: iterate ALL entries including the trailing 0 (NULL)
         * sentinel. On macOS the low fixed addresses (0x10000..0x10000000) are
         * reserved and MAP_FIXED fails for every one of them, so the run only
         * succeeds via the final NULL hint (OS chooses a high address). The old
         * condition `try_bases[i] != 0 || i == 0` stopped at the sentinel without
         * trying it, which masked fine on Linux (low VA is mappable there). */
        for (size_t i = 0; i < sizeof(try_bases) / sizeof(try_bases[0]); i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
#endif
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
#if defined(__APPLE__)
        if (g_host_reservation) {
            munmap(g_host_reservation, g_host_reservation_size);
            g_host_reservation = NULL;
            g_host_reservation_size = 0;
        }
#endif
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }
#if defined(__APPLE__)
    fprintf(stderr, "guest 0x00000000 -> host %p\n", g_memory_base);
#endif

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base (top of stack)
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(0x18, 0x00000000);       /* Self pointer (TIB at VA 0) */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        MEM32_INIT(0x20, 0x00000000);

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        MEM32_INIT(0x28, XBOX_PRIMARY_TLS_CONTEXT_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(XBOX_PRIMARY_TLS_CONTEXT_VA + 0x28,
                   XBOX_PRIMARY_TLS_DATA_VA);

        fprintf(stderr, "  TIB: fake TIB at VA 0x0, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_PRIMARY_TLS_CONTEXT_VA, XBOX_PRIMARY_TLS_DATA_VA);

        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        g_contig_memory = VirtualAlloc(
            (LPVOID)contig_native,
            XBOX_CONTIG_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            xbox_McpxApplyReady();
            xbox_McpxTrapInstall();
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed; %zu status bit(s) held "
                    "ready)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE,
                    sizeof(MCPX_READY) / sizeof(MCPX_READY[0]));
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
#if defined(__APPLE__)
            if (!host_reservation_contains(mirror_base, g_memory_size)) {
                fprintf(stderr,
                        "  Mirror %d: target %p outside host reservation\n",
                        m + 1, (void *)mirror_base);
                continue;
            }
#endif
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
#if defined(__APPLE__)
    if (g_host_reservation) {
        munmap(g_host_reservation, g_host_reservation_size);
        g_host_reservation = NULL;
        g_host_reservation_size = 0;
    }
#endif
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

BOOL xbox_HostAddressToGuest(uintptr_t host_address, uint32_t *guest_address)
{
    uintptr_t base;

    if (!guest_address) return FALSE;

#define MAP_HOST_RANGE(ptr, size, guest_base) do {                         \
    base = (uintptr_t)(ptr);                                                \
    if (base && host_address >= base && host_address - base < (size_t)(size)) { \
        *guest_address = (uint32_t)((guest_base) + (host_address - base));   \
        return TRUE;                                                        \
    }                                                                       \
} while (0)

    MAP_HOST_RANGE(g_memory_base, g_memory_size, 0u);
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        MAP_HOST_RANGE(g_mirror_views[m], g_memory_size,
                       (uint32_t)((m + 1) * g_memory_size));
    }
    MAP_HOST_RANGE(g_contig_memory, XBOX_CONTIG_SIZE, XBOX_CONTIG_BASE);
    MAP_HOST_RANGE(g_nv2a_memory, XBOX_NV2A_SIZE, XBOX_NV2A_BASE);
    MAP_HOST_RANGE(g_mcpx_memory, XBOX_MCPX_SIZE, XBOX_MCPX_BASE);

#undef MAP_HOST_RANGE
    return FALSE;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
#define XBOX_MAX_THREAD_STACKS  8

static int g_thread_stacks_used = 0;

uint32_t xbox_AllocThreadStack(void)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }
    base = XBOX_STACK_BASE +
           (uint32_t)g_thread_stacks_used * XBOX_THREAD_STACK_SIZE;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + XBOX_THREAD_STACK_SIZE - 16;
}

void xbox_SetupCurrentThreadTib(uint32_t tib_va, uint32_t tls_context_va,
                               uint32_t tls_data_va, uint32_t tls_data_size,
                               uint32_t stack_top, uint32_t stack_limit)
{
    uint8_t *tib = (uint8_t *)((uintptr_t)tib_va + g_memory_offset);
    uint8_t *tls_context =
        (uint8_t *)((uintptr_t)tls_context_va + g_memory_offset);

    /* The Xbox executable's thread-start wrapper initializes the TLS payload
     * itself. The kernel supplies the per-thread TIB fields and a pointer to
     * that payload. In this ABI fs:[4] is the end of the TLS allocation: a
     * negative XBE TLS index walks back to its leading pointer slot. */
    memset(tib, 0, 0x30);
    memset(tls_context, 0, 0x2C);
    *(uint32_t *)(tib + 0x00) = 0xFFFFFFFFu;
    *(uint32_t *)(tib + 0x04) = tls_data_va + tls_data_size;
    *(uint32_t *)(tib + 0x08) = stack_limit;
    *(uint32_t *)(tib + 0x18) = tib_va;
    *(uint32_t *)(tib + 0x20) = 0;
    *(uint32_t *)(tib + 0x28) = tls_context_va;
    *(uint32_t *)(tls_context + 0x28) = tls_data_va;
    g_fs_base = tib_va;

    fprintf(stderr,
            "  TIB: thread FS=0x%08X TLS=0x%08X..0x%08X stack=0x%08X..0x%08X\n",
            tib_va, tls_data_va, tls_data_va + tls_data_size,
            stack_limit, stack_top);
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    for (int i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
            continue;
        }
        if (g_heap_blocks[i].addr & (alignment - 1)) {
            continue;   /* wrong alignment for this request */
        }
        g_heap_blocks[i].free = 0;
        result = g_heap_blocks[i].addr;
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        return 0;
    }

    g_heap_next = result + size;

    /* Zero-fill the allocated block (Xbox memory is always zeroed) */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    return result;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. */
        if (i + 1 < g_heap_block_count && g_heap_blocks[i + 1].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[i + 1].addr) {
            g_heap_blocks[i].size += g_heap_blocks[i + 1].size;
            g_heap_blocks[i + 1].size = 0;
            g_heap_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_heap_blocks[i - 1].free &&
            g_heap_blocks[i - 1].addr + g_heap_blocks[i - 1].size == g_heap_blocks[i].addr) {
            g_heap_blocks[i - 1].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        return;
    }
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
