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

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static void *g_tiled_view = NULL;

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
        if (!attached && getenv("RECOMP_OHCI_ATTACH")) {
            volatile uint32_t *ps =
                (volatile uint32_t *)((char *)g_mcpx_regs + 0x500054);
            volatile uint32_t *ctl =
                (volatile uint32_t *)((char *)g_mcpx_regs + 0x500004);
            /* Only once the title has actually brought the controller up,
             * so this cannot be mistaken for a device present at reset. */
            if ((*ctl & 0xC0u) != 0) {
                volatile uint32_t *ist =
                    (volatile uint32_t *)((char *)g_mcpx_regs + 0x50000C);
                volatile uint32_t *ien =
                    (volatile uint32_t *)((char *)g_mcpx_regs + 0x500010);
                attached = 1;
                *ps = 0x00010001u;   /* CurrentConnectStatus | ConnectStatusChange */
                /* A connect that raises no interrupt is invisible: XPP has
                 * RootHubStatusChange enabled in HcInterruptEnable and is
                 * waiting on it, so the status bit is what actually announces
                 * the plug. */
                *ist |= 0x00000040u;                    /* RHSC */
                fprintf(stderr, "  [OHCI] attach probe: port1=0x%08X "
                        "intr_status=0x%08X intr_enable=0x%08X\n",
                        *ps, *ist, *ien);
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
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

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
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
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
        xbox_McpxHoldRegisters();
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
                fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X\n", put);
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

        if (g_mcpx_regs && !g_apu_mmio_trapped) {
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
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;
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
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
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

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
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
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
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
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
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
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
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

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
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

/* Bump allocator for pure address-space reservations, above RAM.
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
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
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
    base = xbox_HeapAlloc(XBOX_THREAD_STACK_SIZE, 4096);
    if (!base)
        return 0;
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
    if (!stack_top)
        return;
    xbox_HeapFree(stack_top + 16 - XBOX_THREAD_STACK_SIZE);
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
     * guest RAM, while the high contiguous window has separate storage.
     * Returning 0x80084000 here made JSRF's raster clear write to 0x00084000
     * (live guest code) instead of its framebuffer. Keep unpinned buffers in
     * the shared guest heap until GPU physical-address translation is added.
     * Fixed-address pinned allocations retain their separate window. */
    return xbox_HeapAlloc(size, alignment);
#else
    uint32_t result;
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
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
#endif
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
    int i;

    if (!xbox_va)
        return 0;
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size)
            return g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
    }
    return 0;
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
