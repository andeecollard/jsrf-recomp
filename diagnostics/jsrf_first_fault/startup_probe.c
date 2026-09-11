/* Read-only, opt-in observations at real generated-code sites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
static uint32_t read_word(uint32_t address) {
    uint32_t value=0;
    const void *p=xbox_GpuMemoryRange(address,4);
    if(p) memcpy(&value,p,4);
    return value;
}

/* D3D's notification dispatcher, sub_00193F70(device, index).
 *
 * A nine-entry switch on index-1, table at 0x001941B4. Index 5 reaches
 * 0x00194144, which is the only code that calls KeSetEvent on device+0x2440 --
 * the event the push-buffer reserve waits on at 0x00191510, and the wait that
 * currently never returns.
 *
 * The other end of that chain is measured and healthy: NV097_NO_OPERATION with
 * a nonzero parameter traps as a PGRAPH software method, 2,530 of them are
 * raised and acknowledged in a run, and many carry parameter 5. What is not
 * known is whether the interrupt path ever reaches this dispatcher, and with
 * which index. Tally them: "never called" and "called with the wrong index"
 * are different defects with different fixes.
 */
void jsrf_notify_probe(uint32_t pc, uint32_t object, uint32_t index,
                       uint32_t return_address)
{
    static unsigned long total, per_index[16];
    static unsigned reports;

    (void)pc; (void)object;
    ++total;
    if (index < 16) ++per_index[index];
    /* First few, then decades, so a dispatcher that runs once and a dispatcher
     * that runs constantly are both legible. */
    if (total <= 8 || total == 100 || total == 10000 || (total % 100000) == 0) {
        if (++reports > 40) return;
        fprintf(stderr, "[NOTIFY] #%lu index=%u caller=%08X"
                " counts 1..8: %lu %lu %lu %lu %lu %lu %lu %lu\n",
                total, index, return_address,
                per_index[1], per_index[2], per_index[3], per_index[4],
                per_index[5], per_index[6], per_index[7], per_index[8]);
        fflush(stderr);
    }
}

/* The title screen's state machine, sub_0004EF90.
 *
 * Global object id 8, vtable 0x001CAAF8. Its update is a 21-entry jump table
 * at 0x001FA008 indexed by `this+0x44`, and for three sessions that index sat
 * at 0x14 -- state 20, the terminal teardown that waits for global object id 2
 * to be destroyed and never got it, because id 2 was waiting on the ADX stream.
 *
 * Sampling the field says where it is; only a trace says whether it is moving.
 * Prints every change, and a heartbeat every 20000 unchanged visits so a state
 * that is stuck is distinguishable from one that is merely quiet -- the
 * distinction the whole stall investigation turned on.
 */
void jsrf_title_state_probe(uint32_t pc, uint32_t object, uint32_t state)
{
    extern double xbox_TraceSeconds(void);
    static uint32_t last_object, last_state = 0xFFFFFFFFu;
    static unsigned long same;

    (void)pc;
    if (object == last_object && state == last_state) {
        if (++same % 20000)
            return;
        fprintf(stderr, "[TITLE-STATE] t=%7.2f this=%08X state=0x%02X still,"
                " %lu visits\n", xbox_TraceSeconds(), object, state, same);
        fflush(stderr);
        return;
    }
    fprintf(stderr, "[TITLE-STATE] t=%7.2f this=%08X state=0x%02X"
            " (was %08X/0x%02X after %lu visits)\n",
            xbox_TraceSeconds(), object, state, last_object, last_state, same);
    fflush(stderr);
    last_object = object;
    last_state = state;
    same = 0;
}

/* sub_000147A0 -- remove one entry from an 8-slot array inside its object.
 *
 *   this+0x70  the array, 8 entries      this+0xB0  its count
 *   this+0x90  a second array, 8 entries this+0xB4  its count
 *
 * The tail is closed by a shift-down loop, `for (i = index; i < count - 1;
 * i++) a[i] = a[i+1]`, walking upwards from `this + index*4 + 0x70`. With a
 * `this` or a count that is out of range that loop writes straight through
 * whatever follows the object, and the first thing it reached in the run that
 * caught it was guest 0x001C4000 -- inside the kernel import thunk table,
 * which it shifted down by one entry. Every kernel call through a shifted
 * entry then lands on the next export along; that is how a call to
 * ObReferenceObjectByHandle arrived at ExQueryNonVolatileSetting with a
 * kernel function pointer where the Type argument belonged.
 *
 * Reports only calls that cannot be right -- a count past the array's 8 slots,
 * an index past the count, or an object that is not in mapped RAM -- so a
 * healthy run is silent. Reads guest memory through the bounds-checked
 * accessor, because a wild `this` is exactly what it is looking for.
 */
void jsrf_list_remove_probe(uint32_t pc, uint32_t object, uint32_t index,
                            uint32_t return_address)
{
    static uint32_t seen[8];
    static unsigned distinct;
    unsigned i;
    uint32_t count;

    (void)pc;
    count = xbox_GpuMemoryRange(object + 0xB0, 4) ? read_word(object + 0xB0)
                                                  : 0xFFFFFFFFu;
    if (object >= 0x10000u && count <= 8u && index < count)
        return;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == return_address)
            return;
    if (distinct >= sizeof(seen) / sizeof(seen[0]))
        return;
    seen[distinct++] = return_address;

    fprintf(stderr,
            "[LIST-REMOVE] caller=%08X this=%08X index=%u count=%u"
            " first=%08X last=%08X\n",
            return_address, object, index, count,
            object + index * 4 + 0x70,
            count && count != 0xFFFFFFFFu ? object + (count - 1) * 4 + 0x70 : 0);
    fflush(stderr);
}

/* CRI's installed-handler dispatcher, sub_00141B60.
 *
 * It calls whatever is at 0x002615E8, with the argument at 0x002615EC, and
 * does nothing when that slot is null -- which is the case for almost every
 * call, so this reports only the calls that actually dispatch. The handler
 * observed in the post-BGM runs is 0x0013F900, two bytes of `eb fe`: `jmp $`.
 * That is the stub CRI parks on a condition it does not expect to return
 * from, and it is not a detected entry point, so RECOMP_ICALL skips the call
 * and the guest runs on past a deliberate stop. The return address is the
 * only record of which middleware path decided to halt.
 *
 * Not env-gated: with the null-handler case filtered out it is silent in a
 * healthy run, and a run where it is not silent is one where that matters.
 */
void jsrf_cri_handler_probe(uint32_t pc, uint32_t handler,
                            uint32_t argument, uint32_t return_address)
{
    static uint32_t seen[8];
    static unsigned distinct;
    unsigned i;

    (void)pc;
    if (!handler)
        return;

    for (i = 0; i < distinct; ++i)
        if (seen[i] == return_address)
            return;
    if (distinct >= sizeof(seen) / sizeof(seen[0]))
        return;
    seen[distinct++] = return_address;

    fprintf(stderr, "[CRI-HANDLER] caller=%08X handler=%08X arg=%08X\n",
            return_address, handler, argument);
    fflush(stderr);
}

/* Read back the WXCI/XB disc driver's own diagnostics.
 *
 * sub_00140190 is its error reporter: it loads the callback the title
 * installed at 0x002615C4 and, only if that is non-null, forwards
 * (context, message, argument) to it. JSRF installs no callback, so the
 * function is a no-op and every message it was handed has been thrown away
 * since the port began -- including the three parameter checks in wxCiReqRd
 * and "E0109232:Timeout. (Waiting for transmission)", which is the driver
 * saying a read never completed.
 *
 * This reads the message pointer out of the caller's frame and prints it. It
 * does not install a callback: doing that would change what the guest does.
 * Repeats are counted rather than printed, because a failing read is retried
 * every frame and the interesting fact is which message and how fast.
 */
