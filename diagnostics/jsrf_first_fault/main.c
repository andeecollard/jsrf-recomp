#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
extern _Bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                  uint32_t fault_xbox_va, int is_write);
#else
#include <signal.h>
/* dladdr, to name the function a crash actually happened in. It is a Darwin
 * extension, so the header hides it under the -std=c11 the build asks for;
 * _DARWIN_C_SOURCE is what unhides it, and must precede the include. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#endif

#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
#include "guest_trace.h"
#include "apu/apu.h"
#include "nv2a_pusher.h"
#include "nv2a_pb_scan.h"
#include "recomp_icall_feedback.h"
extern void nv2a_pb_exec_report(void);
extern ptrdiff_t xbox_GetMemoryOffset(void);
static int pad_sentinel(void);
static void pad_sentinel_scan(void);
static void jsrf_save_dump(void);
static void jsrf_scene_report(void);
static void jsrf_seq_trace_start(void);
static uint32_t jsrf_root_va(const uint8_t *base, uint32_t *via_out,
                             const char **how_out, int verbose);
static void jsrf_object_dump(void);
static void jsrf_guest_trace_report(void);
/* The rasterised surface, and the window that can show it. The executor draws
 * into guest memory and the GL backend owns the window; neither can reach the
 * other without being introduced here. */
extern const void *nv2a_pb_exec_surface(uint32_t *w, uint32_t *h,
                                        uint32_t *pitch, uint32_t *bpp);
#if !defined(_WIN32)
extern void xbox_D3D8SetGuestFramebufferSource(
        const void *(*fn)(uint32_t *, uint32_t *, uint32_t *, uint32_t *));
#endif
extern void xbox_HeapReport(const char *why);
#include "nv2a_pgraph_d3d11.h"
#include "d3d8_xbox.h"   /* PROBE: D3D8 HLE layer */
#include "xinput_xbox.h"
#include "xbox_usb_ohci.h"

/* Defined in src/apu/apu_mmio_hook.c, outside its Win32 guard. The MMIO hook
 * itself is Windows-only; the state pointer is not. */
extern MCPXAPUState *g_apu_state;

/* Sink for guest writes to the APU register aperture. Installed before
 * xbox_MemoryLayoutInit (which arms the trap) and tolerant of the APU not being
 * up yet, because the APU needs the RAM pointer that init produces. */
/*
 * D3D push-buffer acknowledgement.
 *
 * JSRF's D3D8 reserves push-buffer space in sub_00191440, which compares the
 * channel's PUT against its GET and waits when there is not enough room:
 *
 *     edi = [0x0019DCE0]      ; D3D channel
 *     eax = [edi+0x30]        ; PUT  (an index, e.g. 11)
 *     ecx = [[edi+0x34]]      ; GET  (an index, e.g. 3)
 *
 * On hardware the GPU consumes the buffer and advances GET. Here the D3D11
 * layer does the drawing instead, so nothing advances it and the main thread
 * waits forever. Reporting the commands consumed is the same acknowledgement
 * the runtime already makes for the NV2A busy bits and for DMA_PUT/DMA_GET --
 * and the same justification: the work is being done elsewhere.
 *
 * It lives HERE, in the per-title harness, and not in src/kernel, because the
 * addresses are this title's: 0x0019DCE0 is a D3D8 global and GET sits in a
 * contiguous buffer whose address only the guest knows. The runtime's existing
 * ack targets the USER area at 0xFD800040/44, but the software index also needs
 * acknowledgement after the published commands have actually been consumed.
 * The general fix is to route NV2A register writes to a model the way APU
 * writes now are, and learn the notifier address from the channel setup; until
 * that exists this is the honest stand-in, kept out of the shared runtime.
 */
#define JSRF_D3D_CHANNEL_PTR 0x0019DCE0u
#define JSRF_D3D_PUT_OFFSET  0x30u
#define JSRF_D3D_GETPTR_OFFSET 0x34u

static volatile int g_pushbuf_ack_stop;

/* Drive the NV2A pusher from completed JSRF submissions. Device +0 is
 * the writer's allocation cursor, not a publication barrier. sub_001912EC
 * publishes completed packets to the channel's DMA_PUT (0xFD800040).
 * Reading +0 used to consume partly written packets and stale ring tails.
 *
 * Ring bounds remain title-specific: device +0x24/+0x28. The CPU path uses
 * this title's low physical RAM backing; it does not add 0x80000000.
 * The +0x30/+0x34 index acknowledgement is sampled before consumption, and
 * only that sampled index is acknowledged after the published span is read.
 */
#define JSRF_PB_PUT_VA    0x0019B200u
#define JSRF_PB_LIMIT_VA  0x0019B204u
#define JSRF_PB_START_VA  0x0019B224u   /* pb_ring_start, per the BO3 map */
#define JSRF_PB_END_VA    0x0019B228u   /* pb_ring_end */

/* Observe the reserve decision before changing its address contract. */
void jsrf_pb_reserve_probe(uint32_t pc, uint32_t dev, uint32_t get,
                           uint32_t writer, uint32_t limit)
{
    static unsigned samples;
    if (!getenv("RECOMP_PB_RESERVE_TRACE") || samples++ >= 12) return;
    fprintf(stderr, "[PB-RESERVE] pc=%08X ring=%08X-%08X get=%08X writer=%08X limit=%08X raw-get=%08X\n",
            pc, MEM32(dev+0x24), MEM32(dev+0x28), get, writer, limit,
            MEM32(MEM32(dev+0x2264)+0x44));
}

static uint32_t jsrf_pb_cursor(void);

/* The reserve routine writes its notification into a fixed slot of a segment
 * it has already published (loc_001914A9: [esi+0x10] = 0x00040100,
 * [esi+0x14] = 5), then recomputes the distance and, if the GPU has caught up,
 * overwrites it with a zero NOP at loc_001914CD. So the command is a patch to
 * a location the parser may already have walked past.
 *
 * Whether it has is the whole question, and neither GET nor PUT answers it:
 * the consumed cursor does. A patch behind the cursor is a notification this
 * parser will never see, and the wait that follows it cannot return. */
void jsrf_pb_patch_probe(uint32_t pc, uint32_t patch)
{
    static unsigned n;
    uint32_t slot = patch + 0x10, cursor = jsrf_pb_cursor();
    if (!getenv("RECOMP_PB_NOTIFY_TRACE") || n++ >= 64) return;
    fprintf(stderr, "[PB-PATCH] pc=%08X packet=%08X slot=%08X words=%08X/%08X"
            " cursor=%08X (%s) GET=%08X PUT=%08X\n",
            pc, patch, slot, MEM32(slot), MEM32(slot+4), cursor,
            slot < cursor ? "BEHIND cursor - will never be read"
                          : "ahead of cursor",
            MEM32(0xFD800044u), MEM32(0xFD800040u));
    fflush(stderr);
}

static void jsrf_pb_scan_nops(void);

void jsrf_pb_event_probe(uint32_t pc, uint32_t event)
{
    static unsigned n;
    if (!getenv("RECOMP_PB_NOTIFY_TRACE") || n++ >= 64) return;
    if (n == 1) jsrf_pb_scan_nops();
    fprintf(stderr, "[PB-EVENT] pc=%08X event=%08X state=%08X pmc=%08X pgraph=%08X source=%08X trap=%08X data=%08X\n",
            pc, event, MEM32(event+4), MEM32(0xFD000100u),
            MEM32(0xFD400100u), MEM32(0xFD400108u),
            MEM32(0xFD400704u), MEM32(0xFD400708u));
}

static uint32_t g_pb_last;
static uint32_t g_pb_ring_lo, g_pb_ring_hi;

static uint32_t jsrf_pb_cursor(void) { return g_pb_last; }

/* DMA_PUT/DMA_GET contain physical offsets, but command bytes live in the
 * CPU-visible physical aperture.  This matters on Windows, where that
 * aperture deliberately has separate backing from low guest RAM so a buffer
 * at 0x80001000 cannot overlap the XBE image at 0x00010000.  JSRF enables the
 * shared physical-heap alias on POSIX, so the same translation works there. */
static const uint32_t *jsrf_pb_range(uint32_t physical, uint32_t bytes)
{
    return (const uint32_t *)xbox_GpuMemoryRange(
            0x80000000u | (physical & 0x03FFFFFFu), bytes);
}

static uint32_t jsrf_pb_word(uint32_t physical)
{
    const uint32_t *p = jsrf_pb_range(physical, sizeof(*p));
    return p ? *p : 0;
}

/* The window as it was when the parse of it began. */
static const uint32_t *g_pb_replay;
static uint32_t g_pb_replay_from, g_pb_replay_dwords;

/* Did the ring change underneath the parse, or was it always like this?
 *
 * Re-walk the copy taken at the start of the failing window, dispatching
 * nothing, and compare where the second walk stops with where the live one
 * did. Same offset means the bytes were already that way when the window was
 * taken -- a decoding defect at a specific packet, and the offset names it.
 * A later stop, or none, means something wrote the ring while it was being
 * executed, and that write is the thing to find. */
static void jsrf_pb_replay(const char *why, uint32_t stopped_at)
{
    static int done;
    NV2APusherResult r;
    uint32_t live_off;
    if (done || !g_pb_replay || !g_pb_replay_dwords) return;
    done = 1;
    live_off = (stopped_at - g_pb_replay_from) / 4u;
    r = nv2a_pusher_scan_segment(g_pb_replay, g_pb_replay_dwords);
    fprintf(stderr,
            "[PB-REPLAY] %s: window %08X +%u dwords; live stopped at dword %u"
            " (%08X); replay stop=%d at dword %u (%08X) value %08X vs live %08X"
            " -- %s\n",
            why, g_pb_replay_from, g_pb_replay_dwords, live_off, stopped_at,
            (int)r.stop, r.consumed, g_pb_replay_from + r.consumed * 4u,
            r.consumed < g_pb_replay_dwords ? g_pb_replay[r.consumed] : 0,
            MEM32(g_pb_replay_from + r.consumed * 4u),
            r.consumed == live_off
                ? "SAME offset: the bytes were already like this"
                : "DIFFERENT offset: the ring changed during the parse");
    fflush(stderr);
}

/* Where the software-method commands actually are, the first time the title
 * enters the reserve event wait.
 *
 * The wait returns when something signals device+0x2440, and the only thing
 * that can is the guest's own interrupt path, reached by a nonzero
 * NV097_NO_OPERATION trapping as a PGRAPH software method. So the question is
 * whether that command exists in the ring at all, and whether the cursor has
 * already passed it. A literal scan for the packet header answers both, and
 * distinguishes "the title never submitted it" from "we consumed it and did
 * not act on it" from "it is still ahead of us".
 *
 * 0x00040100 is one increasing dword at method 0x100. Data can coincide with
 * that, so every hit is printed with its parameter rather than counted. */
static void jsrf_pb_scan_nops(void)
{
    uint32_t va, hits = 0, put = MEM32(0xFD800040u);
    if (!g_pb_ring_lo || g_pb_ring_hi <= g_pb_ring_lo) return;
    fprintf(stderr, "[PB-NOPSCAN] ring %08X-%08X cursor %08X put %08X\n",
            g_pb_ring_lo, g_pb_ring_hi, g_pb_last, put);
    for (va = g_pb_ring_lo; va + 4 < g_pb_ring_hi; va += 4) {
        if (MEM32(va) != 0x00040100u) continue;
        if (++hits > 24) break;
        fprintf(stderr, "[PB-NOPSCAN]   %08X parameter %08X (%s cursor)\n",
                va, MEM32(va + 4), va < g_pb_last ? "behind" : "ahead of");
    }
    fprintf(stderr, "[PB-NOPSCAN] %u header(s) matched\n", hits);
    fflush(stderr);
}

/* Read-only render-suppression probe; generated-host-code call sites are
 * temporary observations, never guest state or branch overrides. */
void jsrf_render_probe(uint32_t pc)
{
    static unsigned loops, renders, clocks;
    uint32_t root = MEM32(0x22FCE0u);
    if (pc == 0x13F90u && ++loops > 8 && loops % 120 != 0) return;
    if (pc == 0x13A80u) ++renders;
    if (pc == 0x145560u && ++clocks > 60) return;
    fprintf(stderr, "[RENDER-PROBE] t=%u pc=%08X root=%08X stop=%u "
            "loops=%u renders=%u put=%08X eax=%08X ecx=%08X esi=%08X "
            "esp=%08X ret=%08X args=%08X,%08X,%08X,%08X\n",
            GetTickCount(), pc, root, root ? MEM32(root + 0x24) : 0,
            loops, renders, MEM32(JSRF_PB_PUT_VA), g_eax, g_ecx, g_esi,
            g_esp, MEM32(g_esp), MEM32(g_esp+4), MEM32(g_esp+8),
            MEM32(g_esp+12), MEM32(g_esp+16));
    if (pc == 0x25331u || pc == 0x25366u)
        fprintf(stderr, "[TIMEOUT-PROBE] object=%08X active=%u slot=%u "
                "start=%08X:%08X result=%08X:%08X freq=%08X:%08X\n",
                g_esi, MEM32(g_esi+0x44), MEM32(g_esi+0x48),
                MEM32(g_esi+0x194), MEM32(g_esi+0x190),
                MEM32(g_esp+8), MEM32(g_esp+4),
                MEM32(0x20CC54), MEM32(0x20CC50));
    if (pc == 0x6EE5Cu)
        fprintf(stderr, "[RENDER-PROBE] dialog=%08X flags=%08X text=%.240s\n",
                g_esi, MEM32(g_esi + 0x98),
                MEM32(g_esi + 0xA0) ? (char *)XBOX_PTR(MEM32(g_esi + 0xA0)) : "<null>");
    if (pc == 0x6F450u)
        fprintf(stderr, "[RENDER-PROBE] message=%.240s\n",
                MEM32(g_esp+4) ? (char *)XBOX_PTR(MEM32(g_esp+4)) : "<null>");
    if (pc == 0x12770u || pc == 0x6F730u) {
        for (uint32_t va = g_esp; va < g_esp + 0x300; va += 4) {
            uint32_t value = MEM32(va);
            if (value >= 0x11000 && value < 0x1C4000)
                fprintf(stderr, "[RENDER-STACK] %08X=%08X\n", va, value);
        }
    }
}


/* RECOMP_PB_NOTIFY_TRACE, read once. jsrf_pb_feed runs per pushbuffer window;
 * getenv on that path showed up in a profile of the tutorial. */
static int pb_notify_trace(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PB_NOTIFY_TRACE") != NULL;
    return on;
}

static NV2APusherResult jsrf_pb_feed(uint32_t from, uint32_t to)
{
    NV2APusherResult invalid = {0, 0, 0, NV2A_PUSHER_INVALID};
    if (to <= from) return invalid;
    if (to - from > 0x100000u) return invalid;          /* implausible span */
    /* Execute the ring in place.
     *
     * This ran over a memcpy'd copy, and that loses commands the title patches
     * in after the copy is taken. Its reserve routine does exactly that:
     * loc_001914A9 writes [esi+0x10] = 0x00040100, [esi+0x14] = 5 into a
     * segment it has already published, and loc_001914CD overwrites the same
     * two words again if the GPU has caught up in the meantime. A copy taken
     * between those points executes stale bytes, so the notification never
     * reaches PGRAPH, KeSetEvent is never called on device+0x2440, and the
     * reserve wait at 0x00191510 never returns.
     *
     * Measured, with the check below:
     *   [PB-PATCH-LOST] address=005DF9A4 snapshot!=5 live=5
     *                   feed=005A2588-005ED000
     *
     * Copying was protection against the producer overwriting the ring
     * underneath the parser. That is now prevented at its source instead --
     * DMA_GET is published from the consumed cursor, so the producer cannot
     * lap the parser -- and reading the ring in place is what the hardware
     * does anyway. The copy is kept only for the trace, so the same check
     * still reports a patch that lands while a segment is being executed. */
    const uint32_t *live = jsrf_pb_range(from, to - from);
    if (!live) return invalid;
    int trace = pb_notify_trace();
    static uint32_t snapshot[0x100000/4];
    /* RECOMP_PB_REPLAY keeps a copy of every window as it is taken, purely so
     * that a parse failure can be re-walked against the bytes that were there
     * at the start. See jsrf_pb_replay. */
    if (trace || getenv("RECOMP_PB_REPLAY")) {
        memcpy(snapshot, live, to-from);
        g_pb_replay = snapshot;
        g_pb_replay_from = from;
        g_pb_replay_dwords = (to-from)/4u;
    }
    NV2APusherResult result = nv2a_pusher_run_segment(live, (to-from)/4u);
    if (trace) {
        for (uint32_t k=1; k<result.consumed; ++k) {
            if (jsrf_pb_word(from+(k-1)*4)==0x40100u && snapshot[k]!=5
                    && jsrf_pb_word(from+k*4)==5)
                fprintf(stderr, "[PB-PATCH-RACE] address=%08X copy!=5 live=5 feed=%08X-%08X\n", from+k*4, from, to);
        }
    }
    return result;
}

/* PFIFO's subroutine is one level deep: a register holding the saved DMA_GET
 * and a flag saying it is in use. Kept here rather than in the parser because
 * the parser only ever sees one window of a ring it does not own. */
#define JSRF_PB_SUBR_MAX 0x10000u
/* One consumption step. Comfortably above the 8 KB maximum packet, and small
 * enough that GET is republished often enough to hold the producer back. */
#define JSRF_PB_STEP     0x8000u
static uint32_t g_pb_subr_return;
static int      g_pb_subr_active;
static unsigned long g_pb_calls, g_pb_returns;

/* Did the data simply not arrive yet?
 *
 * The remaining failure reads a float where a packet header belongs, at a
 * cursor the previous poll had consumed to exactly. Two explanations, and they
 * need entirely different fixes: either the producer's ring stores had not
 * become visible to this thread when its store to DMA_PUT did -- a publication
 * ordering problem on a weakly ordered host -- or PUT genuinely points into the
 * middle of a packet, which is a different bug altogether.
 *
 * One observation separates them. Re-read the same dword after a barrier and
 * after two waits. If it turns into a valid header the bytes were merely late,
 * and the fix belongs at the producer's store. If it stays a float, PUT is
 * wrong and ordering has nothing to do with it. PUT is sampled alongside it,
 * because a PUT that moves on tells us the producer was mid-flight. */
static void pb_recheck(const char *why, uint32_t va)
{
    static int done;
    uint32_t v0, v1, v2, v3, p0, p1, p2, p3;
    if (done) return;
    done = 1;

    v0 = jsrf_pb_word(va);     p0 = MEM32(0xFD800040u);
    __sync_synchronize();
    v1 = jsrf_pb_word(va);     p1 = MEM32(0xFD800040u);
    Sleep(1);
    __sync_synchronize();
    v2 = jsrf_pb_word(va);     p2 = MEM32(0xFD800040u);
    Sleep(50);
    __sync_synchronize();
    v3 = jsrf_pb_word(va);     p3 = MEM32(0xFD800040u);

    fprintf(stderr,
            "[PUSHER] recheck %s at %08X: now=%08X barrier=%08X +1ms=%08X"
            " +50ms=%08X | PUT %08X/%08X/%08X/%08X\n",
            why, va, v0, v1, v2, v3, p0, p1, p2, p3);

    /* How far out of step is the cursor?
     *
     * Scan back for a dword that decodes as a method header whose packet ends
     * exactly here. If one exists a few dwords back, the parse consumed that
     * packet's payload as if it were shorter or longer than it is, and the
     * distance names the packet. If none exists within a wide window, the
     * cursor is not merely off by a packet and the misalignment came from
     * somewhere else entirely. The guest's own copy of PUT is printed beside
     * the register, because the two disagreeing would explain it outright. */
    {
        uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
        int found = 0;
        fprintf(stderr, "[PUSHER]   guest PUT copy %08X vs register %08X\n",
                dev ? MEM32(dev + 0x2C) : 0, p0);
        for (int back = 1; back <= 512 && !found; ++back) {
            uint32_t h = jsrf_pb_word(va - back * 4u);
            uint32_t masked = h & 0xE0030003u;
            uint32_t count;
            if (masked != 0u && masked != 0x40000000u) continue;
            count = (h >> 18) & 0x7FFu;
            if ((int)(1u + count) != back) continue;
            fprintf(stderr, "[PUSHER]   last aligned header %08X is %d dwords"
                    " back: %s method %04X count %u ends exactly here\n",
                    h, back, masked ? "non-inc" : "inc", h & 0x1FFCu, count);
            found = 1;
        }
        if (!found)
            fprintf(stderr, "[PUSHER]   no header within 512 dwords ends at"
                    " this cursor\n");
    }
    fflush(stderr);
}

/* The last few segment decisions, for the desync post-mortem below. */
static struct { uint32_t from, end, put, stop, consumed; } g_pb_hist[16];
static unsigned g_pb_hist_n;

