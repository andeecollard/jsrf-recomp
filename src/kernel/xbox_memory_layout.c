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
#include "xbox_usb_ohci.h"
#include "kernel.h"
#include "recomp_mem_watch.h"
#include "recomp_gpu_own.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <sys/mman.h>
#endif
#include <setjmp.h>

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

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
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */
static BOOL g_separate_reserve_space = FALSE;
static void reserve_reset(void);

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}

void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

size_t xbox_GetMappedSize(void)
{
    return g_memory_size;
}

void xbox_EnableSeparateReserveSpace(size_t bytes)
{
    if (bytes > UINT32_MAX - g_xbox_total_ram)
        bytes = UINT32_MAX - g_xbox_total_ram;
    g_xbox_map_size = g_xbox_total_ram + bytes;
    g_separate_reserve_space = bytes != 0;
}

BOOL xbox_SeparateReserveSpaceEnabled(void)
{
    return g_separate_reserve_space;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static uint32_t g_mirror_mask;
static void *g_tiled_view = NULL;

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;
static void *g_physical_heap_view = NULL;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two. */
static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    return g_memory_size < max ? g_memory_size : max;
}

/* Set when a push-buffer executor is publishing DMA_GET from real progress;
 * see the acknowledgement in nv2a_ack_thread. */
int g_nv2a_pusher_owns_dma_get = 0;

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
 * Free-running counters in the MCPX aperture -- the fallback for a target that
 * has no APU model behind the aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * 0xFE820010 exactly this way, which is where Halo stopped once input
 * initialisation started working.
 *
 * Ticking it made that wait finish, but it is the wrong register: 0x020010 is
 * NV1BA0_PIO_FREE, free space in the front end's method FIFO, and the guest is
 * asking "is there room for my methods" rather than "has the clock passed N".
 * When the model answers VP reads it answers that question directly and this
 * loop is skipped -- which matters for more than tidiness, because advancing
 * the counter meant unprotecting the page holding the voice-submission
 * registers several hundred thousand times a second. See mcpx_trap_handler.
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
/* MCPX registers whose behaviour plain RAM cannot provide.
 *
 * The aperture is backed as ordinary memory so a read returns zero instead of
 * faulting, which is right for everything nothing depends on. Two OHCI
 * registers are not in that category, and both were measured with
 * RECOMP_OHCI_DUMP rather than assumed:
 *
 *   HcCommandStatus (0xFED00008) bit 0, HostControllerReset, is SELF-CLEARING.
 *   Hardware drops it when the reset completes. XPP sets it during init,
 *   stalls, and on RAM it is still set afterwards -- a reset that never
 *   finishes.
 *
 *   HcRhDescriptorA (0xFED00048) low byte, NDP, is the number of downstream
 *   ports and is READ-ONLY. XPP writes the register to set NPS and NOCP, and
 *   against RAM that write also lands on NDP, leaving it zero. A root hub with
 *   no ports can never have anything attached, so no controller is ever found
 *   however the rest of the stack behaves.
 *
 * Two ports is the conventional split for one Xbox OHCI; it is a choice, not a
 * measurement, and RECOMP_OHCI_PORTS overrides it.
 *
 * This makes the root hub report itself honestly. It does NOT attach a device:
 * HcRhPortStatus stays zero, which is a correct "nothing plugged in". Giving
 * the title a pad needs real transfer emulation and is not this.
 */
static const struct { uint32_t offset; uint32_t clear_mask; } MCPX_ACK[] = {
    { 0x500008, 0x00000001u },   /* OHCI HcCommandStatus, HCR self-clears */
};

static unsigned xbox_OhciPorts(void)
{
    static unsigned n;
    if (!n) {
        const char *e = getenv("RECOMP_OHCI_PORTS");
        long v = e ? strtol(e, NULL, 0) : 2;
        if (v < 0) v = 0;
        if (v > 15) v = 15;
        n = (unsigned)v | 0x100u;   /* 0x100 marks "resolved", not a port bit */
    }
    return n & 0xFFu;
}

/* Enforce the two above on the MCPX aperture. Cheap enough to run beside the
 * NV2A table on every tick, which is where the existing note said this
 * belonged. */
static void mcpx_hw_store(uint32_t offset, uint32_t value);
/* Several of those inside ONE guard window; see the definition. */
static void mcpx_hw_store_n(const uint32_t *offset, const uint32_t *value,
                            unsigned n);
static void mcpx_hw_store_n_or_last(const uint32_t *offset, const uint32_t *value,
                                    unsigned n, uint32_t or_bits);

static void xbox_McpxHoldRegisters(void)
{
    if (!g_mcpx_regs)
        return;
    for (size_t i = 0; i < sizeof(MCPX_ACK) / sizeof(MCPX_ACK[0]); i++) {
        volatile uint32_t *r =
            (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_ACK[i].offset);
        if (*r & MCPX_ACK[i].clear_mask)
            *r &= ~MCPX_ACK[i].clear_mask;
    }
    {
        volatile uint32_t *rh =
            (volatile uint32_t *)((char *)g_mcpx_regs + 0x500048);
        unsigned ndp = xbox_OhciPorts();
        if ((*rh & 0xFFu) != ndp)
            *rh = (*rh & ~0xFFu) | ndp;
    }

    /* HcInterruptEnable/HcInterruptDisable are handled at write time by the
     * MCPX trap (see MCPX_OHCI_INTR_ENABLE). Sampling them here as well would
     * resurrect bits a disable had just cleared. */

    /* RECOMP_OHCI_ATTACH=1: report a device on root-hub port 1.
     *
     * Purely a probe, and the question it asks is whether XPP looks at the
     * port at all. Setting CurrentConnectStatus with ConnectStatusChange is
     * how hardware announces a plug, so a stack that polls the root hub should
     * respond by resetting the port and starting enumeration -- which would
     * show up as writes to this register and as endpoint descriptors appearing
     * in the HCCA. A stack that never looks will leave the change bit standing
     * for ever, and that answers it too.
     *
     * Set once. Nothing here can complete an enumeration: there are no
     * descriptors and no transfer service behind it, so if the title does
     * respond it will ask questions this cannot answer. Finding out which of
     * those two happens is the entire point. */
    {
        static int attached;
        const char *attach = getenv("RECOMP_OHCI_ATTACH");
        if (!attached && attach && attach[0] != '\0' && strcmp(attach, "0") != 0) {
            volatile uint32_t *ps =
                (volatile uint32_t *)((char *)g_mcpx_regs + 0x500054);
            volatile uint32_t *ctl =
                (volatile uint32_t *)((char *)g_mcpx_regs + 0x500004);
            volatile uint32_t *ien =
                (volatile uint32_t *)((char *)g_mcpx_regs + 0x500010);
            /* Hot-plug only after the title has made the controller
             * operational AND enabled the root-hub interrupt and its master
             * gate.  Testing HcControl alone announced JSRF's pad while
             * HcInterruptEnable was still zero and before KeConnectInterrupt:
             * the synthetic device ISR later declined that stale event and
             * enumeration never began.  Hardware cannot deliver a connect
             * notification before the driver is listening for it. */
            if ((*ctl & 0xC0u) == 0x80u
                    && (*ien & 0x80000040u) == 0x80000040u) {
                volatile uint32_t *ist =
                    (volatile uint32_t *)((char *)g_mcpx_regs + 0x50000C);
                uint32_t off[3], val[3];
                unsigned n = 0;
                attached = 1;
                /* Connect and its interrupt in ONE window. Two windows let the
                 * driver's acknowledge land untrapped between them; see
                 * mcpx_hw_store_n. */
                off[n] = 0x500054u; val[n++] = 0x00010001u;
                /* A connect that raises no interrupt is invisible: XPP has
                 * RootHubStatusChange enabled in HcInterruptEnable and is
                 * waiting on it, so the status bit is what actually announces
                 * the plug. */
                off[n] = 0x50000Cu; val[n++] = *ist | 0x00000040u;   /* RHSC */
                /* RECOMP_OHCI_MIE=1 also sets MasterInterruptEnable.
                 *
                 * Strictly a probe, and a dishonest one: bit 31 of
                 * HcInterruptEnable belongs to the driver, and the title has
                 * written 0x40 there -- RootHubStatusChange without the master
                 * enable. Its ISR at 0x001C288F reads HcInterruptStatus and
                 * HcInterruptEnable, ANDs them, and then tests bit 31 of the
                 * enable separately; with the master bit clear it declines
                 * every time, which is what "device ISR vector 1 -> FALSE"
                 * has been reporting.
                 *
                 * The question this asks is whether that gate is the only
                 * thing between here and enumeration. If the ISR claims the
                 * interrupt and the title starts issuing control transfers,
                 * the remaining work is the transfer service. If it claims and
                 * nothing follows, the title is waiting on something else and
                 * faking its register told us so cheaply. Either answer is
                 * worth one line; neither is a fix. */
                if (getenv("RECOMP_OHCI_MIE")) {
                    off[n] = 0x500010u; val[n++] = *ien | 0x80000000u;
                }
                mcpx_hw_store_n(off, val, n);
                (void)ps;            /* announced as hardware, not as the guest */
                fprintf(stderr, "  [OHCI] attach probe: port1=0x%08X "
                        "intr_status=0x%08X intr_enable=0x%08X%s\n",
                        *ps, *ist, *ien,
                        getenv("RECOMP_OHCI_MIE") ? " (MIE forced, probe)" : "");
                fflush(stderr);
            }
        }
    }
}

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

/* OHCI HcInterruptEnable / HcInterruptDisable, at the USB0 register block.
 *
 * They are a set/clear pair: a 1 written to Enable sets that bit, a 1 written
 * to Disable clears it, and both read back the same mask. As plain memory each
 * write replaced the register, and JSRF's XPP driver writes both -- its
 * controller start sets MasterInterruptEnable and sub_001BD295 then writes
 * RootHubStatusChange alone, throwing the master bit away. Its ISR tests bit 31
 * separately and declined every interrupt for the whole run.
 *
 * Sampling cannot recover this: the two writes land microseconds apart in init,
 * so a poll only ever sees the second. It has to be observed at write time. */
#define MCPX_OHCI_INTR_ENABLE   0x500010u
#define MCPX_OHCI_INTR_DISABLE  0x500014u
#define MCPX_OHCI_PORT0         0x500054u
#define MCPX_OHCI_PORT1         0x500058u

/* HcRhPortStatus, which is not a value register either.
 *
 * The low half is a set of commands -- writing a 1 to bit 1 enables the port,
 * to bit 4 resets it, to bit 8 powers it -- and the high half is five change
 * bits that are write-1-to-clear. As plain memory the driver's acknowledge
 * stored the value instead: JSRF writes 0x00010000 to clear ConnectStatusChange
 * and that wiped CurrentConnectStatus with it, so the device vanished the
 * moment it was noticed.
 *
 * A reset completes instantly here. Hardware drives it for 10 ms and then
 * clears PortResetStatus, sets PortEnableStatus and raises PortResetStatusChange;
 * there is nothing to wait for, so do all three at once and raise the root-hub
 * status change with them. */
uint32_t xbox_OhciPortWrite(uint32_t current, uint32_t v)
{
    uint32_t n = current;

    /* A guest write first acknowledges change bits that were already
     * pending. Reset completion is a later hardware event; although this
     * model completes it in the same host call, its new PRSC must therefore
     * be applied after the write-1-to-clear half. XPP leaves unrelated upper
     * bits in its reset write (observed as 0x009E0010), so doing this in the
     * opposite order clears the completion we just generated. */
    n &= ~(v & 0x001F0000u);                 /* old change bits: W1C */
    if (v & 0x0001u) n &= ~0x0002u;          /* ClearPortEnable */
    if (v & 0x0002u) n |=  0x0002u;          /* SetPortEnable */
    if (v & 0x0004u) n |=  0x0004u;          /* SetPortSuspend */
    if (v & 0x0008u) n &= ~0x0004u;          /* ClearSuspendStatus */
    if (v & 0x0100u) n |=  0x0100u;          /* SetPortPower */
    if (v & 0x0200u) n &= ~0x0100u;          /* ClearPortPower */
    if (v & 0x0010u) {                       /* SetPortReset */
        if (n & 0x0001u) {                   /* only if something is attached */
            n &= ~0x0010u;                   /* reset already finished */
            n |=  0x0002u;                   /* port enabled */
            n |=  0x00100000u;               /* PortResetStatusChange */
        }
    }
    return n;
}

/* Dump the control schedule as XPP publishes and then activates its head. This
 * is an opt-in, read-only instrument for building the transfer service from
 * the title's actual ED/TD layout rather than assumptions. */
/* Guest address to host pointer for everything in the OHCI path.
 *
 * Descriptors used to live in the low 64 MB and `g_memory_base + va` was
 * enough. They do not any more: the contiguous allocator returns addresses in
 * the 0x80000000 window, which this file backs with its own VirtualAlloc
 * rather than as an alias of guest RAM, so both following the raw address and
 * masking off its high bit read the wrong bytes. xbox_GpuMemoryRange is the
 * translation that already knows about both ranges. */
static void *ohci_resolve(uint32_t va, uint32_t bytes)
{
    return xbox_GpuMemoryRange(va, bytes);
}

static void ohci_trace_control_ed(uint32_t ed_va)
{
    static unsigned dumps;
    uint32_t *ed;
    uint32_t td_va, tail_va;

    if (!getenv("RECOMP_OHCI_TRANSFER_TRACE") || ++dumps > 32)
        return;
    ed_va &= ~0xFu;
    ed = (uint32_t *)ohci_resolve(ed_va, 16u);
    if (!ed)
        return;
    td_va = ed[2] & ~0xFu;
    tail_va = ed[1] & ~0xFu;
    fprintf(stderr,
            "  [OHCI-ED] #%u va=%08X flags=%08X tail=%08X head=%08X next=%08X\n",
            dumps, ed_va, ed[0], ed[1], ed[2], ed[3]);
    for (unsigned i = 0; td_va && td_va != tail_va && i < 16; ++i) {
        uint32_t *td;
        uint32_t cbp, next, be;
        td = (uint32_t *)ohci_resolve(td_va, 16u);
        if (!td)
            break;
        cbp = td[1];
        next = td[2] & ~0xFu;
        be = td[3];
        fprintf(stderr,
                "  [OHCI-TD] ed=%08X i=%u va=%08X flags=%08X cbp=%08X"
                " next=%08X be=%08X data=",
                ed_va, i, td_va, td[0], cbp, td[2], be);
        if (cbp && cbp < g_memory_size) {
            size_t bytes = 16;
            if (be >= cbp && (size_t)(be - cbp) + 1u < bytes)
                bytes = (size_t)(be - cbp) + 1u;
            if (bytes > g_memory_size - cbp)
                bytes = g_memory_size - cbp;
            const uint8_t *p = (const uint8_t *)g_memory_base + cbp;
            for (size_t j = 0; j < bytes; ++j)
                fprintf(stderr, "%s%02X", j ? " " : "", p[j]);
        }
        fputc('\n', stderr);
        if (next == td_va)
            break;
        td_va = next;
    }
    fflush(stderr);
}

/* OHCI operational registers, as offsets inside the MCPX aperture. The whole
 * block from HcRevision to HcRhPortStatus1 sits inside the single guarded page
 * that starts at 0x500000, so the trap handler can reach any of them while it
 * has that page unprotected for the intercepted store. */
#define MCPX_OHCI_INTR_STATUS   0x50000Cu
#define MCPX_OHCI_HCCA          0x500018u
#define MCPX_OHCI_CONTROL_HEAD  0x500020u
#define MCPX_OHCI_BULK_HEAD     0x500028u
#define MCPX_OHCI_DONE_HEAD     0x500030u
#define MCPX_OHCI_FM_NUMBER     0x50003Cu

/* Run one list against the device model.
 *
 * Called from the write trap the moment the title rings ControlListFilled, so
 * a transfer completes in the same instruction that submitted it. Hardware
 * would take a frame; the driver cannot tell the difference, because it learns
 * of completion through the done queue either way.
 *
 * The register writebacks are deliberately left in `svc` for the caller to
 * apply after it has unprotected the page. Guest RAM, where the descriptors
 * live, is not guarded, so the walk itself is an ordinary set of loads and
 * stores. */
static uint32_t g_ohci_frame;

static unsigned ohci_service(uint32_t head_ed, xbox_ohci_service *svc)
{
    /* head_ed 0 is legitimate: it means "publish whatever is already sitting
     * in HcDoneHead", which is how a writeback deferred behind an
     * unacknowledged interrupt eventually reaches the driver. */
    if (!g_memory_base || !g_mcpx_regs)
        return 0;

    memset(svc, 0, sizeof(*svc));
    svc->ram = (uint8_t *)g_memory_base;
    svc->ram_size = (uint32_t)g_memory_size;
    svc->resolve = ohci_resolve;
    svc->head_ed = head_ed;
    svc->hcca = *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_HCCA);
    svc->done_head =
        *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_DONE_HEAD);
    svc->intr_status =
        *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_INTR_STATUS);
    svc->intr_enable =
        *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_INTR_ENABLE);
    svc->frame_number = g_ohci_frame;
    xbox_OhciServiceList(svc);
    /* "Is there a register writeback owed", which is not the same as "did a TD
     * retire". A pass that publishes a queue held over from an earlier one
     * retires nothing and still owes HcDoneHead and the interrupt; a pass that
     * retires TDs but defers behind an unacknowledged interrupt owes
     * HcDoneHead alone. Returning the TD count alone left HcDoneHead standing
     * and the tick republished the same queue for ever. */
    return svc->published || svc->tds_retired;
}

/* Run one frame of the periodic list.
 *
 * The control list has a doorbell and the periodic list does not: an interrupt
 * endpoint is polled by the controller for the life of the connection, so the
 * pad report only ever arrives if something ticks. This is that tick, driven
 * off the NV2A ack thread because it is the one timer this file already owns.
 *
 * Hardware visits one of the HCCA's 32 interrupt-table entries per frame and
 * that is copied here rather than servicing all 32 at once: a driver that
 * spreads endpoints across the table gets each of them polled at the interval
 * it asked for, and the walk stays bounded whatever the table contains.
 */
