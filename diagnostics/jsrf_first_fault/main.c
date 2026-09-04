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
#include <ucontext.h>
#include <unistd.h>
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

static uint32_t g_pb_last;
static uint32_t g_pb_ring_lo, g_pb_ring_hi;

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

static NV2APusherResult jsrf_pb_feed(uint32_t from, uint32_t to)
{
    NV2APusherResult invalid = {0, 0, 0, NV2A_PUSHER_INVALID};
    if (to <= from) return invalid;
    if (to - from > 0x100000u) return invalid;          /* implausible span */
    /* JSRF's DMA objects and allocations map this low physical range to
     * guest RAM. The opt-in CPU physical heap aliases these same bytes. */
    static uint32_t snapshot[0x100000/4];
    memcpy(snapshot, (const void *)XBOX_PTR(from), to-from);
    return nv2a_pusher_run_segment(snapshot, (to-from)/4u);
}

static int jsrf_pb_poll(void)
{
    static int stream_fault;
    if (stream_fault) return 0;
    /* sub_001912EC publishes completed packets to the DMA channel. The
     * device's +0 writer cursor can point past a header still being filled. */
    uint32_t now = MEM32(0xFD800040u);
    if (!now) return 0;

    if (!g_pb_last) {
        g_pb_ring_lo = MEM32(JSRF_PB_START_VA);
        g_pb_ring_hi = MEM32(JSRF_PB_END_VA);
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
        fflush(stderr);
        g_pb_last = g_pb_ring_lo ? g_pb_ring_lo : now;
    }

    /* A published cursor can split a packet. Advance only by the dwords
     * actually consumed, and follow the ring's jump instead of parsing its
     * unused tail as commands. Bounds come from the title's live device. */
    for (unsigned segment=0; g_pb_last!=now && segment<8; ++segment) {
        if (g_pb_ring_lo && (now<g_pb_ring_lo || now>g_pb_ring_hi)) break;
        uint32_t end = now>g_pb_last ? now : g_pb_ring_hi;
        if (!end || end<=g_pb_last) break;
        NV2APusherResult result = jsrf_pb_feed(g_pb_last, end);
        g_pb_last += result.consumed*4;
        if (result.stop==NV2A_PUSHER_JUMP) {
            if (g_pb_ring_lo && result.jump_address>=g_pb_ring_lo
                    && result.jump_address<g_pb_ring_hi && !(result.jump_address&3)
                    && result.jump_address!=g_pb_last-4) {
                g_pb_last=result.jump_address;
                continue;
            }
            stream_fault=1;
            fprintf(stderr,"[PUSHER] rejected jump %08X at %08X\n",result.jump_address,g_pb_last-4);
            break;
        }
        if (result.stop==NV2A_PUSHER_INVALID) {
            stream_fault=1;
            static unsigned errors;
            if (++errors<=4) fprintf(stderr,"[PUSHER] stopped on invalid header %08X at %08X\n",MEM32(g_pb_last),g_pb_last);
        }
        if (result.stop!=NV2A_PUSHER_END || !result.consumed) break;
        if (g_pb_last==g_pb_ring_hi && now<g_pb_last) g_pb_last=g_pb_ring_lo;
    }

    /* The guest's FLIP_STALL is the only "frame is complete" signal in the
     * ring. Present here, on the thread holding the rendering context --
     * pgraph deliberately does not do it itself. */
    if (pgraph_d3d11_take_frame()) {
        static unsigned long presented;
        pgraph_d3d11_flush();
        d3d8_PresentFrame();
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
        pad_sentinel_scan();
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

static DWORD WINAPI jsrf_pushbuffer_ack(LPVOID unused)
{
    (void)unused;
    /* This thread issues the PGRAPH draws, so it must own the GL context. */
    xbox_d3d8_make_current();
    while (!g_pushbuf_ack_stop) {
        uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
        /* Snapshot the fence before consuming its commands. Reading PUT again
         * afterwards acknowledged newer, unconsumed work and allowed the
         * producer to overwrite the ring underneath the parser. */
        uint32_t getp = dev ? MEM32(dev + JSRF_D3D_GETPTR_OFFSET) : 0;
        uint32_t submitted = dev ? MEM32(dev + JSRF_D3D_PUT_OFFSET) : 0;
        int consumed = jsrf_pb_poll();
        jsrf_pusher_report();
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


static void apu_mmio_write_shim(uint32_t offset, uint32_t value, unsigned width)
{
    if (g_apu_state) {
        mcpx_apu_mmio_write(g_apu_state, offset, value, width);
    }
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

static int pad_sentinel(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_PAD_SENTINEL") ? 1 : 0;
    return on;
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
            hits++;
        }
    }
    fprintf(stderr, "  [PAD-SENTINEL] %u cop%s of the pad report in guest RAM\n",
            hits, hits == 1 ? "y" : "ies");
    fflush(stderr);
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
    if (object >= 0xF0000000u) {
        fprintf(stderr,
                "REFCOUNT #%u %s invalid-object=0x%08X caller-return=0x%08X\n",
                state->sequence, operation, object, MEM32(g_esp));
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
    fprintf(stderr, "TRANSLATED GUEST FUNCTION: sub_%08X\n",
            g_current_guest_function);
    fprintf(stderr,
            "GUEST REGISTERS: EAX=%08X ECX=%08X EDX=%08X EBX=%08X "
            "ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp);
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

    fprintf(stderr, "\n========== FIRST GUEST FAULT ==========\n");
    fprintf(stderr, "SIGNAL: %s (%d)\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", sig);
    fprintf(stderr, "HOST FAULT ADDRESS: 0x%016llX\n", (unsigned long long)fault);
    fprintf(stderr, "HOST PC: 0x%016llX\n", (unsigned long long)host_pc);
    fprintf(stderr, "HOST LR: 0x%016llX\n", (unsigned long long)host_lr);
    fprintf(stderr, "HOST SP: 0x%016llX\n", (unsigned long long)host_sp);
    if (xbox_HostAddressToGuest(fault, &guest_fault)) {
        fprintf(stderr, "VALID MAPPED GUEST ADDRESS: 0x%08X\n", guest_fault);
    } else {
        fprintf(stderr, "HOST ADDRESS IS NOT A VALID MAPPED GUEST ADDRESS\n");
    }
    fprintf(stderr, "TRANSLATED GUEST FUNCTION: sub_%08X\n", g_current_guest_function);
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
static HWND jsrf_create_window(void)
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
    if (hwnd) {
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
    xbox_SetApuMmioWriteHook(apu_mmio_write_shim);
    /* The emulated gamepad's interrupt endpoint reads from here. Installed
     * before the memory layout brings up the MCPX aperture, so the very first
     * poll after enumeration already sees real pad state. */
    xbox_InputInit();
    xbox_SetUsbPadStateHook(usb_pad_state_shim);
    /* Put the executor's output on screen. Harmless when RECOMP_PB_EXEC is
     * unset: the getter simply reports no surface and Present just swaps. */
#if !defined(_WIN32)
    xbox_D3D8SetGuestFramebufferSource(nv2a_pb_exec_surface);
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
     * running for its init to complete. Audio output itself stays silent here:
     * off Windows the module's waveOut path is inert by design. */
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    if (!g_apu_state) {
        fprintf(stderr, "APU: mcpx_apu_init_standalone failed\n");
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

    /* PROBE: bring up the D3D8 HLE layer (src/d3d, OpenGL 3.3 backend on
     * POSIX). Nothing in JSRF routes through it yet -- the title runs its own
     * statically-linked D3D8 and talks to the NV2A model -- so this only
     * answers whether the layer initialises natively on this host at all,
     * which is the first half of the interception route. */
    {
        IDirect3D8 *d3d;
#if defined(_WIN32)
        HWND hwnd = jsrf_create_window();
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
    printf("Starting translated entry 0x%08X with ESP 0x%08X\n",
           JSRF_ENTRY_POINT, g_esp);
    entry();
    fprintf(stderr, "Translated guest block entries: %u; first: 0x%08X\n",
            g_guest_trace_index, g_first_guest_block);
    fprintf(stderr, "translated entry returned without SIGSEGV/SIGBUS\n");
    return 0;
}