static int jsrf_pb_poll(void)
{
    static int stream_fault;
    if (stream_fault) return 0;
    /* sub_001912EC publishes completed packets to the DMA channel. The
     * device's +0 writer cursor can point past a header still being filled. */
    uint32_t now = MEM32(0xFD800040u);
    if (!now) return 0;

    if (!g_pb_last) {
        /* Device bounds are CPU addresses; DMA_PUT and parser cursors are
         * physical offsets into the shared low backing. */
        g_pb_ring_lo = MEM32(JSRF_PB_START_VA) & 0x03FFFFFFu;
        g_pb_ring_hi = MEM32(JSRF_PB_END_VA) & 0x03FFFFFFu;
        /* Only trust the ring bounds if the cursor actually sits inside them;
         * otherwise the +0x24/+0x28 fields are not what the BO3 map says for
         * this title and a wrap has to be skipped rather than mis-parsed. */
        if (!(g_pb_ring_lo < g_pb_ring_hi && g_pb_ring_lo <= now
              && now <= g_pb_ring_hi)) {
            g_pb_ring_lo = g_pb_ring_hi = 0;
        }
        fprintf(stderr, "  [PUSHER] ring cursor 0x%08X limit 0x%08X "
                "bounds 0x%08X-0x%08X%s\n",
                now, MEM32(JSRF_PB_LIMIT_VA), g_pb_ring_lo, g_pb_ring_hi,
                g_pb_ring_lo ? "" : " (bounds rejected; wraps skipped)");
        /* The same question read the way docs/technical/d3d-translation.md
         * maps the D3D8LTCG device: base at +0x08 and size at +0x0C, against
         * the +0x24/+0x28 pair above that the BO3 map suggested. Printed side
         * by side rather than swapped, because every statement made today
         * about where the ring lives rests on which of these is right, and
         * they have never been compared on one line. */
        fprintf(stderr, "  [PUSHER] device fields: +0x00=%08X +0x04=%08X"
                " +0x08=%08X +0x0C=%08X | +0x24=%08X +0x28=%08X\n",
                MEM32(JSRF_PB_PUT_VA), MEM32(JSRF_PB_LIMIT_VA),
                MEM32(0x0019B208u), MEM32(0x0019B20Cu),
                MEM32(JSRF_PB_START_VA), MEM32(JSRF_PB_END_VA));
        fflush(stderr);
        g_pb_last = g_pb_ring_lo ? g_pb_ring_lo : now;
    }

    /* Every backwards move of PUT, classified.
     *
     * A ring wraps by ending its written span with a JUMP and republishing PUT
     * at the base; that is normal and happens several times a second. PUT
     * arriving below the cursor at some *other* address is not, and it is the
     * poll on which the parse desynchronises -- once per run, one bad header in
     * six and a half million dwords.
     *
     * Three things separate the candidate causes. Re-reading the register twice
     * says whether the value is stable or is changing under us. The guest's own
     * copy of PUT, which sub_001912EC writes to device+0x2C as a virtual
     * address just before it writes the register, says whether the two agree.
     * And the count of healthy base wraps says how normal this ring's wrapping
     * has been up to that point. */
    {
        static uint32_t prev_now;
        static unsigned long to_base, to_other;
        if (prev_now && now < prev_now) {
            if (g_pb_ring_lo && now == g_pb_ring_lo) {
                ++to_base;
            } else if (++to_other <= 8) {
                uint32_t again = MEM32(0xFD800040u);
                uint32_t again2 = MEM32(0xFD800040u);
                uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
                fprintf(stderr,
                        "[PUSHER] PUT backwards to a non-base address #%lu:"
                        " %08X -> %08X (cursor %08X ring %08X-%08X)"
                        " re-read %08X/%08X guest-copy %08X;"
                        " healthy base wraps so far %lu\n",
                        to_other, prev_now, now, g_pb_last,
                        g_pb_ring_lo, g_pb_ring_hi, again, again2,
                        dev ? MEM32(dev + 0x2C) : 0, to_base);
                fflush(stderr);
            }
        }
        prev_now = now;
    }

    /* A published cursor can split a packet. Advance only by the dwords
     * actually consumed, and follow the ring's jump instead of parsing its
     * unused tail as commands. Bounds come from the title's live device. */
    for (unsigned segment=0; (g_pb_last!=now || g_pb_subr_active) && segment<16; ++segment) {
        if (!g_pb_subr_active && g_pb_ring_lo
                && (now<g_pb_ring_lo || now>g_pb_ring_hi)) break;
        /* Inside a subroutine the cursor is in a buffer the ring bounds and
         * PUT say nothing about, so neither can end the window; only the
         * RETURN does. Cap it so a target that is not really a push buffer
         * cannot walk guest memory. */
        uint32_t end = g_pb_subr_active ? g_pb_last + JSRF_PB_SUBR_MAX
                     : (now>g_pb_last ? now : g_pb_ring_hi);
        /* Consume in bounded steps so GET can be published between them.
         *
         * GET is the producer's back-pressure and it was only published once
         * the whole poll finished. A poll can cover most of the ring -- 26,920
         * dwords in the run that caught this -- and for all of that time the
         * producer sees a GET that has not moved, so it is free to fill the
         * ring and write over the very bytes being parsed. Measured with
         * RECOMP_PB_REPLAY: the copy of the failing window taken at its start
         * parses cleanly to the end, while the live parse died 1,893 dwords in.
         * The ring changed underneath it.
         *
         * A step must exceed the largest legal packet -- 0x7FF dwords of
         * payload, so 8 KB -- or a packet could never fit in one. */
        if (!g_pb_subr_active && end - g_pb_last > JSRF_PB_STEP)
            end = g_pb_last + JSRF_PB_STEP;
        if (!end || end<=g_pb_last) break;
        NV2APusherResult result = jsrf_pb_feed(g_pb_last, end);
        /* Every segment decision, kept in a ring and dumped when the parse
         * finally lands on a data dword. The stop reason and the dwords
         * consumed are the only things that say WHERE sync was lost; the
         * invalid header itself is several segments downstream of the cause. */
        g_pb_hist[g_pb_hist_n & 15].from = g_pb_last;
        g_pb_hist[g_pb_hist_n & 15].end = end;
        g_pb_hist[g_pb_hist_n & 15].put = now;
        g_pb_hist[g_pb_hist_n & 15].stop = (uint32_t)result.stop;
        g_pb_hist[g_pb_hist_n & 15].consumed = result.consumed;
        g_pb_hist_n++;
        /* END means the parser reached the end of the window, so it must have
         * consumed all of it. A short END would silently skip the tail of the
         * window and resume mid-packet next time, which is exactly the shape of
         * the desync being hunted -- so check it rather than assume it. */
        if (result.stop==NV2A_PUSHER_END
                && result.consumed*4u != end - g_pb_last) {
            static unsigned short_ends;
            if (++short_ends <= 4)
                fprintf(stderr,"[PUSHER] short END: window %08X-%08X is %u"
                        " dwords, consumed %u\n",
                        g_pb_last, end, (end-g_pb_last)/4u, result.consumed);
            fflush(stderr);
        }
        g_pb_last += result.consumed*4;
        /* Republish progress immediately: this is the whole point of stepping. */
        if (g_pb_ring_lo && !g_pb_subr_active)
            MEM32(0xFD800044u) = g_pb_last & 0x03FFFFFFu;
        if (result.stop==NV2A_PUSHER_CALL) {
            /* A target has to be a plausible push-buffer address before the
             * cursor follows it anywhere. Without this the parse, having
             * already lost sync, read a data dword as a call to 0x00000004 and
             * walked low guest memory until it hit the XBE header magic. The
             * jump path has always checked its target; this is the same check,
             * not a recovery. */
            if (!result.jump_address || (result.jump_address & 3u)
                    || result.jump_address >= 0x08000000u) {
                fprintf(stderr,"[PUSHER] rejected CALL target %08X at %08X\n",
                        result.jump_address, g_pb_last-4);
                stream_fault=1;
                break;
            }
            /* Hardware saves DMA_GET as it stands after the call word, which
             * is exactly the cursor the parser has just left us. */
            if (g_pb_subr_active) {
                fprintf(stderr,"[PUSHER] nested CALL %08X at %08X;"
                        " PFIFO's subroutine is one deep\n",
                        result.jump_address, g_pb_last-4);
                stream_fault=1;
                break;
            }
            g_pb_subr_return = g_pb_last;
            g_pb_subr_active = 1;
            if (++g_pb_calls <= 4)
                fprintf(stderr,"[PUSHER] CALL #%lu at %08X -> target %08X,"
                        " saved return %08X\n",
                        g_pb_calls, g_pb_last-4, result.jump_address,
                        g_pb_subr_return);
            g_pb_last = result.jump_address;
            continue;
        }
        if (result.stop==NV2A_PUSHER_RETURN) {
            if (!g_pb_subr_active) {
                fprintf(stderr,"[PUSHER] RETURN at %08X with no active"
                        " subroutine\n", g_pb_last-4);
                stream_fault=1;
                break;
            }
            if (++g_pb_returns <= 4)
                fprintf(stderr,"[PUSHER] RETURN #%lu at %08X -> restored %08X\n",
                        g_pb_returns, g_pb_last-4, g_pb_subr_return);
            g_pb_last = g_pb_subr_return;
            g_pb_subr_active = 0;
            continue;
        }
        if (result.stop==NV2A_PUSHER_JUMP) {
            if (g_pb_ring_lo && result.jump_address>=g_pb_ring_lo
                    && result.jump_address<g_pb_ring_hi && !(result.jump_address&3)
                    && result.jump_address!=g_pb_last-4) {
                g_pb_last=result.jump_address;
                continue;
            }
            stream_fault=1;
            fprintf(stderr,"[PUSHER] rejected jump %08X at %08X\n",result.jump_address,g_pb_last-4);
            pb_recheck("rejected jump", g_pb_last-4);
            jsrf_pb_replay("rejected jump", g_pb_last-4);
            /* Same post-mortem as the invalid-header path: a jump target
             * outside the ring means the parse is reading data, not commands,
             * and where sync was lost is several segments upstream. */
            fprintf(stderr,"[PUSHER]   ring:");
            for (int k=-8;k<=8;++k)
                fprintf(stderr," %s%08X", k ? "" : ">",
                        jsrf_pb_word(g_pb_last - 4 + k * 4));
            fprintf(stderr,"\n");
            for (unsigned k = g_pb_hist_n>16?g_pb_hist_n-16:0; k<g_pb_hist_n; ++k)
                fprintf(stderr,"[PUSHER]   seg %u: from=%08X end=%08X"
                        " put=%08X stop=%u consumed=%u\n", k,
                        g_pb_hist[k&15].from, g_pb_hist[k&15].end,
                        g_pb_hist[k&15].put, g_pb_hist[k&15].stop,
                        g_pb_hist[k&15].consumed);
            fflush(stderr);
            break;
        }
        if (result.stop==NV2A_PUSHER_INVALID) {
            stream_fault=1;
            static unsigned errors;
            if (++errors<=4) {
                fprintf(stderr,"[PUSHER] stopped on invalid header %08X at %08X\n",
                        jsrf_pb_word(g_pb_last), g_pb_last);
                pb_recheck("invalid header", g_pb_last);
                jsrf_pb_replay("invalid header", g_pb_last);
                /* Whether the parser is at a packet boundary or has lost sync
                 * decides everything: a real NV2A call is a feature to add, a
                 * desynchronised cursor is a bug to fix. Print the ring either
                 * side of the cursor, and the head of the would-be call
                 * target, which should itself look like pushbuffer. */
                fprintf(stderr,"[PUSHER]   ring:");
                for (int k=-8;k<=8;++k)
                    fprintf(stderr," %s%08X", k ? "" : ">",
                            jsrf_pb_word(g_pb_last + k * 4));
                fprintf(stderr,"\n");
                uint32_t tgt = jsrf_pb_word(g_pb_last) & 0xFFFFFFFCu;
                fprintf(stderr,"[PUSHER]   target %08X:",tgt);
                for (int k=0;k<8;++k)
                    fprintf(stderr," %08X", jsrf_pb_word(tgt + k * 4));
                fprintf(stderr,"\n");
                for (unsigned k = g_pb_hist_n>16?g_pb_hist_n-16:0; k<g_pb_hist_n; ++k)
                    fprintf(stderr,"[PUSHER]   seg %u: from=%08X end=%08X"
                            " put=%08X stop=%u consumed=%u\n", k,
                            g_pb_hist[k&15].from, g_pb_hist[k&15].end,
                            g_pb_hist[k&15].put, g_pb_hist[k&15].stop,
                            g_pb_hist[k&15].consumed);
                fflush(stderr);
            }
        }
        /* Stop at a frame boundary.
         *
         * FLIP_STALL is the guest saying "this frame is finished". Consumption
         * runs in bounded steps on this thread and presentation happens after
         * the poll, so without this the parser walks straight on into the next
         * frame's clear and the surface presented is blank. Measured: of 46
         * frames captured at the flip, 43 were blank and only one carried the
         * composed picture, while captures taken mid-draw were full of content.
         * A frame boundary is a boundary; stop on it. */
        if (pgraph_d3d11_frame_pending()) break;
        if (result.stop!=NV2A_PUSHER_END || !result.consumed) break;
        if (g_pb_last==g_pb_ring_hi && now<g_pb_last) g_pb_last=g_pb_ring_lo;
    }

    /* Why the feed stopped catching up, once.
     *
     * The GET index below is only acknowledged when this returns having fully
     * consumed the ring, so any reason for not catching up is self-sustaining:
     * the guest waits for GET to move, and GET does not move until the guest
     * publishes the rest. Distinguishing "the guest stopped publishing" from
     * "we refused to consume" is the whole question, and the counters cannot:
     * they only say everything stopped at once.
     */
    {
        static uint32_t last_now, last_pb;
        static unsigned long stuck;
        static int reported;
        if (g_pb_last == now) {
            stuck = 0;
        } else if (now == last_now && g_pb_last == last_pb) {
            if (++stuck == 2000 && !reported) {
                reported = 1;
                fprintf(stderr, "[PUSHER] not catching up for %lu polls:"
                        " put=%08X cursor=%08X ring=%08X-%08X fault=%d\n",
                        stuck, now, g_pb_last, g_pb_ring_lo, g_pb_ring_hi,
                        stream_fault);
                NV2APusherResult r = jsrf_pb_feed(g_pb_last,
                        now > g_pb_last ? now : g_pb_ring_hi);
                fprintf(stderr, "[PUSHER]   replay: stop=%d consumed=%u"
                        " jump=%08X; head", (int)r.stop, r.consumed,
                        r.jump_address);
                for (int k = 0; k < 8; ++k)
                    fprintf(stderr, " %08X", jsrf_pb_word(g_pb_last + k * 4));
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        } else {
            stuck = 0;
        }
        last_now = now;
        last_pb = g_pb_last;
    }

    /* Publish what has actually been consumed.
     *
     * DMA_GET is the ring's back-pressure: the producer reads it to know how
     * much of the buffer it may reuse. The periodic acknowledgement used to
     * copy PUT into it, which told the title the ring was free the moment it
     * submitted, so it lapped this parser and overwrote commands mid-read --
     * measured as exactly one bad header per run, at a different address every
     * time. The cursor is the truth, and the register holds the physical form
     * of it, the same masking sub_001912EC applies when it writes PUT. */
    if (g_pb_ring_lo)
        MEM32(0xFD800044u) = g_pb_last & 0x03FFFFFFu;

    /* The guest's FLIP_STALL is the only "frame is complete" signal in the
     * ring. Present here, on the thread holding the rendering context --
     * pgraph deliberately does not do it itself. */
    if (pgraph_d3d11_take_frame()) {
        static unsigned long presented;
        pgraph_d3d11_flush();
        d3d8_PresentFrame();
        /* RECOMP_FB_DUMP_FLIP=<stride>: capture finished frames, at the only
         * moment a frame is finished. Cadence is the question this answers:
         * whether consecutive presents carry different pictures, not merely
         * whether the surface is non-blank.
         *
         * Two files per capture, and the difference between them is the
         * measurement: flipNNN is the live surface as it stands here, snapNNN
         * the copy taken at FLIP_STALL that the window is actually fed. The
         * parser finishes its bounded step between the two, so they need not
         * agree, and only the second is a picture anyone saw. */
        {
            extern void nv2a_pb_exec_dump_surface(void);
            static long stride = -1;
            static unsigned captured;
            if (stride < 0) {
                const char *e = getenv("RECOMP_FB_DUMP_FLIP");
                stride = e && *e ? strtol(e, NULL, 0) : 0;
                if (stride < 1) stride = 1;
            }
            if (getenv("RECOMP_FB_DUMP_FLIP") && captured < 24
                    && (presented % (unsigned long)stride) == 0) {
                extern uint32_t nv2a_pb_exec_triangles(void);
                static uint32_t last_tris;
                uint32_t tris = nv2a_pb_exec_triangles();
                fprintf(stderr, "  [FLIP] capture %u at frame %lu:"
                        " %u triangles since the previous capture\n",
                        captured, presented, tris - last_tris);
                last_tris = tris;
                captured++;
                nv2a_pb_exec_dump_surface();
            }
        }
        if (++presented <= 3 || (presented % 300) == 0) {
            fprintf(stderr, "  [PUSHER] presented frame %lu\n", presented);
            fflush(stderr);
        }
    }
    return !stream_fault && g_pb_last==now;
}

/* The index pair the guest's own wait loop watches: [dev+0x30] is PUT and
 * [[dev+0x34]] is GET. Distinct from the ring cursor at device +0x00 -- these
 * are counters, not addresses, and this is what the guest blocks on. */
/* RECOMP_PB_PUT_WATCH -- every distinct value the guest's push-buffer cursor
 * takes, from before the guest runs.
 *
 * The Windows oracle freezes with put=0x800109A4, which resolves through the
 * 0x80000000 contiguous window to physical 0x109A4 -- inside the loaded XBE
 * image, where macOS sits in the heap. That is either the cause (the ring was
 * never allocated, so the pusher parses the image as commands, stops on a bad
 * header, and GET never advances past what the guest is waiting for) or a
 * consequence (the guest stalled elsewhere and stopped submitting). The two
 * are told apart by ONE fact: whether PUT ever held a sane heap address here.
 *
 * Sampled rather than trapped. A write trap on a guest global is the heavy
 * instrument this tree has already shown perturbs the title; a poller is
 * read-only and cannot. It can miss a transition it never samples, so it
 * reports the count of samples alongside the values -- a value seen once in
 * tens of thousands of samples is a real transition, and "only ever one value"
 * is only evidence at a sample count that makes a miss implausible. */
static DWORD WINAPI jsrf_pb_put_watch(LPVOID unused)
{
    uint32_t seen_put[16], seen_lim[16];
    unsigned n = 0, i;
    unsigned long samples = 0;

    (void)unused;
    for (;;) {
        uint32_t put = MEM32(JSRF_PB_PUT_VA);
        uint32_t lim = MEM32(JSRF_PB_LIMIT_VA);
        samples++;
        for (i = 0; i < n; i++)
            if (seen_put[i] == put && seen_lim[i] == lim) break;
        if (i == n && n < 16u) {
            seen_put[n] = put; seen_lim[n] = lim; n++;
            fprintf(stderr, "  [PB-PUT] #%u after %lu samples: put=0x%08X"
                    " limit=0x%08X  (put -> phys 0x%08X, %s)\n",
                    n, samples, put, lim, put - 0x80000000u,
                    (put - 0x80000000u) < 0x00290000u ? "INSIDE THE XBE IMAGE"
                  : (put - 0x80000000u) < 0x00510000u ? "below the heap"
                                                      : "heap");
            fflush(stderr);
        }
        Sleep(1);
    }
}

static void jsrf_pb_put_watch_start(void)
{
    HANDLE th;
    if (!getenv("RECOMP_PB_PUT_WATCH")) return;
    th = CreateThread(NULL, 0, jsrf_pb_put_watch, NULL, 0, NULL);
    if (th) CloseHandle(th);
}

static uint32_t jsrf_pb_index(uint32_t which)
{
    uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
    uint32_t v;
    if (!dev) return 0;
    v = MEM32(dev + which);
    if (which == JSRF_D3D_GETPTR_OFFSET) {
        return v ? MEM32(v) : 0;
    }
    return v;
}

/* One-shot: locate live instances of the renderer class, and who points at
 * them.
 *
 * The class was found by scanning the image for a stored pointer to its render
 * entry: a 121-slot vtable at 0x001E0F00, constructor sub_0014CDB0, slot 10
 * the render entry. Neither that slot nor the guarded slot 6 has a static
 * caller, so the call graph cannot say who drives the object -- and JSRF ships
 * with RTTI disabled (4 type descriptors, 0 vtables), so class recovery cannot
 * either.
 *
 * What is left is the object itself. An instance carries the vtable address in
 * its first word, so scanning guest RAM for 0x001E0F00 finds every live one;
 * scanning again for pointers to those finds the global or structure field
 * that owns it. That names the thing the tick would have to consult.
 */
#define JSRF_RENDERER_VTABLE 0x001E0F00u
/* The 26-slot class whose slot 23 (+0x5C) holds sub_001596B0, the
 * render dispatcher. If no instance of this exists, the object that
 * would drive rendering was never built -- which is upstream of every
 * other question about why nothing draws. */
#define JSRF_DISPATCH_VTABLE 0x001E1640u

/* Two different ranges, because the two scans want different things.
 *
 * INSTANCES live on the heap. Scanning the image for them finds the vtable
 * address as an IMMEDIATE inside the constructor that installs it -- the first
 * attempt reported an "instance" at 0x00159DF0, which is .text.
 *
 * REFERENCES are the opposite: the global that owns an object is usually in
 * .data, which is inside the image. Restricting this scan to the heap as well
 * lost every reference to the renderer -- it reported 0, when three globals at
 * 0x00251D6C / 0x002650D8 / 0x00265110 point straight at it. Start at .rdata,
 * which is past the last code section (XPP ends at 0x001C3F58). */
#define JSRF_HEAP_SCAN_LO 0x00290000u
#define JSRF_HEAP_SCAN_HI 0x02000000u
#define JSRF_REF_SCAN_LO  0x001C3F60u

static void jsrf_find_renderer(void)
{
    static int done;
    uint32_t found[8];
    int n = 0;
    uint32_t va;

    if (done) return;
    done = 1;

    /* Guest RAM only; the scan is a diagnostic, so keep it to the mapped
     * image and heap rather than probing apertures that fault. */
    {   /* The dispatcher's class first: it is the more decisive of the two.
         *
         * Skip the image itself. A vtable address also appears as an IMMEDIATE
         * inside the constructor that installs it, so scanning code reports
         * matches at code addresses -- 0x00159DF0 the first time this ran,
         * which is .text, not an object. An instance lives on the heap.
         *
         * Built into one line and written once: worker threads log
         * concurrently and a per-entry fprintf interleaves with them, which is
         * how the first attempt produced a half-finished line with an ADX tick
         * spliced through it. */
        char line[320];
        int off = 0, d = 0, refs = 0;
        uint32_t first = 0;
        for (va = JSRF_HEAP_SCAN_LO; va < JSRF_HEAP_SCAN_HI; va += 4) {
            if (MEM32(va) == JSRF_DISPATCH_VTABLE) {
                if (!d) first = va;
                d++;
            }
        }
        off = snprintf(line, sizeof(line),
                       "  [RENDERER] DISPATCHER class 0x%08X: %d live instance(s)",
                       JSRF_DISPATCH_VTABLE, d);
        if (d) {
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            ", first 0x%08X, referenced from:", first);
            for (va = JSRF_REF_SCAN_LO; va < JSRF_HEAP_SCAN_HI; va += 4) {
                if (MEM32(va) == first && va != first) {
                    refs++;
                    if (refs <= 6 && off > 0 && off < (int)sizeof(line) - 16) {
                        off += snprintf(line + off, sizeof(line) - (size_t)off,
                                        " 0x%08X", va);
                    }
                }
            }
            snprintf(line + off, sizeof(line) - (size_t)off, "  (%d total)", refs);
        }
        fprintf(stderr, "%s\n", line);
    }

    for (va = JSRF_HEAP_SCAN_LO; va < JSRF_HEAP_SCAN_HI; va += 4) {
        if (MEM32(va) == JSRF_RENDERER_VTABLE) {
            if (n < 8) found[n] = va;
            n++;
        }
    }
    fprintf(stderr, "  [RENDERER] %d live instance(s) of vtable 0x%08X\n",
            n, JSRF_RENDERER_VTABLE);

    for (int i = 0; i < n && i < 8; i++) {
        uint32_t obj = found[i];
        int refs = 0;
        char line[256];
        int off = snprintf(line, sizeof(line),
                           "  [RENDERER]   instance 0x%08X referenced from:", obj);
        for (va = JSRF_REF_SCAN_LO; va < JSRF_HEAP_SCAN_HI; va += 4) {
            if (MEM32(va) == obj && va != obj) {
                refs++;
                if (off > 0 && off < (int)sizeof(line) - 16 && refs <= 8) {
                    off += snprintf(line + off, sizeof(line) - (size_t)off,
                                    " 0x%08X", va);
                }
            }
        }
        fprintf(stderr, "%s  (%d total)\n", line, refs);
    }
    fflush(stderr);
}