static void ohci_periodic_tick(void)
{
    static DWORD last_ms;
    volatile uint32_t *ctl, *ist, *ien;
    xbox_ohci_service svc;
    uint32_t hcca, entry, head;
    DWORD now;

    /* mcpx_hw_store falls back to a plain store where no trap is installed, so
     * this needs no host check of its own. */
    if (!g_mcpx_regs || !g_memory_base)
        return;

    /* A USB frame is 1 ms. The ack thread runs far hotter than that, and an
     * interrupt endpoint polled at host speed would bury the driver. */
    now = GetTickCount();
    if (now == last_ms)
        return;
    last_ms = now;
    g_ohci_frame++;

    ctl = (volatile uint32_t *)((char *)g_mcpx_regs + 0x500004u);
    ist = (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_INTR_STATUS);
    ien = (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_INTR_ENABLE);

    /* UsbOperational. Below this the controller is not running frames at all. */
    if ((*ctl & 0xC0u) != 0x80u)
        return;

    hcca = *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_HCCA);

    /* The frame counter lives in the HCCA, which is ordinary guest RAM: free
     * to advance every frame. */
    if (hcca) {
        uint32_t *fn = (uint32_t *)ohci_resolve((hcca & ~0xFFu)
                                                + XBOX_OHCI_HCCA_FRAME_NUMBER,
                                                4u);
        if (fn)
            *fn = g_ohci_frame & 0xFFFFu;
    }

    /* StartOfFrame, but only while the driver is actually counting frames.
     *
     * XPP's enumeration turns SOF on and counts frames to time the 2 ms a
     * device may take after SET_ADDRESS before it has to answer at its new
     * address. Measured in claude-usb-transfer-service-02: the title completes
     * SET_ADDRESS(1), writes HcInterruptEnable <= 0x4, and then waits, because
     * nothing here had ever advanced a frame.
     *
     * Hardware raises the status bit every frame whether or not the driver has
     * enabled it, and this deliberately does not: these registers are on a
     * guarded page, and mcpx_hw_store has to leave that page unprotected
     * across two mprotect calls to reach them. At frame rate that window is
     * open often enough to swallow a guest doorbell write untrapped, which
     * costs an entire transfer. Gating on the enable keeps the window shut
     * except during the few milliseconds the answer is wanted. The driver
     * acknowledges any stale SOF before it enables the bit -- the log shows it
     * doing exactly that -- so nothing is lost by starting the count late. */
    if (*ien & XBOX_OHCI_INTR_SF) {
        uint32_t off[2], val[2];
        off[0] = MCPX_OHCI_FM_NUMBER; val[0] = g_ohci_frame & 0xFFFFu;
        off[1] = MCPX_OHCI_INTR_STATUS; val[1] = *ist | XBOX_OHCI_INTR_SF;
        mcpx_hw_store_n(off, val, 2);
    }

    /* PeriodicListEnable gates only the list walk below it. */
    if (!(*ctl & 0x04u))
        return;
    /* One done queue at a time. Hardware will not overwrite the HCCA's done
     * head while the driver still owes it a WritebackDoneHead acknowledge, and
     * re-raising underneath an unacknowledged interrupt is the same mistake
     * the vblank path documents next door. */
    if (*ist & XBOX_OHCI_INTR_WDH)
        return;

    /* This frame's interrupt-table entry, if the HCCA has one. A zero head is
     * not a reason to stop: the pass still flushes a deferred done queue. */
    head = 0;
    if (hcca) {
        uint32_t *slot;
        entry = (hcca & ~0xFFu) + (g_ohci_frame & 31u) * 4u;
        slot = (uint32_t *)ohci_resolve(entry, 4u);
        if (slot)
            head = *slot;
    }

    if (ohci_service(head, &svc)) {
        /* Through the hardware store: these registers are on a guarded page
         * and this is the runtime asserting a value, not the guest writing
         * one. Status last, for the same reason as in the trap. */
        uint32_t off[3], val[3];
        off[0] = MCPX_OHCI_DONE_HEAD;   val[0] = svc.done_head;
        off[1] = MCPX_OHCI_FM_NUMBER;   val[1] = svc.frame_number;
        /* Status last: it is the bit the ISR gates on, so everything it goes
         * on to read must already be in place -- and inside the same window,
         * so the driver cannot see the interrupt before the queue. */
        off[2] = MCPX_OHCI_INTR_STATUS;
        /* val[2] is unused for the OR form; the bits go in separately so the
         * read-modify-write happens inside the lock. Passing `*ist | bits`
         * here is what let a guest acknowledge be resurrected. */
        val[2] = 0;
        mcpx_hw_store_n_or_last(off, val, 3,
                                svc.intr_status & XBOX_OHCI_INTR_WDH);
    }
}

/* Apply what the service produced. The caller holds the page unprotected.
 *
 * The status register is OR-ed rather than assigned. The service was handed a
 * copy of it and only ever adds to that copy, while the guest's ISR clears
 * bits from another thread; storing the copy back whole would resurrect an
 * acknowledge that landed in between. */
static void ohci_service_commit(const xbox_ohci_service *svc)
{
    *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_DONE_HEAD) =
        svc->done_head;
    *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_FM_NUMBER) =
        svc->frame_number;
    /* Last, because it is the bit the ISR gates on: everything it will go on
     * to read must already be in place. */
    *(volatile uint32_t *)((char *)g_mcpx_regs + MCPX_OHCI_INTR_STATUS) |=
        (svc->intr_status & XBOX_OHCI_INTR_WDH);
}

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
 * With a read hook, the main register bank traps reads and writes so status
 * and interrupt acknowledgements use model state. The separate VP aperture
 * retains mapped reads, including the frequently polled counter at +0x020010.
 *
 * Reached through a hook rather than a direct call so xbox_kernel keeps no link
 * dependency on xbox_apu: targets that link the kernel alone must still build. */
#define MCPX_APU_MMIO_OFFSET 0x000000u
#define MCPX_APU_MMIO_SIZE   0x080000u   /* 512 KB */
/* The part of it the model answers reads for: the main registers at 0x00000
 * and the voice processor's PIO window at 0x20000. Everything above that (GP
 * at 0x30000, EP at 0x50000) has no model behind it, so it stays plain memory
 * rather than reading back as a zero the guest did not write. */
#define MCPX_APU_MODEL_SIZE  0x030000u

static void (*g_mcpx_apu_write)(uint32_t offset, uint32_t value,
                                unsigned width) = NULL;
static uint32_t (*g_mcpx_apu_read)(uint32_t offset, unsigned width) = NULL;

void xbox_SetApuMmioReadHook(uint32_t (*fn)(uint32_t, unsigned))
{
    g_mcpx_apu_read = fn;
}

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
/* Needed by both the AArch64 write trap and the Windows VEH below. */
#define XBOX_NV2A_PCRTC_INTR_0   (XBOX_NV2A_BASE + 0x600100u)
#define XBOX_NV2A_PMC_INTR_0     (XBOX_NV2A_BASE + 0x000100u)
#define XBOX_NV2A_PMC_INTR_PCRTC 0x01000000u
#define XBOX_NV2A_PGRAPH_INTR     (XBOX_NV2A_BASE + 0x400100u)
#define XBOX_NV2A_PGRAPH_ERROR    0x00100000u
#define XBOX_NV2A_PMC_INTR_PGRAPH 0x00001000u
/* The software-method trap the guest reads after a PGRAPH notify. */
#define XBOX_NV2A_PGRAPH_TRAPPED_ADDR (XBOX_NV2A_BASE + 0x400704u)
#define XBOX_NV2A_PGRAPH_TRAPPED_DATA (XBOX_NV2A_BASE + 0x400708u)
#define XBOX_NV2A_PGRAPH_NSOURCE      (XBOX_NV2A_BASE + 0x400108u)
#define XBOX_NV2A_PGRAPH_FIFO_ACCESS  (XBOX_NV2A_BASE + 0x400720u)

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

static uintptr_t g_nv2a_pgraph_page;
static int g_nv2a_pgraph_guarded;
static uintptr_t g_nv2a_guard_page;
static int       g_nv2a_guarded;
static size_t g_mcpx_guard_pages = 0;
static size_t g_mcpx_page_size = 0;

static int g_mcpx_apu_guarded = 0;
/* Set when the model answers reads for the span below MCPX_APU_MODEL_SIZE, so
 * nothing else has to keep a plausible value in that RAM. */
static int g_mcpx_apu_read_trapped = 0;

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

/* Guard accounting for the APU aperture.
 *
 * A guest store to a guarded page reaches the model only if the page is
 * read-only at the instant of the store. Two things drop that guard on
 * purpose -- this handler, and the ack thread's free-space counter, which
 * lives at 0x020010 on the same page as the voice-submission registers at
 * 0x020120-0x020304 -- and either reprotect can fail, which both call sites
 * used to discard silently.
 *
 * These separate the two ways a write can go missing. A raised leak count
 * with reprotect failures at zero is a race against an open window; a
 * reprotect failure is a guard that is gone for the rest of the run. */
static unsigned long g_mcpx_trap_faults;        /* stores that did fault */
static unsigned long g_mcpx_trap_apu_writes;    /* ... and reached the APU */
static unsigned long g_mcpx_trap_apu_vp_writes; /* ... in the VP region */
static unsigned long g_mcpx_reprotect_failures; /* guard lost, permanently */
static unsigned long g_mcpx_ack_windows;        /* ack-thread open/close pairs */

void xbox_McpxTrapReport(void)
{
    fprintf(stderr, "  [MCPX-TRAP] faults=%lu apu=%lu vp=%lu "
            "ack_windows=%lu reprotect_failures=%lu\n",
            g_mcpx_trap_faults, g_mcpx_trap_apu_writes,
            g_mcpx_trap_apu_vp_writes, g_mcpx_ack_windows,
            g_mcpx_reprotect_failures);
    fflush(stderr);
}

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
    if (g_nv2a_pgraph_guarded && page == g_nv2a_pgraph_page) {
        if (page_out) *page_out = page;
        return 1;
    }
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

/* Store to a guarded MCPX register as the hardware, not as the guest.
 *
 * The trap turns a guest write into the register's real semantics -- a 1 to a
 * change bit clears it, a 1 to bit 4 of a port resets it. The runtime asserting
 * a condition means the opposite: "this is now the value". Announcing a connect
 * by writing 0x00010001 through the guest path would read as ClearPortEnable
 * plus an acknowledge of the very change being announced.
 *
 * Drops the guard for the store, under the same lock the handler uses. */
static void mcpx_hw_store(uint32_t offset, uint32_t value)
{
    volatile uint32_t *p;
    uintptr_t page;
    DWORD old_prot;

    if (!g_mcpx_regs) return;
    p = (volatile uint32_t *)((char *)g_mcpx_regs + offset);
    if (!g_mcpx_trap_active) { *p = value; return; }

    page = (uintptr_t)p & ~(uintptr_t)(g_mcpx_page_size - 1);
    mcpx_lock();
    if (VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READWRITE, &old_prot)) {
        *p = value;
        VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READONLY, &old_prot);
    }
    mcpx_unlock();
}

/* The same, for a set of registers that belong to one hardware event.
 *
 * Every unprotect is a window in which a guest store to this page lands as
 * plain memory, with none of the register semantics the trap exists to supply.
 * The lock does not help: it serialises this file's own threads, and the guest
 * is neither of them.
 *
 * That is not theoretical. Measured in claude-usb-transfer-service-05: the
 * attach probe announced the connect and its interrupt in two separate
 * windows, XPP's ISR acknowledged ConnectStatusChange in the gap between them,
 * and its 0x00010000 stored whole -- wiping CurrentConnectStatus, the precise
 * failure the port-status model exists to prevent. The driver spent the rest
 * of the run resetting an empty port. Windows cannot be eliminated while the
 * mechanism is mprotect, so the rule is to open as few as possible: one per
 * event, not one per register. All offsets must share a page. */
static void mcpx_hw_store_n(const uint32_t *offset, const uint32_t *value,
                            unsigned n)
{
    uintptr_t page;
    DWORD old_prot;
    unsigned i;

    if (!g_mcpx_regs || n == 0) return;
    if (!g_mcpx_trap_active) {
        for (i = 0; i < n; i++)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        return;
    }

    page = ((uintptr_t)g_mcpx_regs + offset[0]) & ~(uintptr_t)(g_mcpx_page_size - 1);
    mcpx_lock();
    if (VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READWRITE, &old_prot)) {
        for (i = 0; i < n; i++)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READONLY, &old_prot);
    }
    mcpx_unlock();
}

/* Same, but the LAST register is OR-ed with bits rather than assigned, and the
 * read-modify-write happens INSIDE the lock and the writable window.
 *
 * Why this exists: the frame pump used to compute `*ist | bits` and pass the
 * result to mcpx_hw_store_n, which only then took the lock. A guest ISR
 * acknowledging a status bit between that read and the eventual store had its
 * acknowledge resurrected by the stale copy. ohci_service_commit twelve lines
 * below already documents exactly this hazard -- "storing the copy back whole
 * would resurrect an acknowledge that landed in between" -- and OR-s under the
 * lock for that reason; the frame path did not.
 *
 * This narrows the race to the window itself. It does NOT close the separate
 * hazard that the window exists at all: a guest store landing inside it still
 * completes against plain memory with none of the register semantics. That is
 * the CLAUDE.md rule about guarded pages and it needs the write emulated in
 * the handler, not a lock. */
