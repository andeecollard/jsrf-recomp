/* G49: JSRF's input boundary answered by the host.
 *
 * Copied into the gen by stage_dsound_census.py (the library-tagged staging,
 * lib "xinput"); RECOMP_XINPUT_LIFT=1 arms it. With it armed, the nine XAPI
 * input entry points the title calls (experiments/xinput_boundary/
 * entry_points.json) run these bodies INSTEAD of XPP's. XInitDevices never
 * starts the USB stack, so the OHCI model, the XID pad model, XPP's DPC and its
 * transfer bookkeeping all stay idle -- the defect class of G45 (a half-built
 * control TD, the XPP DPC loop) leaves the path.
 *
 * The pad comes from the same host hook the USB model reads
 * (xbox_UsbPadReadHost), so scripted harness input and the player's controller
 * arrive exactly as before, and rumble leaves through the same hook
 * (xbox_UsbPadRumbleHost).
 *
 * Device types are the title's own objects, named by the address it pushes:
 * 0x001BC860 is the gamepad type (XInputOpen's), 0x001BC7E0 XAPI's
 * g_DeviceType_MU. Memory units are reported absent; saves go to the HDD.
 *
 * Every body pops what the original pops (experiments/xinput_boundary/census.py,
 * checked at run time by the census wrapper when RECOMP_DSOUND_CENSUS=1). */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_USB_PAD_REPORT 20
int  xbox_UsbPadReadHost(uint8_t report[XBOX_USB_PAD_REPORT]);
void xbox_UsbPadRumbleHost(uint16_t left, uint16_t right);
uint32_t xbox_BridgeSetEventHandle(uint32_t token);

#define TYPE_GAMEPAD 0x001BC860u
#define TYPE_MU      0x001BC7E0u
uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment);
#define ERROR_SUCCESS              0u
#define ERROR_DEVICE_NOT_CONNECTED 1167u

static int g_on = -1;
int xil_on(void)
{
    if (g_on < 0) {
        const char *v = getenv("RECOMP_XINPUT_LIFT");
        g_on = v && strcmp(v, "1") == 0;
        fprintf(stderr, "[XINPUT-LIFT] RECOMP_XINPUT_LIFT=%s\n", g_on ? "on" : "off");
    }
    return g_on;
}

static uint32_t g_port = 0xFFFFFFFFu;
static uint32_t port(void)
{
    if (g_port == 0xFFFFFFFFu) {
        const char *e = getenv("RECOMP_XINPUT_PORT");
        g_port = e && *e ? (uint32_t)atoi(e) & 3u : 0u;
    }
    return g_port;
}

static _Atomic unsigned long g_get, g_set, g_open, g_changes_reported, g_absent, g_events;
static uint32_t g_reported_mask;             /* what XGetDevices/Changes last said */
static uint32_t g_packet;
/* THE HANDLE IS XPP'S DEVICE RECORD, so it is real guest memory shaped like
 * one: +0 the device object (zeroed here, so its "removed" flag at +4 reads
 * clear), +8 the packet number, +0x14 the last input report, +0xA3 the
 * pending-feedback byte -- the fields XPP's own XInputGetState reads. A
 * constant fake handle pointed at unmapped memory. */
static uint32_t g_handle;
static uint32_t handle_make(void)
{
    if (!g_handle) {
        g_handle = xbox_ContiguousAlloc(4096u, 4096u);
        if (g_handle) {
            for (uint32_t i = 0; i < 4096u; i += 4) MEM32(g_handle + i) = 0;
            MEM32(g_handle) = g_handle + 0x800u;
        }
    }
    return g_handle;
}
#define IS_HANDLE(h) ((h) && (h) == g_handle)
static uint8_t  g_last[18];
static int      g_opened;

#define ARG(i) MEM32(esp + 4u + 4u * (uint32_t)(i))
static void ret_(uint32_t pop, uint32_t value) { eax = value; esp += 4u + pop; }

static int read_pad(uint8_t report[XBOX_USB_PAD_REPORT])
{
    memset(report, 0, XBOX_USB_PAD_REPORT);
    return xbox_UsbPadReadHost(report);
}

/* ENUMERATION LATENCY. On the Xbox a pad present at boot is not there when
 * XInitDevices returns: XPP enumerates it over USB a little later, and the
 * title first sees it as an INSERTION from XGetDeviceChanges. JSRF's input
 * manager relies on that -- it takes its player controller from the
 * insertion. Two wrong models were tried first (23 Sep): present from the
 * first poll AND reported as an insertion there (the change path ran too
 * early in boot and corrupted the heap), and present from init with no
 * insertion at all (no crash, but Start was never acted on). So a pad becomes
 * visible RECOMP_XINPUT_ENUM_MS after XInitDevices (default 1000), and its
 * arrival is an insertion. */