void jsrf_wxci_error_probe(uint32_t pc, uint32_t message,
                           uint32_t argument, uint32_t return_address)
{
    static int enabled = -1;
    (void)pc;
    static struct { uint32_t message, site; unsigned long count; } seen[24];
    static unsigned distinct;
    unsigned i;

    if (enabled < 0) enabled = getenv("RECOMP_WXCI_ERRORS") != NULL;
    if (!enabled) return;

    for (i = 0; i < distinct; ++i) {
        if (seen[i].message == message && seen[i].site == return_address) {
            /* Every hundredth repeat, so a retry storm is visible as a rate
             * without becoming the whole log. */
            if (++seen[i].count % 100 != 0) return;
            break;
        }
    }
    if (i == distinct) {
        if (distinct >= sizeof(seen) / sizeof(seen[0])) return;
        seen[distinct].message = message;
        seen[distinct].site = return_address;
        seen[distinct].count = 1;
        ++distinct;
    }

    {
        char text[96];
        unsigned n;
        for (n = 0; n + 1 < sizeof(text); ++n) {
            const char *p = xbox_GpuMemoryRange(message + n, 1);
            if (!p || !*p) break;
            text[n] = *p;
        }
        text[n] = 0;
        fprintf(stderr,
                "[WXCI-ERROR] count=%lu caller=%08X arg=%08X msg=%08X \"%s\"\n",
                seen[i].count, return_address, argument, message, text);
        fflush(stderr);
    }
}

/* The D3D pushbuffer free-space wait, which the sampler says is where the
 * title spends 89% of its main thread during the loading stall:
 *
 *   loc_001914F0: ecx = [edx]        ; the GPU's GET pointer, re-read
 *                 esi = edi - ecx    ; edi = PUT
 *                 cmp eax, esi ; jb loc_001914F0
 *
 * It spins until GET advances. The runtime does advance DMA_GET, but only the
 * copy in the NV2A USER aperture at 0xFD800044. Whether this loop is reading
 * that register or some other location nothing updates is the whole question,
 * and the pointer is only knowable at runtime.
 *
 * Samples rather than logs: the loop body runs millions of times a second, so
 * printing per iteration would change what is being measured.
 */
void jsrf_pushbuffer_wait_probe(uint32_t pc, uint32_t get_ptr,
                                uint32_t get_value, uint32_t put,
                                uint32_t needed)
{
    static int enabled = -1;
    static unsigned long long iterations;
    static unsigned reports;
    static uint32_t first_get_ptr, first_get_value;

    if (enabled < 0) enabled = getenv("RECOMP_PB_WAIT_TRACE") != NULL;
    if (!enabled) return;

    if (++iterations == 1) {
        first_get_ptr = get_ptr;
        first_get_value = get_value;
    }
    /* Powers of ten, so a loop that exits promptly prints once and a loop that
     * never exits reports its own divergence without flooding. */
    if (iterations != 1 && iterations % 1000000ull) return;
    if (reports++ > 40) return;

    fprintf(stderr,
            "[PB-WAIT] iter=%llu pc=%08X get_ptr=%08X get=%08X put=%08X"
            " needed=%08X outstanding=%08X get_moved=%s ptr_moved=%s\n",
            iterations, pc, get_ptr, get_value, put, needed,
            (uint32_t)(put - get_value),
            get_value == first_get_value ? "NO" : "yes",
            get_ptr == first_get_ptr ? "no" : "YES");
    fflush(stderr);
}

/* The WXCI cache index lookup, sub_00143540, at both of its exits.
 *
 * wxCiOpen calls it through the callback the title installs at 0x002615D4.
 * A miss is not survivable: the caller falls through to a `rep stosd` that
 * zeroes 0x54 dwords of the handle. At the loading boundary it misses on
 * D:\Media\Z_ADX\BGM\title.adx, the title screen's music, while the file
 * itself opens perfectly well through the ordinary path.
 *
 * The index is 16 mount slots at 0x00269D80, stride 0x30: an active flag at
 * +0x00, the prefix string at +0x18, an entry count at +0x24 and the list
 * head at +0x28. A query matches a slot when it starts with that prefix, and
 * the remainder is then compared against each name in the list.
 *
 * Dumping the table on the first miss answers the question the disassembly
 * cannot: whether the BGM directory has a slot at all, whether that slot is
 * active, and how many names it holds.
 */
static void cache_string(uint32_t address, char *out, unsigned size)
{
    unsigned n;
    for (n = 0; n + 1 < size; ++n) {
        const char *p = xbox_GpuMemoryRange(address + n, 1);
        if (!p || !*p) break;
        out[n] = *p;
    }
    out[n] = 0;
}

void jsrf_cache_lookup_probe(uint32_t pc, uint32_t path, uint32_t found)
{
    static int enabled = -1;
    static unsigned long hits, misses;
    static int dumped;
    char text[160];

    if (enabled < 0) enabled = getenv("RECOMP_CACHE_TRACE") != NULL;
    if (!enabled) return;

    if (found) { ++hits; if (hits > 8) return; }
    else ++misses;

    cache_string(path, text, sizeof(text));
    fprintf(stderr, "[CACHE-%s] hits=%lu misses=%lu entry=%08X path=\"%s\"\n",
            found ? "HIT" : "MISS", hits, misses, found, text);

    if (!found && !dumped) {
        unsigned slot;
        dumped = 1;
        fprintf(stderr, "[CACHE-TABLE] 16 slots at 0x00269D80\n");
        for (slot = 0; slot < 16; ++slot) {
            uint32_t base = 0x00269D80u + slot * 0x30u;
            uint32_t active = read_word(base);
            uint32_t prefix = read_word(base + 0x18);
            uint32_t count = read_word(base + 0x24);
            uint32_t head = read_word(base + 0x28);
            char name[96];

            if (!active && !prefix && !count) continue;
            cache_string(prefix, name, sizeof(name));
            fprintf(stderr,
                    "[CACHE-TABLE]   slot=%2u active=%08X count=%d head=%08X"
                    " prefix=\"%s\"\n",
                    slot, active, (int)count, head, name);
        }
    }
    fflush(stderr);
    (void)pc;
}

/* JSRF's vblank acknowledge spin, loc_00193E40 inside sub_00193D90.
 *
 *     [nv2a+0x600100] = ecx          ; PCRTC_INTR_0, write-1-to-clear
 *     test [nv2a+0x100], 0x1000000   ; PMC_INTR_0, the read-only summary
 *     jne  back                      ; spin until the summary clears
 *     ...  KeSetEvent(device+0x2430) ; only then signal
 *
 * The sampler puts the DPC here, and the event at device+0x2430 -- the only
 * dispatcher object the whole title uses -- is signalled under ten times in
 * ninety seconds while two worker threads wait on it with no timeout. If the
 * summary never clears, this loop never reaches the KeSetEvent below it and
 * that is the loading stall.
 *
 * Reads only; the guarded page traps writes, not loads.
 */