static void mcpx_hw_store_n_or_last(const uint32_t *offset, const uint32_t *value,
                                    unsigned n, uint32_t or_bits)
{
    uintptr_t page;
    DWORD old_prot;
    unsigned i;

    if (!g_mcpx_regs || n == 0) return;
    if (!g_mcpx_trap_active) {
        for (i = 0; i + 1 < n; i++)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        *(volatile uint32_t *)((char *)g_mcpx_regs + offset[n - 1]) |= or_bits;
        return;
    }

    page = ((uintptr_t)g_mcpx_regs + offset[0]) & ~(uintptr_t)(g_mcpx_page_size - 1);
    mcpx_lock();
    if (VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READWRITE, &old_prot)) {
        for (i = 0; i + 1 < n; i++)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        /* Read and OR here, not in the caller: an acknowledge that lands
         * before this line is preserved, and one that lands after it is
         * outside any window this function controls. */
        *(volatile uint32_t *)((char *)g_mcpx_regs + offset[n - 1]) |= or_bits;
        VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READONLY, &old_prot);
    }
    mcpx_unlock();
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
    g_mcpx_trap_faults++;

    guest_va = (uint32_t)(fault - g_memory_offset);
    /* Main APU registers and the voice processor's PIO window must never be a
     * shadow of the last guest store: ISTS is W1C and the voice processor
     * changes trap state asynchronously. Decode ordinary scalar loads and
     * stores here, without ever opening a writable window.
     *
     * The VP window is in this branch, and not in the write trap below, for a
     * measured reason. The write trap has to unprotect the page to complete
     * the store, and so did the ack thread, which advanced a free-running
     * counter at 0x020010 -- on the same page as the voice-submission
     * registers at 0x020120-0x020304, roughly 300,000 times a second. Any
     * guest store landing in one of those windows completed as ordinary
     * memory and the model never saw it. JSRF's DirectSound submission loop
     * lost every one of its NV1BA0_PIO_VOICE_ON writes that way: over 45 s on
     * the title screen, 215 VP writes reached the model and not one of them
     * was VOICE_ON, so no voice ever started and the title was silent.
     * Emulating the access instead of replaying it needs no window at all. */
    if (g_mcpx_apu_read && guest_va >= XBOX_MCPX_BASE &&
            guest_va < XBOX_MCPX_BASE + MCPX_APU_MODEL_SIZE) {
        unsigned opc = (insn >> 22) & 3u;
        unsigned size = insn >> 30;
        int writeback = !(insn & (1u << 24)) && !(insn & (1u << 21)) &&
                         ((insn >> 10) & 1u);
        if ((insn & 0x3E000000u) != 0x38000000u || writeback || size > 2 || opc > 1)
            goto chain;
        width = 1u << size;
        rt = insn & 31u;
        if (opc == 1) {
            value = g_mcpx_apu_read(guest_va - XBOX_MCPX_BASE, width);
            if (width < 4) value &= (1u << (8 * width)) - 1u;
            if (rt < 29) uc->uc_mcontext->__ss.__x[rt] = value;
            else if (rt == 29) uc->uc_mcontext->__ss.__fp = value;
            else if (rt == 30) uc->uc_mcontext->__ss.__lr = value;
        } else {
            uint32_t off = guest_va - XBOX_MCPX_BASE;
            value = rt < 29 ? uc->uc_mcontext->__ss.__x[rt] :
                    rt == 29 ? uc->uc_mcontext->__ss.__fp :
                    rt == 30 ? uc->uc_mcontext->__ss.__lr : 0;
            if (width < 4) value &= (1u << (8 * width)) - 1u;
            if (g_mcpx_apu_write) {
                extern unsigned long long g_apu_trap_host_pc;
                g_mcpx_trap_apu_writes++;
                if (off >= 0x20000u && off < 0x30000u)
                    g_mcpx_trap_apu_vp_writes++;
                g_apu_trap_host_pc = uc->uc_mcontext->__ss.__pc;
                g_mcpx_apu_write(off, (uint32_t)value, width);
            }
        }
        uc->uc_mcontext->__ss.__pc += 4;
        return;
    }

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
            extern unsigned long long g_apu_trap_host_pc;
            g_mcpx_trap_apu_writes++;
            if (off >= 0x20000u && off < 0x30000u)
                g_mcpx_trap_apu_vp_writes++;
            g_apu_trap_host_pc = uc->uc_mcontext->__ss.__pc;
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
    uint32_t ohci_disable = 0;
    int ohci_raise_rhsc = 0;
    xbox_ohci_service ohci_svc;
    int ohci_serviced = 0;
    /* Every guest write to the OHCI operational registers, now that the page is
     * guarded for the interrupt pair. RECOMP_OHCI_TRACE only.
     *
     * This is the whole conversation the driver is trying to have: which
     * registers it programs, in what order, when it resets a port and where it
     * puts its endpoint lists. Guessing at that from the OHCI specification is
     * how a device model ends up answering questions the title never asks. */
    if (guest_va >= XBOX_MCPX_BASE + 0x500000u
            && guest_va < XBOX_MCPX_BASE + 0x500060u) {
        static int on = -1;
        static unsigned long n;
        if (on < 0) on = getenv("RECOMP_OHCI_TRACE") != NULL;
        if (on && ++n <= 400) {
            static const char *nm[] = {
                "HcRevision","HcControl","HcCommandStatus","HcInterruptStatus",
                "HcInterruptEnable","HcInterruptDisable","HcHCCA",
                "HcPeriodCurrentED","HcControlHeadED","HcControlCurrentED",
                "HcBulkHeadED","HcBulkCurrentED","HcDoneHead","HcFmInterval",
                "HcFmRemaining","HcFmNumber","HcPeriodicStart","HcLSThreshold",
                "HcRhDescriptorA","HcRhDescriptorB","HcRhStatus",
                "HcRhPortStatus0","HcRhPortStatus1",
            };
            uint32_t off = guest_va - (XBOX_MCPX_BASE + 0x500000u);
            fprintf(stderr, "  [OHCI-W] #%lu +0x%02X %-18s <= 0x%08X (w%u)\n",
                    n, off, (off / 4) < 23 ? nm[off / 4] : "?",
                    (uint32_t)value, width);
            fflush(stderr);
        }
    }
    if (guest_va == XBOX_MCPX_BASE + 0x500020u && width == 4 && value)
        ohci_trace_control_ed((uint32_t)value);
    if (guest_va == XBOX_MCPX_BASE + 0x500008u && width == 4
            && (value & 0x00000006u)) {
        /* ControlListFilled (bit 1) and BulkListFilled (bit 2) are the
         * doorbells: the title has finished building a list and wants the
         * controller to run it. By the time XPP asserts ControlListFilled it
         * has removed Skip and installed the TDs, whereas the earlier
         * head-register write can expose only its staging state -- which is
         * why servicing hangs off this write and not off that one.
         *
         * HcControlHeadED is six dwords after HcCommandStatus. */
        if (value & 0x00000002u) {
            uint32_t head = *(volatile uint32_t *)((uint8_t *)fault + 0x18u);
            if (head) {
                ohci_trace_control_ed(head);
                ohci_serviced = ohci_service(head, &ohci_svc) != 0;
            }
        }
        if (!ohci_serviced && (value & 0x00000004u)) {
            uint32_t head = *(volatile uint32_t *)((uint8_t *)fault + 0x20u);
            if (head)
                ohci_serviced = ohci_service(head, &ohci_svc) != 0;
        }
        /* Both bits are cleared by the controller once it finds the list
         * empty, and this service drains a list in a single pass. Leaving them
         * set would tell the driver its previous submission is still queued. */
        value &= ~0x00000006u;
    }
    if ((guest_va == XBOX_NV2A_PCRTC_INTR_0 || guest_va == XBOX_NV2A_PGRAPH_INTR) && width == 4) {
        value = *(volatile uint32_t *)fault & ~(uint32_t)value;
    } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_STATUS && width == 4) {
        /* HcInterruptStatus is WRITE-1-TO-CLEAR, and as plain memory it was
         * the opposite: the driver's acknowledge stored the bit it meant to
         * retire, so the condition stood for ever.
         *
         * Measured, in claude-usb-transfer-service-01: XPP's ISR ran the
         * mask/handle/acknowledge/unmask cycle roughly eighty times against a
         * RootHubStatusChange it could not put down, before the connect
         * finally got through to the port reset. WritebackDoneHead has the
         * same shape and matters more -- a done queue that can never be
         * acknowledged is one transfer, then silence -- because the periodic
         * tick refuses to publish a second queue over an unacknowledged
         * first, exactly as hardware does. */
        value = *(volatile uint32_t *)fault & ~(uint32_t)value;
    } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_ENABLE && width == 4) {
        /* Write-1-to-set. */
        value = *(volatile uint32_t *)fault | (uint32_t)value;
    } else if ((guest_va == XBOX_MCPX_BASE + MCPX_OHCI_PORT0
                || guest_va == XBOX_MCPX_BASE + MCPX_OHCI_PORT1) && width == 4) {
        uint32_t before = *(volatile uint32_t *)fault;
        value = xbox_OhciPortWrite(before, (uint32_t)value);
        /* A change bit going up is a root-hub status change. */
        if ((value & 0x001F0000u) & ~(before & 0x001F0000u))
            ohci_raise_rhsc = 1;
        /* A port reset returns the device on it to the default address. The
         * title resets before it enumerates, so without this a second pass
         * would probe address 0 and find a device that only answers at the
         * address the first pass gave it. */
        if ((uint32_t)value & 0x00100000u)
            xbox_UsbDeviceReset();
    } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_DISABLE && width == 4) {
        /* Write-1-to-clear against Enable; both read back the enable mask. */
        ohci_disable = (uint32_t)value;
        value = 0;
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
    if (ohci_raise_rhsc) {
        volatile uint32_t *ist = (volatile uint32_t *)
            (uintptr_t)(XBOX_MCPX_BASE + 0x50000Cu + g_memory_offset);
        *ist |= 0x00000040u;
    }
    if (ohci_serviced) {
        /* The done queue and its interrupt, now that the page is writable.
         * The guest ISR is delivered from bridge_device_irq_poll on its own
         * thread and will find WritebackDoneHead the next time it runs. */
        ohci_service_commit(&ohci_svc);
    }
    if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_DISABLE) {
        /* Enable sits four bytes below, on this same now-writable page. */
        volatile uint32_t *en = (volatile uint32_t *)(fault - 4);
        *en &= ~ohci_disable;
        *(volatile uint32_t *)fault = *en;
    } else if (guest_va == XBOX_NV2A_PGRAPH_INTR) {
        if (value == 0)
            __atomic_fetch_and((uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0 + g_memory_offset),
                               ~XBOX_NV2A_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
    } else if (guest_va == XBOX_NV2A_PCRTC_INTR_0) {
        /* The summary follows its source. Different page, not guarded, so this
         * is an ordinary store. */
        if ((value & 0x1u) == 0) {
            __atomic_fetch_and((uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0 + g_memory_offset),
                               ~XBOX_NV2A_PMC_INTR_PCRTC, __ATOMIC_SEQ_CST);
        }
    } else {
        xbox_McpxApplyReady();   /* the ack thread cannot reach a guarded page */
    }
    if (!VirtualProtect((LPVOID)page, g_mcpx_page_size, PAGE_READONLY, &old_prot))
        g_mcpx_reprotect_failures++;
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
    {
        static const uint32_t extra[] = {
            MCPX_OHCI_INTR_ENABLE, MCPX_OHCI_INTR_DISABLE,
        };
        for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
            uintptr_t addr = (uintptr_t)g_mcpx_regs + extra[i];
            uintptr_t page = addr & ~(uintptr_t)(g_mcpx_page_size - 1);
            int seen = 0;
            for (size_t j = 0; j < g_mcpx_guard_pages; j++)
                if (g_mcpx_guard_page[j] == page) { seen = 1; break; }
            if (!seen && g_mcpx_guard_pages < 8)
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

    /* Software methods require the same source acknowledgement semantics as
     * vblank. The payload and FIFO register share this PGRAPH page. */
    {
        DWORD old_prot;
        g_nv2a_pgraph_page = ((uintptr_t)g_memory_offset + XBOX_NV2A_PGRAPH_INTR)
                             & ~(uintptr_t)(g_mcpx_page_size - 1);
        g_nv2a_pgraph_guarded = VirtualProtect((LPVOID)g_nv2a_pgraph_page,
                g_mcpx_page_size, PAGE_READONLY, &old_prot) != 0;
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
            if (g_mcpx_apu_read) {
                if (VirtualProtect((LPVOID)apu, MCPX_APU_MODEL_SIZE,
                                   PAGE_NOACCESS, &old_prot))
                    g_mcpx_apu_read_trapped = 1;
                else
                    fprintf(stderr,
                            "  APU: failed to trap model register reads\n");
            }
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
        __atomic_fetch_or((uint32_t *)pmc, XBOX_NV2A_PMC_INTR_PCRTC, __ATOMIC_SEQ_CST);
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
        __atomic_fetch_or((uint32_t *)pmc, XBOX_NV2A_PMC_INTR_PCRTC, __ATOMIC_SEQ_CST);
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

BOOL xbox_Nv2aRaiseSoftwareMethod(uint32_t subchannel, uint32_t parameter)
{
    volatile uint32_t *regs = (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + g_memory_offset);
    DWORD old_prot;
    if (!g_nv2a_pgraph_guarded || !parameter || xbox_Nv2aSoftwareMethodPending()) return FALSE;
    mcpx_lock();
    if (!VirtualProtect((LPVOID)g_nv2a_pgraph_page, g_mcpx_page_size,
                        PAGE_READWRITE, &old_prot)) {
        mcpx_unlock();
        return FALSE;
    }
    regs[0x400704/4] = ((subchannel & 7u) << 16) | 0x100u;
    regs[0x400708/4] = parameter;
    regs[0x400108/4] = 1; /* NSOURCE_NOTIFICATION */
    regs[0x400720/4] = 0; /* suspend until the guest restores FIFO access */
    regs[0x400100/4] |= XBOX_NV2A_PGRAPH_ERROR;
    VirtualProtect((LPVOID)g_nv2a_pgraph_page, g_mcpx_page_size,
                   PAGE_READONLY, &old_prot);
    __atomic_fetch_or((uint32_t *)&regs[0x100/4], XBOX_NV2A_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
    mcpx_unlock();
    return TRUE;
}

int xbox_Nv2aSoftwareMethodPending(void)
{
    if (!g_nv2a_pgraph_guarded) return 0;
    volatile uint32_t *regs = (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + g_memory_offset);
    return (regs[0x400100/4] & XBOX_NV2A_PGRAPH_ERROR) != 0
        || (regs[0x400708/4] != 0 && !(regs[0x400720/4] & 1));
}

#else  /* Windows, or a host this decoder does not cover */

/* Declared inside the AArch64 trap block above, but read further down in code
 * common to both hosts. There is no read trap on this host, so 0 is correct. */
static int g_mcpx_apu_read_trapped = 0;

/* Guarded device pages on this host. Declared here because xbox_McpxTrapReport
 * below reports which of them took, and a page that failed to guard is
 * otherwise silent -- it reads downstream as the guest never writing that
 * register, which is a conclusion this tree has drawn wrongly before. */
static uintptr_t g_nv2a_pcrtc_page;
static int       g_nv2a_pcrtc_guarded;
static uintptr_t g_ac97_page;         /* AC97 bus-master boxes, write-clear */
static int       g_ac97_guarded;
static uintptr_t g_ohci_page;         /* USB0 operational registers */
static int       g_ohci_guarded;
static uintptr_t g_nv2a_pgraph_page;  /* PGRAPH_INTR, and the notify trap */
static int       g_nv2a_pgraph_guarded;
/* RECOMP_STORE_WATCH=<va>:<len> -- a guarded page over ordinary guest RAM.
 *
 * RECOMP_MEM_WATCH sees only stores routed through the RECOMP_MEM_WRITE macros,
 * the block-write probe sees only lifted string operations, and
 * RECOMP_KERNEL_WATCH only samples around bridge calls. A value that changes
 * while all three are silent -- which is where 0x0025EFB8 left us -- is being
 * written by something none of them covers, and the only instrument that sees
 * EVERY writer regardless of which thread or which layer it lives in is the
 * hardware one. Guard the page; the VEH below already decodes and completes
 * the store, so this reports the host PC and carries on. */
static uintptr_t g_store_watch_page;
static int       g_store_watch_guarded;
static uint32_t  g_store_watch_lo, g_store_watch_hi;
/* Its OWN lock, not the device one.
 *
 * Sharing g_nv2a_pcrtc_lock put ordinary guest .data stores behind the same
 * interlock the pusher thread takes to raise a PGRAPH notify and the ack thread
 * spins against. Two runs stalled before AvSetDisplayMode with it, which reads
 * as "the guard prevents the corruption" and is really "the guard wedged the
 * title" -- the same shape as the APU lock freezing the guest. A watch page
 * over plain RAM shares nothing with the device pages and needs no common
 * lock. */
static volatile LONG g_store_watch_lock;
static volatile LONG g_nv2a_pcrtc_lock;
static size_t    g_nv2a_page_size;    /* g_mcpx_page_size is AArch64-only */

/* What the VEH trap actually costs, per page.
 *
 * This used to print the guard flags and the words "no fault counters on this
 * host", which was honest and useless: the POSIX branch counts its sigaction
 * faults, so the one number that would let the two hosts be compared -- how
 * often the guest faults into a guarded page -- existed on exactly one of
 * them. A trapped store is the most expensive thing either host does, and
 * "the Windows oracle runs the guest's main loop at a twelfth of macOS's
 * rate" cannot be attributed without it.
 *
 * Counted per page, plus the two ways the handler declines: a fault on a page
 * it does not guard (which it must pass on) and a store whose opcode the
 * decoder does not recognise (which becomes a crash). The second is the one
 * that reads as a mystery fault if it is not counted. */
static volatile LONG g_veh_faults, g_veh_pcrtc, g_veh_ac97, g_veh_ohci;
static volatile LONG g_veh_pgraph;
static volatile LONG g_veh_watch, g_veh_not_ours, g_veh_undecoded;

void xbox_McpxTrapReport(void)
{
    fprintf(stderr, "  [MCPX-TRAP] VEH guards: PCRTC=%d AC97=%d OHCI=%d"
                    " PGRAPH=%d faults=%ld pcrtc=%ld ac97=%ld ohci=%ld"
                    " pgraph=%ld watch=%ld"
                    " declined: not-ours=%ld undecoded=%ld\n",
            g_nv2a_pcrtc_guarded, g_ac97_guarded, g_ohci_guarded,
            g_nv2a_pgraph_guarded, g_veh_faults, g_veh_pcrtc, g_veh_ac97,
            g_veh_ohci, g_veh_pgraph,
            g_veh_watch, g_veh_not_ours, g_veh_undecoded);
    fflush(stderr);
}

/* Defined below xbox_Nv2aHandleWin32Fault, which owns the lock and the page
 * state they need. */
BOOL xbox_Nv2aRaiseSoftwareMethod(uint32_t subchannel, uint32_t parameter);
int  xbox_Nv2aSoftwareMethodPending(void);


/* Assert one or more OHCI register values as hardware.  The Windows guest
 * write path guards this page, so runtime writes have to open it under the
 * same lock as the VEH and close it again as one transaction. */
static void mcpx_hw_store(uint32_t offset, uint32_t value)
{
    mcpx_hw_store_n(&offset, &value, 1);
}

static void mcpx_hw_store_n(const uint32_t *offset, const uint32_t *value,
                            unsigned n)
{
    DWORD old_prot;

    if (!g_mcpx_regs || n == 0) return;
    if (!g_ohci_guarded) {
        for (unsigned i = 0; i < n; ++i)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        return;
    }

    while (InterlockedCompareExchange(&g_nv2a_pcrtc_lock, 1, 0) != 0)
        SwitchToThread();
    if (VirtualProtect((LPVOID)g_ohci_page, g_nv2a_page_size,
                       PAGE_READWRITE, &old_prot)) {
        for (unsigned i = 0; i < n; ++i)
            *(volatile uint32_t *)((char *)g_mcpx_regs + offset[i]) = value[i];
        if (!VirtualProtect((LPVOID)g_ohci_page, g_nv2a_page_size,
                            PAGE_READONLY, &old_prot))
            g_ohci_guarded = 0;
    }
    InterlockedExchange(&g_nv2a_pcrtc_lock, 0);
}

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


/* PCRTC interrupt status: write-1-to-clear, on this host too.
 *
 * NV_PCRTC_INTR_0 is write-1-to-clear in hardware, and the vblank handshake
 * depends on it: bridge_vblank_poll will not raise another vblank while the
 * last is still pending, and the guest ISR clears pending by writing a 1.
 * Against plain RAM that write SETS the bit, so the first vblank stays pending
 * forever and every later one is skipped -- measured on this host, 1859 of
 * 1865 deadlines skipped with nothing ever delivered.
 *
 * The AArch64 branch gets this from its write trap. Here the page is guarded
 * read-only and the VEH does the same work: read the current value, clear the
 * bits the guest wrote, and follow the PMC summary down when the source goes
 * quiet.
 *
 * The unprotect/store/reprotect window is the hazard this tree has already
 * paid for once, so it is held under an interlock and nothing else here
 * touches this page.
 */

static uint64_t *nv2a_ctx_reg(PCONTEXT c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax; case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx; case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp; case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi; case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;  case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10; case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12; case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14; case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static int nv2a_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;
    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;
    if (mod == 0 && (rm & 7) == 5) len += 4;
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

int xbox_Nv2aHandleWin32Fault(PCONTEXT ctx, uintptr_t fault, uint32_t guest_va)
{
    const uint8_t *ip;
    int len = 0, rex_r = 0, rex_b = 0, ilen;
    uint8_t op;
    uint32_t written, before, after;
    unsigned width = 4;
    int opsize16 = 0;
    int rmw = 0;
    int ohci_raise_rhsc = 0;
    int ohci_serviced = 0;
    uint32_t ohci_disable = 0;
    xbox_ohci_service ohci_svc;
    uintptr_t page = ((uintptr_t)fault) & ~(uintptr_t)(g_nv2a_page_size - 1);
    DWORD old_prot;

    /* A guard covers a whole page, so EVERY store on it now faults -- not just
     * the registers with special semantics. Anything else on the page has to
     * be completed as an ordinary store, or guarding one register turns its
     * neighbours into fatal faults. (Measured the hard way: the title writes
     * 0xFD600140 on the PCRTC page and the process died there.) */
    InterlockedIncrement(&g_veh_faults);
    if (!(g_nv2a_pcrtc_guarded  && page == g_nv2a_pcrtc_page) &&
        !(g_ac97_guarded        && page == g_ac97_page) &&
        !(g_ohci_guarded        && page == g_ohci_page) &&
        !(g_nv2a_pgraph_guarded && page == g_nv2a_pgraph_page) &&
        !(g_store_watch_guarded && page == g_store_watch_page)) {
        InterlockedIncrement(&g_veh_not_ours);
        return 0;
    }
    if (g_nv2a_pcrtc_guarded  && page == g_nv2a_pcrtc_page)
        InterlockedIncrement(&g_veh_pcrtc);
    else if (g_ac97_guarded   && page == g_ac97_page)
        InterlockedIncrement(&g_veh_ac97);
    else if (g_ohci_guarded   && page == g_ohci_page)
        InterlockedIncrement(&g_veh_ohci);
    else if (g_nv2a_pgraph_guarded && page == g_nv2a_pgraph_page)
        InterlockedIncrement(&g_veh_pgraph);
    else
        InterlockedIncrement(&g_veh_watch);

    ip = (const uint8_t *)(uintptr_t)ctx->Rip;
    while (ip[len] == 0x66 || ip[len] == 0x67 || ip[len] == 0xF2 || ip[len] == 0xF3) {
        if (ip[len] == 0x66) opsize16 = 1;
        len++;
    }
    if ((ip[len] & 0xF0) == 0x40) { rex_r = (ip[len] >> 2) & 1; rex_b = ip[len] & 1; len++; }
    op = ip[len];

    if (op == 0x89) {
        int reg = ((ip[len + 1] >> 3) & 7) | (rex_r ? 8 : 0);
        written = (uint32_t)*nv2a_ctx_reg(ctx, reg);
        ilen = len + 1 + nv2a_modrm_len(&ip[len + 1], rex_b);
    } else if (op == 0xC7) {
        int ml = nv2a_modrm_len(&ip[len + 1], rex_b);
        const uint8_t *imm = &ip[len + 1 + ml];
        written = (uint32_t)(imm[0] | (imm[1] << 8) | (imm[2] << 16)
                             | ((uint32_t)imm[3] << 24));
        ilen = len + 1 + ml + 4;
    } else if (op == 0x88) {                       /* mov [rm], r8  */
        int reg = ((ip[len + 1] >> 3) & 7) | (rex_r ? 8 : 0);
        written = (uint32_t)(*nv2a_ctx_reg(ctx, reg) & 0xFFu);
        width = 1;
        ilen = len + 1 + nv2a_modrm_len(&ip[len + 1], rex_b);
    } else if (op == 0xC6) {                       /* mov [rm], imm8 */
        int ml = nv2a_modrm_len(&ip[len + 1], rex_b);
        written = ip[len + 1 + ml];
        width = 1;
        ilen = len + 1 + ml + 1;
    } else if (op == 0x81 || op == 0x83) {
        /* Read-modify-write against a guarded page: `or [mem], imm` and
         * `and [mem], imm`.
         *
         * This handler completed plain stores only, which made the guard
         * depend on how the compiler spelled `|=`. GCC emits load / or /
         * store, so the store decoded as 0x89 and everything worked; clang
         * emits `orl $imm, mem` and the page died on the first access. That
         * cost a whole Windows build -- xbox_memory_layout.c itself does
         *
         *     *(volatile uint32_t *)(mcpx + MCPX_AC97_CODEC_STATUS)
         *         |= MCPX_AC97_CODEC_READY;
         *
         * two thousand lines after it guards that page, so the process died
         * in its own init before a single guest instruction ran.
         *
         * Folding the current value in here and handing the RESULT to the
         * paths below makes clang produce exactly the inputs GCC's
         * load/or/store already produced -- the write-clear mask, the
         * write-1-to-clear registers and the summary follow-down all see the
         * same value they saw before. That equivalence is the reason to do it
         * at this point rather than anywhere later.
         *
         * The read is safe: these pages are guarded PAGE_READONLY -- the code
         * below already reads `before` from the faulting address before it
         * unprotects anything. */
        int ml = nv2a_modrm_len(&ip[len + 1], rex_b);
        unsigned ext = (unsigned)((ip[len + 1] >> 3) & 7);
        const uint8_t *imm = &ip[len + 1 + ml];
        uint32_t operand, current;

        if (opsize16) {          /* 16-bit forms are not worth guessing at */
            InterlockedIncrement(&g_veh_undecoded);
            return 0;
        }
        if (op == 0x83) {
            operand = (uint32_t)(int32_t)(int8_t)imm[0];
            ilen = len + 1 + ml + 1;
        } else {
            operand = (uint32_t)(imm[0] | (imm[1] << 8) | (imm[2] << 16)
                                 | ((uint32_t)imm[3] << 24));
            ilen = len + 1 + ml + 4;
        }
        current = *(volatile uint32_t *)fault;
        if (ext == 1)        written = current | operand;   /* OR  /1 */
        else if (ext == 4)   written = current & operand;   /* AND /4 */
        else {
            InterlockedIncrement(&g_veh_undecoded);
            return 0;
        }
        rmw = 1;
    } else {
        InterlockedIncrement(&g_veh_undecoded);
        return 0;
    }

    if (g_ohci_guarded && page == g_ohci_page) {
        static int trace = -1;
        static unsigned long traces;
        if (trace < 0) trace = getenv("RECOMP_OHCI_TRACE") != NULL;
        if (trace && ++traces <= 400) {
            uint32_t off = guest_va - (XBOX_MCPX_BASE + 0x500000u);
            fprintf(stderr, "  [OHCI-W] #%lu +0x%02X <= 0x%08X (w%u)\n",
                    traces, off, written, width);
            fflush(stderr);
        }
        if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_CONTROL_HEAD
                && width == 4 && written)
            ohci_trace_control_ed(written);
    }

    /* AC97 bus-master reset is WRITE-CLEAR: the bit must never stick.
     * sub_001A6F52 writes RR, reads the register back ONCE outside its loop,
     * then spins on that stale value -- so suppressing the bit at the store is
     * the only place it can be done. Same table and same rule the AArch64
     * branch applies in mcpx_apply_write_clear; without it this host hangs in
     * DirectSound init forever. */
    if (!rmw) {
        uint32_t off = guest_va - XBOX_MCPX_BASE;
        size_t k;
        for (k = 0; k < sizeof(MCPX_WRITE_CLEAR)/sizeof(MCPX_WRITE_CLEAR[0]); k++)
            if (MCPX_WRITE_CLEAR[k].offset == off)
                written &= ~(uint32_t)MCPX_WRITE_CLEAR[k].write_clear;
    }

    /* Plain guest RAM: its own lock, no device semantics, and nothing else on
     * this page to interpret. Report inside the watched range, naming the host
     * PC so addr2line can say who it was -- generated code, a bridge, or a
     * device model on its own thread -- then complete the store and return. */
    if (g_store_watch_guarded && page == g_store_watch_page) {
        DWORD wp;
        if (guest_va + width > g_store_watch_lo && guest_va < g_store_watch_hi) {
            fprintf(stderr,
                    "  [STORE-WATCH] va=0x%08X width=%u value=0x%08X"
                    " old=0x%08X host_pc=0x%016llX\n",
                    guest_va, width, written,
                    (width == 1) ? (uint32_t)*(volatile uint8_t *)fault
                                 : *(volatile uint32_t *)fault,
                    (unsigned long long)ctx->Rip);
            fflush(stderr);
        }
        while (InterlockedCompareExchange(&g_store_watch_lock, 1, 0) != 0)
            SwitchToThread();
        if (VirtualProtect((LPVOID)page, g_nv2a_page_size,
                           PAGE_READWRITE, &wp)) {
            if (width == 1) *(volatile uint8_t  *)fault = (uint8_t)written;
            else            *(volatile uint32_t *)fault = written;
            if (!VirtualProtect((LPVOID)page, g_nv2a_page_size,
                                PAGE_READONLY, &wp))
                g_store_watch_guarded = 0;
        }
        InterlockedExchange(&g_store_watch_lock, 0);
        ctx->Rip += ilen;
        return 1;
    }

    while (InterlockedCompareExchange(&g_nv2a_pcrtc_lock, 1, 0) != 0)
        SwitchToThread();

    before = (width == 1) ? (uint32_t)*(volatile uint8_t *)fault
                          : *(volatile uint32_t *)fault;
    /* Write-1-to-clear for the interrupt status register; every other register
     * that happens to share this page is a plain store. */
    /* Both interrupt status registers are write-1-to-clear. PGRAPH_INTR is how
     * the guest acknowledges a software-method notify, so without this the
     * notify stands for ever and the second one is never raised. */
    /* A read-modify-write states the intended FINAL value, so the device
     * transforms above it must not be applied a second time.
     *
     * Learned the expensive way. Host code raises an interrupt with
     * `PGRAPH_INTR |= bit`; folding the current value in gives
     * written = before|bit, and the write-1-to-clear rule then computes
     * before & ~(before|bit) == 0 -- so the raise became a clear, the summary
     * never followed it up, and the pusher sat in [PB-NOTIFY] waiting for a
     * notify that had been erased on its way in. `pmc=00000000 intr=00100000`
     * is what that looks like. Write-1-to-clear is what a guest STORE of a
     * bit pattern means; it is not what `or` means. */
    after  = rmw ? written
                 : ((guest_va == XBOX_NV2A_PCRTC_INTR_0
                     || guest_va == XBOX_NV2A_PGRAPH_INTR) ? (before & ~written)
                                                           : written);

    /* The Xbox USB stack talks straight to OHCI registers.  On Windows these
     * used to be ordinary RAM: Enable/Disable lost their set/clear semantics,
     * status acknowledgements stuck, port commands replaced port state, and
     * ControlListFilled never ran the descriptor service.  The AArch64 trap
     * above already implements this state machine; mirror it here at the
     * decoded guest store boundary. */
    if (g_ohci_guarded && page == g_ohci_page && width == 4) {
        if (guest_va == XBOX_MCPX_BASE + 0x500008u
                && (written & 0x00000006u)) {
            if (written & 0x00000002u) {
                uint32_t head = *(volatile uint32_t *)
                    ((char *)g_mcpx_regs + MCPX_OHCI_CONTROL_HEAD);
                if (head) {
                    ohci_trace_control_ed(head);
                    ohci_serviced = ohci_service(head, &ohci_svc) != 0;
                }
            }
            if (!ohci_serviced && (written & 0x00000004u)) {
                uint32_t head = *(volatile uint32_t *)
                    ((char *)g_mcpx_regs + MCPX_OHCI_BULK_HEAD);
                if (head)
                    ohci_serviced = ohci_service(head, &ohci_svc) != 0;
            }
            after = written & ~0x00000006u;
        } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_STATUS) {
            after = before & ~written;                 /* write 1 to clear */
        } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_ENABLE) {
            after = before | written;                  /* write 1 to set */
        } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_INTR_DISABLE) {
            ohci_disable = written;                    /* clears Enable */
            after = 0;
        } else if (guest_va == XBOX_MCPX_BASE + MCPX_OHCI_PORT0
                || guest_va == XBOX_MCPX_BASE + MCPX_OHCI_PORT1) {
            after = xbox_OhciPortWrite(before, written);
            if ((after & 0x001F0000u) & ~(before & 0x001F0000u))
                ohci_raise_rhsc = 1;
            if (after & 0x00100000u)
                xbox_UsbDeviceReset();
        }
    }

    if (VirtualProtect((LPVOID)page, g_nv2a_page_size,
                       PAGE_READWRITE, &old_prot)) {
        if (width == 1) *(volatile uint8_t  *)fault = (uint8_t)after;
        else            *(volatile uint32_t *)fault = after;
        if (ohci_raise_rhsc) {
            *(volatile uint32_t *)((char *)g_mcpx_regs
                                   + MCPX_OHCI_INTR_STATUS) |= 0x00000040u;
        }
        if (ohci_serviced)
            ohci_service_commit(&ohci_svc);
        if (ohci_disable) {
            volatile uint32_t *enable = (volatile uint32_t *)
                ((char *)g_mcpx_regs + MCPX_OHCI_INTR_ENABLE);
            *enable &= ~ohci_disable;
            *(volatile uint32_t *)fault = *enable;
        }
        if (guest_va == XBOX_NV2A_PCRTC_INTR_0 && (after & 0x1u) == 0) {
            __atomic_fetch_and((uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0
                                                       + g_memory_offset),
                               ~XBOX_NV2A_PMC_INTR_PCRTC, __ATOMIC_SEQ_CST);
        }
        /* The summary follows its source down, exactly as for PCRTC above. */
        if (guest_va == XBOX_NV2A_PGRAPH_INTR && after == 0) {
            __atomic_fetch_and((uint32_t *)(uintptr_t)(XBOX_NV2A_PMC_INTR_0
                                                       + g_memory_offset),
                               ~XBOX_NV2A_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
        }
        if (!VirtualProtect((LPVOID)page, g_nv2a_page_size,
                            PAGE_READONLY, &old_prot)) {
            if      (page == g_nv2a_pcrtc_page)  g_nv2a_pcrtc_guarded = 0;
            else if (page == g_nv2a_pgraph_page) g_nv2a_pgraph_guarded = 0;
            else if (page == g_ohci_page)         g_ohci_guarded = 0;
            else if (page == g_store_watch_page) g_store_watch_guarded = 0;
            else                                 g_ac97_guarded = 0;
        }
    }

    InterlockedExchange(&g_nv2a_pcrtc_lock, 0);
    ctx->Rip += ilen;
    return 1;
}

/* Raise a PGRAPH software-method notify, as the AArch64 branch does.
 *
 * The pusher calls this when it decodes method 0x100 with a non-zero
 * parameter. It is not a flag: the guest's handler reads the trap registers to
 * find out WHAT was trapped, then restores FIFO access and acknowledges
 * PGRAPH_INTR, and the acknowledge is a guest write that only lands correctly
 * because the page above is guarded.
 *
 * Takes g_nv2a_pcrtc_lock -- the same lock xbox_Nv2aHandleWin32Fault holds --
 * so a raise from the pusher thread and a guest acknowledge faulting in cannot
 * overlap.
 *
 * The residual hazard is the one CLAUDE.md names, and it is not solved here:
 * while this holds the page writable, a guest store to any OTHER register on
 * it from another thread completes silently against RAM instead of faulting.
 * The window is a handful of stores wide and the AArch64 branch has exactly
 * the same shape, so this matches it rather than inventing a third contract --
 * but closing it properly needs a writable alias of the page, which needs the
 * NV2A aperture to be file-backed, which on this host it is not. If notifies
 * start going missing on a busy frame, look here first. */
BOOL xbox_Nv2aRaiseSoftwareMethod(uint32_t subchannel, uint32_t parameter)
{
    volatile uint32_t *regs;
    DWORD old_prot;
    BOOL ok = FALSE;

    if (!g_nv2a_pgraph_guarded || !parameter || !g_memory_offset) return FALSE;
    if (xbox_Nv2aSoftwareMethodPending()) return FALSE;

    regs = (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + g_memory_offset);

    while (InterlockedCompareExchange(&g_nv2a_pcrtc_lock, 1, 0) != 0)
        SwitchToThread();

    if (VirtualProtect((LPVOID)g_nv2a_pgraph_page, g_nv2a_page_size,
                       PAGE_READWRITE, &old_prot)) {
        regs[0x400704 / 4] = ((subchannel & 7u) << 16) | 0x100u;
        regs[0x400708 / 4] = parameter;
        regs[0x400108 / 4] = 1;   /* NSOURCE_NOTIFICATION */
        regs[0x400720 / 4] = 0;   /* suspend until the guest restores access */
        regs[0x400100 / 4] |= XBOX_NV2A_PGRAPH_ERROR;
        if (!VirtualProtect((LPVOID)g_nv2a_pgraph_page, g_nv2a_page_size,
                            PAGE_READONLY, &old_prot))
            g_nv2a_pgraph_guarded = 0;
        __atomic_fetch_or((uint32_t *)&regs[0x100 / 4],
                          XBOX_NV2A_PMC_INTR_PGRAPH, __ATOMIC_SEQ_CST);
        ok = TRUE;
    }

    InterlockedExchange(&g_nv2a_pcrtc_lock, 0);
    return ok;
}

/* Has the guest not finished with the last notify? Reads are permitted on the
 * guarded page, so this needs no lock and no window. Same two conditions the
 * AArch64 branch uses: the error bit still set, or trap data still standing
 * with FIFO access not yet restored. */
int xbox_Nv2aSoftwareMethodPending(void)
{
    volatile uint32_t *regs;

    if (!g_nv2a_pgraph_guarded || !g_memory_offset) return 0;
    regs = (volatile uint32_t *)(uintptr_t)(XBOX_NV2A_BASE + g_memory_offset);
    return (regs[0x400100 / 4] & XBOX_NV2A_PGRAPH_ERROR) != 0
        || (regs[0x400708 / 4] != 0 && !(regs[0x400720 / 4] & 1));
}

static void xbox_McpxTrapInstall(void)
{
    DWORD old_prot;

    /* The APU aperture is guarded by the harness (RECOMP_AC97_READY). What was
     * missing on this host is the NV2A side: without it the vblank
     * acknowledge has nowhere to land. */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_nv2a_page_size = (size_t)si.dwPageSize;
    }
    if (!g_memory_offset || !g_nv2a_page_size)
        return;
    g_nv2a_pcrtc_page = ((uintptr_t)g_memory_offset + XBOX_NV2A_PCRTC_INTR_0)
                        & ~(uintptr_t)(g_nv2a_page_size - 1);
    g_nv2a_pcrtc_guarded = VirtualProtect((LPVOID)g_nv2a_pcrtc_page,
                                          g_nv2a_page_size, PAGE_READONLY,
                                          &old_prot) != 0;
    g_ac97_page = ((uintptr_t)g_memory_offset + XBOX_MCPX_BASE + MCPX_AC97_NABM)
                  & ~(uintptr_t)(g_nv2a_page_size - 1);
    g_ac97_guarded = VirtualProtect((LPVOID)g_ac97_page, g_nv2a_page_size,
                                    PAGE_READONLY, &old_prot) != 0;
    /* USB0's operational registers occupy one page.  Guarding it gives the
     * Windows VEH the same set/clear, port-command and transfer-doorbell
     * boundary the AArch64 store trap already has. */
    g_ohci_page = ((uintptr_t)g_memory_offset + XBOX_MCPX_BASE + 0x500000u)
                  & ~(uintptr_t)(g_nv2a_page_size - 1);
    g_ohci_guarded = VirtualProtect((LPVOID)g_ohci_page, g_nv2a_page_size,
                                    PAGE_READONLY, &old_prot) != 0;
    /* PGRAPH_INTR and the software-method trap registers share a page. Guarding
     * it is what lets the guest's acknowledge be seen; without it a notify can
     * be raised but never retired, and the pusher blocks on the second one. */
    g_nv2a_pgraph_page = ((uintptr_t)g_memory_offset + XBOX_NV2A_PGRAPH_INTR)
                         & ~(uintptr_t)(g_nv2a_page_size - 1);
    g_nv2a_pgraph_guarded = VirtualProtect((LPVOID)g_nv2a_pgraph_page,
                                           g_nv2a_page_size, PAGE_READONLY,
                                           &old_prot) != 0;
    fprintf(stderr, "  NV2A: PCRTC_INTR_0 page %s for write-1-to-clear\n",
            g_nv2a_pcrtc_guarded ? "guarded" : "NOT guarded");
    fprintf(stderr, "  AC97: bus-master page %s for write-clear (RR)\n",
            g_ac97_guarded ? "guarded" : "NOT guarded");
    fprintf(stderr, "  OHCI: USB0 page %s for register semantics\n",
            g_ohci_guarded ? "guarded" : "NOT guarded");
    fprintf(stderr, "  NV2A: PGRAPH page %s for software-method notify\n",
            g_nv2a_pgraph_guarded ? "guarded" : "NOT guarded");
    {
        const char *spec = getenv("RECOMP_STORE_WATCH");
        if (spec) {
            char *end = NULL;
            uint32_t va = (uint32_t)strtoul(spec, &end, 0);
            uint32_t len = (end && *end == ':') ? (uint32_t)strtoul(end + 1, NULL, 0) : 4;
            if (!len) len = 4;
            g_store_watch_lo = va;
            g_store_watch_hi = va + len;
            g_store_watch_page = ((uintptr_t)g_memory_offset + va)
                                 & ~(uintptr_t)(g_nv2a_page_size - 1);
            g_store_watch_guarded = VirtualProtect((LPVOID)g_store_watch_page,
                                                   g_nv2a_page_size,
                                                   PAGE_READONLY, &old_prot) != 0;
            fprintf(stderr, "  STORE-WATCH: guest 0x%08X..0x%08X page %s\n",
                    g_store_watch_lo, g_store_watch_hi,
                    g_store_watch_guarded ? "guarded" : "NOT guarded");
        }
    }
    fflush(stderr);
}

#endif

/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}