/* Periodic pusher report. Separate from the ADX tick so it survives that
 * probe being removed. */
static void jsrf_pusher_report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    NV2APusherStats st;

    /* RECOMP_FB_DUMP writes one BMP per report, so the report interval is also
     * the framebuffer sampling interval. Five seconds is right for a title that
     * sits on one screen, and far too coarse for a startup sequence that steps
     * through several -- the SEGA logo is on screen for about as long as the
     * gap between two samples. Overridable rather than lowered outright: every
     * report also prints several hundred bytes of counters. */
    static DWORD interval;
    if (interval == 0) {
        const char *ms = getenv("RECOMP_REPORT_MS");
        long v = ms ? strtol(ms, NULL, 10) : 0;
        interval = (v >= 100 && v <= 600000) ? (DWORD)v : 5000;
    }
    if (last == 0) { last = now; return; }
    if (now - last < interval) return;
    last = now;

    jsrf_find_renderer();

    {   /* Was the render chain ever dispatched?
         *
         * Both links are vtable-only, so this is the one instrument that can
         * answer it. Read directly rather than through the dump file: the
         * question is about two specific addresses, and a dump has to be
         * merged by a separate tool before it says anything.
         *
         * Dumped PERIODICALLY, never from atexit. Upstream found the atexit
         * dump is dead code for a title that does not exit cleanly, and every
         * run here ends in kill -9, so the same applies for a different
         * reason. An empty dump would read as "no indirect calls observed",
         * which is the false conclusion this effort has already drawn twice
         * from a missing instrument. */
        static int said;
        uint32_t disp = 0x001596B0u - RECOMP_ICALL_FB_BASE;
        uint32_t rend = 0x00155050u - RECOMP_ICALL_FB_BASE;
        unsigned d = (disp < RECOMP_ICALL_FB_SIZE) ? g_icall_seen[disp] : 0;
        unsigned r = (rend < RECOMP_ICALL_FB_SIZE) ? g_icall_seen[rend] : 0;
        if (!said || d || r) {
            said = 1;
            fprintf(stderr, "  [ICALL-FB] dispatcher sub_001596B0=%s  "
                    "render sub_00155050=%s\n",
                    d ? (d & RECOMP_ICALL_SEEN_RESOLVED ? "DISPATCHED"
                                                        : "seen-unresolved")
                      : "never",
                    r ? (r & RECOMP_ICALL_SEEN_RESOLVED ? "DISPATCHED"
                                                        : "seen-unresolved")
                      : "never");
            fflush(stderr);
        }
        RECOMP_ICALL_FEEDBACK_DUMP();
        if (getenv("RECOMP_PB_EXEC")) nv2a_pb_exec_report();
        /* Voice lifecycle: separates "no voice ever started" from "voices
         * start and never retire", which is what decides whether a silent run
         * is the guest's fault or the APU's. */
        {
            extern void mcpx_apu_voice_report(void);
            mcpx_apu_voice_report();
        }
        /* And how often the sound engine was gated off. Voices starting says
         * nothing about whether they were processed: se_frame is skipped
         * whenever the front end is not free-running, so a title that submits
         * voices perfectly can still be inaudible if the gate is shut. */
        {
            extern void mcpx_apu_frame_report(void);
            mcpx_apu_frame_report();
            /* Which APU registers the guest reads. Opt-in, and silent unless
             * RECOMP_APU_READ_TRACE is set -- see mcpx_apu_read_report. */
            extern void mcpx_apu_read_report(void);
            mcpx_apu_read_report();
            extern void xbox_VblankReport(void);
            xbox_VblankReport();
            /* And whether the code all of the above is measuring is still the
             * code we loaded. First call takes the baseline, so this is also
             * the startup arm. */
            {
                extern void xbox_TextChecksumReport(void);
                xbox_TextChecksumReport();
            }
            /* And where the two D3D contiguous allocators stopped. Silent
             * unless RECOMP_D3D_ALLOC_TRACE is set and the sites are armed. */
            jsrf_d3d_alloc_report();
            /* And whether DirectSound is still answering at all. One latch at
             * 0x001BA04C turns every DSOUND entry point into an E_FAIL stub,
             * and the counters that say so are only evidence if they are
             * printed when they are zero. Silent unless RECOMP_DSOUND_FATAL
             * is set and the sites are armed. */
            jsrf_dsound_fatal_report();
            /* And the CRI DirectSound driver above it, whose own diagnostics
             * have never had anywhere to go. */
            jsrf_cri_dsound_report();
            jsrf_cri_server_report();
        }
        /* And the boundary those voices have to cross. The APU aperture is
         * guarded read-only so stores fault and reach the model; anything the
         * guard was not covering at the moment of the store lands in plain
         * memory and is lost. This says how much of each is happening. */
        {
            extern void xbox_McpxTrapReport(void);
            xbox_McpxTrapReport();
        }
        /* And whether the samples those voices produce are leaving at the
         * rate the device consumes them. gen_hz against 48000 is the whole
         * question; the queue depth and clear count say what it costs. */
        {
            extern void mcpx_apu_pacing_report(void);
            mcpx_apu_pacing_report();
        }
        /* Whether the pad is being asked, and whether it answers. The
         * "(opened)" line at startup answers neither. */
        {
            extern void xbox_InputPollReport(void);
            xbox_InputPollReport();
        }
        jsrf_guest_trace_report();
        pad_sentinel_scan();
        jsrf_seq_trace_start();
        jsrf_save_dump();
        jsrf_scene_report();
        jsrf_object_dump();
        jsrf_func_hit_report();
        /* The allocator prints its owner breakdown once, when a request
         * fails. That names who holds the heap at the end and says nothing
         * about how it got there -- a working set that plateaus and a leak
         * that climbs look identical in a single sample. */
        if (getenv("RECOMP_HEAP_REPORT")) xbox_HeapReport("periodic");
        /* How much of the 8 MB stack region the title has ever touched.
         *
         * The arena starts where that region ends, so every byte reserved for
         * a stack nobody uses is a byte the heap does not have. The mapping is
         * zero-filled at init and the main stack grows down from the top, so
         * the lowest non-zero word is the high-water mark. Worker slices, if a
         * title used them, sit at the bottom -- reported separately so the two
         * are not confused. */
        if (getenv("RECOMP_STACK_REPORT")) {
            uint32_t lo = 0, worker = 0, a;
            for (a = XBOX_STACK_BASE; a < XBOX_HEAP_BASE; a += 4)
                if (MEM32(a)) { lo = a; break; }
            for (a = XBOX_WORKER_STACK_END; a < XBOX_HEAP_BASE; a += 4)
                if (MEM32(a)) { worker = a; break; }
            fprintf(stderr, "  [STACK] region 0x%08X-0x%08X (%u KB):"
                    " lowest touched 0x%08X, main-stack depth %u KB,"
                    " untouched below it %u KB (worker slices %s)\n",
                    (unsigned)XBOX_STACK_BASE, (unsigned)XBOX_HEAP_BASE,
                    (unsigned)(XBOX_STACK_SIZE / 1024u),
                    lo, lo ? ((unsigned)XBOX_HEAP_BASE - lo) / 1024u : 0u,
                    lo ? (lo - (unsigned)XBOX_STACK_BASE) / 1024u
                       : (unsigned)(XBOX_STACK_SIZE / 1024u),
                    (lo && lo < (uint32_t)XBOX_WORKER_STACK_END) ? "in use"
                                                                 : "untouched");
            (void)worker;
            fflush(stderr);
        }
    }

    nv2a_pusher_get_stats(&st);
    {
        PgraphD3D11Stats ps;
        pgraph_d3d11_get_stats(&ps);
        fprintf(stderr, "  [PUSHER] runs=%lu dwords=%lu methods=%lu "
                "unhandled=%lu bad_headers=%lu | clears=%u flips=%u draws=%u"
                " | put=0x%08X limit=0x%08X idx put=%u get=%u\n",
                st.runs, st.dwords, st.methods, st.unhandled, st.bad_headers,
                ps.clears, ps.flips, ps.draw_calls,
                MEM32(JSRF_PB_PUT_VA), MEM32(JSRF_PB_LIMIT_VA),
                jsrf_pb_index(JSRF_D3D_PUT_OFFSET),
                jsrf_pb_index(JSRF_D3D_GETPTR_OFFSET));
    }
    fflush(stderr);
    nv2a_pusher_dump_unhandled(20);
    {   /* Submission has gone quiet: show what it did last, once. */
        static unsigned long prev_methods;
        static int shown;
        if (st.methods == prev_methods && st.methods && !shown) {
            shown = 1;
            nv2a_pusher_dump_recent(48);
        }
        prev_methods = st.methods;
    }
}

static void jsrf_software_method(uint32_t subchannel, uint32_t parameter)
{
    static unsigned n;
    DWORD start = GetTickCount();
    int raised = xbox_Nv2aRaiseSoftwareMethod(subchannel, parameter);
    if (++n <= 16 || parameter == 5)
        fprintf(stderr, "[PB-NOTIFY] #%u parameter=%u raised=%d\n", n, parameter, raised);
    /* The parser must not execute a later software method until this one is
     * acknowledged. Preserve a missing delivery as a visible stop. */
    while ((!raised || xbox_Nv2aSoftwareMethodPending()) && !g_pushbuf_ack_stop) {
        if (GetTickCount() - start >= 2000) {
            fprintf(stderr, "[PB-NOTIFY] waiting parameter=%u raised=%d pmc=%08X intr=%08X fifo=%08X\n",
                    parameter, raised, MEM32(0xFD000100u), MEM32(0xFD400100u), MEM32(0xFD400720u));
            {
                extern void xbox_PgraphIrqReport(void);
                xbox_PgraphIrqReport();
            }
            start = GetTickCount();
        }
        Sleep(0);
    }
    if (n <= 16 || parameter == 5)
        fprintf(stderr, "[PB-NOTIFY] completed parameter=%u\n", parameter);
}

static DWORD WINAPI jsrf_pushbuffer_ack(LPVOID unused)
{
    (void)unused;
    /* This thread issues the PGRAPH draws, so it must own the GL context. */
    xbox_d3d8_make_current();
    /* From here on GET means "consumed", not "submitted". */
    g_nv2a_pusher_owns_dma_get = 1;
    /* Lend the executor the ring's recent-method dump, so RECOMP_FLIP_TRACE
     * can print the command order that led up to a flip. */
    {
        extern void nv2a_pb_exec_set_recent_dump(void (*)(int));
        nv2a_pb_exec_set_recent_dump(nv2a_pusher_dump_recent);
    }
    /* Both hosts now. This was POSIX/AArch64-only, and on Windows the parser
     * decoded the title's software methods and dropped them: measured as
     * [PB-NOP] with no [PB-NOTIFY] beside it. The guard could not simply be
     * deleted -- jsrf_software_method spins on `!raised`, and the Windows
     * xbox_Nv2aRaiseSoftwareMethod was a hardcoded FALSE, so installing the
     * handler alone would have wedged this thread instead of dropping the
     * notify. The raise and the PGRAPH page guard it needs landed first. */
    nv2a_pusher_set_software_method_handler(jsrf_software_method);
    while (!g_pushbuf_ack_stop) {
        uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
        /* Snapshot the fence before consuming its commands. Reading PUT again
         * afterwards acknowledged newer, unconsumed work and allowed the
         * producer to overwrite the ring underneath the parser. */
        uint32_t getp = dev ? MEM32(dev + JSRF_D3D_GETPTR_OFFSET) : 0;
        uint32_t submitted = dev ? MEM32(dev + JSRF_D3D_PUT_OFFSET) : 0;
        int consumed = jsrf_pb_poll();
        jsrf_pusher_report();
        /* Cheap and constant: the guest-clock anchor must not inherit the
         * report's interval, or the two hosts dump at different guest
         * instants. Self-gating and a no-op unless RECOMP_OBJECT_DUMP_AT is
         * set. */
        jsrf_object_dump();
        /* Why the guest gets so few grants.
         *
         * The title's pushbuffer reserve spins until GET catches up with PUT,
         * and this line is the only thing that moves GET. Measured, it moves
         * about four times a second, against the hundreds a frame needs -- so
         * either this loop barely runs, or it runs and takes an early exit.
         * Those need opposite fixes, and only the counts tell them apart:
         * `loops` is the thread's own rate, and each `no_*` is one reason the
         * acknowledgement did not happen. */
        {
            static unsigned long loops, no_dev, no_consume, already, acked;
            static DWORD last;
            DWORD now_ms = GetTickCount();
            ++loops;
            if (!getp)                        ++no_dev;
            else if (!consumed)               ++no_consume;
            else if (MEM32(getp)==submitted)  ++already;
            else                              ++acked;
            if (!last) last = now_ms;
            if (now_ms - last >= 2000) {
                fprintf(stderr, "  [PB-ACK] %lu loops/s: acked=%lu already=%lu"
                        " not-consumed=%lu no-device=%lu (totals)\n",
                        loops * 1000ul / (now_ms - last),
                        acked, already, no_consume, no_dev);
                fflush(stderr);
                loops = 0;
                last = now_ms;
            }
        }
        if (getp && consumed && MEM32(getp)!=submitted) MEM32(getp)=submitted;
        Sleep(0);
    }
    return 0;
}

/*
 * ADX (CRI audio middleware) manager/server handshake watcher.  TEMPORARY.
 *
 * sub_0013B110 (manager) sets [0x25EFA4] = 1 and then spins up to 200,000,000
 * times resuming [0x27D0E8] and waiting for the flag to clear; on timeout it
 * reports "1060102: Internal Error: adxm_goto_mwidle_border" (string at
 * 0x001DF060), which is what puts the zero-byte JSRF_FATAL.ERR on Z:.
 *
 * sub_0013B2A0 (server) is supposed to clear the flag and increment the tick
 * at [0x25EFB0] every pass. Whether that tick advances distinguishes the two
 * candidate explanations, which need opposite fixes:
 *   tick advancing  -> the server runs; the flag/handshake logic is at fault
 *   tick frozen     -> the server never runs; suspend/resume is at fault
 */
#define ADX_FLAG_VA   0x0025EFA4u
#define ADX_TICK_VA   0x0025EFB0u
#define ADX_SHUTDOWN_VA 0x0025EFD8u
#define ADX_THREAD_VA 0x0027D0E8u

static DWORD WINAPI jsrf_adx_watch(LPVOID unused)
{
    uint32_t last_tick = 0xFFFFFFFFu;
    int quiet = 0;
    (void)unused;
    for (;;) {
        uint32_t tick = MEM32(ADX_TICK_VA);
        uint32_t flag = MEM32(ADX_FLAG_VA);
        if (tick != last_tick) {
            fprintf(stderr, "  [ADX] tick=%u flag=%u shutdown=%u thread=0x%08X\n",
                    tick, flag, MEM32(ADX_SHUTDOWN_VA), MEM32(ADX_THREAD_VA));
            fflush(stderr);
            last_tick = tick;
            quiet = 0;
        } else if (++quiet % 20 == 0) {
            fprintf(stderr, "  [ADX] tick STUCK at %u (flag=%u) for %ds\n",
                    tick, flag, quiet / 2);
            fflush(stderr);
        }
        Sleep(500);
    }
    return 0;
}


#if !defined(_WIN32)
/* Pump the host window, on the one thread that may and no faster than a frame.
 *
 * SDL_PollEvent drives the platform event loop, which on macOS belongs to the
 * process's main thread. The guest owns that thread, so this runs from inside
 * the kernel bridge's blocking wait -- the moment the guest is idle. Every
 * other thread that reaches that wait returns immediately, and the rate limit
 * keeps a title that waits thousands of times a second from spending its time
 * in Cocoa. */
static void jsrf_pump_host_events(void)
{
    extern void xbox_d3d8_pump_events(void);
    extern int pthread_main_np(void);
    static DWORD last_ms;
    DWORD now;

    if (!pthread_main_np())
        return;
    now = GetTickCount();
    if (last_ms && (now - last_ms) < 16)
        return;
    last_ms = now;
    xbox_d3d8_pump_events();
}
#endif

static void apu_mmio_write_shim(uint32_t offset, uint32_t value, unsigned width)
{
    if (g_apu_state) {
        mcpx_apu_mmio_write(g_apu_state, offset, value, width);
    }
}

static uint32_t apu_mmio_read_shim(uint32_t offset, unsigned width)
{
    return (uint32_t)mcpx_apu_mmio_read(g_apu_state, offset, width);
}

/* Compare the guest-visible register aperture with the actual APU model at
 * voice stop, wait, idle-trap dispatch and completion. No guest state changes. */
void jsrf_audio_completion_probe(uint32_t pc, uint32_t object, uint32_t arg)
{
    static int enabled = -1;
    static unsigned count;
    static const uint32_t offsets[] = {0x1000, 0x1004, 0x1100, 0x1300, 0x1304, 0x1504, 0x2000};
    /* The cap answers "does this site ever run"; the ratio between sites needs
     * a longer window than 80 events, which JSRF exhausts before the title
     * screen. RECOMP_AUDIO_COMPLETION_TRACE=<n> sets it; bare =1 keeps 80. */
    static unsigned cap;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_AUDIO_COMPLETION_TRACE");
        enabled = e != NULL;
        cap = 80;
        if (e && *e) {
            long v = strtol(e, NULL, 0);
            if (v > 1) cap = (unsigned)v;
        }
    }
    if (!enabled) return;
    if (pc == 0x001A308Eu && !(MEM16(object + 0x12) & 0x8000u)) return;
    if (++count > cap) return;
    fprintf(stderr, "[AUDIO-COMPLETE] pc=%08X object=%08X arg=%08X flags=%04X\n",
            pc, object, arg, (unsigned)MEM16(object + 0x12));
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        uint32_t off = offsets[i];
        fprintf(stderr, "  +%04X guest=%08X model=%08X\n", off,
                MEM32(0xFE800000u + off),
                (uint32_t)mcpx_apu_mmio_read(g_apu_state, off, 4));
    }
    fflush(stderr);
}