void jsrf_vblank_ack_probe(uint32_t pc, uint32_t regs, uint32_t pmc,
                           uint32_t pcrtc, uint32_t written)
{
    static int enabled = -1;
    static unsigned long long iterations;
    static unsigned reports;

    if (enabled < 0) enabled = getenv("RECOMP_VBLANK_ACK_TRACE") != NULL;
    if (!enabled) return;

    ++iterations;
    if (iterations != 1 && iterations % 100000ull) return;
    if (reports++ > 30) return;

    fprintf(stderr,
            "[VBLANK-ACK] iter=%llu pc=%08X nv2a=%08X pmc=%08X pcrtc=%08X"
            " wrote=%08X summary=%s\n",
            iterations, pc, regs, pmc, pcrtc, written,
            (pmc & 0x01000000u) ? "STILL SET" : "clear");
    fflush(stderr);
}

/* wxCiReqRd, the WXCI disc read request (sub_001403B0).
 *
 * The title issues 1341 NtOpenFile and matching NtClose but exactly one
 * NtReadFile in a ninety second run, so the whole "loading" phase is file
 * probing, not loading, and no asset data is ever fetched through the kernel.
 * Either the game never asks the disc layer for data, or it asks and the
 * request is never serviced. This counts the asks.
 *
 * wxCiWait's "E0109232:Timeout. (Waiting for transmission)" never fires in a
 * run, which rules out asked-and-timed-out, so the two remaining cases are
 * never-asked and asked-and-completed-without-a-kernel-read.
 */
void jsrf_read_request_probe(uint32_t pc, uint32_t handle, uint32_t buffer,
                             uint32_t sectors)
{
    static int enabled = -1;
    static unsigned long calls;

    if (enabled < 0) enabled = getenv("RECOMP_READ_REQUESTS") != NULL;
    if (!enabled) return;

    ++calls;
    if (calls > 24 && calls % 1000) return;
    fprintf(stderr,
            "[READ-REQ] call=%lu pc=%08X handle=%08X buffer=%08X sectors=%d\n",
            calls, pc, handle, buffer, (int)sectors);
    fflush(stderr);
}

/* Watch one CRI ring buffer's acquire/commit pair.
 *
 * The title screen waits on an ADX stream whose input ring buffer at
 * 0x00277180 has its whole capacity acquired as a single write block that is
 * never committed: +0x10 (what the writer may still take) falls from the full
 * 0x000D0000 to zero, +0x0C (what the reader may take) never leaves zero, and
 * both cursors stay at zero because a full-capacity acquire wraps them. The
 * file data really is in the buffer -- a valid ADX header sits at 0x00C3DF80 --
 * so the defect is the bookkeeping, not the read.
 *
 * Peeking cannot name the acquirer: the acquire and commit are vtable slots
 * +0x18 and +0x1C, reached from several call sites through pointers, and the
 * candidate owner tables run to 25 and 40 entries. The return address is the
 * whole point of this probe -- it says which of them actually ran.
 *
 * Read-only: it reads the arguments the caller has already pushed and the
 * object's own counters, and writes nothing to guest memory.
 */
void jsrf_ringbuf_probe(uint32_t pc, uint32_t object, uint32_t view,
                        uint32_t arg)
{
    static int enabled = -1;
    static uint32_t target;
    static unsigned long calls;

    /* One-shot diagnostic: stop at the first write to a suspect page once
     * ADX starts consuming its payload. Never enabled in a normal run. */
#if !defined(_WIN32)
    static int guarded;
    if (!guarded && pc == 0x13F9E0u && object == 0x277180u
        && view == 1 && arg == 0x7FFFFFFFu) {
        const char *spec = getenv("RECOMP_ADX_GUARD_PAGE");
        if (spec) {
            uint32_t va = (uint32_t)strtoul(spec, NULL, 0);
            size_t page = (size_t)sysconf(_SC_PAGESIZE);
            void *ptr = xbox_GpuMemoryRange(va, page);
            if (ptr && ((uintptr_t)ptr % page) == 0) {
                guarded = 1;
                fprintf(stderr, "[ADX-GUARD] protecting %08X size=%zu result=%d\n",
                        va, page, mprotect(ptr, page, PROT_READ));
            }
        }
    }
#endif
    if (enabled < 0) {
        const char *spec = getenv("RECOMP_RINGBUF_TRACE");
        enabled = spec != NULL;
        /* Any other ring buffer of this class can be watched by naming it;
         * the default is the ADX stream's input buffer. */
        target = (spec && *spec) ? (uint32_t)strtoul(spec, NULL, 0) : 0x00277180u;
        if (target < 0x1000u) target = 0x00277180u;
    }
    if (!enabled || object != target) return;

    /* The interesting events are the first few and any that change the
     * counters; a stream that works would otherwise flood the log. */
    ++calls;
    if (calls > 64 && calls % 500) return;
    fprintf(stderr,
            "[RINGBUF] call=%lu pc=%08X ret=%08X obj=%08X view=%u arg=%08X"
            " filled=%08X free=%08X rd=%08X wr=%08X blksize=%08X\n",
            calls, pc, read_word(g_esp), object, (unsigned)view, arg,
            read_word(object + 0x0C), read_word(object + 0x10),
            read_word(object + 0x14), read_word(object + 0x18),
            arg ? read_word(arg + 4) : 0);
    fflush(stderr);
}

/* The ADXF read server's entry, and the filter that decides to call it.
 *
 * sub_0013C070 acquires the ADX input ring buffer's entire capacity at
 * 0x0013C1C3 exactly once and never commits it. Its caller sub_0013C290 only
 * services a table entry while MEM8(f) == 1 and MEM8(f + 1) == 2, so the
 * question is whether the entry stops matching that filter while still holding
 * the block. Reading the three state bytes at the server's entry answers it:
 * a server that is still being called every tick and bailing looks nothing
 * like one that is never called again.
 *
 * Read-only. +0 and +1 are the filter bytes, +2 selects "issue a read" from
 * "poll the read for completion".
 */
void jsrf_adxf_probe(uint32_t pc, uint32_t entry)
{
    static int enabled = -1;
    static unsigned long calls;
    static uint32_t last;
    uint32_t word, handle, req, req_word;

    if (enabled < 0) enabled = getenv("RECOMP_ADXF_TRACE") != NULL;
    if (!enabled || !entry) return;

    word = read_word(entry);
    handle = read_word(entry + 8);
    req = handle ? read_word(handle + 4) : 0;
    req_word = req ? read_word(req) : 0;
    ++calls;
    /* Every state change, plus a heartbeat: the interesting run is the one
     * where this stops being called at all, and a heartbeat is what tells
     * "stopped" apart from "quiet". */
    if (calls > 16 && word == last && calls % 2000) return;
    last = word;
    fprintf(stderr,
            "[ADXF] call=%lu pc=%08X ret=%08X entry=%08X state=%02X/%02X/%02X"
            " buf=%08X handle=%08X vt=%08X done_fn=%08X want=%08X"
            " req=%08X req_status=%02X req_word=%08X"
            " io_status=%08X io_info=%08X issue=%08X pending=%08X\n",
            calls, pc, read_word(g_esp), entry,
            word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF,
            read_word(entry + 4), handle,
            read_word(handle), read_word(read_word(handle) + 0x2C),
            read_word(entry + 0x18), req, (req_word >> 8) & 0xFF, req_word,
            read_word(req + 0x12C), read_word(req + 0x130),
            read_word(req + 0x148), read_word(req + 0x14C));
    fflush(stderr);
}

/* Trace the status-2 request server at the four points surrounding its native
 * read and completion publication. Read-only and limited to one request. */