/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    return (size_t)va + bytes <= g_memory_size;
}

void *xbox_GpuMemoryRange(uint32_t address, size_t bytes)
{
    if (bytes > UINT32_MAX || !fence_readable(address, (uint32_t)bytes)) return NULL;
    return (void *)((uintptr_t)g_memory_offset + address);
}

const uint8_t *xbox_Nv2aRegisterMemory(void)
{
    return (const uint8_t *)g_nv2a_memory;
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
/* NOTE: 16 ms is 62.5 Hz, not the 59.94 Hz of NTSC progressive -- the same
 * error BRIDGE_VBLANK_PERIOD_US in kernel_bridge.c was corrected for. Left
 * alone deliberately: xbox_Nv2aFrameCounter has no callers anywhere in the
 * tree, so this is dead for JSRF and changing it would be changing code nothing
 * runs. If it is ever wired up, fix it the way kernel_bridge.c does -- carry
 * the sub-millisecond remainder -- rather than copying this literal. */
#define XBOX_FRAME_PERIOD_MS    16      /* 62.5 Hz; see note above. Unused. */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;
/* Printing every DMA_PUT is a bring-up question; running the survey is not.
 * They shared s_nv2a_trace, and RECOMP_PB_EXEC arms that -- so a title could
 * not be rendered without also emitting one flushed stderr line per
 * pushbuffer submission. A 15-minute JSRF session wrote 24 GB, and the
 * fflush put a syscall on the submission path. Only RECOMP_NV2A_TRACE, asked
 * for explicitly, turns the printing on. */
static int s_nv2a_trace_print = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    s_fb_pitch = pitch;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    /* Report the surface being drawn, not whatever PCRTC_START happens to
     * hold. JSRF never programs a scanout address, so that register is not a
     * scanout in the usual sense.
     *
     * It was not junk either, and the earlier reading that it pointed at "a
     * heap block" was too strong. RECOMP_FLIP_TRACE measures the surface bound
     * at every FLIP_STALL as 0x0071E000 -- the address PCRTC_START held, and
     * the address the guard page saw xbox_HeapAlloc zero, which is what
     * allocating a flip chain from the heap looks like. What the old probe
     * actually lacked was a moment: it sampled once a second, and between
     * flips that page holds a frame nobody is composing into. */
    {
        extern uint32_t nv2a_pb_exec_surface_va(void);
        uint32_t drawn = nv2a_pb_exec_surface_va();
        if (drawn) s_fb_va = drawn;
    }
    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    /* Two numbers, because they answer different questions. The first is the
     * surface being composed right now, which is legitimately mid-clear as
     * often as not. The second is the copy taken at the guest's own FLIP_STALL
     * -- the only thing the window is ever fed -- and it is the one that means
     * "there is a picture on screen". Reading a blank first number as a blank
     * display is the mistake this line exists to prevent. */
    {
        extern int nv2a_pb_exec_snapshot_nonzero(void);
        extern double xbox_TraceSeconds(void);
        int presented = nv2a_pb_exec_snapshot_nonzero();
        fprintf(stderr, "  [FB] t=%7.2f 0x%08X sum=%08X nonzero=%u/%u %s |"
                " presented nonzero=%d\n",
                xbox_TraceSeconds(), s_fb_va, sum, nonzero, n,
                sum != last_sum ? "CHANGED" : "same", presented);
    }
    last_sum = sum;
    fflush(stderr);
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
            uint32_t mask = NV2A_ACK[i].busy_mask;
            /* Keyed on the guard, not the host. While PGRAPH is guarded the
             * software-method notify owns these two bits and this thread must
             * not touch them: acking 0x400100 or clearing PGRAPH out of the
             * PMC summary retires the notify before the guest's ISR has seen
             * it, and the pusher then blocks for ever on an acknowledge that
             * can never come. Measured on Windows the moment the raise started
             * working -- raised=1 with pmc=0 intr=0 on every waiting line. */
            if (g_nv2a_pgraph_guarded) {
                if (NV2A_ACK[i].offset == 0x400100) continue; /* W1C source is modeled */
                if (NV2A_ACK[i].offset == 0x100) mask &= ~XBOX_NV2A_PMC_INTR_PGRAPH;
            }
            if (*r & mask) {
#if !defined(_WIN32)
                if (NV2A_ACK[i].offset == 0x100)
                    __atomic_fetch_and((uint32_t *)r, ~mask, __ATOMIC_SEQ_CST);
                else
#endif
                    *r &= ~mask;
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
            /* Copying PUT into GET says "the GPU has consumed everything you
             * submitted" the instant it is submitted. For a title whose push
             * buffer nothing executes that is the honest acknowledgement, and
             * it is why this exists. For one whose buffer IS being executed it
             * is a lie with consequences: the producer believes the ring is
             * free, laps the parser, and overwrites the commands it is part way
             * through reading. When something owns GET it publishes the point
             * it has actually reached, and this must keep out of the way. */
            if (!g_nv2a_pusher_owns_dma_get && *get != *put) {
                *get = *put;
            }
        }
        xbox_McpxHoldRegisters();
        ohci_periodic_tick();
        fence_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        if (s_nv2a_trace) {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t, uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    if (last_put && put > last_put)
                        nv2a_pb_scan(XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                     XBOX_CONTIG_BASE | (put      & 0x0FFFFFFFu));
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    if (now_ms - last_report > 10000) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                last_put = put; last_put_ms = now_ms;
                if (s_nv2a_trace_print) {
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X\n", put);
                    fflush(stderr);
                }
            }
        }
        if (s_nv2a_trace_print) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        /* Skipped entirely once the model answers reads for this span: the
         * counter existed only because 0xFE820010 was plain memory, and
         * writing it here is what kept the page unprotected often enough to
         * swallow the guest's voice submissions. */
        if (g_mcpx_regs && !g_apu_mmio_trapped && !g_mcpx_apu_read_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
#if !defined(_WIN32) && defined(__aarch64__)
                if (g_mcpx_apu_guarded) {
                    uintptr_t pg = (uintptr_t)c
                                   & ~(uintptr_t)(g_mcpx_page_size - 1);
                    DWORD op;
                    mcpx_lock();
                    if (VirtualProtect((LPVOID)pg, g_mcpx_page_size,
                                       PAGE_READWRITE, &op)) {
                        g_mcpx_ack_windows++;
                        *c += 1;
                        if (!VirtualProtect((LPVOID)pg, g_mcpx_page_size,
                                            PAGE_READONLY, &op))
                            g_mcpx_reprotect_failures++;
                    }
                    mcpx_unlock();
                } else
#endif
                {
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

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_low_base = XBOX_LOW_BASE_DEFAULT;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;
/* The .text section proper. See where these are set for why the pair above is
 * not a substitute. */
uint32_t g_xbox_text_lo = 0;
uint32_t g_xbox_text_hi = 0;
/* The loaded image, every section. Nothing the GPU writes may land here: a
 * surface address that resolves into the title's own code and data is always a
 * bug, and silently honouring it corrupts the guest in ways that surface much
 * later as garbage pointers. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;
/* Size of the image's TLS block, as the loader built it. Zero until the XBE
 * is loaded, and zero for an image with no TLS directory. */
uint32_t g_image_tls_total;

RECOMP_TLS uint32_t g_fs_base = XBOX_PRIMARY_TIB_VA;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0, int kind)
{
    /* 32 filled during asset loading alone -- every slot went to
     * FileManager's readers -- so nothing at gameplay time was ever reachable.
     *
     * The key is the (callee VA, reach kind) pair, not the VA alone. A tail
     * jump reaching a shared epilogue restores its caller's caller's registers
     * by design, so 'T' rows are expected and 'C'/'I' rows are not; pooling
     * them produced a count that could not be read. Pairing also means a
     * target reached both ways is reported once for each, which is the more
     * useful answer than whichever way happened to come first. */
    enum { SLOTS = 512 };
    static uint32_t seen[SLOTS];
    static int seen_kind[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va && seen_kind[i] == kind)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        seen_kind[count] = kind;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI/%c] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                kind, va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    {
        const char *spec = getenv("RECOMP_PEEK");
        char buf[256], *q, *end;
        if (spec && *spec) {
            strncpy(buf, spec, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "  peek:");
            for (q = buf; *q; ) {
                unsigned long va = strtoul(q, &end, 0);
                if (end == q)
                    break;
                if (va >= XBOX_BASE_ADDRESS && va < XBOX_TOTAL_RAM)
                    fprintf(stderr, " [%08lX]=%08X", va,
                            *(const uint32_t *)(mem + va));
                q = (*end == ',') ? end + 1 : end;
            }
            fprintf(stderr, "\n");
        }
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;

RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

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
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

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

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                            " (null dereferences fault)\n");
    }

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

        uint32_t image_hi = 0;
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

            /* The section actually holding instructions, as distinct from the
             * executable RANGE below. This XBE marks .rdata and .data
             * executable too -- XBEs commonly do -- so g_xbox_code_lo..hi spans
             * most of the image and is useless for asking "did code change".
             * Anything that means .text must use these. */
            if (sec_name[0] == '.' && sec_name[1] == 't' && sec_name[2] == 'e'
                && sec_name[3] == 'x' && sec_name[4] == 't') {
                g_xbox_text_lo = sec_va;
                g_xbox_text_hi = sec_va + sec_vsize;
            }

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            /* Every section, not only the executable ones: the low block
             * has to clear .data and BSS too, and g_xbox_code_hi
             * deliberately excludes them. */
            if (sec_va + sec_vsize > image_hi)
                image_hi = sec_va + sec_vsize;
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);

        /* Tell the GPU model where the image is, so it can refuse to render
         * into it. See nv2a_range_hits_image. */
        {
            extern void nv2a_set_image_bounds(uint32_t, uint32_t);
            nv2a_set_image_bounds(g_xbox_image_lo, g_xbox_image_hi);
        }

        /* Put the fixed low block just above the image instead of at a base
         * chosen to clear any XBE. Everything below the heap is memory the
         * title cannot allocate, so the gap between the two was charged to the
         * arena: 4.5 MB of it for JSRF. 64 KB granularity because the block
         * holds page-aligned sub-regions at fixed offsets from this base. */
        if (image_hi > XBOX_BASE_ADDRESS) {
            uint32_t base = (image_hi + 0xFFFFu) & ~0xFFFFu;
            if (base < XBOX_LOW_BASE_DEFAULT)
                g_xbox_low_base = base;
            else
                g_xbox_low_base = base;   /* a larger image pushes it up */
            fprintf(stderr, "  Low block: 0x%08X (image ends 0x%08X);"
                            " %u KB returned to the heap\n",
                    g_xbox_low_base, image_hi,
                    (unsigned)((XBOX_LOW_BASE_DEFAULT > g_xbox_low_base)
                               ? (XBOX_LOW_BASE_DEFAULT - g_xbox_low_base) / 1024u
                               : 0u));
        }
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

        /* Primary TIB; guest page zero remains available for fault detection. */
        MEM32_INIT(XBOX_FS_BASE + 0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(XBOX_FS_BASE + 0x04, XBOX_STACK_TOP);   /* Stack base (high address) */
        MEM32_INIT(XBOX_FS_BASE + 0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(XBOX_FS_BASE + 0x18, XBOX_FS_BASE);     /* Self pointer */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. A zeroed stand-in makes that field
         * read as zero, causing the cache init to be skipped.
         */
        /* A zeroed block rather than a null pointer. The read is
         * [fs:[0x20] + 0x250], and this used to be left at 0 so that read
         * landed on guest address 0x250 and returned zero by accident -- which
         * only worked while page zero was mapped. Pointing at real zeroed
         * memory says the same thing to the title and survives that page being
         * unmapped, which is what makes a genuine null dereference visible. */
        #define FAKE_PRCB_VA (g_xbox_low_base + 0x61000u)  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        MEM32_INIT(XBOX_FS_BASE + 0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        MEM32_INIT(XBOX_FS_BASE + 0x28, XBOX_PRIMARY_TLS_CONTEXT_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(XBOX_PRIMARY_TLS_CONTEXT_VA + 0x28,
                   XBOX_PRIMARY_TLS_DATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place.
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  (g_xbox_low_base + 0x70000u)  /* image TLS data   */
        #define FAKE_TLS_THREAD_VA (g_xbox_low_base + 0x70200u)  /* slot 0 target    */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                /* Published so a thread created OUTSIDE the guest --
                 * ohci_thread, which runs the title's own USB ISR -- can build
                 * a TLS block the same size the loader built here. It was a
                 * local, so xbox_AllocThreadTib had nothing to size itself
                 * from and could not be written at all. */
                g_image_tls_total = total;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                MEM32_INIT(XBOX_FS_BASE + 0x04, FAKE_TLS_BLOCK_VA + total);

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at VA 0x%X, TLS at 0x%08X, RW data at 0x%08X\n",
                XBOX_FS_BASE, XBOX_PRIMARY_TLS_CONTEXT_VA,
                XBOX_PRIMARY_TLS_DATA_VA);

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
     * its own code. Separate storage preserves that pinned-pool layout.
     * POSIX titles using low-heap unpinned GPU allocations
     * must explicitly enable the shared heap view after layout initialisation:
     * D3D resource locks add 0x80000000 to offsets in that heap.
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
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        s_nv2a_trace_print = getenv("RECOMP_NV2A_TRACE") != NULL;
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
            /* Resolve the USB service's diagnostics here rather than on first
             * use: first use is inside the write trap's signal handler, where
             * getenv is not async-signal-safe. */
            xbox_UsbOhciInit();
            xbox_UsbDeviceReset();
            xbox_McpxTrapInstall();
#if defined(_WIN32)
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. Only the APU's own 512K is unmapped: AC'97 above it
                 * stays plain memory, which is what the codec-ready bit needs.
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, 0x00080000u,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped)
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + 0x00080000u);
                *(volatile uint32_t *)((char *)g_mcpx_memory
                                       + MCPX_AC97_CODEC_STATUS)
                    |= MCPX_AC97_CODEC_READY;
                fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                " (DirectSound will initialise)\n",
                        XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
            }