/* JSRF's voice submission loop, sub_001A3E58 (guest 0x001A3E58-0x001A4003).
 *
 * The APU model reports on=0 at the tutorial: no voice is ever started. The
 * store that would start one is present in the generated code --
 *
 *   0x001A3F8D   mov [0xFE820124], ecx      NV1BA0_PIO_VOICE_ON
 *
 * (the translator prints that address as the signed constant -25034460) -- so
 * either execution never arrives there, or it arrives and the write does not
 * reach the model. This tells those apart. Every site reads registers and
 * guest memory only: no branch, register or guest byte changes.
 *
 * The path has four places it can stop short of the store:
 *
 *   1. the entry gate        test [obj+0x12],2 / jne 0x001A4000. There is no
 *      label between that test and its jump, so the gate is read at the entry
 *      site instead: flags & 2 set on arrival means this call returns at once
 *   2. a PIO_FREE spin       0x001A3EB3, until (FREE & ~3) >= 0x80
 *   3. a second PIO_FREE spin 0x001A3F24, until (FREE >> 2) >= count * 7
 *   4. a zero voice count    MEM8(obj+0x64) == 0 skips the loop body entirely
 *
 * Both spin heads are sites of their own, so a spin that never exits shows as
 * its cap being spent with nothing printed after it -- the distinction the
 * live counters cannot make, because a thread stuck in a guest spin loop is
 * indistinguishable from one that was never called.
 *
 * Caps are per site, not shared: the two spin heads would otherwise spend the
 * whole budget before the loop body printed anything.
 *
 * RECOMP_VOICE_TRACE=1 enables it; =<n> multiplies every cap by n. */
/* Where the two D3D contiguous allocators stop, on each host.
 *
 * MmAllocateContiguousMemoryEx (ordinal 166) is called 59 times on macOS and 0
 * on Windows, and every one of those calls comes from sub_0018E670 or
 * sub_00199760. Reading both (CLAUDE_PROGRESS_2026-09-11_TWO_FUNCTIONS_READ.md)
 * showed they have the same shape and exactly one branch ahead of the
 * allocation:
 *
 *     descriptor = sub_0014A83E(0x40, tag)   // the title's own heap
 *     if (!descriptor) return E_OUTOFMEMORY; // <-- and no allocation happens
 *     mem = (*(void**)0x001C40F8)(size, ...) // thunk slot 102 = ordinal 166
 *     if (!mem) { free(descriptor); return E_OUTOFMEMORY; }
 *
 * So "Windows never allocates" has two possible shapes, and they are
 * distinguishable at one site each: the descriptor is null and the guest never
 * reaches the allocation, or it reaches it and gets zero back. Entry counters
 * cannot separate them -- sub_0014A83E and the free are general-purpose and
 * have callers all over the title -- which is why this is a path probe and not
 * another --va list.
 *
 * Counts everything and prints the first few of each, because the counts are
 * the measurement and the values are how you read it. The thunk word at
 * 0x001C40F8 is sampled at the call site: if it is ever not a synthetic kernel
 * VA, the allocation cannot dispatch and neither branch above is the story.
 *
 * RECOMP_D3D_ALLOC_TRACE=1 enables it. Read-only: every site is a call placed
 * after an existing label, taking registers and guest memory as arguments. */
static const struct { uint32_t pc; const char *what; } g_d3d_alloc_sites[] = {
        { 0x0018E670u, "E670 entry" },
        { 0x0018E6CBu, "E670 descriptor" },
        { 0x0018E6D1u, "E670 pre-alloc" },
        { 0x0018E6E9u, "E670 alloc returned" },
        { 0x0018E6F3u, "E670 fail return" },
        { 0x0018E6FEu, "E670 success" },
        { 0x00199760u, "9760 entry" },
        { 0x0019976Au, "9760 descriptor" },
        { 0x00199770u, "9760 pre-alloc" },
        { 0x00199789u, "9760 alloc returned" },
        { 0x00199793u, "9760 fail return" },
        { 0x0019979Cu, "9760 success" },
};
enum { D3D_ALLOC_NSITES =
           (int)(sizeof(g_d3d_alloc_sites) / sizeof(g_d3d_alloc_sites[0])),
       D3D_ALLOC_PRINT_CAP = 6 };
static unsigned long g_d3d_alloc_hits[D3D_ALLOC_NSITES];
static unsigned      g_d3d_alloc_printed[D3D_ALLOC_NSITES];
static int           g_d3d_alloc_enabled = -1;

void jsrf_d3d_alloc_probe(uint32_t pc, uint32_t a, uint32_t b)
{
    int i, site = -1;

    if (g_d3d_alloc_enabled < 0)
        g_d3d_alloc_enabled = getenv("RECOMP_D3D_ALLOC_TRACE") ? 1 : 0;
    if (!g_d3d_alloc_enabled) return;

    for (i = 0; i < D3D_ALLOC_NSITES; i++)
        if (g_d3d_alloc_sites[i].pc == pc) { site = i; break; }
    if (site < 0) return;

    g_d3d_alloc_hits[site]++;
    if (g_d3d_alloc_printed[site] < D3D_ALLOC_PRINT_CAP) {
        g_d3d_alloc_printed[site]++;
        fprintf(stderr, "  [D3D-ALLOC] %-20s a=0x%08X b=0x%08X (hit %lu)\n",
                g_d3d_alloc_sites[site].what, a, b,
                g_d3d_alloc_hits[site]);
        fflush(stderr);
    }
}

void jsrf_d3d_alloc_report(void)
{
    int i;

    if (g_d3d_alloc_enabled <= 0) return;

    /* Every site, INCLUDING the zeroes. A site at 0 is the whole point of this
     * probe -- "the guest never got here" is the answer it exists to give, and
     * a report that skipped empty rows could not give it. */
    fprintf(stderr, "  [D3D-ALLOC] thunk slot 102 (0x001C40F8) = 0x%08X\n",
            MEM32(0x1C40F8u));
    for (i = 0; i < D3D_ALLOC_NSITES; i++)
        fprintf(stderr, "  [D3D-ALLOC] %-20s %lu\n",
                g_d3d_alloc_sites[i].what, g_d3d_alloc_hits[i]);
    fflush(stderr);
}

void jsrf_voice_submit_probe(uint32_t pc, uint32_t a, uint32_t b, uint32_t c)
{
    extern unsigned long g_apu_voice_on_count;
    extern unsigned long g_apu_fe_method_count;
    extern unsigned long g_apu_set_current_voice_count;

    static const struct { uint32_t pc; unsigned cap; const char *what; } sites[] = {
        { 0x001A3E58u, 16, "entry" },
        { 0x001A3E74u, 16, "past-gate" },
        { 0x001A3EB3u,  8, "free-spin-1" },
        { 0x001A3EC2u, 16, "free-spin-1-done" },
        { 0x001A3F17u, 16, "read-count" },
        { 0x001A3F24u,  8, "free-spin-2" },
        { 0x001A3F36u, 16, "loop-entered" },
        { 0x001A3F3Bu, 32, "iteration" },
        { 0x001A3F7Au, 32, "pre-voice-on" },
        { 0x001A3FA9u, 16, "loop-done" },
        { 0x001A3FDBu,  8, "free-spin-3" },
        { 0x001A3FEAu, 16, "free-spin-3-done" },
        { 0x001A4000u, 16, "returned" },
    };
    enum { NSITES = (int)(sizeof(sites) / sizeof(sites[0])) };
    static unsigned used[NSITES];
    static int enabled = -1;
    static unsigned scale = 1;
    int i, site = -1;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_VOICE_TRACE");
        enabled = e != NULL;
        if (e && *e) {
            long v = strtol(e, NULL, 0);
            if (v > 1) scale = (unsigned)v;
        }
    }
    if (!enabled) return;

    for (i = 0; i < NSITES; i++) {
        if (sites[i].pc == pc) { site = i; break; }
    }
    if (site < 0) return;
    if (used[site] >= sites[site].cap * scale) return;
    used[site]++;

    switch (pc) {
    case 0x001A3E58u:
        /* a = ecx (the voice object), b = the return address at [esp]. */
        fprintf(stderr, "[VOICE] %-16s obj=%08X flags=%04X count=%u ret=%08X\n",
                sites[site].what, a, (unsigned)MEM16(a + 0x12),
                (unsigned)MEM8(a + 0x64), b);
        break;
    case 0x001A3E74u:
    case 0x001A3F17u:
    case 0x001A4000u:
        fprintf(stderr, "[VOICE] %-16s obj=%08X flags=%04X count=%u\n",
                sites[site].what, a, (unsigned)MEM16(a + 0x12),
                (unsigned)MEM8(a + 0x64));
        break;
    case 0x001A3EB3u:
    case 0x001A3EC2u:
    case 0x001A3F24u:
    case 0x001A3FDBu:
    case 0x001A3FEAu:
        /* b is the PIO_FREE word as the guest reads it. The aperture is plain
         * memory for reads, advanced by the ack thread's counter, so this is
         * the value the spin's comparison actually sees. */
        fprintf(stderr, "[VOICE] %-16s obj=%08X free=%08X need=%08X\n",
                sites[site].what, a, b, c);
        break;
    case 0x001A3F36u:
    case 0x001A3F3Bu:
        fprintf(stderr, "[VOICE] %-16s obj=%08X i=%d handle_ptr=%08X\n",
                sites[site].what, a, (int)b, c);
        break;
    case 0x001A3F7Au:
        /* a = ecx, the VOICE_ON argument about to be stored; b = the
         * SET_ANTECEDENT_VOICE argument; c = the loop index. */
        fprintf(stderr, "[VOICE] %-16s voice_on=%08X antecedent=%08X i=%d "
                "model_on=%lu fe=%lu\n",
                sites[site].what, a, b, (int)c,
                g_apu_voice_on_count, g_apu_fe_method_count);
        break;
    case 0x001A3FA9u:
        /* After the loop. shadow_* are the plain-memory contents of the two
         * VP registers the loop stores to. The write trap does not write
         * through, so a nonzero shadow means the store landed as ordinary
         * memory -- it fell into one of the ack thread's unprotect windows on
         * that page -- while a zero shadow with a raised model count means it
         * was trapped and delivered. */
        fprintf(stderr, "[VOICE] %-16s obj=%08X model_on=%lu fe=%lu scv=%lu "
                "shadow_av=%08X shadow_on=%08X\n",
                sites[site].what, a,
                g_apu_voice_on_count, g_apu_fe_method_count,
                g_apu_set_current_voice_count,
                (unsigned)MEM32(0xFE820120u), (unsigned)MEM32(0xFE820124u));
        break;
    default:
        break;
    }
    fflush(stderr);
}

/* Host pad state, packed as the Xbox controller's own USB interrupt-IN report.
 *
 * The emulated device on root-hub port 1 asks for this whenever the title
 * polls its interrupt endpoint. Going through the USB device rather than
 * calling the input backend from an XPP override is what lets JSRF's own
 * driver stay in the loop: it enumerated the pad, so it is entitled to read it
 * the way it knows how.
 *
 * The layouts line up field for field. XBOX_GAMEPAD::wButtons already uses the
 * report's digital bit assignments, and bAnalogButtons is in report order
 * (A, B, X, Y, Black, White, LeftTrigger, RightTrigger), so the only work here
 * is the two-byte header and little-endian thumbsticks. */
static int usb_pad_state_shim(uint8_t report[XBOX_USB_PAD_REPORT])
{
    XBOX_INPUT_STATE state;
    static const struct { int lo; int off; } axis[4] = {
        { 12, 0 }, { 14, 1 }, { 16, 2 }, { 18, 3 }
    };
    const SHORT *thumb;
    int i;

    if (xbox_InputGetState(0, &state) != ERROR_SUCCESS)
        return 0;                 /* no controller: the endpoint NAKs */

    memset(report, 0, XBOX_USB_PAD_REPORT);
    report[0] = 0x00;
    report[1] = XBOX_USB_PAD_REPORT;
    report[2] = (uint8_t)(state.Gamepad.wButtons & 0xFFu);
    for (i = 0; i < 8; i++)
        report[4 + i] = state.Gamepad.bAnalogButtons[i];

    thumb = &state.Gamepad.sThumbLX;
    for (i = 0; i < 4; i++) {
        uint16_t v = (uint16_t)thumb[axis[i].off];
        report[axis[i].lo]     = (uint8_t)(v & 0xFFu);
        report[axis[i].lo + 1] = (uint8_t)(v >> 8);
    }

    /* RECOMP_PAD_SENTINEL stamps a recognisable value into the left stick.
     *
     * The report demonstrably reaches guest RAM -- the runtime writes it there
     * itself -- but that says nothing about whether the title's own input
     * layer ever copies it out. A value nothing else would produce can be
     * searched for: one hit is the USB transfer buffer alone, which means XPP
     * takes delivery and stops; more than one means it propagates, and the
     * addresses say where to look next. */
    if (pad_sentinel()) {
        report[12] = 0x5A; report[13] = 0x5A;      /* left stick X */
        report[14] = 0xA5; report[15] = 0xA5;      /* left stick Y */
    }
    return 1;
}

static uint32_t g_pad_inject_va[8];
static unsigned g_pad_inject_n;
static int      g_pad_inject_found;
static int pad_inject(void);

static int pad_sentinel(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PAD_SENTINEL") ? 1 : 0;
    /* RECOMP_PAD_INJECT needs the stamp only until it has found the guest's
     * copies; after that the stamp would fight the live report it writes. */
    if (pad_inject() && !g_pad_inject_found) return 1;
    return on;
}

/*
 * RECOMP_PAD_INJECT -- deliver input without the guest's USB driver.
 *
 * Every route from the host pad into the title runs through one door:
 * xbox_InputGetState is called from exactly one place, usb_gamepad_report,
 * which only runs when the guest's XPP driver schedules an interrupt
 * transfer. When that driver stops -- and it does, in roughly a third of
 * runs, mid-play and without recovering -- the game keeps calling its own
 * readInput() at full rate and reads a buffer nobody is filling any more.
 * Measured: polls frozen at 6614 for 60 s while readInput climbed 3349 ->
 * 4257 and the frame loop ran normally.
 *
 * Nothing in the guest can be asked to restart that schedule from here. But
 * the report's destination is knowable: the sentinel already proves the title
 * keeps its own copies of the 20-byte report, and prints their addresses. So
 * this writes the live report straight into those copies on a host thread,
 * and the USB path becomes irrelevant to whether input works.
 *
 * Discovery costs one report's worth of a pinned left stick (the sentinel
 * stamp), after which stamping stops and real input flows. Bytes 0 and 1 --
 * type and length -- are left alone; only the button and axis payload is
 * written, so a partially-read struct can only ever mix two frames of input.
 *
 * This is a diagnostic bypass, not a fix: it hides the driver stall rather
 * than explaining it. Its purpose is to let the title be PLAYED while that is
 * still open, and to answer a question no amount of further tracing can --
 * whether the tutorial's jump is counted once a press genuinely arrives.
 */

static int pad_inject(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PAD_INJECT") ? 1 : 0;
    return on;
}

static DWORD WINAPI pad_inject_thread(LPVOID arg)
{
    extern int usb_gamepad_report(uint8_t *out, int max);
    uint8_t rep[20];
    (void)arg;
    for (;;) {
        uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
        if (base && g_pad_inject_n && usb_gamepad_report(rep, (int)sizeof rep) == 20) {
            /* Write ONLY the last copy found.
             *
             * The first hit is the USB transfer buffer itself -- the address
             * our own OHCI model writes into, and which XPP's descriptors
             * point at. Writing there races the device model and is the most
             * likely cause of the hang the first version of this produced:
             * the guest's frame loop stopped while audio kept running. The
             * later copy is XPP's own, downstream of delivery, which is the
             * one the title reads and the one nothing else is writing. */
            uint32_t va = g_pad_inject_va[g_pad_inject_n - 1];
            memcpy(base + va + 2, rep + 2, 18);
        }
        Sleep(16);      /* 60 Hz. The title reads this buffer; it does not need
                         * to be written faster than it is read. */
    }
    return 0;
}

static void pad_inject_start(void)
{
    static int started;
    HANDLE th;
    if (started || !g_pad_inject_n) return;
    started = 1;
    /* CreateThread, not pthreads: every other thread in this file uses it, and
     * win32_compat.c supplies it on POSIX -- so this one call site was the only
     * thing keeping the harness from building for Windows. */
    th = CreateThread(NULL, 0, pad_inject_thread, NULL, 0, NULL);
    if (th) {
        CloseHandle(th);
        fprintf(stderr, "  [PAD-INJECT] writing live reports to guest 0x%08X"
                " at 60 Hz (%u copies found, last one used); the USB schedule"
                " is now bypassed\n",
                g_pad_inject_va[g_pad_inject_n - 1], g_pad_inject_n);
    } else {
        fprintf(stderr, "  [PAD-INJECT] thread create failed\n");
    }
    fflush(stderr);
}

/*
 * RECOMP_SAVE_DUMP -- read JSRF's own save/progress state, read-only.
 *
 * The decompilation documents both the structure and where it lives. The
 * singleton is loaded into ecx immediately before every CSaveData call:
 *
 *     loc_00054810:  ecx = 0x1EB938
 *                    call 0x000149E0   (CSaveData::GetReturnMissionNo)
 *
 * and CSaveData begins with the on-disk sdData, so the first fields are at
 * fixed offsets (JSRF-Decompilation, decompile/src/JSRF/SaveData.hpp):
 *
 *     +0x000 m_dwReturnChapterNo     +0x004 m_dwReturnMissionNo
 *     +0x008 m_dwSpawnPosIndex       +0x00C m_dwPlaytimeFrames
 *     +0x010 m_dwUnlockedChars       +0x014 m_dwChapterFlags[16]
 *     +0x054 m_dwGlobalFlags[16]     +0x094 m_dwSpecialFlags[16]
 *
 * m_dwSpecialFlags is the one the header describes as carrying "special
 * effects like unlocking/completing tutorials", which is why this exists:
 * the tutorial's progress is a value we can now watch rather than infer from
 * whether the dialogue advances.
 */
/* CSaveData is polymorphic, so a vtable pointer sits at +0x00 and the on-disk
 * sdData begins at +0x04. Not a guess: GetReturnChapterNo reads [ecx+4] and
 * GetReturnMissionNo reads [ecx+8] in the generated code. */
#define JSRF_SAVEDATA_VA        0x001EB938u
#define JSRF_SD_VPTR            0x000u
#define JSRF_SD_RETURN_CHAPTER  0x004u
#define JSRF_SD_RETURN_MISSION  0x008u
#define JSRF_SD_SPAWN_INDEX     0x00Cu
#define JSRF_SD_PLAYTIME        0x010u
#define JSRF_SD_CHAPTER_FLAGS   0x018u
#define JSRF_SD_GLOBAL_FLAGS    0x058u
#define JSRF_SD_SPECIAL_FLAGS   0x098u

static void jsrf_save_dump(void)
{
    static int on = -1;
    const uint8_t *base;
    uint32_t v[4];
    unsigned i;

    if (on < 0) on = getenv("RECOMP_SAVE_DUMP") ? 1 : 0;
    if (!on) return;
    base = (const uint8_t *)xbox_GetMemoryOffset();
    if (!base) return;

    memcpy(&v[0], base + JSRF_SAVEDATA_VA + JSRF_SD_RETURN_CHAPTER, 4);
    memcpy(&v[1], base + JSRF_SAVEDATA_VA + JSRF_SD_RETURN_MISSION, 4);
    memcpy(&v[2], base + JSRF_SAVEDATA_VA + JSRF_SD_SPAWN_INDEX, 4);
    memcpy(&v[3], base + JSRF_SAVEDATA_VA + JSRF_SD_PLAYTIME, 4);
    fprintf(stderr, "  [SAVE] chapter=%u mission=%u spawn=%u playtime_frames=%u\n",
            v[0], v[1], v[2], v[3]);

    {
        static const struct { const char *name; uint32_t off; } lists[] = {
            { "chapter", JSRF_SD_CHAPTER_FLAGS },
            { "global",  JSRF_SD_GLOBAL_FLAGS  },
            { "special", JSRF_SD_SPECIAL_FLAGS },
        };
        unsigned l;
        for (l = 0; l < sizeof lists / sizeof lists[0]; l++) {
            uint32_t w[16];
            unsigned set = 0;
            memcpy(w, base + JSRF_SAVEDATA_VA + lists[l].off, sizeof w);
            for (i = 0; i < 16; i++) {
                unsigned b;
                for (b = 0; b < 32; b++)
                    if (w[i] & (1u << b)) set++;
            }
            fprintf(stderr, "  [SAVE] %-7s flags: %3u bit(s) set  "
                    "%08X %08X %08X %08X\n",
                    lists[l].name, set, w[0], w[1], w[2], w[3]);
        }
    }
    fflush(stderr);
}

/* Count and locate copies of the sentinel across guest RAM. */
static void pad_sentinel_scan(void)
{
    static const uint8_t pat[4] = { 0x5A, 0x5A, 0xA5, 0xA5 };
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    /* The main RAM window, indexed by guest address exactly as the pushbuffer
     * executor indexes it. Stopping at 4 MB below the 64 MB top keeps the scan
     * inside what the layout reserves. */
    const uint32_t LIMIT = 0x03C00000u;
    uint32_t i;
    unsigned hits = 0;

    if (!pad_sentinel() || !base)
        return;

    for (i = 0x10000u; i + 4 <= LIMIT; i++) {
        if (base[i] == pat[0] && base[i + 1] == pat[1] &&
            base[i + 2] == pat[2] && base[i + 3] == pat[3]) {
            if (hits < 16)
                fprintf(stderr, "  [PAD-SENTINEL] copy at guest 0x%08X\n",
                        (unsigned)i);
            /* The sentinel sits at report bytes 12..15, so the report starts
             * twelve bytes earlier. Guard the subtraction: a hit below that
             * is not a report. */
            if (pad_inject() && !g_pad_inject_found &&
                    g_pad_inject_n < (unsigned)(sizeof g_pad_inject_va /
                                                sizeof g_pad_inject_va[0]) &&
                    i >= 12u)
                g_pad_inject_va[g_pad_inject_n++] = i - 12u;
            hits++;
        }
    }
    fprintf(stderr, "  [PAD-SENTINEL] %u cop%s of the pad report in guest RAM\n",
            hits, hits == 1 ? "y" : "ies");
    fflush(stderr);

    if (pad_inject() && !g_pad_inject_found && g_pad_inject_n) {
        /* Latch, so the stamp stops and the pinned stick goes away. */
        g_pad_inject_found = 1;
        pad_inject_start();
    }
}