void jsrf_wxci_request_probe(uint32_t pc, uint32_t req)
{
    static int enabled = -1;
    static unsigned long calls;

    if (enabled < 0) enabled = getenv("RECOMP_WXCI_REQUEST_TRACE") != NULL;
    if (!enabled || req != 0x00273780u) return;

    ++calls;
    if (calls > 80 && calls % 1000) return;
    fprintf(stderr,
            "[WXCI-REQ] call=%lu pc=%08X req=%08X status=%02X"
            " io_status=%08X io_info=%08X issue=%08X pending=%08X\n",
            calls, pc, req, (read_word(req) >> 8) & 0xFF,
            read_word(req + 0x12C), read_word(req + 0x130),
            read_word(req + 0x148), read_word(req + 0x14C));
    fflush(stderr);
}

void jsrf_unresolved_flag_probe(uint32_t guest_function, uint32_t site)
{
    static unsigned char seen[1024];
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("RECOMP_UNRESOLVED_FLAGS") != NULL;
    if (!enabled || site >= sizeof(seen) || seen[site])
        return;
    seen[site] = 1;
    fprintf(stderr,
            "[UNRESOLVED-FLAG] executed site=%u function=0x%08X "
            "fallback=0 (branch forced not-taken)\n",
            site, guest_function);
    fflush(stderr);
}
static uint32_t startup_root_object;

void jsrf_adx_decode_probe(uint32_t pc, uint32_t stack)
{
    static unsigned calls;
    if (!getenv("RECOMP_ADX_DECODE_TRACE") || calls++ >= 16) return;
    fprintf(stderr, "[ADX-DECODE] pc=%08X esp=%08X args=", pc, stack);
    for (unsigned i = 0; i < 9; ++i)
        fprintf(stderr, "%s%08X", i ? "," : "", read_word(stack + i * 4));
    fprintf(stderr, " output-ring=");
    for (unsigned i = 0; i < 12; ++i)
        fprintf(stderr, "%s%08X", i ? "," : "", read_word(0x2771B0u + i * 4));
    fputc('\n', stderr);
}

/* Check the update call boundary even when the general ABI log has filled
 * with unrelated compiler-private calling conventions. */
static void update_call_probe(uint32_t pc)
{
    static _Thread_local struct {
        uint32_t node, target, esp, ebx, edi;
    } calls[128];
    static _Thread_local unsigned depth;
    static unsigned reports;
    if (!getenv("RECOMP_TREE_TRACE")) return;
    if (pc == 0x11083u) {
        if (depth >= 128) abort();
        calls[depth].node = g_esi;
        calls[depth].target = read_word(read_word(g_esi) + 4);
        calls[depth].esp = g_esp;
        calls[depth].ebx = g_ebx;
        calls[depth++].edi = g_edi;
    } else if (depth) {
        --depth;
        if (reports < 8 && (calls[depth].node != g_esi || calls[depth].esp != g_esp
            || calls[depth].ebx != g_ebx || calls[depth].edi != g_edi)) {
            ++reports;
            fprintf(stderr, "[UPDATE-ABI] target=%08X node=%08X->%08X esp=%08X->%08X"
                    " ebx=%08X->%08X edi=%08X->%08X\n",
                    calls[depth].target, calls[depth].node, g_esi,
                    calls[depth].esp, g_esp, calls[depth].ebx, g_ebx,
                    calls[depth].edi, g_edi);
        }
    }
}

typedef struct tree_event {
    uint32_t pc, node, related, parent, child, next, flags, vtable, method;
} tree_event_t;

void jsrf_tree_probe(uint32_t pc, uint32_t node, uint32_t related)
{
    enum { HISTORY = 128 };
    static tree_event_t history[HISTORY];
    static unsigned cursor;
    static int enabled = -1, dumped;
    tree_event_t *event;
    void *valid;

    if (enabled < 0) enabled = getenv("RECOMP_TREE_TRACE") != NULL;
    if (!enabled) return;
    if (pc == 0x1108Au) update_call_probe(pc);

    event = &history[cursor++ % HISTORY];
    memset(event, 0, sizeof *event);
    event->pc = pc;
    event->node = node;
    event->related = related;
    valid = node ? xbox_GpuMemoryRange(node, 0x34) : NULL;
    if (valid) {
        event->flags = read_word(node + 4);
        event->parent = read_word(node + 0x24);
        event->child = read_word(node + 0x28);
        event->next = read_word(node + 0x30);
        event->vtable = read_word(node);
        event->method = read_word(event->vtable + (pc == 0x11D63u ? 8 : 0));
    }

    /* 0x11D24 is immediately before the failing traversal consumes +0x28.
     * Preserve the preceding link/unlink history once, while the valid parent
     * is still readable. */
    if (!dumped && ((pc == 0x11D63u && event->method == 0x177FE0u)
                    || ((pc == 0x00011D24u || pc == 0x0001108Au)
                     && related && !xbox_IsXboxAddress(related))
                    || ((pc == 0x00011070u || pc == 0x00011B90u)
                        && node && !xbox_IsXboxAddress(node)))) {
        unsigned first = cursor > HISTORY ? cursor - HISTORY : 0;
        dumped = 1;
        fprintf(stderr,
                "[TREE] corrupt traversal pc=%08X node=%08X related=%08X flags=%08X"
                " owner=%08X next=%08X history=%u\n",
                pc, node, related, event->flags, event->parent, event->next,
                cursor - first);
        for (unsigned n = first; n < cursor; ++n) {
            tree_event_t *h = &history[n % HISTORY];
            fprintf(stderr,
                    "[TREE] #%u pc=%08X node=%08X related=%08X flags=%08X"
                    " owner=%08X child=%08X next=%08X vt=%08X method=%08X\n",
                    n + 1, h->pc, h->node, h->related, h->flags,
                    h->parent, h->child, h->next, h->vtable, h->method);
        }
        fflush(stderr);
    }
}