#endif
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

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
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
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        g_mirror_mask = 0;
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
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;
            uint64_t contig_lo = XBOX_CONTIG_BASE;
            uint64_t contig_hi = contig_lo + XBOX_CONTIG_SIZE;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            /* With a map larger than 64 MB, the generic mirror sequence can
             * reach 0x80000000. Mapping that view after the contiguous window
             * silently replaces pinned memory and the fake kernel page. The
             * architectural aperture wins over a generic wrap mirror, just as
             * the tiled aperture does above. */
            if (guest_lo < contig_hi && contig_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the contiguous"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_CONTIG_BASE);
                continue;
            }
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
                g_mirror_mask |= 1u << m;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        g_tiled_view = MapViewOfFileEx(
            g_mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            tiled_size,
            (LPVOID)tiled_native
        );
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_ram =
                    (volatile uint32_t *)((uintptr_t)0x1000 + g_memory_offset);
                uint32_t saved = *via_ram;

                *via_tiled = 0xA5C30F17u;
                if (*via_ram != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " RAM (wrote A5C30F17, read %08X) -- rendering"
                            " will read empty buffers\n", *via_ram);
                else
                    fprintf(stderr, "  Tiled aperture alias verified\n");
                *via_ram = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases RAM)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }
    }

    recomp_mem_watch_init(g_memory_size, g_mirror_mask, XBOX_TILED_BASE,
                          g_tiled_view ? xbox_TiledApertureSize() : 0);
    /* The ownership map arms a resident surface at every guest window that
     * names it, and this file is the only place that knows what those windows
     * are.  Registered here rather than at first use so a surface armed before
     * the first alias query cannot be armed at the low window alone. */
    recomp_gpu_own_set_aliases(recomp_mem_watch_ram_aliases);