/* JSRF's own object bookkeeping, read from the host side.
 *
 * "The player does not move" and "the player is not drawn" are two symptoms of
 * one cause if the player object does not exist: nothing to drive, and nothing
 * to draw. Asking the title's registry what is alive separates that from an
 * input fault, which the pad sentinel has already ruled out.
 *
 * Layout measured 2026-09-04 and recorded in docs/jsrf/handovers:
 *   root object      guest 0x005E3A70, also pointed to by MEM32(0x0022FCE0)
 *   +0x98            7668 object pointers indexed by global object id
 *   +0x87DC          root of the scene graph
 *   +0x87E8          count of live registered objects
 * and per scene node:
 *   +0x04 flags (bit31 = dead)   +0x08 own global id
 *   +0x28 first child            +0x30 next sibling
 *
 * Both ways of naming the root are tried and reported. At the title screen the
 * pointer at 0x0022FCE0 has been observed holding 0x040D3A70, which is past the
 * top of the 64 MB RAM window and so cannot be the object -- the title has not
 * filled it in yet. The static address is the fallback, and printing which one
 * answered keeps a stale pointer from being mistaken for an empty registry.
 *
 * Read-only and bounded: the walk has a visit cap, so a corrupt or cyclic
 * graph cannot spin it. */
#define JSRF_ROOT_PTR_VA   0x0022FCE0u
#define JSRF_ROOT_VA       0x005E3A70u
#define JSRF_IDS_OFF       0x98u
#define JSRF_IDS_COUNT     7668u
#define JSRF_SCENE_OFF     0x87DCu
#define JSRF_LIVE_OFF      0x87E8u
#define JSRF_WALK_CAP      8192u
#define JSRF_RAM_TOP       0x08000000u  /* reserve space sits above the retail 64 MB arena */

static int jsrf_scene_probe(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_SCENE_REPORT") ? 1 : 0;
    return on;
}

static int jsrf_va_ok(uint32_t va)
{
    return va >= 0x10000u && va < JSRF_RAM_TOP;
}

/* A full object inventory in exactly the schema /private/tmp/jsrf_xemu_objects.py
 * prints from xemu's GDB stub, so the two can be diffed field by field.
 *
 * The registry comparison at the tutorial is already known to match xemu on
 * cardinality and on the id set itself, so counting is finished: what is not
 * yet measured is each object's own eACTFLAG, draw links and vtable, which is
 * where a "present but never drawn" character would show. Writing the same
 * JSON both sides keeps the comparison mechanical rather than by eye.
 *
 * Read-only: every read is bounds-checked against the same RAM window the
 * scene walk uses, and nothing is written back into guest memory. Opt-in via
 * RECOMP_OBJECT_DUMP=<path>, and it writes once per run so a bounded run does
 * not accumulate hundreds of megabytes.
 *
 * Per-object offsets are the xemu tool's, which came from CActBase in the
 * decompilation: +00 vtable, +04 eACTFLAG, +08 own id, +0C draw child mask,
 * +24 parent, +28 child, +2C/+30 siblings, +34/+38/+3C draw links, +40 zsort.
 * Manager draw-side offsets are CActMan's: +74 m_bSkipDraw, +94 m_DrawMode,
 * +7FA4 m_lpDrawRoot, +7FAC m_lpDrawSortRoot, +7FB4 m_lpDrawSortBinRoots[256]. */
static int g_obj_dump_force;
static void jsrf_object_dump(void);
static int g_obj_dump_fired;      /* the alarm got there first */
static void jsrf_object_dump_alarm(void)
{
    g_obj_dump_force = 1;
    jsrf_object_dump();
    g_obj_dump_force = 0;
    g_obj_dump_fired = 1;
}

static void jsrf_object_dump(void)
{
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    int dump_exit = 0;
    static unsigned seq = 0;
    const char *dir = getenv("RECOMP_OBJECT_DUMP");
    char path[1024];
    uint32_t via_ptr, root;
    unsigned i, emitted = 0;
    FILE *f;

    /* A sequence, not a single shot. Which report coincides with a given line
     * of dialogue is only knowable afterwards, and a per-object field that
     * changes mid-tutorial is itself the interesting measurement -- one file
     * could not show either. The cap bounds a long run's disk use. */
    if (!dir || !base || seq >= 128u)
        return;

    /* RECOMP_OBJECT_DUMP_AT=<n>[,<va>] anchors the dump to the GUEST's clock.
     *
     * This function is called from the periodic report, which is wall-clock
     * driven -- fine for watching one host, useless for comparing two. Windows
     * runs the guest main loop at about a fifth of macOS's rate, so the same
     * report interval catches the two hosts at completely different points in
     * the guest's life, and every field that has simply moved on reads as a
     * divergence. Three handovers in this project made that mistake before the
     * rule was written down.
     *
     * With this set, the dump waits until an armed site has been entered <n>
     * times and then writes ONCE. Both hosts then describe the same instant of
     * guest execution. Default site is sub_000123E0, the manager's per-frame
     * tick, which is the clock the loop-rate figures already use; arm it with
     *     instrument_func_hit.py --va 000123E0
     * and the site must be armed or the count is always zero and the dump
     * never fires -- deliberately, because a dump at the wrong moment is worse
     * than no dump. */
    /* Applies to the alarm path as much as the polling one.
     *
     * This used to be set inside the polling block, which the alarm skips --
     * so the precise dump landed and the run then carried on for ever, and the
     * only reason anything ever exited was the second, sloppier dump that the
     * poll produced a few frames later. Suppressing that duplicate exposed it.
     * Gated on an anchor being configured so that a plain RECOMP_OBJECT_DUMP
     * run, which wants a sequence of dumps, still gets one. */
    {
        static int exit_env = -1;
        if (exit_env < 0)
            exit_env = getenv("RECOMP_OBJECT_DUMP_EXIT") != NULL
                    && getenv("RECOMP_OBJECT_DUMP_AT") != NULL;
        dump_exit = exit_env;
    }

    {
        /* Cached, and polled from the ack loop rather than only from the
         * periodic report.
         *
         * Gating this inside the report tied the anchor's PRECISION to the
         * report interval, and the report is expensive -- dropping it to 2 s
         * to tighten the anchor cost about 2.7x of guest throughput, and the
         * overshoot was still asymmetric because the two hosts run different
         * numbers of guest loops per report. Both hosts must dump at the same
         * guest instant or the comparison is worthless, so the check has to be
         * cheap enough to run constantly: one hash lookup, no getenv. */
        static int configured, armed, done;
        static unsigned long long want;
        static uint32_t clock_va = 0x000123E0u;
        if (!configured) {
            const char *at = getenv("RECOMP_OBJECT_DUMP_AT");
            configured = 1;
            if (at) {
                const char *comma = strchr(at, ',');
                want = strtoull(at, NULL, 0);
                if (comma) clock_va = (uint32_t)strtoul(comma + 1, NULL, 16);
                armed = 1;
            }
        }
        if (armed && !g_obj_dump_force) {
            /* The polling path is now only a fallback. jsrf_func_hit_alarm
             * fires the dump on the thread that takes the clock site's want-th
             * entry, which is the same instruction boundary on both hosts;
             * this poll cannot be that precise because it runs on another
             * thread. Registered once, below. */
            static int registered;
            unsigned long long now;
            if (done || g_obj_dump_fired)   /* the alarm already dumped */
                return;
            if (!registered) {
                registered = 1;
                jsrf_func_hit_alarm(clock_va, want, jsrf_object_dump_alarm);
            }
            now = jsrf_func_hit_count(clock_va);
            if (now < want)
                return;
            done = 1;
            fprintf(stderr, "  [OBJ-DUMP] guest clock %08X reached %llu"
                            " (wanted %llu, overshoot %llu); dumping once\n",
                    clock_va, now, want, now - want);
            fflush(stderr);
            /* RECOMP_OBJECT_DUMP_EXIT=1 ends the run the moment the anchor is
             * reached.
             *
             * A differential sweep is many runs, and every second spent after
             * the dump is waste -- a Windows run to a deep anchor is minutes,
             * and without this it kept going until something killed it, which
             * also meant killing it under Wine and getting a winedbg window.
             * Exiting here makes the cost of an anchor exactly the cost of
             * reaching it. _exit, not exit: atexit handlers in this harness
             * dump and flush things that would confuse the run that follows,
             * and the dump has already been written and fflushed above. */
        }
    }

#define R32(va) (*(const uint32_t *)(base + (va)))

    /* The same low-16 rule the scene report uses, not a bare range check.
     *
     * This function had its own weaker copy -- accept any pointer inside RAM --
     * which is exactly the error 12 Sep traced to a run reporting a clear fatal
     * flag while the guest was acting on a set one. Here it cost a whole
     * measurement a different way: before the registry exists the pointer is
     * garbage that still passes a range check, so every report wrote an EMPTY
     * dump, and the 128-file cap was spent long before the tutorial loaded.
     * All 128 files came back with live=0 and no objects. */
    root = jsrf_root_va(base, &via_ptr, NULL, 0);
    if (!jsrf_va_ok(root + 0x87E8u + 3u))
        return;
    /* Nothing to say is not worth a sequence number. The cap exists to bound a
     * long run's disk use, and an empty dump consumes it without recording
     * anything -- which is how a 220 s run produced 128 files and no data. */
    if (R32(root + JSRF_LIVE_OFF) == 0u)
        return;

    snprintf(path, sizeof path, "%s/objects_%03u.json", dir, seq);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "  [JSRF-DUMP] cannot write %s\n", path);
        fflush(stderr);
        return;
    }
    seq++;

    fprintf(f, "{\n  \"source\": \"recomp\",\n");
    fprintf(f, "  \"seq\": %u,\n", seq);
    fprintf(f, "  \"root\": %u,\n", (unsigned)root);
    fprintf(f, "  \"live\": %u,\n", (unsigned)R32(root + 0x87E8u));
    fprintf(f, "  \"exec_root\": %u,\n", (unsigned)R32(root + 0x87DCu));
    fprintf(f, "  \"draw_root\": %u,\n", (unsigned)R32(root + 0x7FA4u));
    fprintf(f, "  \"draw_root_tail\": %u,\n", (unsigned)R32(root + 0x7FA8u));
    fprintf(f, "  \"draw_sort_root\": %u,\n", (unsigned)R32(root + 0x7FACu));
    fprintf(f, "  \"draw_sort_tail\": %u,\n", (unsigned)R32(root + 0x7FB0u));
    fprintf(f, "  \"skip_draw\": %u,\n", (unsigned)R32(root + 0x74u));
    fprintf(f, "  \"draw_mode\": %u,\n", (unsigned)R32(root + 0x94u));
    fprintf(f, "  \"state_7930\": %u,\n", (unsigned)R32(root + 0x7930u));
    fprintf(f, "  \"state_7934\": %u,\n", (unsigned)R32(root + 0x7934u));
    fprintf(f, "  \"state_7EC4\": %u,\n", (unsigned)R32(root + 0x7EC4u));

    fprintf(f, "  \"draw_sort_bins\": [");
    for (i = 0; i < 256; i++) {
        uint32_t v = R32(root + 0x7FB4u + i * 4u);
        if (!v) continue;
        fprintf(f, "%s\n    {\"bin\": %u, \"head\": %u}", emitted ? "," : "",
                i, (unsigned)v);
        emitted++;
    }
    fprintf(f, "%s],\n", emitted ? "\n  " : "");

    emitted = 0;
    fprintf(f, "  \"objects\": [");
    for (i = 0; i < JSRF_IDS_COUNT; i++) {
        uint32_t a = R32(root + JSRF_IDS_OFF + i * 4u);
        if (!jsrf_va_ok(a) || !jsrf_va_ok(a + 0x4Fu))
            continue;
        fprintf(f, "%s\n    {\"id\": %u, \"address\": %u, \"vtable\": %u,"
                " \"flags\": %u, \"stored_id\": %u, \"draw_child_mask\": %u,"
                " \"state_11C\": %u, \"state_E50\": %u,"
                " \"gate_E54\": %u, \"gate_1144\": %u,"
                " \"fz\": %u, \"zsort_key\": %u,"
                " \"tx\": %u, \"ty\": %u, \"tz\": %u,"
                " \"parent\": %u, \"child\": %u, \"sibling_before\": %u,"
                " \"sibling_next\": %u, \"draw_next\": %u,"
                " \"draw_before_ptr\": %u, \"draw_last_ptr\": %u,"
                " \"zsort\": %u, \"extra_44\": %u, \"extra_48\": %u}",
                emitted ? "," : "", i, (unsigned)a,
                (unsigned)R32(a), (unsigned)R32(a + 4u), (unsigned)R32(a + 8u),
                (unsigned)R32(a + 0x0Cu),
                /* The animation gate: bit 0 of +0xE54 with +0x1144, read by
                 * sub_00094AB0 before it will touch the +0xCE0 transform. Both
                 * are far outside the 0x50-byte record, so they are read
                 * separately and bounds-checked like everything else. */
                /* The CPlayer state/animation index and its companion.
                 * Sampled per report so the question "does it advance, or
                 * settle and stop?" is answered by a series rather than by a
                 * snapshot -- a snapshot of a mutable field has already been
                 * mistaken here for a fixed identity. */
                jsrf_va_ok(a + 0x1147u) ? (unsigned)R32(a + 0x11Cu) : 0u,
                jsrf_va_ok(a + 0x1147u) ? (unsigned)R32(a + 0xE50u) : 0u,
                jsrf_va_ok(a + 0x1147u) ? (unsigned)R32(a + 0xE54u) : 0u,
                jsrf_va_ok(a + 0x1147u) ? (unsigned)R32(a + 0x1144u) : 0u,
                (unsigned)R32(a + 0x10u), (unsigned)R32(a + 0x14u),
                (unsigned)R32(a + 0x18u), (unsigned)R32(a + 0x1Cu),
                (unsigned)R32(a + 0x20u), (unsigned)R32(a + 0x24u),
                (unsigned)R32(a + 0x28u), (unsigned)R32(a + 0x2Cu),
                (unsigned)R32(a + 0x30u), (unsigned)R32(a + 0x34u),
                (unsigned)R32(a + 0x38u), (unsigned)R32(a + 0x3Cu),
                (unsigned)R32(a + 0x40u), (unsigned)R32(a + 0x44u),
                (unsigned)R32(a + 0x48u));
        emitted++;
    }
    fprintf(f, "%s]\n}\n", emitted ? "\n  " : "");
    fclose(f);

    fprintf(stderr, "  [JSRF-DUMP] seq=%u wrote %u objects to %s\n",
            seq - 1u, emitted, path);
    fflush(stderr);
    if (dump_exit) {
        /* What CODE had run by this anchor, not just what the objects hold.
         *
         * The object inventory answers "is the state the same"; it cannot
         * answer "did the same functions run", and the open question between
         * the two hosts is exactly that -- Windows entered 12 of the 17 CPlayer
         * gameplay sites where macOS entered 17. Those two counts were taken at
         * different guest clocks, which makes them uncomparable; printed here
         * they are taken at the same one. Input is reported alongside because
         * it is the first thing a missing gameplay path suggests, and because
         * "the pad was never polled" and "the pad was polled and said nothing"
         * need opposite fixes. */
        jsrf_func_hit_report();
        {
            extern void xbox_InputPollReport(void);
            xbox_InputPollReport();
        }
        /* And how that drawing was done.
         *
         * [RASTER] is the only statement of how much geometry went through the
         * software rasteriser, and an anchored run switches the periodic report
         * off, so without this the one measurement that compares the two hosts'
         * renderers could not be taken at a guest clock at all -- only at a
         * wall clock, which section 1 of the WINDOWS_PLAYABLE handover spent
         * three wrong findings establishing is meaningless. */
        nv2a_pb_exec_report();
        fprintf(stderr, "  [OBJ-DUMP] RECOMP_OBJECT_DUMP_EXIT set; stopping\n");
        fflush(stderr);
        _exit(0);
    }
#undef R32
}

/* Which pointer is the object registry, by the one rule that has held.
 *
 * Factored out of jsrf_scene_report because a second reader appeared and the
 * rule is the expensive part: the root literal moves between runs --
 * 0x005E3A70, 0x040D3A70 and 0x00363A70 have all been seen -- but its LOW 16
 * BITS ARE INVARIANT at 0x3A70. A bare range check accepts anything in RAM,
 * and on 12 Sep it accepted 0x040FFF40 in the middle of a run whose real
 * `this` was 0x040D3A70, so every field printed came from the wrong object.
 * One copy of that rule, not two. */
static uint32_t jsrf_root_va(const uint8_t *base, uint32_t *via_out,
                             const char **how_out, int verbose)
{
    uint32_t via = *(const uint32_t *)(base + JSRF_ROOT_PTR_VA);
    uint32_t root;
    const char *how;

    if (jsrf_va_ok(via) && (via & 0xFFFFu) == (JSRF_ROOT_VA & 0xFFFFu)) {
        root = via; how = "ptr";
    } else {
        root = JSRF_ROOT_VA; how = "static";
        if (verbose && jsrf_va_ok(via))
            fprintf(stderr, "  [JSRF-SCENE] ptr %08X rejected: low16 is not"
                    " %04X, so it is not the object\n",
                    (unsigned)via, (unsigned)(JSRF_ROOT_VA & 0xFFFFu));
    }
    if (via_out) *via_out = via;
    if (how_out) *how_out = how;
    return root;
}

/*
 * RECOMP_SEQ_TRACE -- name the title's own top-level state, live.
 *
 * Everything about where the title has got to has so far been inferred from
 * side effects: how many objects are live, how many files have been opened,
 * whether the screen is black. Those are downstream of a single number the
 * title keeps for exactly this purpose. CActSequence::Exec0Default, at guest
 * 0x0007BDD0, is
 *
 *     if (this->m_dwNextMethod < 64)
 *         (this->*fSequenceMethods[this->m_dwNextMethod])();
 *
 * -- confirmed in the generated C, which reads MEM32(esi + 0x48), compares it
 * against 0x40 and dispatches through a table at guest 0x0020D2B8.
 *
 * The 64 entries of that table are named by the decompilation, and the names
 * are checked rather than trusted: the table is read out of guest RAM on the
 * first poll and compared entry for entry against the addresses the
 * decompilation gives. A mismatch is reported and the names are suppressed,
 * because a wrong name here would be worse than no name -- it is the kind of
 * thing section 4 of the 12 Sep handover is a list of.
 *
 * So a log line now says "the title is in WaitEndTitle" or "it has entered
 * PrepareTutorial", and driving it with RECOMP_PAD_SCRIPT stops being blind.
 *
 * Read-only and off the guest's threads: one host thread, one dword read per
 * poll, printing only on a transition. Nothing is written into guest memory
 * and no generated code is touched, which matters because this title punishes
 * instrumentation weight -- 578 probe sites froze it at 253 polls where an
 * unprobed run reached 8,790.
 */
#define JSRF_SEQ_TABLE_VA   0x0020D2B8u
#define JSRF_SEQ_COUNT      64u
#define JSRF_SEQ_NEXT_OFF   0x48u        /* CActSequence::m_dwNextMethod */
#define JSRF_SEQ_ID         0u           /* its global object id */

/* fSequenceMethods, in table order. Names and addresses both from
 * JSRF-Decompilation decompile/src/JSRF/ActSequence.cpp; the addresses are
 * what the runtime check below compares against. */