void jsrf_startup_probe(uint32_t pc,uint32_t object)
{
    if (pc == 0x11083u) update_call_probe(pc);
    static int enabled=-1;
    static unsigned ticks, opens, total;
    static uint32_t last_root[15];
    static struct { uint32_t object,pc,flags,target; } seen[256];
    static unsigned count;
    static int state_enabled=-1, snapshot;
    static uint32_t last_snapshot;
    if(enabled<0) enabled=getenv("RECOMP_STARTUP_TRACE")!=NULL;
    if(state_enabled<0) state_enabled=getenv("RECOMP_STARTUP_STATE")!=NULL;
    if(!enabled) return;
    if(pc==0x13a80) {
        startup_root_object=object;
        uint32_t now=GetTickCount();
        snapshot=state_enabled && (!last_snapshot || now-last_snapshot>=2000);
        if(snapshot) {
            last_snapshot=now;
            fprintf(stderr,"[STARTUP-STATE] tick=%u ms=%u root=%08X\n",ticks,now,object);
        }
        /* +0x50..+0x60 are the triggers sub_00013A80 tests each tick and
         * +0x40/+0x44 the flags it derives from them. Every one has been
         * observed stuck at zero while the title sits on the loading screen,
         * so watching the triggers themselves says whether anything upstream
         * -- input included -- ever sets one. */
        const unsigned offsets[]={0x24,0x40,0x44,0x48,0x4c,0x50,0x54,0x58,0x5c,
                                  0x60,0x74,0x94,0x7f9c,0x87dc,0x87e8};
        enum { NSTATE = 15 };
        uint32_t state[NSTATE];
        for(unsigned i=0;i<NSTATE;++i) state[i]=read_word(object+offsets[i]);
        ++ticks;
        if(ticks<=3 || memcmp(state,last_root,sizeof(state)) || ticks%10000==0) {
            fprintf(stderr,"[STARTUP] tick=%u root=%08X",ticks,object);
            for(unsigned i=0;i<NSTATE;++i) fprintf(stderr," +%04X=%08X",offsets[i],state[i]);
            fputc('\n',stderr); memcpy(last_root,state,sizeof(state));
        }
        return;
    }
    if(pc==0x25dd0) {
        if(++opens>32) return;
        char path[257]={0};
        for(unsigned i=0;i<256;++i) {
            const char *p=xbox_GpuMemoryRange(object+i,1);
            if(!p) break;
            path[i]=*p; if(!path[i]) break;
        }
        fprintf(stderr,"[STARTUP-ASSET] call=%u return=%08X pathptr=%08X path=%s\n",
                opens,read_word(g_esp),object,path);
        return;
    }
    if(!object || !xbox_GpuMemoryRange(object,128)) return;
    uint32_t flags=read_word(object+4), vtable=read_word(object);
    uint32_t target=read_word(vtable+(pc==0x11083 ? 4 : 0xc));
    if(snapshot && pc==0x11083) {
        const unsigned offsets[]={0x40,0x44,0x48,0x4c,0x50,0x54,0x58,0x5c,0x60,0x64,0x68,0x6c,0x70,0x74,0x78,0x7c,
                                  0x98,0x9c,0xa0,0xa4,0xa8,0xac,0xb0,0xb4,0xb8,0xbc,0xc0,0xc4,0xc8,
                                  0x180,0x190,0x194};
        fprintf(stderr,"[STARTUP-STATE] object=%08X vtable=%08X update=%08X flags=%08X",object,vtable,target,flags);
        for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i)
            fprintf(stderr," +%03X=%08X",offsets[i],read_word(object+offsets[i]));
        fputc('\n',stderr);
    }
    ++total;
    for(unsigned i=0;i<count;++i)
        if(seen[i].object==object && seen[i].pc==pc && seen[i].flags==flags && seen[i].target==target) return;
    if(count==256) return;
    seen[count].object=object; seen[count].pc=pc; seen[count].flags=flags; seen[count++].target=target;
    fprintf(stderr,"[STARTUP-OBJECT] pc=%08X object=%08X vtable=%08X flags=%08X target=%08X calls=%u words=",
            pc,object,vtable,flags,target,total);
    for(unsigned i=0;i<32;++i) fprintf(stderr,"%s%08X",i ? "," : "",read_word(object+i*4));
    fputc('\n',stderr);
}

/* Observe the only direct writers of the root object's transient command
 * fields.  The setters are shared by many game systems, so the object and
 * return address distinguish a genuine root transition request from an
 * unrelated call.  This probe is deliberately read-only. */
void jsrf_trigger_probe(uint32_t pc, uint32_t object, uint32_t argument)
{
    static int enabled = -1;
    static unsigned calls, root_calls;
    uint32_t return_address;

    if (enabled < 0) enabled = getenv("RECOMP_TRIGGER_TRACE") != NULL;
    if (!enabled) return;

    return_address = read_word(g_esp);
    ++calls;
    if (object == startup_root_object) ++root_calls;
    if (calls <= 512 || object == startup_root_object)
        fprintf(stderr,
                "[TRIGGER-WRITE] call=%u root_call=%u pc=%08X ret=%08X"
                " object=%08X root=%08X arg=%08X%s\n",
                calls, root_calls, pc, return_address, object,
                startup_root_object, argument,
                object == startup_root_object ? " ROOT" : "");
}

/*
 * Read-only observation of the first USB device-enumeration step. The XPP
 * root-hub path enters sub_001BF72C for a newly connected port, allocates a
 * 32-byte device object from the fixed pool at 0x264858, then calls
 * sub_001C06B3 to link it. This distinguishes "connect was never dispatched"
 * from "device pool allocation failed" without changing either outcome.
 */
void jsrf_usb_device_probe(uint32_t pc, uint32_t controller,
                           uint32_t device, uint32_t arg1, uint32_t arg2)
{
    static int enabled = -1;
    static unsigned calls[3];
    unsigned slot = pc == 0x001BF72Cu ? 0u :
                    pc == 0x001BF73Bu ? 1u : 2u;

    if (enabled < 0) enabled = getenv("RECOMP_USB_DEVICE_TRACE") != NULL;
    if (!enabled) return;
    if (++calls[slot] > 64) return;

    fprintf(stderr,
            "[USB-DEVICE] pc=%08X call=%u controller=%08X device=%08X"
            " arg1=%08X arg2=%08X pool_state=%02X pool_base=%08X"
            " list_head=%08X\n",
            pc, calls[slot], controller, device, arg1, arg2,
            read_word(0x264858) & 0xFFu, read_word(0x264938),
            read_word(0x2648D4));
}

/*
 * Follow XPP's singly linked device/event list without putting the global
 * RECOMP_MEM_WATCH hook on every guest store.  The latter has already been
 * shown to perturb this title enough to cause unrelated wild-pointer faults.
 *
 * The list head is 0x2648D4 and the link is node+0x10.  Logging the complete
 * 32-byte pool entry at the append and consume sites makes the surviving Mac
 * run a direct oracle for Windows while keeping the probe O(1) and read-only.
 */
void jsrf_usb_list_probe(uint32_t pc, uint32_t node, uint32_t related)
{
    static int enabled = -1;
    static unsigned calls;
    uint32_t head, words[8] = {0};
    int valid;

    if (enabled < 0) enabled = getenv("RECOMP_USB_LIST_TRACE") != NULL;
    if (!enabled || ++calls > 64) return;

    head = read_word(0x2648D4);
    valid = node != 0 && xbox_GpuMemoryRange(node, sizeof(words)) != NULL;
    if (valid)
        for (unsigned i = 0; i < 8; ++i)
            words[i] = read_word(node + i * 4);

    fprintf(stderr,
            "[USB-LIST] pc=%08X call=%u head=%08X tail=%08X"
            " node=%08X related=%08X valid=%d words="
            "%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
            pc, calls, head, read_word(0x2648D8), node, related, valid,
            words[0], words[1], words[2], words[3], words[4], words[5],
            words[6], words[7]);
    fflush(stderr);
}

/*
 * Read-only observation of the four-channel colour interpolator that the
 * update list reaches through vtable+4 on object 0x01A41E60.
 *
 * The 25-second run codex-startup-state-13 sampled +0x98 as zero on every
 * snapshot while +0xA8 was 1.0, +0xB8 was ~1/120 and the +0xC0 "settled" flag
 * was clear, so the update either never runs, returns early, or its result is
 * discarded. Snapshots two seconds apart cannot tell those apart. These sites
 * record the state on entry and on exit of one call, plus every call into the
 * two setters, so the difference is measured rather than inferred.
 *
 * Nothing here writes guest memory or guest registers.
 */
static void interp_read(uint32_t object, uint32_t out[6])
{
    static const unsigned offsets[6] = { 0x98, 0xA8, 0xB8, 0xBC, 0xC0, 0x9C };
    for (unsigned i = 0; i < 6; ++i) out[i] = read_word(object + offsets[i]);
}