#if defined(_WIN32)
    /* Here rather than in the harness, for two reasons. The heap bounds are
     * only final once the image is loaded and the stacks are sized, which is
     * this far into init and no earlier; and init is still single-threaded,
     * which is what makes rebuilding the window safe. POSIX keeps it opt-in
     * per title because fixed-address pinned pools may legitimately overlap
     * there -- on this host the alternative is an arena that aliases the
     * guest's own stacks and heap, so the default is the other way round and
     * RECOMP_PHYSICAL_HEAP_ALIAS=0 is the way out. */
    xbox_EnablePhysicalHeapAlias();
#endif
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

BOOL xbox_EnablePhysicalHeapAlias(void)
{
#if defined(_WIN32)
    /* Give this host the same one-pool contract POSIX has always had.
     *
     * Physical offset N and XBOX_CONTIG_BASE + N have to name the same bytes.
     * They did not: the window here is VirtualAlloc'd storage deliberately
     * aliasing nothing, so a contiguous block the guest filled through
     * 0x80XXXXXX was invisible to a GPU resolving the low offset -- measured
     * as 45 mismatching surface resolves against zero matching, with the
     * guest's texture bytes in the window and zeros where the GPU reads. The
     * separate arena that follows from it also climbs through the guest's own
     * stacks and heap, which is what overwrote a texture-cache slot and
     * crashed the render chain.
     *
     * The mechanism is already proven on this host: the tiled aperture maps a
     * view of the same section at another VA for exactly this reason, and says
     * so. The only complication is that the window is one VirtualAlloc
     * reservation, and a file view cannot be mapped inside one -- MEM_RELEASE
     * frees whole reservations only. So rebuild it as up to three: committed
     * storage below the heap, a view of RAM across the heap, committed storage
     * above it.
     *
     * The low slice is copied out and back rather than assumed empty. The
     * kernel's fake PE header lives at window offset 0x10000 and is written
     * during init, and "nothing else has written there yet" is the kind of
     * claim that is true until it is not.
     *
     * Runs while init is still single-threaded, which is what makes the gap
     * between releasing the reservation and remapping it safe. */
    uintptr_t expected = (uintptr_t)g_memory_offset + XBOX_CONTIG_BASE;
    uint32_t start = XBOX_HEAP_BASE, end = XBOX_HEAP_TOP;
    void *low_copy = NULL;
    void *low_region, *high_region = NULL, *target;
    const char *disabled = getenv("RECOMP_PHYSICAL_HEAP_ALIAS");

    if (g_physical_heap_view) return TRUE;
    if (disabled && !strcmp(disabled, "0")) {
        fprintf(stderr, "  Physical heap alias: disabled by"
                        " RECOMP_PHYSICAL_HEAP_ALIAS=0; the contiguous arena"
                        " will alias guest low memory\n");
        return FALSE;
    }
    if (!g_mapping_handle || !g_memory_base
            || (uintptr_t)g_contig_memory != expected
            || end <= start || end > XBOX_CONTIG_SIZE || end > g_memory_size
            || (start & 0xffffu) || (end & 0xffffu)) {
        fprintf(stderr, "  Physical heap alias: incompatible or uninitialised"
                        " layout (heap 0x%08X..0x%08X)\n", start, end);
        return FALSE;
    }

    low_copy = malloc(start);
    if (!low_copy) {
        fprintf(stderr, "  Physical heap alias: cannot preserve the low"
                        " %u bytes of the window\n", start);
        return FALSE;
    }
    memcpy(low_copy, (const void *)expected, start);

    if (!VirtualFree((LPVOID)expected, 0, MEM_RELEASE)) {
        fprintf(stderr, "  Physical heap alias: releasing the window failed"
                        " (error %lu)\n", (unsigned long)GetLastError());
        free(low_copy);
        return FALSE;
    }

    low_region = VirtualAlloc((LPVOID)expected, start,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (low_region != (void *)expected) {
        /* The window is gone and could not be retaken. Nothing downstream can
         * recover from that, so say which step lost it rather than fault
         * later somewhere unrelated. */
        fprintf(stderr, "  Physical heap alias: FATAL, could not re-reserve"
                        " 0x%08X..0x%08X (error %lu)\n",
                XBOX_CONTIG_BASE, XBOX_CONTIG_BASE + start,
                (unsigned long)GetLastError());
        free(low_copy);
        g_contig_memory = NULL;
        return FALSE;
    }
    memcpy((void *)expected, low_copy, start);
    free(low_copy);

    target = (void *)(expected + start);
    g_physical_heap_view = MapViewOfFileEx(g_mapping_handle,
                                           FILE_MAP_ALL_ACCESS, 0, start,
                                           (size_t)end - start, target);
    if (g_physical_heap_view != target) {
        if (g_physical_heap_view) UnmapViewOfFile(g_physical_heap_view);
        g_physical_heap_view = NULL;
        /* Put plain storage back so the window is whole and the run continues
         * on the old contract rather than faulting on the first pinned pool. */
        VirtualAlloc(target, (size_t)XBOX_CONTIG_SIZE - start,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        fprintf(stderr, "  Physical heap alias: mapping failed (error %lu);"
                        " window restored unaliased\n",
                (unsigned long)GetLastError());
        return FALSE;
    }
    if (end < XBOX_CONTIG_SIZE) {
        high_region = VirtualAlloc((LPVOID)(expected + end),
                                   (size_t)XBOX_CONTIG_SIZE - end,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!high_region)
            fprintf(stderr, "  WARNING: contiguous window above 0x%08X is"
                            " unmapped (error %lu); GPU instance memory will"
                            " fault\n",
                    XBOX_CONTIG_BASE + end, (unsigned long)GetLastError());
    }

    /* Prove the alias rather than assert it, the way the tiled aperture does.
     * If these are not the same bytes the GPU reads empty buffers and the
     * screen stays black with nothing anywhere to say why. */
    {
        volatile uint32_t *via_window =
            (volatile uint32_t *)(expected + start + 0x1000);
        volatile uint32_t *via_ram =
            (volatile uint32_t *)((uintptr_t)g_memory_offset + start + 0x1000);
        uint32_t saved = *via_ram;

        *via_window = 0xA5C30F17u;
        if (*via_ram != 0xA5C30F17u) {
            fprintf(stderr, "  WARNING: physical heap alias does NOT share"
                            " bytes (wrote A5C30F17, read %08X)\n", *via_ram);
        }
        *via_ram = saved;
    }

    recomp_mem_watch_add_ram_alias(XBOX_CONTIG_BASE + start, start,
                                   (size_t)end - start);
    fprintf(stderr, "  Physical heap alias: 0x%08X..0x%08X shares low RAM;"
                    " contiguous allocations now come from the heap\n",
            XBOX_CONTIG_BASE + start, XBOX_CONTIG_BASE + end);
    return TRUE;
#else
    if (g_physical_heap_view) return TRUE;
    /* MapViewOfFileEx uses MAP_FIXED on POSIX. Replace only pages inside the
     * contiguous window this layout already owns, never an unchecked address.
     * Expanded layouts and pinned-pool overlap need a full physical allocator;
     * this opt-in does not claim to solve those contracts. */
    uintptr_t expected=(uintptr_t)g_memory_offset+XBOX_CONTIG_BASE;
    uint32_t start=XBOX_HEAP_BASE, end=XBOX_HEAP_TOP;
    if (!g_mapping_handle || !g_memory_base || (uintptr_t)g_contig_memory!=expected
            || end<=start || end>XBOX_CONTIG_SIZE || end>g_memory_size
            || (start&0xffffu) || (end&0xffffu)) {
        fprintf(stderr,"  Physical heap alias: incompatible or uninitialised layout\n");
        return FALSE;
    }
    void *target=(void *)(expected+start);
    g_physical_heap_view=MapViewOfFileEx(g_mapping_handle,FILE_MAP_ALL_ACCESS,
            0,start,(size_t)end-start,target);
    if (g_physical_heap_view!=target) {
        if (g_physical_heap_view) UnmapViewOfFile(g_physical_heap_view);
        g_physical_heap_view=NULL;
        fprintf(stderr,"  Physical heap alias: mapping failed (error %lu)\n",(unsigned long)GetLastError());
        return FALSE;
    }
    recomp_mem_watch_add_ram_alias(XBOX_CONTIG_BASE + start, start,
                                   (size_t)end - start);
    fprintf(stderr,"  Physical heap alias: 0x%08X..0x%08X shares low RAM\n",
            XBOX_CONTIG_BASE+start,XBOX_CONTIG_BASE+end);
    return TRUE;
#endif
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
    recomp_mem_watch_shutdown();
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
    if (g_physical_heap_view) {
        UnmapViewOfFile(g_physical_heap_view);
        g_physical_heap_view=NULL;
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
    g_mirror_mask = 0;
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
    reserve_reset();
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Address-space allocator for pure reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * Releases are tracked too. Treating an above-RAM release as an ordinary heap
 * free made it a permanent leak and left the heap diagnostics claiming that a
 * valid pointer had never been allocated.
 */
#define XBOX_RESERVE_MAX_BLOCKS 128
static struct {
    uint32_t addr, size;
    BOOL free;
} g_reserve_blocks[XBOX_RESERVE_MAX_BLOCKS];
static int g_reserve_block_count;

static void reserve_reset(void)
{
    g_reserve_block_count = 0;
}

static void reserve_init(void)
{
    if (g_reserve_block_count || g_memory_size <= g_xbox_total_ram)
        return;
    g_reserve_blocks[0].addr = (uint32_t)g_xbox_total_ram;
    g_reserve_blocks[0].size = (uint32_t)(g_memory_size - g_xbox_total_ram);
    g_reserve_blocks[0].free = TRUE;
    g_reserve_block_count = 1;
}

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    reserve_init();

    for (int i = 0; i < g_reserve_block_count; ++i) {
        uint32_t start, lead, tail;
        if (!g_reserve_blocks[i].free)
            continue;
        start = (g_reserve_blocks[i].addr + align - 1) & ~(align - 1);
        if (start < g_reserve_blocks[i].addr)
            continue;
        lead = start - g_reserve_blocks[i].addr;
        if (lead > g_reserve_blocks[i].size
                || size > g_reserve_blocks[i].size - lead)
            continue;
        tail = g_reserve_blocks[i].size - lead - size;
        if (g_reserve_block_count + (lead != 0) + (tail != 0)
                > XBOX_RESERVE_MAX_BLOCKS)
            continue;

        if (lead) {
            memmove(&g_reserve_blocks[i + 1], &g_reserve_blocks[i],
                    (size_t)(g_reserve_block_count - i)
                        * sizeof g_reserve_blocks[0]);
            ++g_reserve_block_count;
            g_reserve_blocks[i].size = lead;
            ++i;
            g_reserve_blocks[i].addr = start;
            g_reserve_blocks[i].size -= lead;
        }
        if (tail) {
            memmove(&g_reserve_blocks[i + 2], &g_reserve_blocks[i + 1],
                    (size_t)(g_reserve_block_count - i - 1)
                        * sizeof g_reserve_blocks[0]);
            ++g_reserve_block_count;
            g_reserve_blocks[i + 1].addr = start + size;
            g_reserve_blocks[i + 1].size = tail;
            g_reserve_blocks[i + 1].free = TRUE;
        }
        g_reserve_blocks[i].addr = start;
        g_reserve_blocks[i].size = size;
        g_reserve_blocks[i].free = FALSE;
        memset((void *)((uintptr_t)start + g_memory_offset), 0, size);
        return start;
    }
    return 0;
}

BOOL xbox_ReserveFree(uint32_t address)
{
    reserve_init();
    for (int i = 0; i < g_reserve_block_count; ++i) {
        if (g_reserve_blocks[i].free || g_reserve_blocks[i].addr != address)
            continue;
        g_reserve_blocks[i].free = TRUE;
        while (i + 1 < g_reserve_block_count && g_reserve_blocks[i + 1].free) {
            g_reserve_blocks[i].size += g_reserve_blocks[i + 1].size;
            memmove(&g_reserve_blocks[i + 1], &g_reserve_blocks[i + 2],
                    (size_t)(g_reserve_block_count - i - 2)
                        * sizeof g_reserve_blocks[0]);
            --g_reserve_block_count;
        }
        if (i > 0 && g_reserve_blocks[i - 1].free) {
            g_reserve_blocks[i - 1].size += g_reserve_blocks[i].size;
            memmove(&g_reserve_blocks[i], &g_reserve_blocks[i + 1],
                    (size_t)(g_reserve_block_count - i - 1)
                        * sizeof g_reserve_blocks[0]);
            --g_reserve_block_count;
        }
        return TRUE;
    }
    return FALSE;
}

BOOL xbox_QueryReserveAddress(uint32_t address, uint32_t *base, uint32_t *size)
{
    reserve_init();
    for (int i = 0; i < g_reserve_block_count; ++i) {
        if (g_reserve_blocks[i].free
                || address < g_reserve_blocks[i].addr
                || (uint64_t)address >= (uint64_t)g_reserve_blocks[i].addr
                                         + g_reserve_blocks[i].size)
            continue;
        if (base) *base = g_reserve_blocks[i].addr;
        if (size) *size = g_reserve_blocks[i].size;
        return TRUE;
    }
    return FALSE;
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
/* Zero until first use: XBOX_HEAP_BASE follows g_xbox_low_base, which
 * xbox_MemoryLayoutInit derives from the loaded image, so it is no longer a
 * compile-time constant. */
static uint32_t g_heap_next = 0;

static int g_heap_alloc_count = 0;
static int g_heap_reuse_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
/* Allocation granularity for splitting a reused block, and the smallest
 * remainder worth recording as a separate free block. Both are one page
 * because every kernel export that reaches this heap asks for at least page
 * alignment. */
#define XBOX_HEAP_PAGE       4096
#define XBOX_HEAP_SPLIT_MIN  4096
/* `req` is what the caller asked for; `size` is the block's capacity, which is
 * larger whenever a reused block was not split. Keeping both is what turns
 * "the heap is full" into "the heap is full of retained capacity" or "of live
 * allocations", which are different bugs with different fixes.
 *
 * `ord` and `ra` name the kernel export and the guest call site that asked, so
 * an exhausted heap can be attributed rather than guessed at. */
static struct {
    uint32_t addr;
    uint32_t size;
    uint32_t req;
    uint32_t ra;
    uint16_t ord;
    uint8_t  free;
    uint8_t  reused;
} g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/* Who is allocating right now. kernel_thunk_dispatch sets this before each
 * bridge runs, so a heap block records the export and the guest return address
 * that produced it. Zero means "the runtime itself", not a guest call. */
static uint32_t g_heap_owner_ord = 0;

/* Allocations and frees per kernel export.
 *
 * "The heap is full" does not distinguish a title whose working set is genuinely
 * that large from one whose frees are not reaching this allocator. A per-export
 * tally does: an export with thousands of allocations and no frees is a missing
 * release path, and one whose counts track each other is a title using what it
 * asked for. Indexed by ordinal; 0 is the runtime itself. */
#define XBOX_HEAP_ORD_MAX 512
static uint32_t g_heap_ord_allocs[XBOX_HEAP_ORD_MAX];
static uint32_t g_heap_ord_frees[XBOX_HEAP_ORD_MAX];
static uint64_t g_heap_ord_alloc_bytes[XBOX_HEAP_ORD_MAX];
static uint64_t g_heap_ord_free_bytes[XBOX_HEAP_ORD_MAX];
static uint32_t g_heap_owner_ra  = 0;

/* How many individual heap events to narrate. The default is enough to see
 * the shape of a title's alloc/free pattern without burying the log; raise it
 * with RECOMP_HEAP_TRACE when chasing a specific block. */
static int heap_trace_limit(void)
{
    static int limit = -1;

    if (limit < 0) {
        const char *env = getenv("RECOMP_HEAP_TRACE");
        limit = env ? (int)strtol(env, NULL, 0) : 64;
        if (limit < 0) limit = 0;
    }
    return limit;
}

/* RECOMP_HEAP_POISON=<byte> fills freed blocks with that byte; unset means no
 * poison, which is the default because the fill costs a pass over the block. */
static int heap_poison_byte(void)
{
    static int value = -2;

    if (value == -2) {
        const char *env = getenv("RECOMP_HEAP_POISON");
        value = env ? (int)(strtol(env, NULL, 0) & 0xFF) : -1;
    }
    return value;
}

void xbox_HeapSetOwner(uint32_t ordinal, uint32_t guest_ra)
{
    g_heap_owner_ord = ordinal;
    g_heap_owner_ra  = guest_ra;
}

/* Guest addresses in the physical-memory mirror name the same RAM as the
 * ordinary address 0x80000000 below them: physical page P is visible at
 * 0x80000000 + P. A title that allocates through the normal address and frees
 * through the mirrored one is freeing the block it owns, so the table has to
 * be matched on the underlying address rather than the alias. */
static uint32_t heap_canonical_va(uint32_t va)
{
    uint32_t ram = (uint32_t)(g_xbox_total_ram ? g_xbox_total_ram
                                               : XBOX_TOTAL_RAM);
    if (va >= 0x80000000u && va < 0x80000000u + ram)
        return va - 0x80000000u;
    return va;
}

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

#define XBOX_THREAD_STACK_SLOTS 64
static struct { uint32_t top, bytes; } g_thread_stack_sizes[XBOX_THREAD_STACK_SLOTS];

uint32_t xbox_AllocThreadStack(uint32_t bytes)
{
    uint32_t base;

    if (g_thread_stacks_used >= XBOX_MAX_THREAD_STACKS) {
        return 0;
    }

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    /* Honour the size the title asked for.
     *
     * PsCreateSystemThreadEx takes KernelStackSize and JSRF passes 65,536 for
     * every worker. Handing each one the fixed 512 KB slice instead cost
     * 458,752 bytes per thread -- 1.75 MB across its four workers, on an arena
     * the stage loader exhausts. Hardware gives a title what it asks for.
     *
     * Clamped rather than trusted: zero means "the caller did not say", and a
     * guest stack still has to hold recompiled frames, so keep a floor. The
     * measured main-thread high-water is 3 KB. */
    if (!bytes) bytes = XBOX_THREAD_STACK_SIZE;
    if (bytes < XBOX_THREAD_STACK_MIN) bytes = XBOX_THREAD_STACK_MIN;
    if (bytes > XBOX_THREAD_STACK_SIZE) bytes = XBOX_THREAD_STACK_SIZE;
    bytes = (bytes + 0xFFFu) & ~0xFFFu;

    base = xbox_HeapAlloc(bytes, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Remember the size so the free path can find the block again; the API
     * takes only the top, and callers should not have to do the arithmetic. */
    for (unsigned i = 0; i < XBOX_THREAD_STACK_SLOTS; ++i) {
        if (g_thread_stack_sizes[i].top) continue;
        g_thread_stack_sizes[i].top = base + bytes - 16;
        g_thread_stack_sizes[i].bytes = bytes;
        break;
    }

    /* Top of the slice, 16-byte aligned, growing down. */
    return base + bytes - 16;
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
    *(uint32_t *)(tib + 0x20) = 0x00761000u; /* mapped zeroed PRCB */
    *(uint32_t *)(tib + 0x28) = tls_context_va;
    *(uint32_t *)(tls_context + 0x28) = tls_data_va;
    g_fs_base = tib_va;

    fprintf(stderr,
            "  TIB: thread FS=0x%08X TLS=0x%08X..0x%08X stack=0x%08X..0x%08X\n",
            tib_va, tls_data_va, tls_data_va + tls_data_size,
            stack_limit, stack_top);
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    uint32_t bytes = XBOX_THREAD_STACK_SIZE;
    if (!stack_top)
        return;
    for (unsigned i = 0; i < XBOX_THREAD_STACK_SLOTS; ++i) {
        if (g_thread_stack_sizes[i].top != stack_top) continue;
        bytes = g_thread_stack_sizes[i].bytes;
        g_thread_stack_sizes[i].top = 0;
        break;
    }
    xbox_HeapFree(stack_top + 16 - bytes);
    if (g_thread_stacks_used > 0)
        g_thread_stacks_used--;
}

/* Bump allocator over the contiguous window mapped at XBOX_CONTIG_BASE.
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base; XBOX_GPU_INSTANCE_DEFAULT is carved off the top by
 * the GPU-instance bridge, so the two do not meet until the window is full.
 * Never freed: contiguous blocks are framebuffers and pushbuffers, which a
 * title allocates once. */
#if defined(_WIN32)
static uint32_t g_contig_next = XBOX_CONTIG_BASE;
#endif

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    if (alignment < 4096) alignment = 4096;
#if !defined(_WIN32)
    /* Preserve the POSIX GPU backing contract: command offsets address low
     * guest RAM. The high contiguous window is separate unless the caller
     * explicitly enables the physical heap alias (as the JSRF harness does).
     * Returning 0x80084000 here made JSRF's raster clear write to 0x00084000
     * (live guest code) instead of its framebuffer. Keep unpinned buffers in
     * the shared guest heap until GPU physical-address translation is added.
     * When the heap alias is enabled, high CPU addresses now share those
     * same low GPU bytes. Fixed-address pinned allocations retain their
     * separate window. */
    uint32_t result = xbox_HeapAlloc(size, alignment);
    /* With the physical heap mapped, return its CPU address while retaining
     * the same low backing for GPU offsets. D3D reconstructs DMA_GET with
     * bit 31 set before comparing it with its allocation; returning a low
     * pointer makes that comparison reject every GET as outside the ring. */
    return result && g_physical_heap_view ? result | XBOX_CONTIG_BASE : result;
#else
    uint32_t result;

    /* With the window aliased to RAM there is one pool, as the hardware has.
     *
     * The bump arena below cannot be made safe by raising its floor: the
     * stacks and the heap sit above the image too, and the heap grows to the
     * top of RAM, so no floor clears them. It survives only as the fallback
     * for a layout where the alias could not be established, and its overlap
     * report says when that fallback is corrupting guest memory.
     *
     * The XBOX_CONTIG_BASE bit is not decoration: D3D reconstructs DMA_GET
     * with bit 31 set before comparing it against its own allocation, and the
     * GPU addresses this window by physical offset -- which, with the alias in
     * place, is the heap VA itself. */
    if (g_physical_heap_view) {
        uint32_t heap = xbox_HeapAlloc(size, alignment);
        return heap ? (heap | XBOX_CONTIG_BASE) : 0;
    }

    /* Never hand out a physical offset that overlaps the loaded image.
     *
     * The arena starts at XBOX_CONTIG_BASE, so the first allocation has
     * physical offset 0 and they climb from there -- straight through the
     * title's own code and data. The GPU addresses this window BY PHYSICAL
     * OFFSET: JSRF puts a depth surface here and programs
     * SET_SURFACE_ZETA_OFFSET with the low 26 bits, and the clear then writes
     * through that offset into low guest RAM. Measured: offset 0x00248000 on
     * this host against 0x007B4000 on POSIX, 1.2 MB of cleared depth
     * (0xFFFFFF00) painted over .data, and the title dying later on a function
     * pointer and a list head that had been inside the surface.
     *
     * The POSIX branch above does not have this problem because it allocates
     * from the guest heap, which already sits above the image -- and its
     * comment records the same bug being fixed there, for this same title.
     * This is that fix for this host.
     *
     * Read lazily rather than at init: the image bounds are known only after
     * the sections are loaded, and the first caller is later than that. */
    if (g_xbox_image_hi) {
        uint32_t floor_va = XBOX_CONTIG_BASE
                          + ((g_xbox_image_hi + 0xFFFFu) & ~0xFFFFu);
        if (g_contig_next < floor_va) {
            fprintf(stderr, "  [CONTIG] arena starts above the image: "
                            "0x%08X (image ends 0x%08X)\n",
                    floor_va, g_xbox_image_hi);
            fflush(stderr);
            g_contig_next = floor_va;
        }
    }

    result = (g_contig_next + alignment - 1) & ~(alignment - 1);

    /* Leave the top of the window for GPU instance memory. */
    if ((uint64_t)result + size >
            (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                - XBOX_GPU_INSTANCE_DEFAULT) {
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, %u of %u used)\n",
                size, g_contig_next - XBOX_CONTIG_BASE,
                (unsigned)XBOX_CONTIG_SIZE);
        fflush(stderr);
        return 0;
    }

    g_contig_next = result + size;

    /* Does this block land on the guest heap or the stacks?
     *
     * The arena is a bump allocator over PHYSICAL offsets and knows only two
     * things: where the image ends, and where the window ends. It does not
     * know where the guest's low memory lives -- and on this host the stacks
     * and the heap sit at a FIXED low address above the image, so a long
     * enough run of contiguous allocations climbs straight into them. The
     * POSIX branch cannot reach this state: it takes contiguous memory from
     * that same heap, so the heap allocator itself keeps the two apart.
     *
     * Report, do not clamp. Which allocation crosses the line, and what the
     * title then does with it, is the evidence; moving the floor first would
     * hide the producer. One line per offending block, capped, because a
     * title that crosses once crosses for every allocation afterwards. */
    {
        static unsigned reported;
        uint32_t phys = result - XBOX_CONTIG_BASE;
        uint32_t phys_end = phys + size;

        if (phys_end > XBOX_STACK_BASE && reported < 64u) {
            ++reported;
            fprintf(stderr,
                    "  [CONTIG] block 0x%08X..0x%08X overlaps guest low memory"
                    " (stacks 0x%08X, heap 0x%08X, %u of arena used)%s\n",
                    phys, phys_end, (uint32_t)XBOX_STACK_BASE,
                    (uint32_t)XBOX_HEAP_BASE, phys,
                    reported == 64u ? " [last report]" : "");
            fflush(stderr);
        }
    }

    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
#endif
}

/* Account for the whole heap and say who is holding it.
 *
 * "out of memory (used N/M)" reports the bump high-water mark, which says
 * nothing about how much is actually live: a heap that has freed and reused
 * everything reads the same as one that has leaked everything. This separates
 * the four things that can be true at once -- bytes live, bytes free and
 * reusable, the largest single free block (a large request can fail with
 * megabytes free), and capacity retained beyond what callers asked for --
 * and then attributes the live bytes to the kernel export and guest call site
 * that requested them.
 */
void xbox_HeapReport(const char *why)
{
    if (!g_heap_next) g_heap_next = XBOX_HEAP_BASE;
    struct heap_owner { uint32_t ord; uint32_t ra; uint32_t n;
                        uint64_t bytes; uint64_t slack; };
    struct heap_owner own[512];
    int own_used = 0;
    uint64_t live_bytes = 0, free_bytes = 0, retained = 0;
    int live_n = 0, free_n = 0, dead_n = 0;
    uint32_t largest_free = 0;
    uint32_t top_addr[16], top_size[16], top_ord[16], top_ra[16];
    int top_n = 0;
    int i, j;

    for (i = 0; i < g_heap_block_count; i++) {
        uint32_t sz = g_heap_blocks[i].size;

        if (!sz) { dead_n++; continue; }
        if (g_heap_blocks[i].free) {
            free_n++;
            free_bytes += sz;
            if (sz > largest_free) largest_free = sz;
            continue;
        }

        live_n++;
        live_bytes += sz;
        if (sz > g_heap_blocks[i].req)
            retained += sz - g_heap_blocks[i].req;

        for (j = 0; j < own_used; j++) {
            if (own[j].ord == g_heap_blocks[i].ord &&
                own[j].ra  == g_heap_blocks[i].ra)
                break;
        }
        if (j == own_used && own_used < (int)(sizeof own / sizeof own[0])) {
            own[own_used].ord = g_heap_blocks[i].ord;
            own[own_used].ra  = g_heap_blocks[i].ra;
            own[own_used].n = 0;
            own[own_used].bytes = 0;
            own[own_used].slack = 0;
            own_used++;
        }
        if (j < own_used) {
            own[j].n++;
            own[j].bytes += sz;
            if (sz > g_heap_blocks[i].req)
                own[j].slack += sz - g_heap_blocks[i].req;
        }

        /* Keep the 16 largest live blocks, insertion-sorted. */
        {
            int slot = top_n;
            while (slot > 0 && top_size[slot - 1] < sz) slot--;
            if (slot < 16) {
                int k = (top_n < 16 ? top_n : 15);
                for (; k > slot; k--) {
                    top_addr[k] = top_addr[k - 1];
                    top_size[k] = top_size[k - 1];
                    top_ord[k]  = top_ord[k - 1];
                    top_ra[k]   = top_ra[k - 1];
                }
                top_addr[slot] = g_heap_blocks[i].addr;
                top_size[slot] = sz;
                top_ord[slot]  = g_heap_blocks[i].ord;
                top_ra[slot]   = g_heap_blocks[i].ra;
                if (top_n < 16) top_n++;
            }
        }
    }

    fprintf(stderr, "  [HEAP] ---- report: %s ----\n", why ? why : "(no reason)");
    fprintf(stderr, "  [HEAP] arena 0x%08X..0x%08X (%u bytes), bump high-water %u,"
                    " unreached %u\n",
            (unsigned)XBOX_HEAP_BASE, (unsigned)XBOX_HEAP_TOP,
            (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE),
            (unsigned)(g_heap_next - XBOX_HEAP_BASE),
            (unsigned)(XBOX_HEAP_TOP - g_heap_next));
    fprintf(stderr, "  [HEAP] live %d blocks / %llu bytes;"
                    " free %d blocks / %llu bytes; largest free %u;"
                    " retained-beyond-request %llu; table %d entries (%d dead)\n",
            live_n, (unsigned long long)live_bytes,
            free_n, (unsigned long long)free_bytes,
            (unsigned)largest_free, (unsigned long long)retained,
            g_heap_block_count, dead_n);

    /* Owners, largest first. own_used is small; a selection sort is clearer
     * here than dragging in qsort's comparator indirection. */
    for (i = 0; i < own_used && i < 20; i++) {
        int best = i;
        for (j = i + 1; j < own_used; j++)
            if (own[j].bytes > own[best].bytes) best = j;
        if (best != i) {
            struct heap_owner t = own[i]; own[i] = own[best]; own[best] = t;
        }
        if (own[i].bytes < 64 * 1024) break;
        /* Slack is what the allocator kept beyond the request: rounding a
         * reused block up to a whole page charges a 16-byte pool request
         * 4096 bytes. Printed per owner because "the heap is full" and
         * "the heap is full of padding" have different fixes. */
        fprintf(stderr, "  [HEAP]   ordinal %-4u ra=0x%08X : %u blocks,"
                        " %llu bytes (%llu slack)\n",
                own[i].ord, own[i].ra, own[i].n,
                (unsigned long long)own[i].bytes,
                (unsigned long long)own[i].slack);
    }
    for (i = 0; i < top_n; i++) {
        fprintf(stderr, "  [HEAP]   live 0x%08X size %-9u ordinal %-4u ra=0x%08X\n",
                top_addr[i], top_size[i], top_ord[i], top_ra[i]);
    }
    for (i = 0; i < XBOX_HEAP_ORD_MAX; i++) {
        if (!g_heap_ord_allocs[i] && !g_heap_ord_frees[i]) continue;
        fprintf(stderr, "  [HEAP]   export %-4d %u allocs / %llu bytes,"
                        " %u frees / %llu bytes\n",
                i, g_heap_ord_allocs[i],
                (unsigned long long)g_heap_ord_alloc_bytes[i],
                g_heap_ord_frees[i],
                (unsigned long long)g_heap_ord_free_bytes[i]);
    }
    fflush(stderr);
}

/* Fold the untouched tail of the arena into the free block that ends at it.
 *
 * The bump frontier and the free list describe the same arena but were only
 * ever consulted separately, so a free block sitting immediately below the
 * frontier could never combine with the space above it. JSRF failed an 827,904
 * byte request holding 940,208 bytes of free blocks and 219,824 bytes of
 * untouched arena, no single piece of which was large enough -- while the two
 * largest pieces were adjacent.
 *
 * After this runs the frontier is at the top and every free byte is described
 * by the block table, which is also what makes the ordinary coalescing in
 * xbox_HeapFree able to reach it. Costs one backwards scan to the last entry
 * that still describes a block; the table is address-ordered by construction.
 */
static void heap_absorb_frontier(void)
{
    int i;

    if (g_heap_next >= XBOX_HEAP_TOP)
        return;
    for (i = g_heap_block_count - 1; i >= 0; i--) {
        if (!g_heap_blocks[i].size)
            continue;                       /* merged away, keep looking */
        if (!g_heap_blocks[i].free)
            return;                         /* live at the frontier */
        if (g_heap_blocks[i].addr + g_heap_blocks[i].size != g_heap_next)
            return;                         /* a gap, not the frontier block */
        g_heap_blocks[i].size = XBOX_HEAP_TOP - g_heap_blocks[i].addr;
        g_heap_next = XBOX_HEAP_TOP;
        return;
    }
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    if (!g_heap_next) g_heap_next = XBOX_HEAP_BASE;

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    heap_absorb_frontier();

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    for (int i = 0; i < g_heap_block_count; i++) {
        uint32_t capacity, base, aligned, lead, avail, kept, spare;

        if (!g_heap_blocks[i].free) {
            continue;
        }

        /* Reach an aligned start inside the block instead of demanding that
         * the block already begin on one.
         *
         * Rejecting a misaligned free block was the reason the split boundary
         * had to be page-rounded: a remainder starting at an arbitrary offset
         * could never serve a 4 KB-aligned caller, so the heap filled with
         * free memory nothing could allocate. Carving the lead off as its own
         * free block fixes that at the source, and once it is fixed the
         * trailing boundary only has to respect this caller's alignment.
         *
         * That matters because the callers are not all page-aligned:
         * ExAllocatePool and ExAllocatePoolWithTag ask for 16. Charging those
         * a whole page each cost JSRF 2,725,856 bytes across 706 live pool
         * blocks holding 3,126,516 -- 87% padding -- and 4,581,336 bytes of
         * slack overall, on a 48.5 MB arena, which is what turned a 1,092,096
         * byte request into "out of memory" and the title's disc-error dialog.
         */
        base    = g_heap_blocks[i].addr;
        aligned = (base + alignment - 1) & ~(uint32_t)(alignment - 1);
        if (aligned < base) continue;                 /* alignment overflow */
        lead    = aligned - base;
        capacity = g_heap_blocks[i].size;
        if (capacity < lead) continue;
        avail = capacity - lead;
        if (avail < size) continue;

        /* Both splits insert, and the table is address-ordered by
         * construction (bump order) because coalescing depends on it, so make
         * room for the worst case before touching anything. */
        if (lead && g_heap_block_count + 2 > XBOX_HEAP_MAX_BLOCKS) {
            continue;
        }

        /* Leading fragment: stays free, keeps its original address. */
        if (lead) {
            memmove(&g_heap_blocks[i + 1], &g_heap_blocks[i],
                    (size_t)(g_heap_block_count - i) * sizeof g_heap_blocks[0]);
            g_heap_block_count++;
            g_heap_blocks[i].size = lead;             /* the lead, still free */
            i++;                                      /* the request's block */
            g_heap_blocks[i].addr = aligned;
            g_heap_blocks[i].size = avail;
            capacity = avail;
        }

        /* Split, rather than handing the whole block over.
         *
         * Taking a 4 MB free block to satisfy a 16-byte request and recording
         * it at its original size retires the rest of it for good: the block
         * is live, so nothing can reuse the remainder, and it never comes back
         * separately because the free that follows returns one entry. A title
         * that churns small allocations against a heap whose free blocks came
         * from big ones loses the difference every time.
         *
         * XBOX_HEAP_SPLIT_MIN keeps the table from filling with slivers --
         * below it the padding stays with the block, which is what an
         * allocator's minimum granularity is for. */
        kept = (size + alignment - 1) & ~(uint32_t)(alignment - 1);
        if (kept < size) kept = capacity;         /* overflow: do not split */
        spare = capacity > kept ? capacity - kept : 0;
        if (spare >= XBOX_HEAP_SPLIT_MIN &&
            g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
            memmove(&g_heap_blocks[i + 2], &g_heap_blocks[i + 1],
                    (size_t)(g_heap_block_count - i - 1) *
                        sizeof g_heap_blocks[0]);
            g_heap_block_count++;
            g_heap_blocks[i + 1].addr = g_heap_blocks[i].addr + kept;
            g_heap_blocks[i + 1].size = spare;
            g_heap_blocks[i + 1].req = 0;
            g_heap_blocks[i + 1].ra = 0;
            g_heap_blocks[i + 1].ord = 0;
            g_heap_blocks[i + 1].reused = 0;
            g_heap_blocks[i + 1].free = 1;
            g_heap_blocks[i].size = kept;
        }

        g_heap_blocks[i].free = 0;
        g_heap_blocks[i].req = size;
        g_heap_blocks[i].ord = (uint16_t)g_heap_owner_ord;
        g_heap_blocks[i].ra = g_heap_owner_ra;
        g_heap_blocks[i].reused = 1;
        if (++g_heap_reuse_count <= heap_trace_limit())
            fprintf(stderr, "  [HEAP] reuse #%d 0x%08X size=%u (of %u) align=%u"
                            " ordinal %u ra=0x%08X\n",
                    g_heap_reuse_count, g_heap_blocks[i].addr, size,
                    capacity, alignment,
                    g_heap_owner_ord, g_heap_owner_ra);
        result = g_heap_blocks[i].addr;
        if (g_heap_owner_ord < XBOX_HEAP_ORD_MAX) {
            g_heap_ord_allocs[g_heap_owner_ord]++;
            g_heap_ord_alloc_bytes[g_heap_owner_ord] += size;
        }
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    /* Align the next pointer.
     *
     * The bytes skipped to reach the boundary stay with the allocation below
     * them and are deliberately not recycled. Handing them out was tried and
     * reverted: NtAllocateVirtualMemory and MmAllocateContiguousMemoryEx are
     * page-granular on hardware, so a title that asks for 5,000 bytes owns the
     * whole 8,192-byte span and writes into it. Reclaiming the tail put a
     * 16-byte pool block inside a page the title was still using, and JSRF
     * stopped dead at IoCreateDevice with no draws and no ADX tick. */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP) {
        /* Name the caller. "Out of memory" without it says how much was left
         * and nothing about which export asked, so the failing request could
         * not be matched against the owner breakdown printed just below. */
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, align %u,"
                        " used %u/%u, ordinal %u ra=0x%08X)\n",
                size, alignment, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE),
                g_heap_owner_ord, g_heap_owner_ra);
        /* Who ate the heap? The size histogram this used to print named the
         * repeated request size and nothing else -- not whether those blocks
         * were still live, and not who asked for them. The full report does
         * both, once, because the failing request usually repeats. */
        {
            static int dumped = 0;
            if (!dumped) {
                dumped = 1;
                xbox_HeapReport("allocation failed");
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
        g_heap_blocks[g_heap_block_count].req = size;
        g_heap_blocks[g_heap_block_count].ord = (uint16_t)g_heap_owner_ord;
        g_heap_blocks[g_heap_block_count].ra = g_heap_owner_ra;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_blocks[g_heap_block_count].reused = 0;
        g_heap_block_count++;
    }

    if (g_heap_owner_ord < XBOX_HEAP_ORD_MAX) {
        g_heap_ord_allocs[g_heap_owner_ord]++;
        g_heap_ord_alloc_bytes[g_heap_owner_ord] += size;
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

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    uint32_t va;
    int i;

    if (!xbox_va)
        return 0;
    va = heap_canonical_va(xbox_va);
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free || !g_heap_blocks[i].size)
            continue;
        if (va >= g_heap_blocks[i].addr &&
            va <  g_heap_blocks[i].addr + g_heap_blocks[i].size)
            return g_heap_blocks[i].size - (va - g_heap_blocks[i].addr);
    }
    return 0;
}

/* Next/previous table entry that still describes a block.
 *
 * Coalescing used to test index i-1 and i+1 directly, which stops working the
 * moment anything has been merged: a merged-away entry is left with size 0 as
 * a hole, and a hole between two adjacent free blocks made them permanently
 * un-mergeable. The heap then fragments in a way no amount of freeing undoes.
 * Skipping holes costs a short scan and keeps address order intact. */
static int heap_live_entry_after(int i)
{
    for (i++; i < g_heap_block_count; i++)
        if (g_heap_blocks[i].size) return i;
    return -1;
}

static int heap_live_entry_before(int i)
{
    for (i--; i >= 0; i--)
        if (g_heap_blocks[i].size) return i;
    return -1;
}

/* Describe the block covering this address, and who asked for it.
 *
 * Written for one question the renderer could not otherwise answer: a vertex
 * array that reads as all zeros is either a buffer the title has not filled or
 * a buffer this heap handed to somebody else and zeroed underneath it, and
 * those have opposite fixes. The owner's kernel ordinal and guest return
 * address separate them.
 *
 * Formats into the caller's buffer rather than exposing the block table, so a
 * consumer needs one extern and no shared struct. Returns 0, with `buf` set to
 * a sentence saying so, when this heap never issued the address.
 */
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size)
{
    uint32_t va;
    int i;

    if (!buf || !size)
        return 0;
    buf[0] = 0;
    if (!xbox_va) {
        snprintf(buf, size, "null address");
        return 0;
    }
    va = heap_canonical_va(xbox_va);
    for (i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].size)
            continue;
        if (va < g_heap_blocks[i].addr ||
            va >= g_heap_blocks[i].addr + g_heap_blocks[i].size)
            continue;
        snprintf(buf, size,
                 "block 0x%08X+%u (asked %u) ordinal %u ra=0x%08X %s%s",
                 g_heap_blocks[i].addr, g_heap_blocks[i].size,
                 g_heap_blocks[i].req, g_heap_blocks[i].ord,
                 g_heap_blocks[i].ra,
                 g_heap_blocks[i].free ? "FREE" : "live",
                 g_heap_blocks[i].reused ? ", from a reused block" : "");
        return 1;
    }
    snprintf(buf, size, "not a block this heap issued");
    return 0;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0, missed = 0;
    uint32_t va;
    int next, prev;

    if (!xbox_va) {
        return;
    }
    /* RECOMP_HEAP_NO_FREE restores the behaviour this heap had before the
     * kernel free bridges were wired up: nothing is ever reclaimed. It exists
     * to A/B a suspected use-after-free against the build that could not have
     * one, which is the only way to tell "we recycled memory the title still
     * uses" from "the title never wrote there". It exhausts the heap. */
    {
        static int disabled = -1;
        if (disabled < 0) disabled = getenv("RECOMP_HEAP_NO_FREE") ? 1 : 0;
        if (disabled) return;
    }
    frees++;
    va = heap_canonical_va(xbox_va);
    for (int i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].size || g_heap_blocks[i].addr != va ||
            g_heap_blocks[i].free) {
            continue;
        }
        /* Opt-in poison. Routing the title's frees to this heap means memory
         * can now be handed to a second owner, so a stale guest pointer stops
         * being harmless and starts reading someone else's data -- which looks
         * like ordinary corruption from the fault site. Filling a freed block
         * with a pattern no valid pointer or float has makes the read visible
         * as itself: the faulting address is the poison. */
        if (heap_poison_byte() >= 0)
            memset((void *)((uintptr_t)g_heap_blocks[i].addr + g_memory_offset),
                   heap_poison_byte(), g_heap_blocks[i].size);
        if (frees <= heap_trace_limit())
            fprintf(stderr, "  [HEAP] free #%d va=0x%08X size=%u ordinal %u "
                            "ra=0x%08X\n",
                    frees, xbox_va, g_heap_blocks[i].size,
                    g_heap_owner_ord, g_heap_owner_ra);
        if (g_heap_owner_ord < XBOX_HEAP_ORD_MAX) {
            g_heap_ord_frees[g_heap_owner_ord]++;
            g_heap_ord_free_bytes[g_heap_owner_ord] += g_heap_blocks[i].size;
        }
        g_heap_blocks[i].free = 1;
        g_heap_blocks[i].req = 0;
        g_heap_blocks[i].ord = 0;
        g_heap_blocks[i].ra = 0;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d missed=%d blocks=%d\n",
                    frees, matched, missed, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in address order, so
         * adjacency is a simple end==start test against the nearest entry that
         * still describes a block. Keeps large contiguous requests satisfiable
         * after a lot of small churn. */
        next = heap_live_entry_after(i);
        if (next >= 0 && g_heap_blocks[next].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[next].addr) {
            g_heap_blocks[i].size += g_heap_blocks[next].size;
            g_heap_blocks[next].size = 0;
            g_heap_blocks[next].addr = 0;
        }
        prev = heap_live_entry_before(i);
        if (prev >= 0 && g_heap_blocks[prev].free &&
            g_heap_blocks[prev].addr + g_heap_blocks[prev].size == g_heap_blocks[i].addr) {
            g_heap_blocks[prev].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        return;
    }

    /* A free that matches nothing is not automatically a bug -- pinned
     * contiguous allocations never came from this table -- but it is exactly
     * what a leak looks like from here, so say it rather than returning in
     * silence. Bounded: a title that does it once does it constantly. */
    if (++missed <= heap_trace_limit()) {
        fprintf(stderr, "  [HEAP] free of 0x%08X (canonical 0x%08X) matched no "
                        "block (miss #%d of %d frees, ordinal %u ra=0x%08X)\n",
                xbox_va, va, missed, frees, g_heap_owner_ord, g_heap_owner_ra);
        fflush(stderr);
    }
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}