static const struct { uint32_t va; const char *name; } jsrf_seq_tab[JSRF_SEQ_COUNT] = {
    { 0x0007BE30, "Init" },
    { 0x0007BFD0, "LoadSprNorm" },
    { 0x0007C020, "WaitLoadSprNorm" },
    { 0x0007C050, "StartBuildCache" },
    { 0x0007C070, "WaitDestructAct0x1" },
    { 0x0007C270, "SwitchOnGlobal" },
    { 0x0007C090, "LoadLogos" },
    { 0x0007C0D0, "StartLogos" },
    { 0x0007C110, "WaitDestructLogos" },
    { 0x0007C140, "FreeLogos" },
    { 0x0007C160, "PrepareTitle" },
    { 0x0007C230, "SetNextMethod" },
    { 0x0007C250, "WaitEndTitle" },
    { 0x0007C270, "SwitchOnGlobal" },
    { 0x0007C290, "PrepareHandleTitleMenuSelection" },
    { 0x0007C410, "PrepareLoadGameMenu" },
    { 0x0007C450, "WaitEndLoadGameMenu" },
    { 0x0007C4B0, "LoadTags_MAYBE" },
    { 0x0007C510, "WaitLoadTags_MAYBE" },
    { 0x0007C560, "StartHandleTitleMenuSelection" },
    { 0x0007C600, "LoadFullRoboyMenu" },
    { 0x0007C720, "StartFullRoboyMenu" },
    { 0x0007C7E0, "WaitEndFullRoboyMenu" },
    { 0x0007C800, "ReturnFromFullRoboyMenu" },
    { 0x00011C90, "nop" },
    { 0x00011C90, "nop" },
    { 0x00011C90, "nop" },
    { 0x00011C90, "nop" },
    { 0x0007C9E0, "PrepareStoryOrVsMission" },
    { 0x0007CA60, "NopStoryOrVsMission" },
    { 0x0007CA70, "WaitEndStoryOrVsMission" },
    { 0x0007CA90, "ReturnFromStoryOrVsMission" },
    { 0x0007CAE0, "PrepareTutorial" },
    { 0x0007CBB0, "NopTutorial" },
    { 0x0007CBC0, "WaitEndTutorial" },
    { 0x0007CBE0, "ReturnFromTutorial" },
    { 0x0007CC40, "PrepareTestRun" },
    { 0x0007CD10, "NopTestRun" },
    { 0x0007CD20, "WaitEndTestRun" },
    { 0x0007CD40, "ReturnFromTestRun" },
    { 0x0007CDA0, "PrepareUnused" },
    { 0x0007CE70, "NopUnused" },
    { 0x0007CE80, "WaitEndUnused" },
    { 0x0007CEA0, "ReturnFromUnused" },
    { 0x0007CF00, "PrepareGraffitiMenu" },
    { 0x0007CFB0, "WaitLoadGraffitiMenu" },
    { 0x0007CFE0, "WaitEndGraffitiMenu" },
    { 0x0007D000, "ReturnFromGraffitiMenu" },
    { 0x0007D450, "NopStinger" },
    { 0x0007D460, "PrepareStinger" },
    { 0x0007D520, "WaitEndStinger" },
    { 0x0007D540, "ReturnFromStinger" },
    { 0x0007D2D0, "PrepareEnding" },
    { 0x0007D320, "WaitLoadEnding" },
    { 0x0007D3D0, "WaitEndEnding" },
    { 0x0007D3F0, "ReturnFromEnding" },
    { 0x0007D030, "PrepareVsMenu" },
    { 0x0007D080, "WaitLoadVsMenu" },
    { 0x0007D100, "WaitEndVsMenu" },
    { 0x0007D120, "ReturnFromVsMenu" },
    { 0x0007D1A0, "PrepareEndingSaveMenu" },
    { 0x0007D210, "StartEndingSaveMenu" },
    { 0x0007D270, "WaitEndEndingSaveMenu" },
    { 0x0007D290, "ReturnFromEndingSaveMenu" },
};

extern double xbox_InputSeconds(void);

static int      g_seq_names_ok = -1;    /* -1 not yet checked */
static uint32_t g_seq_last = 0xFFFFFFFFu;

static const char *jsrf_seq_name(uint32_t i)
{
    if (i >= JSRF_SEQ_COUNT) return "OUT-OF-RANGE";
    return g_seq_names_ok > 0 ? jsrf_seq_tab[i].name : "?";
}

/* The names are only as good as the table they were read off. Check once. */
static void jsrf_seq_check_table(const uint8_t *base)
{
    unsigned i, bad = 0, first_bad = 0;

    g_seq_names_ok = 0;
    if (!jsrf_va_ok(JSRF_SEQ_TABLE_VA + JSRF_SEQ_COUNT * 4u)) return;
    for (i = 0; i < JSRF_SEQ_COUNT; i++) {
        uint32_t live = *(const uint32_t *)(base + JSRF_SEQ_TABLE_VA + i * 4u);
        if (live != jsrf_seq_tab[i].va) {
            if (!bad) first_bad = i;
            bad++;
        }
    }
    if (bad) {
        fprintf(stderr, "  [JSRF-SEQ] table at %08X does not match the"
                " decompilation: %u of %u entries differ (first at %u:"
                " live %08X, expected %08X). Names SUPPRESSED -- indices"
                " only.\n",
                JSRF_SEQ_TABLE_VA, bad, JSRF_SEQ_COUNT, first_bad,
                (unsigned)*(const uint32_t *)(base + JSRF_SEQ_TABLE_VA
                                              + first_bad * 4u),
                (unsigned)jsrf_seq_tab[first_bad].va);
    } else {
        g_seq_names_ok = 1;
        fprintf(stderr, "  [JSRF-SEQ] fSequenceMethods at %08X matches the"
                " decompilation on all %u entries; names are trustworthy\n",
                JSRF_SEQ_TABLE_VA, JSRF_SEQ_COUNT);
    }
    fflush(stderr);
}

/* The CActSequence itself: global object id 0 in the registry. Validated, not
 * assumed -- an object that is not it will not have id 0 at +0x08 and will not
 * hold a dispatchable index at +0x48. */
static uint32_t jsrf_seq_object(const uint8_t *base)
{
    uint32_t root = jsrf_root_va(base, NULL, NULL, 0);
    uint32_t p;

    if (!jsrf_va_ok(root + JSRF_IDS_OFF + 3u)) return 0;
    p = *(const uint32_t *)(base + root + JSRF_IDS_OFF + JSRF_SEQ_ID * 4u);
    if (!jsrf_va_ok(p + JSRF_SEQ_NEXT_OFF + 3u)) return 0;
    if (*(const uint32_t *)(base + p + 0x08u) != JSRF_SEQ_ID) return 0;
    if (*(const uint32_t *)(base + p + JSRF_SEQ_NEXT_OFF) >= JSRF_SEQ_COUNT)
        return 0;
    return p;
}

static DWORD WINAPI jsrf_seq_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t obj, idx;
        if (base) {
            if (g_seq_names_ok < 0) jsrf_seq_check_table(base);
            obj = jsrf_seq_object(base);
            if (obj) {
                idx = *(const uint32_t *)(base + obj + JSRF_SEQ_NEXT_OFF);
                if (idx != g_seq_last) {
                    fprintf(stderr, "  [JSRF-SEQ] t=%8.2f  %2u %-32s ->"
                            " %2u %s\n",
                            xbox_InputSeconds(),
                            (unsigned)g_seq_last,
                            g_seq_last == 0xFFFFFFFFu ? "(start)"
                                                      : jsrf_seq_name(g_seq_last),
                            (unsigned)idx, jsrf_seq_name(idx));
                    fflush(stderr);
                    g_seq_last = idx;
                }
            }
        }
        /* 100 Hz. The state advances at frame rate at most, and a transition
         * that is missed is a transition that never appears in the log --
         * which is the failure mode every counter in section 4 had. */
        Sleep(10);
    }
    return 0;
}