#include <time.h>
static double g_init_at;
static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static int enumerated(void)
{
    static double delay = -1;
    if (delay < 0) {
        const char *e = getenv("RECOMP_XINPUT_ENUM_MS");
        delay = (e && *e ? atoi(e) : 1000) / 1000.0;
    }
    return g_init_at > 0 && now_s() - g_init_at >= delay;
}

static uint32_t mask_for(uint32_t type)
{
    uint8_t r[XBOX_USB_PAD_REPORT];
    if (type != TYPE_GAMEPAD) return 0;          /* no memory units, nothing else */
    if (!enumerated()) return 0;
    return read_pad(r) ? (1u << port()) : 0u;
}

static void report(const char *why)
{
    fprintf(stderr, "[XINPUT-LIFT] %s port=%u get=%lu set=%lu open=%lu changes=%lu absent=%lu events=%lu mask=%X\n",
            why, port(), (unsigned long)g_get, (unsigned long)g_set, (unsigned long)g_open,
            (unsigned long)g_changes_reported, (unsigned long)g_absent, (unsigned long)g_events, g_reported_mask);
    fflush(stderr);
}

/* VOID XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE) --
 * the USB stack is never started. */
void xil_XInitDevices(void)
{
    static int once;
    if (g_init_at <= 0) g_init_at = now_s();
    g_reported_mask = 0;                         /* nothing enumerated yet */
    MEM32(TYPE_GAMEPAD) = 0; MEM32(TYPE_GAMEPAD + 4u) = 0; MEM32(TYPE_GAMEPAD + 8u) = 0;
    if (!once++)
        fprintf(stderr, "[XINPUT-LIFT] XInitDevices(%u, %08X): USB stack not started; pad on port %u"
                        " enumerates after RECOMP_XINPUT_ENUM_MS\n", ARG(0), ARG(1), port());
    ret_(8, 0);
}

/* THE DEVICE-TYPE OBJECT IS SHARED STATE, NOT JUST AN ARGUMENT.
 *
 * XPP keeps each type's connection masks in the type object itself:
 * +0 Current, +4 Change, +8 Previous. The title reads them directly -- its
 * pad-open routine does `mov eax, [0x1BC860]` to record the connected ports
 * -- so a lift that answers XGetDevices from a private copy leaves the title
 * reading zeros, and it then never acts on the pad (23 Sep: Start delivered
 * by XInputGetState on every press, the attract loop never left). So the
 * enumeration below updates the guest object exactly as XPP's insertion and
 * removal paths would, and XGetDevices / XGetDeviceChanges are the originals'
 * arithmetic over it (0x1BD5FF, 0x1BD621). */
static void sync_type(void)
{
    uint32_t cur = mask_for(TYPE_GAMEPAD), was = MEM32(TYPE_GAMEPAD);
    if (cur != was) {
        MEM32(TYPE_GAMEPAD) = cur;
        MEM32(TYPE_GAMEPAD + 4u) |= cur ^ was;
        atomic_fetch_add(&g_changes_reported, 1);
        fprintf(stderr, "[XINPUT-LIFT] %s on port %u\n", cur & ~was ? "pad inserted" : "pad removed", port());
    }
    g_reported_mask = cur;
}

/* DWORD XGetDevices(PXPP_DEVICE_TYPE): Previous := Current, Change := 0. */
void xil_XGetDevices(void)
{
    uint32_t t = ARG(0), cur;
    if (t == TYPE_GAMEPAD) sync_type();
    cur = MEM32(t);
    MEM32(t + 4u) = 0;
    MEM32(t + 8u) = cur;
    ret_(4, cur);
}

/* BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE, PDWORD pInsertions, PDWORD pRemovals) */
void xil_XGetDeviceChanges(void)
{
    uint32_t t = ARG(0), pins = ARG(1), prem = ARG(2);
    if (t == TYPE_GAMEPAD) sync_type();
    uint32_t cur = MEM32(t), chg = MEM32(t + 4u), prev = MEM32(t + 8u), ins = 0, rem = 0;
    if (chg) {
        uint32_t both = chg & prev & cur;            /* removed and re-inserted */
        ins = (cur & ~prev) | both;
        rem = (~cur & prev) | both;
        MEM32(t + 4u) = 0;
        MEM32(t + 8u) = cur;
    }
    if (pins) MEM32(pins) = ins;
    if (prem) MEM32(prem) = rem;
    ret_(12, (ins | rem) ? 1u : 0u);
}

/* HANDLE XInputOpen(PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot, PXINPUT_POLLING_PARAMETERS) */
void xil_XInputOpen(void)
{
    uint32_t type = ARG(0), p = ARG(1), h = 0;
    atomic_fetch_add(&g_open, 1);
    if (type == TYPE_GAMEPAD && p == port() && mask_for(type)) { h = handle_make(); g_opened = 1; }
    fprintf(stderr, "[XINPUT-LIFT] XInputOpen(type=%08X port=%u slot=%u) -> %08X\n", type, p, ARG(2), h);
    ret_(16, h);
}