/* ── Is the title's code still the code we loaded? ─────────────────────────
 *
 * The Windows oracle's push-buffer ring was observed at 0x1000-0x81000, and
 * the title's executable range starts at 0x11000. That is a 448 KB overlap
 * between where the guest writes command words and where its own instructions
 * live. If the ring really is there, the guest is scribbling over its own
 * code, every divergence measured on that host is an artefact, and no other
 * finding from it survives -- so this has to be settled before the rest of the
 * Windows plan means anything.
 *
 * MEASURED OVER .text ONLY, and that distinction is the whole instrument. This
 * XBE marks .rdata and .data executable as well, so g_xbox_code_lo..hi -- the
 * range indirect calls may target -- covers most of the image. Sampling that
 * range reports the guest writing its own globals, every run, on both hosts,
 * which is what normal execution looks like and says nothing about code. The
 * first version of this probe did exactly that and its verdict line called it
 * "the guest is writing where its own instructions live". Use g_xbox_text_*.
 *
 * Sixteen pages, summed at the first call and re-summed on every periodic
 * report. HALF of them inside the observed ring window and half across the
 * rest of the range, rather than sixteen spread evenly -- an even spread puts
 * only three pages inside the window that the hypothesis is actually about,
 * and spends the other thirteen on ground nothing is accused of touching.
 *
 * The outer half is the control, and it is what makes the result readable: the
 * ring covers only the bottom of the range, so if the ring is the writer the
 * inner pages move and the outer ones do not. All sixteen moving is a
 * different fault entirely, and none moving retires the hypothesis.
 *
 * The window is RECOMP_TEXT_CK_WINDOW (default 0x81000, the ring limit seen on
 * Windows) so a run that measures a different ring can re-aim this without a
 * rebuild.
 *
 * Read-only and opt-in, per the tree's convention, though the weight that rule
 * exists for is not really in question here -- 64 KB summed every few seconds.
 * The Windows steps need RECOMP_TEXT_CHECKSUM=1 set.
 */
