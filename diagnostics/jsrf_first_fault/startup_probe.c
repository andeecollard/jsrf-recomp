/* Read-only, opt-in observations at real generated-code sites. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
static uint32_t read_word(uint32_t address) {
    uint32_t value=0;
    const void *p=xbox_GpuMemoryRange(address,4);
    if(p) memcpy(&value,p,4);
    return value;
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

void jsrf_startup_probe(uint32_t pc,uint32_t object)
{
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

/* Read-only observation of the title's indexed texture cache. A store with a
 * non-zero old_resource is a replacement; sub_0014F640 should have released
 * and cleared that slot first. */
void jsrf_texture_cache_probe(uint32_t pc, uint32_t index,
                              uint32_t resource, uint32_t old_resource)
{
    static int enabled = -1;
    static unsigned releases, stores, replacements, teardowns;

    if (enabled < 0) enabled = getenv("RECOMP_RESOURCE_TRACE") != NULL;
    if (!enabled) return;

    if (pc == 0x0014F640u) {
        ++releases;
        if (old_resource || releases <= 32)
            fprintf(stderr,
                    "[TEXTURE-CACHE] release=%u index=%u old=%08X\n",
                    releases, index, old_resource);
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
    if (stores <= 512 || old_resource)
        fprintf(stderr,
                "[TEXTURE-CACHE] store=%u pc=%08X index=%u new=%08X old=%08X"
                " releases=%u replacements=%u\n",
                stores, pc, index, resource, old_resource, releases,
                replacements);
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