/* VOID XInputClose(HANDLE) */
void xil_XInputClose(void)
{
    if (IS_HANDLE(ARG(0))) g_opened = 0;
    report("close");
    ret_(4, 0);
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES).
 *
 * PACKED, 25 bytes -- read off XPP's own body (0x1C3C22), which zeroes six
 * dwords and a byte, stores SubType at +0 and Reserved (a WORD) at +1, fetches
 * the input report to +1 (so In.Gamepad, after its 2-byte report header, is
 * at +3) and the output report to +0x13 (Out.Rumble at +0x15). The first
 * version used natural alignment -- In at +4, Out at +22, 26 bytes -- and its
 * last byte landed on the next heap block's header: the title then asked
 * NtAllocateVirtualMemory for 3.2 GB and faulted (23 Sep). */
void xil_XInputGetCapabilities(void)
{
    uint32_t c = ARG(1);
    if (!IS_HANDLE(ARG(0))) { ret_(8, ERROR_DEVICE_NOT_CONNECTED); return; }
    if (c) {
        for (uint32_t i = 0; i < 25; ++i) MEM8(c + i) = 0;
        /* As the USB model answers: SubType is its XID bSubType (2, the
         * standard "S" controller) and every capability bit is set. */
        MEM8(c) = 2;
        MEM16(c + 3u) = 0xFFFF;
        for (uint32_t i = 0; i < 8; ++i) MEM8(c + 5u + i) = 0xFF;
        for (uint32_t i = 0; i < 4; ++i) MEM16(c + 13u + 2u * i) = 0xFFFF;
        MEM16(c + 21u) = 0xFFFF; MEM16(c + 23u) = 0xFFFF;
    }
    ret_(8, ERROR_SUCCESS);
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE): dwPacketNumber +0, then
 * XINPUT_GAMEPAD, which is the XID report from byte 2 on, field for field. */
void xil_XInputGetState(void)
{
    uint32_t s = ARG(1);
    uint8_t r[XBOX_USB_PAD_REPORT];
    atomic_fetch_add(&g_get, 1);
    if (!IS_HANDLE(ARG(0)) || !read_pad(r)) {
        atomic_fetch_add(&g_absent, 1);
        ret_(8, ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (memcmp(g_last, r + 2, 18)) {
        static unsigned shown;
        memcpy(g_last, r + 2, 18); g_packet++;
        if (shown < 12 && (r[2] | r[3] | r[4] | r[5])) {
            shown++;
            fprintf(stderr, "[XINPUT-LIFT] state #%u to %08X: buttons=%02X%02X A=%u B=%u\n",
                    g_packet, s, r[3], r[2], r[4], r[5]);
        }
    }
    if (s) {
        MEM32(s) = g_packet;
        for (uint32_t i = 0; i < 18; ++i) MEM8(s + 4u + i) = r[2 + i];
    }
    MEM32(g_handle + 8u) = g_packet;
    for (uint32_t i = 0; i < 18; ++i) MEM8(g_handle + 0x14u + i) = r[2 + i];
    if ((atomic_load(&g_get) % 3000u) == 0) report("periodic");
    ret_(8, ERROR_SUCCESS);
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK): completed at once, as Cxbx
 * does -- dwStatus +0 := success, hEvent +4 signalled; rumble motors at +66
 * and +68, after the 66-byte header. */
void xil_XInputSetState(void)
{
    uint32_t f = ARG(1);
    atomic_fetch_add(&g_set, 1);
    if (!IS_HANDLE(ARG(0))) {
        if (f) MEM32(f) = ERROR_DEVICE_NOT_CONNECTED;
        ret_(8, ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    if (f) {
        uint16_t l = MEM16(f + 66u), r = MEM16(f + 68u);
        uint32_t ev = MEM32(f + 4u);
        MEM8(f + 64u) = 0;              /* bReportId, as XPP's body sets it */
        MEM8(f + 65u) = 6;              /* bLength: 2 + the 4-byte rumble report */
        xbox_UsbPadRumbleHost(l, r);
        MEM32(f) = ERROR_SUCCESS;
        if (ev) { atomic_fetch_add(&g_events, 1); xbox_BridgeSetEventHandle(ev); }
        if (atomic_load(&g_set) <= 4)
            fprintf(stderr, "[XINPUT-LIFT] rumble left=%u right=%u event=%08X\n", l, r, ev);
    }
    ret_(8, ERROR_SUCCESS);
}

/* DWORD XInputPoll(HANDLE): state is read live, so there is nothing to poll. */
void xil_XInputPoll(void) { ret_(4, ERROR_SUCCESS); }