static void jsrf_seq_trace_start(void)
{
    static int started;
    HANDLE th;
    if (started || !getenv("RECOMP_SEQ_TRACE")) return;
    started = 1;
    th = CreateThread(NULL, 0, jsrf_seq_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    fprintf(stderr, "  [JSRF-SEQ] tracing CActSequence::m_dwNextMethod%s\n",
            th ? "" : " -- THREAD CREATE FAILED");
    fflush(stderr);
}

/* WHICH objects are still changing, and which have stopped.
 *
 * "Corn is frozen" is only half a finding: the useful question is whether
 * anything else in the scene is still moving, because that separates "the
 * character update is gated" from "the whole cutscene driver has stopped".
 *
 * The object dump answers it in principle and could not in practice. It writes
 * a JSON file per report, and at any cadence fast enough to bracket the stall
 * the file I/O perturbs the guest enough that the title screen stops
 * responding -- measured twice, live=157 instead of live=61, both runs
 * rejected. It also spends its 128-file cap long before the tutorial loads.
 *
 * A digest needs no files. Once per report, hash a window of each registered
 * object and compare against the previous report; print only the ids whose
 * hash moved. Read-only, no allocation, no disk, ~72k guest reads per report
 * against a frame loop doing millions -- which is why this one survives its
 * control run where the dump did not.
 */
#define JSRF_ACT_IDS    512u
#define JSRF_ACT_WINDOW 0x1800u      /* covers CPlayer's transform block */
static uint32_t g_act_digest[JSRF_ACT_IDS];
static int      g_act_primed;

static void jsrf_object_activity(const uint8_t *base, uint32_t root)
{
    static int on = -1;
    unsigned id, changed = 0, present = 0, shown = 0;
    char line[512];
    int n = 0;

    if (on < 0) on = getenv("RECOMP_OBJECT_ACTIVITY") ? 1 : 0;
    if (!on) return;

    line[0] = 0;
    for (id = 0; id < JSRF_ACT_IDS; id++) {
        uint32_t slot = root + JSRF_IDS_OFF + id * 4u;
        uint32_t p, h = 2166136261u, off;
        if (!jsrf_va_ok(slot + 3u)) break;
        p = *(const uint32_t *)(base + slot);
        if (!p || !jsrf_va_ok(p + JSRF_ACT_WINDOW)) {
            g_act_digest[id] = 0;
            continue;
        }
        present++;
        for (off = 0; off < JSRF_ACT_WINDOW; off += 4u)
            h = (h ^ *(const uint32_t *)(base + p + off)) * 16777619u;
        if (!h) h = 1u;
        if (g_act_primed && g_act_digest[id] && g_act_digest[id] != h) {
            changed++;
            if (shown < 24u) {
                n += snprintf(line + n, sizeof line - (size_t)n, " %u", id);
                shown++;
            }
        }
        g_act_digest[id] = h;
    }
    if (g_act_primed)
        fprintf(stderr, "  [JSRF-ACT] %u of %u objects changed since the last"
                " report:%s%s\n", changed, present,
                changed ? line : " none",
                changed > shown ? " ..." : "");
    g_act_primed = 1;
    fflush(stderr);
}

/* WHICH PART of one object changes, at 64-byte resolution.
 *
 * The whole-object digest says Corn's memory moves every report while its
 * state index is pinned, which is the interesting half of the earlier finding
 * and does not say what moves. The 11 Sep xemu differential names the answer to
 * check: xemu writes +0x0CE0..+0x0DE4 -- 66 dwords of transform block -- and we
 * never do, along with +0x11D4.., +0x12C0.. and +0x1484... If those blocks are
 * still absent here while others move, the fault is in the pose/transform
 * submission and not in gameplay scheduling at all.
 *
 * Same cost profile as the object digest: read-only, no files, two objects. */
#define JSRF_BLK 64u
static uint32_t g_blk[2][JSRF_ACT_WINDOW / JSRF_BLK];
static int      g_blk_primed;

static void jsrf_object_blocks(const uint8_t *base, uint32_t root)
{
    static int on = -1;
    unsigned k;
    if (on < 0) on = getenv("RECOMP_OBJECT_BLOCKS") ? 1 : 0;
    if (!on) return;

    for (k = 0; k < 2u; k++) {
        unsigned id = 44u + k, b, changed = 0, n = 0;
        uint32_t slot = root + JSRF_IDS_OFF + id * 4u;
        uint32_t p;
        char line[768];
        if (!jsrf_va_ok(slot + 3u)) continue;
        p = *(const uint32_t *)(base + slot);
        if (!p || !jsrf_va_ok(p + JSRF_ACT_WINDOW)) continue;
        line[0] = 0;
        for (b = 0; b < JSRF_ACT_WINDOW / JSRF_BLK; b++) {
            uint32_t h = 2166136261u, off;
            for (off = 0; off < JSRF_BLK; off += 4u)
                h = (h ^ *(const uint32_t *)(base + p + b * JSRF_BLK + off))
                    * 16777619u;
            if (!h) h = 1u;
            if (g_blk_primed && g_blk[k][b] && g_blk[k][b] != h) {
                changed++;
                if (n < 600)
                    n += snprintf(line + n, sizeof line - (size_t)n,
                                  " %04X", b * JSRF_BLK);
            }
            g_blk[k][b] = h;
        }
        if (g_blk_primed)
            fprintf(stderr, "  [JSRF-BLK] id=%u %u/%u blocks changed:%s\n",
                    id, changed, JSRF_ACT_WINDOW / JSRF_BLK,
                    changed ? line : " none");
    }
    g_blk_primed = 1;
    fflush(stderr);
}

/* Is an object in the draw list at all, read from the host.
 *
 * Two counter hooks on sub_000131F0's dispatch sites answered this directly
 * and cost the measurement: both runs stalled at the title (live=158 and 162)
 * where eight consecutive unprobed runs had reached the tutorial, so both were
 * rejected. Two cheap counters at a per-object-per-frame site is still too
 * much for this title.
 *
 * The draw phase walks its own list -- CActMan +0x7FA4 m_lpDrawRoot, threaded
 * through each object's +0x34 -- so membership can be read from outside
 * without touching the guest at all. It is one step weaker than "Draw was
 * called": an object in the list that the walker skips would read as present.
 * It is bounded, and it costs nothing.
 */
#define JSRF_DRAW_ROOT_OFF 0x7FA4u
#define JSRF_DRAW_NEXT_OFF 0x34u

static void jsrf_draw_list_report(const uint8_t *base, uint32_t root)
{
    static int on = -1;
    uint32_t p, n = 0;
    int saw44 = 0, saw45 = 0;

    if (on < 0) on = getenv("RECOMP_DRAW_LIST") ? 1 : 0;
    if (!on || !jsrf_va_ok(root + JSRF_DRAW_ROOT_OFF + 3u)) return;

    p = *(const uint32_t *)(base + root + JSRF_DRAW_ROOT_OFF);
    while (p && jsrf_va_ok(p + JSRF_DRAW_NEXT_OFF + 3u) && n < JSRF_WALK_CAP) {
        uint32_t id = *(const uint32_t *)(base + p + 0x08u);
        if (id == 44u) saw44 = 1;
        if (id == 45u) saw45 = 1;
        p = *(const uint32_t *)(base + p + JSRF_DRAW_NEXT_OFF);
        n++;
    }
    fprintf(stderr, "  [JSRF-DRAW] list=%u id44=%s id45=%s\n",
            n, saw44 ? "present" : "ABSENT", saw45 ? "present" : "ABSENT");
    fflush(stderr);
}

static void jsrf_scene_report(void)
{
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    static uint32_t stack[JSRF_WALK_CAP];
    unsigned sp = 0, visited = 0, dead = 0, ids_live = 0, shown = 0;
    uint32_t via_ptr, root, scene, live;
    const char *how;
    unsigned i;

    if (!jsrf_scene_probe() || !base)
        return;

#define R32(va) (*(const uint32_t *)(base + (va)))

    /* Range alone is not enough to believe this pointer.
     *
     * The root literal moves between runs -- 0x005E3A70, 0x040D3A70 and
     * 0x00363A70 have all been seen -- but its LOW 16 BITS ARE INVARIANT at
     * 0x3A70, which the xemu rig's README states outright. A bare range check
     * accepts anything in RAM, and on 12 Sep it accepted 0x040FFF40 in the
     * middle of a run where the guest's own `this` was 0x040D3A70. Every field
     * this function printed from that pointer was read from the wrong object,
     * which is how a fatal flag came to be reported clear while the guest was
     * demonstrably acting on a set one. */
    root = jsrf_root_va(base, &via_ptr, &how, 1);
    if (!jsrf_va_ok(root + JSRF_LIVE_OFF + 3u)) {
        fprintf(stderr, "  [JSRF-SCENE] no usable root (ptr=%08X)\n",
                (unsigned)via_ptr);
        fflush(stderr);
        return;
    }

    live  = R32(root + JSRF_LIVE_OFF);
    scene = R32(root + JSRF_SCENE_OFF);

    jsrf_object_activity(base, root);
    jsrf_draw_list_report(base, root);
    jsrf_object_blocks(base, root);

    /* Sample the registered players outside guest execution. Function-entry
     * samples can otherwise describe a different object, or a temporary state
     * used during drawing, and be mistaken for the tutorial player's state. */
    if (getenv("RECOMP_ANIMATION_STATE")) {
        for (unsigned id = 44; id <= 45; ++id) {
            uint32_t p = R32(root + JSRF_IDS_OFF + id * 4u);
            if (!p || !jsrf_va_ok(p + 0x1178u)) continue;
            fprintf(stderr, "  [ANIMATION-STATE] id=%u self=%08X vt=%08X"
                    " own_id=%u state=%u anim=%u previous=%u phase=%u"
                    " clip=%u frame=%g blend=%g flags=%08X\n",
                    id, p, R32(p), R32(p + 8), R32(p + 0xE50),
                    R32(p + 0x11C), R32(p + 0xE6C), R32(p + 0x9C4),
                    R32(p + 0x9E4),
                    (double)*(const float *)(base + p + 0xE80),
                    (double)*(const float *)(base + p + 0x9C0),
                    R32(p + 0xE54));

            /* Does the transform block actually move?
             *
             * This is the question the second-writer hunt exists to answer,
             * and it has so far been asked only by diffing two objects across
             * a three-second window by hand. A hash of the band, kept per id
             * and compared report to report, answers it continuously and
             * costs the guest nothing -- which matters, because every
             * guest-side probe aimed at this band so far has either missed it
             * (the address is computed, so write hooks never fire) or cost
             * enough to stall the title before the tutorial.
             *
             * The self pointer is part of the state: the tutorial CONSTRUCTS
             * NEW CPlayers, so id 44/45 change object mid-run. A hash that
             * changes because the id now points at a different object is not
             * animation, and is reported as such rather than as movement. */
            {
                static uint32_t prev_self[46], prev_hash[46];
                static float prev_blk[46][63];
                static unsigned have_prev[46], static_runs[46];
                uint32_t r2d0 = R32(p + 0x2D0), r3c8 = R32(p + 0x3C8);
                uint32_t h = 2166136261u;
                const unsigned char *q =
                    (const unsigned char *)(base + p + 0xCE0u);
                const float *fv = (const float *)(base + p + 0xCE0u);
                const char *verdict;
                unsigned k;

                for (k = 0; k < 252u; ++k) { h ^= q[k]; h *= 16777619u; }

                if (!have_prev[id] || prev_self[id] != p) {
                    verdict = "new-object";
                    static_runs[id] = 0;
                } else if (prev_hash[id] != h) {
                    verdict = "MOVED";
                    static_runs[id] = 0;
                } else {
                    verdict = "static";
                    static_runs[id]++;
                }

                /* WHICH of the 21 entries moved, and by how much.
                 *
                 * "The block changed" is too coarse to settle the open
                 * question: a root position that jitters in its last mantissa
                 * bit and a skeleton that is genuinely animating both read as
                 * changed. The handover's 64-byte block diff had the same
                 * problem in the other direction -- it straddles entries, so
                 * one moving float can only be reported as a whole 64-byte
                 * region moving, or, if it lands beside enough static bytes,
                 * be described as a region that does not move at all. */
                if (have_prev[id] && prev_self[id] == p) {
                    char mask[22];
                    float worst = 0.0f;
                    unsigned moved = 0, e;

                    for (e = 0; e < 21u; ++e) {
                        float d = 0.0f;
                        unsigned c;
                        for (c = 0; c < 3u; ++c) {
                            float a = fv[e * 3u + c], b = prev_blk[id][e * 3u + c];
                            float diff = a > b ? a - b : b - a;
                            if (diff > d) d = diff;
                        }
                        mask[e] = d > 0.0f ? 'X' : '.';
                        if (d > 0.0f) moved++;
                        if (d > worst) worst = d;
                    }
                    mask[21] = 0;
                    fprintf(stderr, "  [ANIM-DELTA] id=%u %s moved=%u/21"
                            " maxdelta=%g root=%g,%g,%g\n",
                            id, mask, moved, (double)worst,
                            (double)fv[0], (double)fv[1], (double)fv[2]);
                }
                for (k = 0; k < 63u; ++k) prev_blk[id][k] = fv[k];

                have_prev[id] = 1; prev_self[id] = p; prev_hash[id] = h;

                /* -1 means the pointer did not survive the range check,
                 * which is a different claim from "the field reads zero" and
                 * has to stay distinguishable from it. */
                long r38  = jsrf_va_ok(r2d0 + 0x3Bu) ? (long)R32(r2d0 + 0x38) : -1;
                long pend = jsrf_va_ok(r3c8 + 0x3Fu) ? (long)R32(r3c8 + 0x38) : -1;
                long kind = jsrf_va_ok(r3c8 + 0x3Fu) ? (long)R32(r3c8 + 0x3C) : -1;

                fprintf(stderr, "  [ANIM-BLOCK] id=%u self=%08X pose=%08X %s"
                        " runs=%u res2D0=%08X r38=%ld res3C8=%08X pend=%ld"
                        " kind=%ld sel3CC=%u\n",
                        id, p, h, verdict, static_runs[id],
                        r2d0, r38, r3c8, pend, kind, R32(p + 0x3CC));

                /* The record at +0x3C8, which is where the selector is copied
                 * from. On xemu at this beat it reads as a position pair, a
                 * sentinel, a static descriptor pointer and a link to the other
                 * player's record, with +0x38 and +0x3C both zero. Dump it only
                 * when it changes: it is 0x50 bytes and most reports repeat. */
                {
                    static uint32_t prev_rec[46], prev_rp[46];
                    static unsigned have_rec[46];
                    if (r3c8 && jsrf_va_ok(r3c8 + 0x4Fu)) {
                        const unsigned char *rb =
                            (const unsigned char *)(base + r3c8);
                        uint32_t rh = 2166136261u;
                        unsigned k;
                        for (k = 0; k < 0x50u; ++k) {
                            rh ^= rb[k]; rh *= 16777619u;
                        }
                        if (!have_rec[id] || prev_rec[id] != rh
                                || prev_rp[id] != r3c8) {
                            fprintf(stderr, "  [ANIM-REC]   id=%u rec=%08X"
                                    " hash=%08X\n", id, r3c8, rh);
                            for (k = 0; k < 0x50u; k += 16u)
                                fprintf(stderr, "  [ANIM-REC]     +%02X: %08X"
                                        " %08X %08X %08X\n", k,
                                        R32(r3c8 + k), R32(r3c8 + k + 4),
                                        R32(r3c8 + k + 8), R32(r3c8 + k + 12));
                        }
                        have_rec[id] = 1; prev_rec[id] = rh; prev_rp[id] = r3c8;
                    }
                }
            }
        }
    }

    /* How much of the id-indexed array is populated. This counts objects that
     * were constructed at all, independently of whether the scene graph has
     * linked them in -- so "constructed but not in the tree" stays visible. */
    for (i = 0; i < JSRF_IDS_COUNT; i++) {
        uint32_t slot = root + JSRF_IDS_OFF + i * 4u;
        if (!jsrf_va_ok(slot + 3u)) break;
        if (R32(slot) != 0) ids_live++;
    }

    fprintf(stderr, "  [JSRF-SCENE] root=%08X via=%s ptr=%08X live=%u"
            " ids_populated=%u scene_root=%08X\n",
            (unsigned)root, how, (unsigned)via_ptr, (unsigned)live,
            ids_live, (unsigned)scene);

    /* Tutorial-state candidates, from the xemu snapshot diff: +0x7930/+0x7934
     * read 0xFFFFFFFF throughout the tutorial and become 1/2 once it
     * completes.  Printed here so that transition can be read straight from
     * the log -- attaching a debugger to a live run cost one session already,
     * when the SIGSTOP coincided with the guest ceasing to poll the pad. */
    if (jsrf_va_ok(root + 0x7EC4u + 3u)) {
        /* m_bFatal, CActMan+0x24. CActMan::Idle() runs IdleSub() while it is
         * clear and `readInput(); Sleep(0x10);` forever once it is set, so a
         * black screen with a live frame loop is this byte. Measured 12 Sep:
         * a fresh New Game reaches it every time, and CActMan::Fatal()
         * (0x00012770) is never called -- so one of the other ten writers of
         * [reg+0x24] is responsible and this says when. */
        fprintf(stderr, "  [JSRF-FATAL] m_bFatal=%u m_InitState=%08X"
                " m_bLogosStarted=%u\n",
                (unsigned)R32(root + 0x24u), (unsigned)R32(root + 0x10u),
                (unsigned)R32(root + 0x20u));
        fprintf(stderr, "  [JSRF-STATE] +7930=%08X +7934=%08X +7BB0=%08X"
                " +7E48=%08X +7EC4=%08X\n",
                (unsigned)R32(root + 0x7930u), (unsigned)R32(root + 0x7934u),
                (unsigned)R32(root + 0x7BB0u), (unsigned)R32(root + 0x7E48u),
                (unsigned)R32(root + 0x7EC4u));
    }

    /* Is the title PAUSED?
     *
     * CActMan +0x3C/+0x40 both read 1 in covered pause. In that mode most
     * objects do nothing by design: the pad stops being polled, characters
     * stop moving, and the run is indistinguishable from a hang or a dead
     * controller unless someone says so. A whole session was spent chasing a
     * "pad-poll stall" that was this. Printed every report so it can never be
     * mistaken again. */
    if (jsrf_va_ok(root + 0x4Cu + 3u)) {
        uint32_t p3c = R32(root + 0x3Cu), p40 = R32(root + 0x40u);
        fprintf(stderr, "  [JSRF-PAUSE] +3C=%08X +40=%08X%s\n",
                (unsigned)p3c, (unsigned)p40,
                (p3c && p40) ? "  <== PAUSED (covered pause)" : "");
    }

    /* The animation gate for the two CPlayers, read cheaply.
     *
     * Sampling this through the full object dump meant a 7668-slot walk per
     * report, and raising the report rate to catch the flag being set was
     * enough extra work on a timing-sensitive path to trip the pad-poll stall
     * three runs running. Two id lookups and four reads cost nothing, so the
     * rate can go up without perturbing the run. */
    {
        unsigned oid;
        for (oid = 44; oid <= 45; oid++) {
            uint32_t slot = root + JSRF_IDS_OFF + oid * 4u;
            uint32_t a;
            if (!jsrf_va_ok(slot + 3u)) continue;
            a = R32(slot);
            if (!jsrf_va_ok(a) || !jsrf_va_ok(a + 0x1147u)) continue;
            fprintf(stderr, "  [JSRF-GATE] id=%u obj=%08X +E54=%08X bit0=%u"
                    " +1144=%08X\n", oid, (unsigned)a,
                    (unsigned)R32(a + 0xE54u), (unsigned)(R32(a + 0xE54u) & 1u),
                    (unsigned)R32(a + 0x1144u));
        }
    }

    /* The populated id set itself, not just its cardinality: two different
     * object sets can share a count, so the count alone cannot show the
     * registry matches. */
    {
        char line[512];
        unsigned emitted = 0;
        int col = snprintf(line, sizeof line, "  [JSRF-IDS]");
        for (i = 0; i < JSRF_IDS_COUNT; i++) {
            uint32_t slot = root + JSRF_IDS_OFF + i * 4u;
            if (!jsrf_va_ok(slot + 3u)) break;
            if (R32(slot) == 0) continue;
            if (col > (int)sizeof line - 16) {
                fprintf(stderr, "%s\n", line);
                col = snprintf(line, sizeof line, "  [JSRF-IDS]");
            }
            col += snprintf(line + col, sizeof line - (size_t)col, " %u",
                            (unsigned)i);
            emitted++;
        }
        if (emitted) fprintf(stderr, "%s\n", line);
    }

    if (jsrf_va_ok(scene)) stack[sp++] = scene;
    while (sp > 0 && visited < JSRF_WALK_CAP) {
        uint32_t n = stack[--sp];
        uint32_t flags, id, child, sib;
        if (!jsrf_va_ok(n + 0x33u)) continue;
        visited++;
        flags = R32(n + 4u);
        id    = R32(n + 8u);
        child = R32(n + 0x28u);
        sib   = R32(n + 0x30u);
        if (flags & 0x80000000u) dead++;
        else if (shown < 24) {
            fprintf(stderr, "  [JSRF-SCENE]   node %08X id=%u flags=%08X\n",
                    (unsigned)n, (unsigned)id, (unsigned)flags);
            shown++;
        }
        if (jsrf_va_ok(sib)   && sp < JSRF_WALK_CAP) stack[sp++] = sib;
        if (jsrf_va_ok(child) && sp < JSRF_WALK_CAP) stack[sp++] = child;
    }

    fprintf(stderr, "  [JSRF-SCENE] nodes=%u dead=%u%s\n",
            visited, dead, visited >= JSRF_WALK_CAP ? " (CAPPED)" : "");
    fflush(stderr);
#undef R32
}

#define JSRF_ENTRY_POINT 0x00148023u
#define JSRF_THREAD_START 0x00147EBBu
#define JSRF_CALLBACK 0x00147FB4u
#define JSRF_ADDREF 0x00177FE0u
#define JSRF_RELEASE 0x001664D0u
#define JSRF_DUP_FREE_OBJECT 0x0105DE68u
#define JSRF_XBE_PATH \
    "/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)/default.xbe"
#define JSRF_GAME_DIR \
    "/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)"
#define JSRF_HDD_ROOT \
    "/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean/build-macos/jsrf-first-fault/emulated-hdd"
#define GUEST_TRACE_SIZE 32u

typedef struct GuestTraceRecord {
    uint32_t block;
    uint32_t function;
    uint32_t esp;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esi;
    uint32_t edi;
    uint32_t ebp;
} GuestTraceRecord;

static volatile GuestTraceRecord g_guest_trace[GUEST_TRACE_SIZE];
static volatile uint32_t g_guest_trace_index;
static volatile uint32_t g_first_guest_block;
static _Thread_local uint32_t g_current_guest_function;

/* Global, unlike g_current_guest_function, which is thread-local and therefore
 * always 0 when the report thread reads it. Every guest thread records here,
 * so a spinning guest shows as the same handful of functions repeating across
 * consecutive reports -- which is the only way to see a guest that neither
 * faults nor calls the kernel. */
#define GUEST_FN_RING 8u
static volatile uint32_t g_fn_ring[GUEST_FN_RING];
static volatile uint32_t g_fn_ring_index;


/* RECOMP_APU_REG_TRACE asks the model who issued each register write; only the
 * harness knows. Thread-local, and the trap runs on the guest's own thread, so
 * this is the function that issued the store we are about to trace. */
static uint32_t jsrf_guest_pc_for_apu(void)
{
    return g_current_guest_function;
}

/* RECOMP_GUEST_TRACE_REPORT -- the tail of the guest-block ring, printed with
 * the periodic report.
 *
 * The same ring is dumped by the crash handler, but only on a fault. A guest
 * that SPINS never faults and never calls the kernel, so nothing in this
 * harness says where it is; the counters all read zero and it is
 * indistinguishable from a guest that is blocked, or dead. The ring is global
 * (every thread records into it), so the entries that keep changing are the
 * ones still executing -- and a spin shows up as the same few blocks repeating.
 */
static void jsrf_guest_trace_report(void)
{
    static int on = -1;
    uint32_t count, shown, i;

    if (on < 0) on = getenv("RECOMP_GUEST_TRACE_REPORT") ? 1 : 0;
    if (!on) return;

    count = g_guest_trace_index;
    shown = count < 8u ? count : 8u;
    fprintf(stderr, "  [GUEST-RING] %u blocks recorded; last %u:\n",
            count, shown);
    for (i = 0; i < shown; ++i) {
        uint32_t seq = count - shown + i;
        volatile GuestTraceRecord *r =
            &g_guest_trace[seq & (GUEST_TRACE_SIZE - 1u)];
        fprintf(stderr, "    block=%08X in sub_%08X ESP=%08X EAX=%08X ECX=%08X\n",
                r->block, r->function, r->esp, r->eax, r->ecx);
    }
    /* The block ring is empty unless the gen was built with RECOMP_GUEST_BLOCK.
     * The function ring always has entries, so it is the one that answers
     * "where is the guest now". */
    count = g_fn_ring_index;
    shown = count < GUEST_FN_RING ? count : GUEST_FN_RING;
    fprintf(stderr, "  [GUEST-FN] %u function entries; last %u:", count, shown);
    for (i = 0; i < shown; ++i)
        fprintf(stderr, " sub_%08X",
                g_fn_ring[(count - shown + i) & (GUEST_FN_RING - 1u)]);
    fprintf(stderr, "\n");
    fflush(stderr);
}
static _Thread_local int g_entry_announced;
static _Thread_local int g_thread_start_announced;
static _Thread_local int g_callback_announced;
#if defined(_WIN32)
static volatile LONG g_handling_fault;
#else
static volatile sig_atomic_t g_handling_fault;
#endif

typedef struct RefTraceState {
    uint32_t object;
    uint32_t before;
    uint32_t sequence;
} RefTraceState;

static _Thread_local RefTraceState g_addref_trace;
static _Thread_local RefTraceState g_release_trace;
static volatile uint32_t g_ref_trace_sequence;

static void ref_trace_enter(RefTraceState *state, const char *operation)
{
    uint32_t object = MEM32(g_esp + 4);
    state->object = object;
    state->before = object == JSRF_DUP_FREE_OBJECT ? MEM32(object + 8) : 0;
    state->sequence = ++g_ref_trace_sequence;
    if (!xbox_IsXboxAddress(object)) {
        fprintf(stderr,
                "REFCOUNT #%u %s invalid-object=0x%08X caller-return=0x%08X"
                " ecx=0x%08X esi=0x%08X edi=0x%08X ebx=0x%08X"
                " stack=%08X,%08X,%08X,%08X\n",
                state->sequence, operation, object, MEM32(g_esp),
                g_ecx, g_esi, g_edi, g_ebx,
                MEM32(g_esp + 4), MEM32(g_esp + 8),
                MEM32(g_esp + 12), MEM32(g_esp + 16));
        fflush(stderr);
    }
    if (object == JSRF_DUP_FREE_OBJECT) {
        fprintf(stderr,
                "REFCOUNT #%u %s enter object=0x%08X before=%u\n",
                state->sequence, operation, object, state->before);
    }
}

static void ref_trace_exit(RefTraceState *state, const char *operation)
{
    uint32_t after = state->object == JSRF_DUP_FREE_OBJECT
        ? MEM32(state->object + 8)
        : 0;
    if (state->object == JSRF_DUP_FREE_OBJECT) {
        fprintf(stderr,
                "REFCOUNT #%u %s exit object=0x%08X %u -> %u "
                "return=%u destructor=%s\n",
                state->sequence, operation, state->object,
                state->before, after, g_eax,
                strcmp(operation, "Release") == 0 && after == 0
                    ? "yes" : "no");
    }
}

void jsrf_trace_function(uint32_t guest_function)
{
    g_fn_ring[g_fn_ring_index++ & (GUEST_FN_RING - 1u)] = guest_function;
    g_current_guest_function = guest_function;
    if (guest_function == JSRF_ENTRY_POINT && !g_entry_announced) {
        g_entry_announced = 1;
        fprintf(stderr, "ENTRY EXECUTED: 0x%08X\n", JSRF_ENTRY_POINT);
    }
    if (guest_function == JSRF_THREAD_START && !g_thread_start_announced) {
        g_thread_start_announced = 1;
        fprintf(stderr, "THREAD START EXECUTED: 0x%08X\n", JSRF_THREAD_START);
    }
    if (guest_function == JSRF_CALLBACK && !g_callback_announced) {
        g_callback_announced = 1;
        fprintf(stderr, "CALLBACK EXECUTED: 0x%08X\n", JSRF_CALLBACK);
    }
}

void recomp_trace_enter(const char *name, uint32_t guest_function)
{
    if (guest_function == JSRF_ADDREF)
        ref_trace_enter(&g_addref_trace, "AddRef");
    else if (guest_function == JSRF_RELEASE)
        ref_trace_enter(&g_release_trace, "Release");
    if (guest_function != JSRF_ADDREF && guest_function != JSRF_RELEASE)
        fprintf(stderr, "TRACE EXECUTED: %s (0x%08X)\n", name, guest_function);
    jsrf_trace_function(guest_function);
}

void recomp_trace_exit(const char *name, uint32_t guest_function)
{
    (void)name;
    if (guest_function == JSRF_ADDREF)
        ref_trace_exit(&g_addref_trace, "AddRef");
    else if (guest_function == JSRF_RELEASE)
        ref_trace_exit(&g_release_trace, "Release");
}

void recomp_trace_esp(const char *name, const char *tag)
{
    (void)name;
    (void)tag;
}

void jsrf_trace_block(uint32_t guest_block)
{
    uint32_t slot = g_guest_trace_index++ & (GUEST_TRACE_SIZE - 1u);
    volatile GuestTraceRecord *r = &g_guest_trace[slot];
    if (slot == 0 && g_first_guest_block == 0)
        g_first_guest_block = guest_block;
    r->block = guest_block;
    r->function = g_current_guest_function;
    r->esp = g_esp;
    r->eax = g_eax;
    r->ecx = g_ecx;
    r->edx = g_edx;
    r->ebx = g_ebx;
    r->esi = g_esi;
    r->edi = g_edi;
    r->ebp = g_ebp;
    if (g_current_guest_function == JSRF_THREAD_START &&
        !g_thread_start_announced) {
        uint64_t host_tid;
        g_thread_start_announced = 1;
        host_tid = GetCurrentThreadId();
        fprintf(stderr, "THREAD START RESOLVED: 0x%08X\n", JSRF_THREAD_START);
        fprintf(stderr,
                "FIRST BLOCK EXECUTED BY NEW THREAD: 0x%08X (host thread %llu)\n",
                guest_block, (unsigned long long)host_tid);
    }
    if (g_current_guest_function == JSRF_CALLBACK && !g_callback_announced) {
        g_callback_announced = 1;
        fprintf(stderr, "CALLBACK DISPATCH RESOLVED: YES\n");
        fprintf(stderr, "FIRST CALLBACK BLOCK: 0x%08X\n", guest_block);
    }
}

#if defined(_WIN32)
static LONG CALLBACK crash_handler(PEXCEPTION_POINTERS ep)
{
    uintptr_t fault = 0;
    uint32_t guest_fault = 0;
    uint32_t count = g_guest_trace_index;
    uint32_t available = count < GUEST_TRACE_SIZE ? count : GUEST_TRACE_SIZE;

    if (!ep || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters >= 2)
        fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];

    /* Windows leaves the APU's 512 KiB window inaccessible so every guest
     * register access reaches this VEH. Route it to the MCPX model before
     * treating the exception as a crash. The generated x86-64 instruction is
     * decoded by the APU hook, which advances RIP after a handled access. */
    if (xbox_HostAddressToGuest(fault, &guest_fault) &&
        guest_fault >= 0xFE800000u && guest_fault < 0xFE880000u &&
        apu_hook_handle_mmio(ep->ContextRecord, fault, guest_fault,
            ep->ExceptionRecord->ExceptionInformation[0] != 0)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* And the guarded write-semantics pages. PCRTC_INTR_0 needs
     * write-1-to-clear, while the AC97 bus-master reset registers need their
     * RR bit suppressed at the store. The latter page is also written by the
     * runtime when it applies codec-ready, so route both guest and native
     * runtime stores through the same decoder. */
    {
        extern int xbox_Nv2aHandleWin32Fault(PCONTEXT, uintptr_t, uint32_t);
        /* No address filter. The handler checks the faulting PAGE against the
         * set it guards and returns 0 for anything else, so the filter here
         * only ever duplicated that -- and it silently excluded
         * RECOMP_STORE_WATCH, which guards a page of ordinary guest RAM and so
         * can sit anywhere. A guarded page that never reaches its handler
         * reads as a hard crash at the first legitimate write. */
        if (xbox_Nv2aHandleWin32Fault(ep->ContextRecord, fault, guest_fault))
            return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (InterlockedExchange(&g_handling_fault, 1))
        return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "\n========== FIRST GUEST FAULT ==========\n");
    fprintf(stderr, "EXCEPTION: access violation (0x%08lX)\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode);
    fprintf(stderr, "HOST FAULT ADDRESS: 0x%016llX\n",
            (unsigned long long)fault);
#if defined(_M_X64) || defined(__x86_64__)
    fprintf(stderr, "HOST PC: 0x%016llX\n",
            (unsigned long long)ep->ContextRecord->Rip);
    fprintf(stderr, "HOST SP: 0x%016llX\n",
            (unsigned long long)ep->ContextRecord->Rsp);
#endif
    fprintf(stderr, "LAST INSTRUMENTED GUEST FUNCTION (may have returned): sub_%08X\n",
            g_current_guest_function);
    fprintf(stderr,
            "GUEST REGISTERS: EAX=%08X ECX=%08X EDX=%08X EBX=%08X "
            "ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp);
    if (g_esp >= (uint32_t)XBOX_STACK_BASE &&
        g_esp + 32u * 4u >= g_esp && g_esp + 32u * 4u <= XBOX_STACK_TOP) {
        fprintf(stderr, "GUEST STACK FROM ESP:");
        for (uint32_t i = 0; i < 32u; ++i)
            fprintf(stderr, " %08X", MEM32(g_esp + i * 4u));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "LAST %u GUEST BLOCKS:\n", available);
    for (uint32_t i = 0; i < available; ++i) {
        uint32_t sequence = count - available + i;
        volatile GuestTraceRecord *r =
            &g_guest_trace[sequence & (GUEST_TRACE_SIZE - 1u)];
        fprintf(stderr, "[%02u] block=%08X function=sub_%08X ESP=%08X\n",
                i, r->block, r->function, r->esp);
    }
    fprintf(stderr, "=======================================\n");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int install_crash_handlers(void)
{
    return AddVectoredExceptionHandler(1, crash_handler) != NULL;
}
#else
/* Name a host address, because on a worker thread nothing else can.
 *
 * Every translated guest function is an ordinary symbol in this binary, so
 * dladdr turns the faulting PC into `sub_00123456 +0x1c` -- which is the one
 * thing the report was missing. `LAST INSTRUMENTED GUEST FUNCTION` only names
 * the most recent function the tracer saw, and on a fault inside a thread
 * started through XapiThreadStartup that is the thread entry trampoline, many
 * frames above the code that actually died. Every such report so far has been
 * unattributable for exactly that reason.
 *
 * Read the offset before trusting the name. dladdr reports the nearest
 * PRECEDING exported symbol, so an address that is not inside any symbolised
 * function is attributed to whatever happens to sit below it, with a large
 * offset to say so. 9100 of the 9140 generated sub_* functions are external
 * symbols, so a fault in translated code lands on the right one; a fault in a
 * static helper will name the function before it. A four-digit offset means
 * the name is a neighbour, not the answer.
 *
 * dladdr is not async-signal-safe. Neither is the fprintf immediately below
 * it, nor the guest memory walk further down; this handler has always traded
 * strict safety for a report, and a deadlock here costs a crash log that was
 * previously useless anyway. Returns a leading-space suffix, or "".
 */
static const char *describe_host_address(uintptr_t address, char *out, size_t size)
{
    Dl_info info;
    if (!address || !dladdr((void *)address, &info) || !info.dli_sname)
        return "";
    snprintf(out, size, "  %s +0x%llX", info.dli_sname,
             (unsigned long long)(address - (uintptr_t)info.dli_saddr));
    return out;
}

static void crash_handler(int sig, siginfo_t *si, void *context)
{
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
    uintptr_t host_pc = 0, host_lr = 0, host_sp = 0;
    uint32_t guest_fault = 0;
    uint32_t count = g_guest_trace_index;
    uint32_t available = count < GUEST_TRACE_SIZE ? count : GUEST_TRACE_SIZE;

    if (g_handling_fault) _Exit(128 + sig);
    g_handling_fault = 1;

#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)context;
    if (uc && uc->uc_mcontext) {
        host_pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
        host_lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
        host_sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
    }
#elif defined(__APPLE__) && defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *)context;
    if (uc && uc->uc_mcontext) {
        host_pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
        host_sp = (uintptr_t)uc->uc_mcontext->__ss.__rsp;
    }
#endif
    char pc_name[256], lr_name[256];

    fprintf(stderr, "\n========== FIRST GUEST FAULT ==========\n");
    fprintf(stderr, "SIGNAL: %s (%d)\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", sig);
    fprintf(stderr, "HOST FAULT ADDRESS: 0x%016llX\n", (unsigned long long)fault);
    fprintf(stderr, "HOST PC: 0x%016llX%s\n", (unsigned long long)host_pc,
            describe_host_address(host_pc, pc_name, sizeof pc_name));
    fprintf(stderr, "HOST LR: 0x%016llX%s\n", (unsigned long long)host_lr,
            describe_host_address(host_lr, lr_name, sizeof lr_name));
    fprintf(stderr, "HOST SP: 0x%016llX\n", (unsigned long long)host_sp);
    if (xbox_HostAddressToGuest(fault, &guest_fault)) {
        fprintf(stderr, "VALID MAPPED GUEST ADDRESS: 0x%08X\n", guest_fault);
    } else {
        fprintf(stderr, "HOST ADDRESS IS NOT A VALID MAPPED GUEST ADDRESS\n");
    }
    fprintf(stderr, "LAST INSTRUMENTED GUEST FUNCTION (may have returned): sub_%08X\n", g_current_guest_function);
    if (available) {
        uint32_t last = (count - 1u) & (GUEST_TRACE_SIZE - 1u);
        fprintf(stderr, "GUEST EIP/BLOCK: 0x%08X\n", g_guest_trace[last].block);
    } else {
        fprintf(stderr, "GUEST EIP/BLOCK: unavailable\n");
    }
    fprintf(stderr,
            "GUEST REGISTERS: EAX=%08X ECX=%08X EDX=%08X EBX=%08X "
            "ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp);
    fprintf(stderr,
            "HEAP DIAGNOSTIC: head=%08X tail=%08X flags=%08X "
            "frame_args=%08X,%08X,%08X\n",
            MEM32(0x00F80180u), MEM32(0x00F80184u), MEM32(0x00F80018u),
            MEM32(g_ebp + 8u), MEM32(g_ebp + 0xCu), MEM32(g_ebp + 0x10u));
    fprintf(stderr,
            "WORKER CALLBACKS: common=%08X/%08X first=%08X/%08X "
            "second=%08X/%08X\n",
            MEM32(0x0025EFB8u), MEM32(0x0025EFBCu),
            MEM32(0x00261638u), MEM32(0x0026163Cu),
            MEM32(0x00261640u), MEM32(0x00261644u));

    fprintf(stderr, "\nLAST 32 GUEST BLOCKS (%u available):\n", available);
    for (uint32_t i = 0; i < available; i++) {
        uint32_t sequence = count - available + i;
        volatile GuestTraceRecord *r =
            &g_guest_trace[sequence & (GUEST_TRACE_SIZE - 1u)];
        fprintf(stderr,
                "[%02u] block=%08X function=sub_%08X ESP=%08X "
                "EAX=%08X ECX=%08X EDX=%08X EBX=%08X ESI=%08X "
                "EDI=%08X EBP=%08X\n",
                i, r->block, r->function, r->esp, r->eax, r->ecx, r->edx,
                r->ebx, r->esi, r->edi, r->ebp);
    }
    fprintf(stderr, "\nPRIMARY GUEST STACK CODE POINTERS:\n");
    if (g_esp >= (uint32_t)XBOX_STACK_BASE && g_esp < XBOX_STACK_TOP) {
        uint32_t stack_end = g_esp + 0x2000u;
        if (stack_end < g_esp || stack_end > XBOX_STACK_TOP)
            stack_end = XBOX_STACK_TOP;
        for (uint32_t va = g_esp; va + 4u <= stack_end; va += 4u) {
            uint32_t value = MEM32(va);
            if (value >= 0x00011000u && value < 0x0028C000u)
                fprintf(stderr, "stack[%08X] = %08X\n", va, value);
        }
    } else {
        fprintf(stderr, "guest ESP is outside the primary stack\n");
    }
    fprintf(stderr, "=======================================\n");
    fflush(stderr);

    signal(sig, SIG_DFL);
    raise(sig);
}

static int install_crash_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGSEGV, &sa, NULL) == 0 &&
           sigaction(SIGBUS, &sa, NULL) == 0;
}
#endif

static int load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return 1;
}

static int ensure_directory(const char *path)
{
#if defined(_WIN32)
    DWORD attrs;
    if (CreateDirectoryA(path, NULL))
        return 1;
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return 0;
    attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;

    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

#if defined(_WIN32)
static HWND jsrf_create_window(int visible)
{
    static const char class_name[] = "JSRFFirstFaultWindow";
    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    RECT rect = { 0, 0, 640, 480 };

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, class_name, "Jet Set Radio Future",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                NULL, NULL, instance, NULL);
    if (hwnd && visible) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
    return hwnd;
}
#endif

int main(int argc, char **argv)
{
    const char *xbe_path = argc > 1 ? argv[1] : JSRF_XBE_PATH;
    const char *game_dir = argc > 2 ? argv[2] : JSRF_GAME_DIR;
    const char *hdd_root = argc > 3 ? argv[3] : getenv("RECOMP_HDD_ROOT");
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    recomp_func_t entry;
    recomp_func_t thread_start;
    recomp_func_t callback;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== JSRF fresh-upstream first-fault diagnostic ===\n");
    printf("XBE: %s\nGame dir: %s\n", xbe_path, game_dir);

    if (!install_crash_handlers()) {
        fprintf(stderr, "failed to install SIGSEGV/SIGBUS handlers\n");
        return 1;
    }
    if (!load_xbe(xbe_path, &xbe_data, &xbe_size)) {
        fprintf(stderr, "failed to load XBE\n");
        return 1;
    }
    /* The validated streaming pusher below is the sole renderer owner.
     * MemoryLayoutInit starts a legacy scan thread; with shared physical RAM
     * it would otherwise execute the same methods concurrently a second time. */
    nv2a_pb_scan_set_external_executor(1);
    { extern void mcpx_apu_set_trace_pc_fn(uint32_t (*)(void));
      mcpx_apu_set_trace_pc_fn(jsrf_guest_pc_for_apu); }
    xbox_SetApuMmioWriteHook(apu_mmio_write_shim);
    xbox_SetApuMmioReadHook(apu_mmio_read_shim);
    /* This harness always exposes one Xbox controller on the emulated USB
     * root hub.  The host backend may still report a neutral controller when
     * no physical pad is attached, but requiring a diagnostic environment
     * variable here made an otherwise working DualShock invisible to JSRF in
     * an ordinary launch. */
#if defined(_WIN32)
    if (!getenv("RECOMP_OHCI_ATTACH"))
        _putenv_s("RECOMP_OHCI_ATTACH", "1");
#else
    if (!getenv("RECOMP_OHCI_ATTACH"))
        setenv("RECOMP_OHCI_ATTACH", "1", 0);
#endif
    /* The emulated gamepad's interrupt endpoint reads from here. Installed
     * before the memory layout brings up the MCPX aperture, so the very first
     * poll after enumeration already sees real pad state. */
    xbox_InputInit();
    xbox_SetUsbPadStateHook(usb_pad_state_shim);
    /* Put the executor's output on screen. Harmless when RECOMP_PB_EXEC is
     * unset: the getter simply reports no surface and Present just swaps. */
#if !defined(_WIN32)
    xbox_D3D8SetGuestFramebufferSource(nv2a_pb_exec_surface);
    /* Service the window's event loop whenever the guest blocks on the main
     * thread. Without it the window draws but never answers the OS, and macOS
     * shows a beachball over a running emulator -- which reads as a crash. */
    {
        extern void xbox_SetWaitPollHook(void (*)(void));
        xbox_SetWaitPollHook(jsrf_pump_host_events);
    }
#endif
    /* Diagnostic only: RECOMP_TOTAL_RAM_MB maps more than a retail console has.
     *
     * It exists to answer one question that the failure itself cannot -- whether
     * the title's demand is bounded. A working set larger than the arena settles
     * somewhere and stops; a leak fills whatever it is given. Every fix is
     * measured against the retail 64 MB, and any run that sets this is not
     * evidence of anything except which of those two it is. */
    {
        const char *ram = getenv("RECOMP_TOTAL_RAM_MB");
        if (ram) {
            long mb = strtol(ram, NULL, 10);
            if (mb >= 64 && mb <= 512) {
                xbox_SetTotalRam((size_t)mb * 1024 * 1024);
                fprintf(stderr, "  [DIAG] RECOMP_TOTAL_RAM_MB=%ld -- not a retail"
                        " configuration, for bounded-demand testing only\n", mb);
            }
        }
    }
    /* JSRF's guest heaps reserve 1+2+4+8 MB but currently commit only about
     * 57% of it. Keep those virtual ranges outside the retail 64 MB physical
     * arena while leaving ordinary and GPU-visible allocations capped there. */
    /* Opt-out for the reserve-space experiment. With the reserves held outside
     * the retail arena the title's heap runs at a different base than on
     * hardware, so its free-list history differs and different blocks carry
     * stale contents -- which is the standing explanation for the object
     * fields that read garbage here and zero in xemu. Setting
     * RECOMP_SEPARATE_RESERVE=0 puts the heap back where hardware puts it so
     * that explanation can be tested. Default is unchanged. */
    {
        const char *sep = getenv("RECOMP_SEPARATE_RESERVE");
        if (sep && strcmp(sep, "0") == 0) {
            fprintf(stderr, "  [DIAG] RECOMP_SEPARATE_RESERVE=0 -- guest heap"
                    " reserves stay inside the retail arena\n");
            xbox_EnableSeparateReserveSpace(0u);
        } else {
            xbox_EnableSeparateReserveSpace(64u * 1024u * 1024u);
        }
    }
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
#if !defined(_WIN32)
    /* JSRF uses low-heap unpinned GPU buffers, then locks them through the
     * CPU physical window. Both views must share bytes. Keep this layout
     * opt-in here: other titles can have overlapping fixed-address pools. */
    const char *physical_alias=getenv("RECOMP_PHYSICAL_HEAP_ALIAS");
    if ((!physical_alias || strcmp(physical_alias,"0")) && !xbox_EnablePhysicalHeapAlias()) {
        fprintf(stderr,"JSRF physical heap alias failed\n");
        free(xbe_data);
        return 1;
    }
#endif

    /* Bring up the MCPX APU. JSRF's statically linked DSOUND drives the audio
     * hardware directly -- it writes a command ring in guest RAM and spins on a
     * doorbell the APU is expected to clear -- so the emulated APU has to be
     * running for its init to complete. */
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    if (!g_apu_state) {
        fprintf(stderr, "APU: mcpx_apu_init_standalone failed\n");
    } else if (getenv("RECOMP_AUDIO_TEST_TONE")) {
        /* Host-output diagnostic only; never enabled during normal play. */
        mcpx_apu_play_test_tone(g_apu_state);
    }

    xbox_kernel_init();
    if (!hdd_root || !*hdd_root) hdd_root = JSRF_HDD_ROOT;
    if (!ensure_directory(hdd_root)) {
        fprintf(stderr, "failed to create emulated HDD root: %s\n", hdd_root);
        return 1;
    }
    printf("Writable emulated HDD root: %s\n", hdd_root);
    xbox_path_init(game_dir, hdd_root);
    xbox_kernel_bridge_init();

    /* Baseline the guest's code pages here, and here specifically.
     *
     * Not at the first periodic report -- that lands seconds in, and anything
     * that had already overwritten .text would be baselined as if it were the
     * original image. And not at section load either, which is where this call
     * started: xbox_kernel_bridge_init rewrites the kernel thunk table, at VA
     * 0x001C3F60, and the executable range this probe covers includes it. A
     * load-time baseline reports that page as CHANGED on the first report, on
     * macOS, every run -- our own write, read back as the guest corrupting
     * itself. Measured, not reasoned about: it fired on the first run.
     *
     * Between the two is the only point where the image is complete and
     * nothing but the guest writes to it again. */
    {
        extern void xbox_TextChecksumReport(void);
        xbox_TextChecksumReport();
    }

    /* RECOMP_DUMP_VA=<addr>[,<addr>...] prints those guest dwords once, here,
     * after the image is loaded and the runtime's own writes to it are done
     * and before a single guest instruction runs.
     *
     * It exists because a global can be wrong without anything ever writing
     * it: 0x0025EFB8 is a function pointer in .data's BSS that the vsync pump
     * calls when non-zero, and it reads 0 on macOS and 0xFFFFFF00 on Windows
     * with no guest store, no kernel call and no block copy touching it on
     * either host. Either the load leaves it different, or one of those three
     * instruments is lying; this says which without another argument. */
    {
        const char *spec = getenv("RECOMP_DUMP_VA");
        while (spec && *spec) {
            uint32_t va = (uint32_t)strtoul(spec, NULL, 0);
            const char *comma = strchr(spec, ',');
            fprintf(stderr, "  [DUMP] guest 0x%08X = 0x%08X\n", va, MEM32(va));
            spec = comma ? comma + 1 : NULL;
        }
        fflush(stderr);
    }

    /* PROBE: bring up the D3D8 HLE layer (src/d3d, OpenGL 3.3 backend on
     * POSIX). Nothing in JSRF routes through it yet -- the title runs its own
     * statically-linked D3D8 and talks to the NV2A model -- so this only
     * answers whether the layer initialises natively on this host at all,
     * which is the first half of the interception route. */
#if defined(_WIN32)
    /* ...but not on this host by default. The guest framebuffer is never
     * connected to the window here -- xbox_D3D8SetGuestFramebufferSource and
     * the event-pump hook are both POSIX-only above -- so the window can only
     * ever be a black, unresponsive rectangle, and it reads as a hang to
     * anyone watching. This build's output is the counters on stderr. Opt in
     * with RECOMP_D3D8_PROBE to check the layer still initialises. */
    /* RECOMP_D3D11 needs this block for a different reason. The accelerated
     * raster path in nv2a_d3d11.c draws through the D3D11 device this layer
     * creates, so it cannot come up until CreateDevice has. Its window is a
     * swap-chain requirement only -- nothing presents through it, the picture
     * belongs to RECOMP_FB_WINDOW -- so it stays hidden rather than becoming a
     * second black rectangle for someone to read as a hang. */
    if (getenv("RECOMP_D3D8_PROBE") || getenv("RECOMP_D3D11"))
#endif
    {
        IDirect3D8 *d3d;
#if defined(_WIN32)
        HWND hwnd = jsrf_create_window(getenv("RECOMP_D3D8_PROBE") != NULL);
#else
        xbox_d3d8_set_window_title("Jet Set Radio Future");
#endif
        d3d = xbox_Direct3DCreate8(0);
        fprintf(stderr, "  [D3D8-HLE] xbox_Direct3DCreate8 -> %p\n", (void *)d3d);
        if (d3d) {
            IDirect3DDevice8 *dev = NULL;
            D3DPRESENT_PARAMETERS pp;
            HRESULT hr;
            memset(&pp, 0, sizeof(pp));
            pp.BackBufferWidth  = 640;
            pp.BackBufferHeight = 480;
#if defined(_WIN32)
            pp.hDeviceWindow = hwnd;
            pp.Windowed = TRUE;
#endif
            hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev);
            fprintf(stderr, "  [D3D8-HLE] CreateDevice -> hr=0x%08X dev=%p\n",
                    (unsigned)hr, (void *)dev);
            /* The PGRAPH translator emits through this device; without init
             * every method returns "unhandled" and the pusher measures
             * nothing. */
            if (dev) pgraph_d3d11_init();
        }
        fflush(stderr);
    }

    /* The flat dispatch table. OFF by default, and the measurement is why.
     *
     * recomp_lookup falls back to a binary search over the function table when
     * recomp_dispatch_init() is not called -- log2(8923) is about 13 compares
     * on every indirect call -- and JSRF's render chain is entirely
     * vtable-dispatched, so that search sits underneath every frame. The
     * generator has emitted recomp_dispatch_init() for this title all along
     * and nothing here ever called it: the only caller in the tree is
     * templates/new-game/src/main.c, which this harness is not built from.
     * Upstream's template comment records Half-Life 2 spending most of its
     * static initialisation inside recomp_lookup for exactly this reason.
     *
     * So it looked like free speed, and for this title it is not.
     * MEASURED 13 Sep 2026: a 12 s `sample` of the Corn tutorial taken with
     * RECOMP_FLAT_DISPATCH=0, so every indirect call went through the search,
     * does not contain recomp_lookup AT ALL -- not in the top-of-stack list,
     * not anywhere above the 5-sample cut. The search is not a measurable cost
     * in JSRF. Half-Life 2 has 45,000 functions and a static-initialiser storm;
     * this title has 8,923 and does its dispatch from a settled scene graph.
     *
     * Left in and defaulted off rather than deleted: it costs 13.6 MiB of
     * mostly-untouched address space, one run with it enabled ended in a
     * SIGSEGV that could not be attributed (13% of archived runs fault anyway,
     * so one run proves nothing either way), and there is no measured upside to
     * weigh against even an unproven risk. RECOMP_FLAT_DISPATCH=1 turns it on;
     * a title with a bigger function table should start by turning it on and
     * re-taking that profile.
     *
     * Here, and nowhere later, if it is enabled: it swaps the lookup strategy
     * under recomp_lookup and nothing serialises that against a concurrent
     * reader, so it has to happen while this program is still single-threaded
     * -- before any CreateThread on this path and before the preflight lookups
     * below. */
    {
        const char *flat = getenv("RECOMP_FLAT_DISPATCH");
        if (!flat || !strcmp(flat, "0")) {
            /* Silent: this is the default, and a line every boot saying
             * nothing happened is how a log stops being read. */
        } else if (!recomp_dispatch_init()) {
            fprintf(stderr, "  [BOOT] flat dispatch unavailable; indirect "
                            "calls will use the binary search\n");
        } else {
            fprintf(stderr, "  [BOOT] flat dispatch built: %.1f MiB reserved\n",
                    recomp_dispatch_flat_bytes() / 1048576.0);
        }
        fflush(stderr);
    }

    jsrf_pb_put_watch_start();
    CreateThread(NULL, 0, jsrf_pushbuffer_ack, NULL, 0, NULL);
    CreateThread(NULL, 0, jsrf_adx_watch, NULL, 0, NULL);
    g_esp = XBOX_STACK_TOP;

    entry = recomp_lookup(JSRF_ENTRY_POINT);
    thread_start = recomp_lookup(JSRF_THREAD_START);
    callback = recomp_lookup(JSRF_CALLBACK);
    printf("Preflight recomp_lookup(0x%08X) = %p\n",
           JSRF_ENTRY_POINT, (void *)entry);
    printf("Preflight recomp_lookup(0x%08X) = %p\n",
           JSRF_THREAD_START, (void *)thread_start);
    printf("Preflight recomp_lookup(0x%08X) = %p\n",
           JSRF_CALLBACK, (void *)callback);
    if (!thread_start) {
        fprintf(stderr, "seeded thread start absent from dispatch\n");
        return 1;
    }
    if (!callback) {
        fprintf(stderr, "seeded callback absent from dispatch\n");
        return 1;
    }
    if (!entry) {
        fprintf(stderr, "entry point absent from dispatch\n");
        return 1;
    }
    /* RECOMP_GUARD_PAGE=<guest VA>: make that page read-only before the guest
     * runs, so the first write to it faults and the report above names the
     * host PC and the last instrumented guest function.
     *
     * The kernel import thunk table needs this. Its entries are patched once
     * at bridge init and must not change afterwards, but the entry at
     * 0x001C4034 (ObReferenceObjectByHandle) is observed holding 0x001C4038's
     * synthetic VA later in the run -- the whole run of entries reads as
     * shifted by one -- which sends the guest's kernel calls to the next
     * export along. RECOMP_KERNEL_WATCH can only say the change happened
     * across a blocking wait, which names no writer.
     */
#if !defined(_WIN32)
    {
        const char *spec = getenv("RECOMP_GUARD_PAGE");
        if (spec) {
            uint32_t va = (uint32_t)strtoul(spec, NULL, 0);
            size_t page = (size_t)sysconf(_SC_PAGESIZE);
            uint32_t base = va & ~(uint32_t)(page - 1);
            void *ptr = xbox_GpuMemoryRange(base, page);
            fprintf(stderr, "[GUARD] page 0x%08X (for 0x%08X) size=%zu ptr=%p"
                    " result=%d\n", base, va, page, ptr,
                    ptr ? mprotect(ptr, page, PROT_READ) : -1);
            fflush(stderr);
        }
    }
#endif

    printf("Starting translated entry 0x%08X with ESP 0x%08X\n",
           JSRF_ENTRY_POINT, g_esp);
    entry();
    fprintf(stderr, "Translated guest block entries: %u; first: 0x%08X\n",
            g_guest_trace_index, g_first_guest_block);
    fprintf(stderr, "translated entry returned without SIGSEGV/SIGBUS\n");
    return 0;
}