#define TEXT_CK_PAGES 16

void xbox_TextChecksumReport(void)
{
    static int enabled = -1;
    static uint32_t page_va[TEXT_CK_PAGES];
    static uint32_t baseline[TEXT_CK_PAGES];
    /* A checksum says a page moved; it cannot say what moved, and "the guest
     * is overwriting its code" and "our own loader patched a table" look
     * identical through one. Keep the bytes so the report can name the offset
     * and the values -- 64 KB, and it is the difference between a finding and
     * an alarm. */
    static unsigned char snapshot[TEXT_CK_PAGES][4096];
    static uint32_t page_len[TEXT_CK_PAGES];
    static int armed;
    static unsigned long reports;
    uint32_t now[TEXT_CK_PAGES];
    int i, changed = 0, inner_changed = 0;

    if (enabled < 0) enabled = getenv("RECOMP_TEXT_CHECKSUM") ? 1 : 0;
    if (!enabled) return;
    if (!g_xbox_text_lo || g_xbox_text_hi <= g_xbox_text_lo) return;

    {
        static uint32_t window;
        uint32_t inner_hi, outer_span;

        if (!window) {
            const char *w = getenv("RECOMP_TEXT_CK_WINDOW");
            window = w ? (uint32_t)strtoul(w, NULL, 0) : 0x81000u;
        }
        /* Clamp: a window past the end of .text would put every page in the
         * inner half and leave no control at all. */
        inner_hi = window;
        if (inner_hi <= g_xbox_text_lo || inner_hi > g_xbox_text_hi)
            inner_hi = g_xbox_text_lo + (g_xbox_text_hi - g_xbox_text_lo) / 4;
        outer_span = g_xbox_text_hi - inner_hi;

        for (i = 0; i < TEXT_CK_PAGES; i++) {
            uint32_t va;
            if (armed) {
                va = page_va[i];
            } else if (i < TEXT_CK_PAGES / 2) {
                va = g_xbox_text_lo
                   + (uint32_t)((uint64_t)(inner_hi - g_xbox_text_lo)
                        * (uint32_t)i / (TEXT_CK_PAGES / 2));
            } else {
                va = inner_hi
                   + (uint32_t)((uint64_t)outer_span
                        * (uint32_t)(i - TEXT_CK_PAGES / 2)
                        / (TEXT_CK_PAGES / 2));
            }
            const unsigned char *p;
            uint32_t h = 2166136261u;
            uint32_t n = 4096, k;

            va &= ~0xFFFu;
            if (va < g_xbox_text_lo) va = g_xbox_text_lo;
            if (va + n > g_xbox_text_hi) n = g_xbox_text_hi - va;
            page_va[i] = va;
            page_len[i] = n;
            p = (const unsigned char *)((uintptr_t)va + g_memory_offset);
            for (k = 0; k < n; k++) { h ^= p[k]; h *= 16777619u; }
            now[i] = h;
        }
    }

    if (!armed) {
        armed = 1;
        for (i = 0; i < TEXT_CK_PAGES; i++) {
            baseline[i] = now[i];
            memcpy(snapshot[i],
                   (const void *)((uintptr_t)page_va[i] + g_memory_offset),
                   page_len[i]);
        }
        fprintf(stderr, "  [TEXT-CK] baseline over .text 0x%08X-0x%08X: "
                "%d pages in the ring window 0x%08X-0x%08X, %d outside it "
                "as the control (0x%08X-0x%08X)\n",
                g_xbox_text_lo, g_xbox_text_hi,
                TEXT_CK_PAGES / 2, page_va[0], page_va[TEXT_CK_PAGES / 2 - 1],
                TEXT_CK_PAGES / 2, page_va[TEXT_CK_PAGES / 2],
                page_va[TEXT_CK_PAGES - 1]);
        fflush(stderr);
        return;
    }

    reports++;
    for (i = 0; i < TEXT_CK_PAGES; i++) {
        if (now[i] == baseline[i]) continue;
        changed++;
        if (i < TEXT_CK_PAGES / 2) inner_changed++;
        {
            const unsigned char *live =
                (const unsigned char *)((uintptr_t)page_va[i] + g_memory_offset);
            uint32_t off, first = page_len[i], last = 0, dwords = 0;
            for (off = 0; off + 4 <= page_len[i]; off += 4) {
                if (memcmp(snapshot[i] + off, live + off, 4) == 0) continue;
                if (dwords == 0) first = off;
                last = off;
                dwords++;
            }
            fprintf(stderr, "  [TEXT-CK] page %2d VA 0x%08X (%s) CHANGED "
                    "0x%08X -> 0x%08X (report %lu): %u dwords, "
                    "VA 0x%08X..0x%08X, first 0x%08X -> 0x%08X\n",
                    i, page_va[i],
                    (i < TEXT_CK_PAGES / 2) ? "ring window" : "control",
                    baseline[i], now[i], reports, dwords,
                    page_va[i] + first, page_va[i] + last,
                    *(const uint32_t *)(snapshot[i] + first),
                    *(const uint32_t *)(live + first));
            memcpy(snapshot[i], live, page_len[i]);
        }
        /* Re-baseline so the next report says "changed again" rather than
         * repeating this one forever. The count below is the running total. */
        baseline[i] = now[i];
    }
    if (inner_changed) {
        fprintf(stderr, "  [TEXT-CK] %d of %d pages INSIDE the ring window "
                "differ (and %d of %d in the control) -- the guest is writing "
                "where its own instructions live; treat every other "
                "measurement on this host as void until it is explained\n",
                inner_changed, TEXT_CK_PAGES / 2,
                changed - inner_changed, TEXT_CK_PAGES / 2);
        fflush(stderr);
    } else if (changed) {
        /* Outside the window the ring cannot reach, so this is not the
         * hypothesis this probe was built for and must not be reported as if
         * it were. The executable range covers .rdata in this XBE, and the
         * guest writes there in the ordinary course of running: on macOS the
         * only page that ever moves is 0x001C3000, one dword at 0x001C3F20,
         * "MU_0" -> "MU_7" -- a memory-unit drive letter, not code. Read the
         * offset and the values before treating any of these as a fault. */
        fprintf(stderr, "  [TEXT-CK] %d page(s) changed, all OUTSIDE the ring "
                "window -- see the offsets above; not the ring, and not on "
                "its own a reason to distrust this run\n", changed);
        fflush(stderr);
    } else if (reports == 1) {
        fprintf(stderr, "  [TEXT-CK] %d pages unchanged\n", TEXT_CK_PAGES);
        fflush(stderr);
    }
}