void jsrf_interp_path_probe(uint32_t pc, uint32_t object)
{
    static int enabled = -1;
    static unsigned lines;

    if (enabled < 0) enabled = getenv("RECOMP_INTERP_PATH_TRACE") != NULL;
    if (!enabled || lines >= 256 || !object ||
        !xbox_GpuMemoryRange(object, 0xC4)) return;

    ++lines;
    fprintf(stderr,
            "[INTERP-PATH] line=%u pc=%08X object=%08X"
            " cur=%08X target=%08X step=%08X settled=%08X"
            " eax=%08X ecx=%08X edx=%08X fp_top=%u fp_cmp=%d\n",
            lines, pc, object, read_word(object + 0x98),
            read_word(object + 0xA8), read_word(object + 0xB8),
            read_word(object + 0xC0), g_eax, g_ecx, g_edx,
            g_fp_top, g_fp_cmp);
}

void jsrf_interp_probe(uint32_t pc, uint32_t object)
{
    static int enabled = -1;
    static unsigned calls[4], lines;
    static uint32_t entry_state[6], entry_object, entry_esp;
    static uint32_t last_logged[6];
    static int have_last;

    if (enabled < 0) enabled = getenv("RECOMP_INTERP_TRACE") != NULL;
    if (!enabled) return;
    if (!object || !xbox_GpuMemoryRange(object, 0xC4)) return;

    if (pc == 0x24700u) {
        ++calls[0];
        entry_object = object;
        entry_esp = g_esp;
        interp_read(object, entry_state);
        return;
    }
    if (pc == 0x24962u) {
        uint32_t now[6];
        ++calls[1];
        if (object != entry_object) return;   /* unpaired: ignore rather than guess */
        interp_read(object, now);
        int changed = memcmp(now, entry_state, sizeof(now)) != 0;
        int novel = !have_last || memcmp(entry_state, last_logged, sizeof(now)) != 0;
        if (calls[1] <= 16 || changed || novel || calls[1] % 5000 == 0) {
            if (lines++ < 4000)
                fprintf(stderr,
                        "[INTERP] call=%u object=%08X ret=%08X esp=%08X->%08X changed=%d"
                        " in +098=%08X +0A8=%08X +0B8=%08X +0BC=%08X +0C0=%08X +09C=%08X"
                        " out +098=%08X +0A8=%08X +0B8=%08X +0BC=%08X +0C0=%08X +09C=%08X\n",
                        calls[1], object, read_word(g_esp + 4), entry_esp, g_esp, changed,
                        entry_state[0], entry_state[1], entry_state[2], entry_state[3],
                        entry_state[4], entry_state[5],
                        now[0], now[1], now[2], now[3], now[4], now[5]);
            memcpy(last_logged, entry_state, sizeof(last_logged));
            have_last = 1;
        }
        return;
    }
    /* 0x24480 sets the current colour, 0x24540 sets target colour and step.
     * Both are cdecl with the colour at esp+4; 0x24540 also takes the step at
     * esp+8. Log every call: the game reaches them rarely. */
    {
        unsigned slot = pc == 0x24480u ? 2u : 3u;
        uint32_t state[6];
        if (++calls[slot] > 256) return;
        interp_read(object, state);
        fprintf(stderr,
                "[INTERP-SET] pc=%08X call=%u object=%08X ret=%08X arg1=%08X arg2=%08X"
                " before +098=%08X +0A8=%08X +0B8=%08X +0BC=%08X +0C0=%08X\n",
                pc, calls[slot], object, read_word(g_esp), read_word(g_esp + 4),
                read_word(g_esp + 8), state[0], state[1], state[2], state[3], state[4]);
    }
}

/*
 * Read-only observation of JSRF's statically linked D3D resource lifetime.
 *
 * Resource Common encodes the low-16 reference count, type in bits 16..18,
 * and lock/reference state in bits 19..22. The allocation and release vector
 * probes establish whether the title reaches its release callback; the
 * sub_00192990 probes show how the canonical Release helper decides; and the
 * sub_00192830 probes distinguish destructor entry from a real contiguous
 * free. Nothing here changes guest state.
 */
void jsrf_resource_probe(uint32_t pc, uint32_t resource, uint32_t value,
                         uint32_t size)
{
    enum { SITE_COUNT = 15, MAIN_ALLOCS = 512 };
    static const uint32_t sites[SITE_COUNT] = {
        0x0018E6E9u, 0x00191A80u, 0x00191A8Bu,
        0x00192990u, 0x001929A4u, 0x001929C6u, 0x001929D2u,
        0x00192A10u, 0x00192A5Du, 0x00192A67u,
        0x00192830u, 0x00192865u, 0x0019288Cu, 0x001928B1u,
        0
    };
    static struct {
        uint32_t resource, address, size;
        unsigned releases, lock_releases, destructors, frees;
    } main_allocs[MAIN_ALLOCS];
    static int enabled = -1;
    static unsigned counts[SITE_COUNT];
    static unsigned lines, main_count;
    unsigned slot = SITE_COUNT - 1;
    uint32_t common = 0, address = 0, parent = 0, child = 0;
    int main_index = -1;

    if (enabled < 0) enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL;
    if (!enabled) return;

    for (unsigned i = 0; i + 1 < SITE_COUNT; ++i) {
        if (sites[i] == pc) { slot = i; break; }
    }
    ++counts[slot];

    if (pc == 0x0018E6E9u && value && main_count < MAIN_ALLOCS) {
        main_allocs[main_count].resource = resource;
        main_allocs[main_count].address = value;
        main_allocs[main_count].size = size;
        main_index = (int)main_count++;
    } else {
        for (unsigned i = 0; i < main_count; ++i) {
            if (main_allocs[i].resource == resource) {
                main_index = (int)i;
                break;
            }
        }
    }

    if (resource && xbox_GpuMemoryRange(resource, 0x18)) {
        common = read_word(resource);
        address = read_word(resource + 4);
        parent = read_word(resource + 0x10);
        child = read_word(resource + 0x14);
    }

    /* Log early calls in full, then sample long runs. Destructor/free events
     * remain one-for-one because their imbalance is the question being asked. */
    if (lines < 12000 &&
        (pc == 0x0018E6E9u || counts[slot] <= 32 ||
         counts[slot] % 100 == 0 ||
         pc == 0x00192830u || pc == 0x00192865u ||
         pc == 0x0019288Cu || pc == 0x001928B1u)) {
        ++lines;
        fprintf(stderr,
                "[RESOURCE] pc=%08X call=%u resource=%08X common=%08X"
                " refs=%u type=%X locks=%X address=%08X parent=%08X child=%08X"
                " value=%08X size=%u\n",
                pc, counts[slot], resource, common, common & 0xFFFFu,
                (common >> 16) & 7u, (common >> 19) & 0xFu,
                address, parent, child, value, size);
    }

    if (main_index >= 0 && pc != 0x0018E6E9u) {
        const char *action = NULL;
        if (pc == 0x00192990u) {
            ++main_allocs[main_index].releases;
            action = "release";
        } else if (pc == 0x00192A10u) {
            ++main_allocs[main_index].lock_releases;
            action = "lock-release";
        } else if (pc == 0x00192830u) {
            ++main_allocs[main_index].destructors;
            action = "destructor";
        } else if (pc == 0x00192865u || pc == 0x0019288Cu ||
                   pc == 0x001928B1u) {
            ++main_allocs[main_index].frees;
            action = "free";
        }
        if (action && lines < 12000) {
            ++lines;
            fprintf(stderr,
                    "[RESOURCE-MAIN] action=%s index=%d resource=%08X"
                    " allocated=%08X bytes=%u common=%08X refs=%u locks=%X\n",
                    action, main_index, resource,
                    main_allocs[main_index].address,
                    main_allocs[main_index].size, common, common & 0xFFFFu,
                    (common >> 19) & 0xFu);
        }
    }

    if (lines < 12000 &&
        ((pc == 0x0018E6E9u && main_count % 50 == 0) ||
         (pc == 0x00192990u && counts[slot] % 1000 == 0))) {
        unsigned released = 0, lock_released = 0, destroyed = 0, freed = 0;
        uint64_t bytes = 0, destroyed_bytes = 0;
        for (unsigned i = 0; i < main_count; ++i) {
            bytes += main_allocs[i].size;
            if (main_allocs[i].releases) ++released;
            if (main_allocs[i].lock_releases) ++lock_released;
            if (main_allocs[i].destructors) {
                ++destroyed;
                destroyed_bytes += main_allocs[i].size;
            }
            if (main_allocs[i].frees) ++freed;
        }
        ++lines;
        fprintf(stderr,
                "[RESOURCE-SUMMARY] alloc=%u vector_enter=%u vector_exit=%u"
                " release=%u last=%u destroy=%u decrement=%u destructor=%u"
                " bind_release=%u bind_destroy=%u bind_decrement=%u"
                " free5=%u free2=%u free_other=%u"
                " main=%u/%llu_bytes main_released=%u main_bind_released=%u"
                " main_destroyed=%u/%llu_bytes main_freed=%u\n",
                counts[0], counts[1], counts[2], counts[3], counts[4],
                counts[5], counts[6], counts[10], counts[7], counts[8],
                counts[9], counts[11], counts[12], counts[13], main_count,
                (unsigned long long)bytes, released, lock_released, destroyed,
                (unsigned long long)destroyed_bytes, freed);
    }
}

