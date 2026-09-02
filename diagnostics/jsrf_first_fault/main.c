#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <sys/stat.h>

#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
#include "guest_trace.h"
#include "apu/apu.h"
#include "nv2a_pusher.h"
#include "nv2a_pgraph_d3d11.h"
#include "d3d8_xbox.h"   /* PROBE: D3D8 HLE layer */

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
 * ack targets the USER area at 0xFD800040/44, which this channel does not use.
 * The general fix is to route NV2A register writes to a model the way APU
 * writes now are, and learn the notifier address from the channel setup; until
 * that exists this is the honest stand-in, kept out of the shared runtime.
 */
#define JSRF_D3D_CHANNEL_PTR 0x0019DCE0u
#define JSRF_D3D_PUT_OFFSET  0x30u
#define JSRF_D3D_GETPTR_OFFSET 0x34u

static volatile int g_pushbuf_ack_stop;

/*
 * Drive the NV2A pusher from JSRF's live command ring.
 *
 * The ring write cursor and its limit are the D3D device's first two fields --
 * device +0x00 / +0x04, which the push primitive sub_0018E930 advances, and
 * which d3d8ltcg-device-context.md calls pb_put / pb_limit. The +0x30 / +0x34
 * pair the ack below uses are INDICES, not addresses; reading them as
 * addresses yields two-byte "ranges".
 *
 * RING DISCOVERY IS THE ONE TITLE-SPECIFIC PART of this route, and it lives
 * here rather than in src/nv2a for that reason. JSRF does not use the PFIFO
 * USER area at 0xFD800040/44, so there is nothing generic to read yet.
 *
 * The index ack is left exactly as it was and still runs. It is what the guest
 * waits on, and replacing it with real consumption timing is a separate change
 * -- this one only adds a reader alongside it.
 */
#define JSRF_PB_PUT_VA    0x0019B200u
#define JSRF_PB_LIMIT_VA  0x0019B204u
#define JSRF_PB_START_VA  0x0019B224u   /* pb_ring_start, per the BO3 map */
#define JSRF_PB_END_VA    0x0019B228u   /* pb_ring_end */

static uint32_t g_pb_last;
static uint32_t g_pb_ring_lo, g_pb_ring_hi;

static void jsrf_pb_feed(uint32_t from, uint32_t to)
{
    if (to <= from) return;
    if (to - from > 0x100000u) return;          /* implausible span */
    nv2a_pusher_run((const uint32_t *)XBOX_PTR(from), (to - from) / 4u);
}

static void jsrf_pb_poll(void)
{
    uint32_t now = MEM32(JSRF_PB_PUT_VA);
    if (!now) return;

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
        g_pb_last = now;
        return;
    }

    if (now > g_pb_last) {
        jsrf_pb_feed(g_pb_last, now);
    } else if (now < g_pb_last && g_pb_ring_lo) {
        /* Wrapped: finish the tail, then take the head. */
        jsrf_pb_feed(g_pb_last, g_pb_ring_hi);
        jsrf_pb_feed(g_pb_ring_lo, now);
    }
    g_pb_last = now;
}

/* Periodic pusher report. Separate from the ADX tick so it survives that
 * probe being removed. */
static void jsrf_pusher_report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    NV2APusherStats st;

    if (last == 0) { last = now; return; }
    if (now - last < 5000) return;
    last = now;

    nv2a_pusher_get_stats(&st);
    fprintf(stderr, "  [PUSHER] runs=%lu dwords=%lu methods=%lu "
            "unhandled=%lu bad_headers=%lu\n",
            st.runs, st.dwords, st.methods, st.unhandled, st.bad_headers);
    fflush(stderr);
    nv2a_pusher_dump_unhandled(20);
}

static DWORD WINAPI jsrf_pushbuffer_ack(LPVOID unused)
{
    (void)unused;
    /* This thread issues the PGRAPH draws, so it must own the GL context. */
    xbox_d3d8_make_current();
    while (!g_pushbuf_ack_stop) {
        uint32_t dev = MEM32(JSRF_D3D_CHANNEL_PTR);
        jsrf_pb_poll();
        jsrf_pusher_report();
        if (dev) {
            uint32_t getp = MEM32(dev + JSRF_D3D_GETPTR_OFFSET);
            if (getp) {
                uint32_t put = MEM32(dev + JSRF_D3D_PUT_OFFSET);
                uint32_t get = MEM32(getp);
                if (get != put) {
                    MEM32(getp) = put;
                }
            }
        }
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
static volatile sig_atomic_t g_handling_fault;

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
    if (g_esp >= 0x00780000u && g_esp < XBOX_STACK_TOP) {
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
    struct stat st;

    if (mkdir(path, 0755) == 0)
        return 1;
    if (errno != EEXIST)
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    const char *xbe_path = argc > 1 ? argv[1] : JSRF_XBE_PATH;
    const char *game_dir = argc > 2 ? argv[2] : JSRF_GAME_DIR;
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
    xbox_SetApuMmioWriteHook(apu_mmio_write_shim);
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();

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
    if (!ensure_directory(JSRF_HDD_ROOT)) {
        fprintf(stderr, "failed to create emulated HDD root: %s\n", JSRF_HDD_ROOT);
        return 1;
    }
    printf("Writable emulated HDD root: %s\n", JSRF_HDD_ROOT);
    xbox_path_init(game_dir, JSRF_HDD_ROOT);
    xbox_kernel_bridge_init();

    /* PROBE: bring up the D3D8 HLE layer (src/d3d, OpenGL 3.3 backend on
     * POSIX). Nothing in JSRF routes through it yet -- the title runs its own
     * statically-linked D3D8 and talks to the NV2A model -- so this only
     * answers whether the layer initialises natively on this host at all,
     * which is the first half of the interception route. */
    {
        IDirect3D8 *d3d;
        xbox_d3d8_set_window_title("Jet Set Radio Future");
        d3d = xbox_Direct3DCreate8(0);
        fprintf(stderr, "  [D3D8-HLE] xbox_Direct3DCreate8 -> %p\n", (void *)d3d);
        if (d3d) {
            IDirect3DDevice8 *dev = NULL;
            D3DPRESENT_PARAMETERS pp;
            HRESULT hr;
            memset(&pp, 0, sizeof(pp));
            pp.BackBufferWidth  = 640;
            pp.BackBufferHeight = 480;
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