/* One cache index followed for its whole life, uncapped.
 *
 * RECOMP_TEXTURE_SLOT=<index> names a slot whose every creation, store,
 * release, bind and teardown is printed regardless of the ordinary print
 * limits below. Those limits are counters: a slot created after the 512th
 * store, or bound after the eighth bind, is invisible without this, and an
 * absent line would read as "the slot was never touched". Accepts decimal or
 * 0x-prefixed hex; unset means -1, which no index can equal. */
static long jsrf_texture_slot_watch(void)
{
    static long slot = -2;
    const char *spec;

    if (slot != -2) return slot;
    spec = getenv("RECOMP_TEXTURE_SLOT");
    slot = (spec && *spec) ? (long)strtoul(spec, NULL, 0) : -1;
    return slot;
}

/* Read-only observation of the title's indexed texture cache. A store with a
 * non-zero old_resource is a replacement; sub_0014F640 should have released
 * and cleared that slot first. */
void jsrf_texture_cache_probe(uint32_t pc, uint32_t index,
                              uint32_t resource, uint32_t old_resource)
{
    static int enabled = -1;
    static unsigned releases, stores, replacements, teardowns;
    int watched;

    if (enabled < 0)
        enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL ||
                  jsrf_texture_slot_watch() >= 0;
    if (!enabled) return;
    watched = (long)index == jsrf_texture_slot_watch();

    if (pc == 0x0014F640u) {
        ++releases;
        if (watched || old_resource || releases <= 32)
            fprintf(stderr,
                    "[TEXTURE-CACHE]%s release=%u index=%u old=%08X\n",
                    watched ? " WATCH" : "", releases, index, old_resource);
        if (watched) fflush(stderr);
        return;
    }
    if (pc == 0x00154B20u) {
        ++teardowns;
        fprintf(stderr,
                "[TEXTURE-CACHE] teardown=%u slots=%u table=%08X"
                " stores=%u releases=%u replacements=%u\n",
                teardowns, resource, old_resource, stores, releases,
                replacements);
        return;
    }

    ++stores;
    if (old_resource) ++replacements;
    if (watched || stores <= 512 || old_resource)
        fprintf(stderr,
                "[TEXTURE-CACHE]%s store=%u pc=%08X index=%u new=%08X old=%08X"
                " releases=%u replacements=%u\n",
                watched ? " WATCH" : "", stores, pc, index, resource,
                old_resource, releases, replacements);
    if (watched) fflush(stderr);
}

/* Entry of the shared texture-creation body sub_0014F720, which ends by
 * publishing its result into cacheTable[index] -- either a resource pointer
 * or the 0xFFFFFFFF its local was initialised to when no resource was made.
 * Recording the attempt separates "creation was never tried for this slot"
 * from "creation was tried and failed", which the store alone cannot. */
void jsrf_texture_create_probe(uint32_t pc, uint32_t index, uint32_t arg1,
                               uint32_t arg3, uint32_t return_address)
{
    static int enabled = -1;
    static unsigned creates;

    if (enabled < 0)
        enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL ||
                  jsrf_texture_slot_watch() >= 0;
    if (!enabled) return;

    ++creates;
    if ((long)index == jsrf_texture_slot_watch() || creates <= 16) {
        fprintf(stderr,
                "[TEXTURE-CACHE]%s create=%u pc=%08X index=%u arg1=%08X"
                " arg3=%08X caller=%08X slot-was=%08X\n",
                (long)index == jsrf_texture_slot_watch() ? " WATCH" : "",
                creates, pc, index, arg1, arg3, return_address,
                read_word(read_word(0x264F68u) + index * 4u));
        fflush(stderr);
    }
}

/* A game render object resolves a texture-cache index and calls D3D's binding
 * helper with the resulting resource pointer. Windows reached the call with a
 * live index whose cache slot still held the 0xFFFFFFFF empty sentinel;
 * sub_0018DF10 accepts null but dereferences every other value. Observe the
 * lookup at its caller so the original cache index is not lost. The first few
 * healthy calls are a positive control; invalid values are always reported. */
void jsrf_texture_bind_probe(uint32_t pc, uint32_t cache_index,
                             uint32_t stage, uint32_t resource,
                             uint32_t return_address)
{
    static int enabled = -1;
    static unsigned long calls, invalid;
    static int dumped;
    int bad = resource != 0 && !xbox_IsXboxAddress(resource);
    int watched;

    if (enabled < 0)
        enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL ||
                  jsrf_texture_slot_watch() >= 0;
    if (!enabled) return;
    watched = (long)cache_index == jsrf_texture_slot_watch();

    ++calls;
    if (bad) ++invalid;
    if (watched || calls <= 8 || bad) {
        fprintf(stderr,
                "[TEXTURE-BIND]%s call=%lu pc=%08X cache-index=%u stage=%u"
                " resource=%08X caller=%08X invalid=%lu\n",
                watched ? " WATCH" : "", calls, pc, cache_index, stage,
                resource, return_address, invalid);
        fflush(stderr);
    }

    /* The first bad bind, once: the table around the offending index, its
     * base and its entry count. A slot holding 0xFFFFFFFF beside neighbours
     * holding real pointers is a creation that failed; a whole region of
     * 0xFFFFFFFF is a table that was never populated, and the two want
     * different fixes. Printed here because the guest faults immediately
     * afterwards and nothing later gets to read the table. */
    if (bad && !dumped) {
        uint32_t table = read_word(0x264F68u), count = read_word(0x264F70u);
        long first = (long)cache_index - 8, i;

        dumped = 1;
        if (first < 0) first = 0;
        fprintf(stderr, "[TEXTURE-BIND] table=%08X count=%u slots %ld..%ld:",
                table, count, first, first + 16);
        for (i = first; i <= first + 16 && (uint32_t)i < count; ++i)
            fprintf(stderr, " %ld=%08X", i,
                    read_word(table + (uint32_t)i * 4u));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

void jsrf_error_dialog_probe(uint32_t pc, uint32_t return_address,
                             uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    static int enabled = -1;
    static unsigned calls;

    if (enabled < 0) enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL;
    if (!enabled || calls++ >= 64) return;
    fprintf(stderr,
            "[ERROR-DIALOG] call=%u pc=%08X return=%08X"
            " arg1=%08X arg2=%08X arg3=%08X arg4=%08X arg5=%08X\n",
            calls, pc, return_address, arg1, arg2, arg3,
            read_word(g_esp + 0x10), read_word(g_esp + 0x14));
}

/* Which render states the title sets, and which of them actually move.
 *
 * G1 asks what computes the intro fade. Against xemu a combiner factor ramps
 * through 107 distinct values across the cards while ours holds one value for
 * the whole opening, and the guest-side question -- what writes it -- has no
 * static answer: CMGameGL::setRenderState is reached only through a vtable, so
 * its callers are invisible to the call graph.
 *
 * Deliberately NOT filtered to one state. Xbox's D3DRENDERSTATETYPE numbering
 * is not the PC one, so picking the state to watch would be an assumption
 * standing exactly where the bug is. Instead every state is counted with the
 * number of distinct values it takes, and the ramp identifies itself: it is
 * whichever state moves through many values while the cards are on screen.
 *
 * The histogram is also the positive control this project keeps paying for.
 * "The texture factor never moves" proves nothing on its own -- it reads the
 * same whether the title holds it constant or the probe is dead. It means
 * something only beside the states that do move.
 *
 * Read-only: the caller's stack is read, never written.
 */
#define RSTATE_MAX     128u   /* the guest itself rejects state >= 0x52 */
#define RSTATE_VALUES    8u   /* distinct values remembered per state */

static struct {
    unsigned long calls;
    unsigned long changes;
    uint32_t      values[RSTATE_VALUES];
    unsigned      value_count;      /* capped at RSTATE_VALUES */
    int           overflowed;       /* more distinct values than we can hold */
    uint32_t      last_value;
    uint32_t      last_caller;
} g_rstate[RSTATE_MAX];
static unsigned long g_rstate_calls, g_rstate_out_of_range;

void jsrf_render_state_probe(uint32_t pc, uint32_t state, uint32_t value,
                             uint32_t return_address)
{
    extern double xbox_TraceSeconds(void);
    static int enabled = -1;
    static double next_report;
    static double interval;

    (void)pc;
    if (enabled < 0) {
        enabled = getenv("RECOMP_RSTATE_TRACE") != NULL;
        const char *ms = getenv("RECOMP_RSTATE_REPORT_MS");
        long v = ms ? strtol(ms, NULL, 10) : 0;
        interval = (v >= 100 && v <= 600000) ? v / 1000.0 : 5.0;
    }
    if (!enabled)
        return;

    g_rstate_calls++;
    if (state >= RSTATE_MAX) {
        g_rstate_out_of_range++;
    } else {
        g_rstate[state].calls++;
        if (g_rstate[state].calls == 1 || value != g_rstate[state].last_value) {
            if (g_rstate[state].calls > 1)
                g_rstate[state].changes++;
            unsigned i;
            for (i = 0; i < g_rstate[state].value_count; i++)
                if (g_rstate[state].values[i] == value)
                    break;
            if (i == g_rstate[state].value_count) {
                if (g_rstate[state].value_count < RSTATE_VALUES)
                    g_rstate[state].values[g_rstate[state].value_count++] = value;
                else
                    g_rstate[state].overflowed = 1;
            }
        }
        g_rstate[state].last_value = value;
        g_rstate[state].last_caller = return_address;
    }

    double now = xbox_TraceSeconds();
    if (now < next_report)
        return;
    next_report = now + interval;

    /* Ordered by how much each state moves, because that is the question.
     * A state set a hundred thousand times to one value is not the fade; a
     * state set thirty times to thirty values is.
     *
     * Selection sort over an index array. The counters are cumulative and are
     * never touched here -- an earlier draft marked entries "consumed" by
     * zeroing calls, which silently reset the histogram on every report and
     * would have produced exactly the kind of counter this tree keeps having
     * to disown. */
    unsigned order[RSTATE_MAX], n = 0;
    for (unsigned s = 0; s < RSTATE_MAX; s++)
        if (g_rstate[s].calls)
            order[n++] = s;

    for (unsigned i = 0; i < n; i++) {
        unsigned best = i;
        for (unsigned j = i + 1; j < n; j++) {
            unsigned a = order[j], b = order[best];
            unsigned long a_d = g_rstate[a].value_count
                              + (g_rstate[a].overflowed ? 1000u : 0u);
            unsigned long b_d = g_rstate[b].value_count
                              + (g_rstate[b].overflowed ? 1000u : 0u);
            if (a_d > b_d
                || (a_d == b_d && g_rstate[a].changes > g_rstate[b].changes))
                best = j;
        }
        unsigned t = order[i]; order[i] = order[best]; order[best] = t;
    }

    for (unsigned i = 0; i < n && i < 12; i++) {
        unsigned s = order[i];
        fprintf(stderr, "  state 0x%02X  calls=%-8lu changes=%-6lu distinct=%u%s"
                "  last=0x%08X from=0x%08X\n",
                s, g_rstate[s].calls, g_rstate[s].changes,
                g_rstate[s].value_count, g_rstate[s].overflowed ? "+" : "",
                g_rstate[s].last_value, g_rstate[s].last_caller);
    }
    if (n > 12)
        fprintf(stderr, "  ... and %u more states set\n", n - 12);
    fflush(stderr);
}

/* The intro-card state machine, sampled where it runs.
 *
 * Opening's fields were read out of the disassembly rather than guessed:
 *   +0x98  card index      (Exec0Default switches on it; drawDefault draws
 *                           index/3, so three states per visible card)
 *   +0x9c  per-state timer (incremented per tick, compared against 0x78)
 *   +0xa0  skip flag       (set when any of the four pads has a button down)
 * +0xa8/+0xac are FileGet() handles and +0xb0/+0xb4 a language id and chapter
 * number, all written once in the constructor -- so nothing in this object can
 * ramp, which is the point. Recording the machine that DOES move gives the
 * card timing a direct comparison against the reference's 726 white-card
 * frames, and tells us whether the fade could ever have been this object's
 * job.
 *
 * It doubles as the positive control for the render-state probe beside it:
 * both are installed by the same script through the same mechanism, so if this
 * one reports and that one does not, the difference is the guest, not the
 * instrument.
 */
void jsrf_opening_probe(uint32_t pc, uint32_t object, uint32_t card,
                        uint32_t timer, uint32_t skip)
{
    extern double xbox_TraceSeconds(void);
    static int enabled = -1;
    static uint32_t last_card = 0xFFFFFFFFu, last_skip = 0xFFFFFFFFu;
    static unsigned long ticks, draws, since;

    if (enabled < 0)
        enabled = getenv("RECOMP_OPENING_TRACE") != NULL;
    if (!enabled)
        return;

    /* One site is the tick and one the draw; count them apart so "the object
     * draws but never ticks" is distinguishable from "it does neither". */
    if (pc == 0x0007E360u) ticks++; else draws++;
    since++;

    if (card == last_card && skip == last_skip)
        return;

    fprintf(stderr, "[OPENING] t=%7.2f this=%08X card=%-3u timer=%-5u skip=%u"
            "  (held %lu calls; ticks=%lu draws=%lu)\n",
            xbox_TraceSeconds(), object, card, timer, skip, since,
            ticks, draws);
    fflush(stderr);
    last_card = card;
    last_skip = skip;
    since = 0;
}
