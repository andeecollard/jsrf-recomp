/**
 * OHCI transfer service and the Xbox gamepad behind it. See xbox_usb_ohci.h.
 *
 * The shape of this file follows the evidence in
 * build-macos/jsrf-first-fault/render-investigation/codex-usb-transfer-dump-04:
 * XPP publishes a control ED at 0x009E29A0 with the skip bit set and an empty
 * TD list, then removes skip, installs three TDs and writes ControlListFilled.
 * The first request it asks is the ordinary address-0 probe,
 *
 *     80 06 00 01 00 00 08 00
 *     GET_DESCRIPTOR(DEVICE), first eight bytes
 *
 * on an ED with FunctionAddress 0, EndpointNumber 0 and MaxPacketSize 8. So
 * the list walker below is written against a real captured schedule rather
 * than against the specification's full generality: it retires TDs
 * synchronously in one pass, because a schedule that completes instantly is
 * indistinguishable to the driver from one that completed in the frame it
 * asked for.
 */
#include "xbox_usb_ohci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Descriptor field accessors
 * ================================================================
 *
 * OHCI endpoint descriptor, dword 0:
 *   0-6 FunctionAddress, 7-10 EndpointNumber, 11-12 Direction, 13 Speed,
 *   14 sKip, 15 Format, 16-26 MaximumPacketSize.
 * Transfer descriptor, dword 0:
 *   18 bufferRounding, 19-20 Direction/PID, 21-23 DelayInterrupt,
 *   24-25 DataToggle, 26-27 ErrorCount, 28-31 ConditionCode.
 */
#define ED_SKIP          0x00004000u
#define ED_FA(f)         ((f) & 0x7Fu)
#define ED_EN(f)         (((f) >> 7) & 0x0Fu)
#define ED_MPS(f)        (((f) >> 16) & 0x7FFu)
#define ED_HEAD_HALTED   0x1u
#define ED_HEAD_CARRY    0x2u

#define TD_ROUNDING(f)   (((f) >> 18) & 0x1u)
#define TD_PID(f)        (((f) >> 19) & 0x3u)
#define TD_PID_SETUP     0x0u
#define TD_PID_OUT       0x1u
#define TD_PID_IN        0x2u
#define TD_TOGGLE(f)     (((f) >> 24) & 0x3u)

/* A list with a cycle in it, or a descriptor chain the title is in the middle
 * of rebuilding, must not spin inside a signal handler. Both bounds are far
 * above anything an enumeration produces. */
#define OHCI_MAX_EDS 32u
#define OHCI_MAX_TDS 64u

static int g_trace;

void xbox_UsbOhciInit(void)
{
    static int resolved;
    if (resolved)
        return;
    resolved = 1;
    g_trace = getenv("RECOMP_OHCI_TRANSFER_TRACE") != NULL;
}

/* Map a guest range onto the RAM window, or NULL if any of it lies outside.
 *
 * Every pointer that reaches this comes out of a descriptor the title wrote,
 * so "outside" is a real possibility and not a defensive formality. */
static void *ohci_at(const xbox_ohci_service *s, uint32_t va, uint32_t bytes)
{
    if (!va || bytes == 0)
        return NULL;
    if (va >= s->ram_size || bytes > s->ram_size - va)
        return NULL;
    return s->ram + va;
}

/* ================================================================
 * The device
 * ================================================================ */

/* An Xbox controller as it describes itself. bDeviceClass is zero and the
 * class lives on the interface, which is where XID sits: class 0x58, subclass
 * 0x42. bMaxPacketSize0 is 8, and the ED the title built says 8 too -- the one
 * field in here the capture already confirms. */
static const uint8_t USB_DEVICE_DESC[18] = {
    0x12, 0x01,             /* bLength, DEVICE */
    0x10, 0x01,             /* bcdUSB 1.10 */
    0x00, 0x00, 0x00,       /* class, subclass, protocol: deferred to interface */
    0x08,                   /* bMaxPacketSize0 */
    0x5E, 0x04,             /* idVendor  0x045E Microsoft */
    0x89, 0x02,             /* idProduct 0x0289 Controller S */
    0x21, 0x01,             /* bcdDevice 1.21 */
    0x00, 0x00, 0x00,       /* iManufacturer, iProduct, iSerialNumber: none */
    0x01                    /* bNumConfigurations */
};

/* One configuration, one interface, two interrupt endpoints: EP1 IN carries
 * the 20-byte pad report, EP2 OUT carries rumble.
 *
 * KNOWN INCOMPLETE. JSRF asks for 0x50 bytes of configuration set and builds
 * exactly ten eight-byte IN transfer descriptors for it, which is what a
 * driver does when it knows the length rather than guessing at a buffer. That
 * is evidence the real controller's set is 80 bytes and this 32-byte one is
 * missing something -- plausibly the two expansion-slot interfaces an Xbox pad
 * carries for memory units. Nothing here invents them: 32 bytes of descriptor
 * that are right beats 80 bytes that are half guessed, and the short read is
 * absorbed in ohci_run_td instead. Replace this with a real dump when one is
 * available, and the note there can go with it. */
static const uint8_t USB_CONFIG_DESC[32] = {
    0x09, 0x02,             /* bLength, CONFIGURATION */
    0x20, 0x00,             /* wTotalLength 32 */
    0x01,                   /* bNumInterfaces */
    0x01,                   /* bConfigurationValue */
    0x00,                   /* iConfiguration */
    0x80,                   /* bmAttributes: bus powered */
    0x32,                   /* bMaxPower 100 mA */

    0x09, 0x04,             /* bLength, INTERFACE */
    0x00, 0x00,             /* bInterfaceNumber, bAlternateSetting */
    0x02,                   /* bNumEndpoints */
    0x58, 0x42, 0x00,       /* XID class, gamepad subclass, protocol */
    0x00,                   /* iInterface */

    0x07, 0x05,             /* bLength, ENDPOINT */
    0x81,                   /* EP1 IN */
    0x03,                   /* interrupt */
    0x20, 0x00,             /* wMaxPacketSize 32 */
    0x04,                   /* bInterval */

    0x07, 0x05,             /* bLength, ENDPOINT */
    0x02,                   /* EP2 OUT */
    0x03,                   /* interrupt */
    0x20, 0x00,             /* wMaxPacketSize 32 */
    0x04                    /* bInterval */
};

/* The XID descriptor, requested as a VENDOR GET_DESCRIPTOR of type 0x42 on the
 * interface. JSRF asks for it immediately after SET_CONFIGURATION; see the
 * request handler for the captured setup packet. */
static const uint8_t USB_XID_DESC[16] = {
    0x10, 0x42,             /* bLength, XID */
    0x00, 0x01,             /* bcdXid 1.00 */
    0x01,                   /* bType: gamepad */
    0x02,                   /* bSubType: standard S controller */
    XBOX_USB_PAD_REPORT,    /* bMaxInputReportSize */
    0x06,                   /* bMaxOutputReportSize */
    0xFF, 0xFF, 0xFF, 0xFF, /* wAlternateProductIds: none */
    0xFF, 0xFF, 0xFF, 0xFF
};

static int (*g_pad_state)(uint8_t report[XBOX_USB_PAD_REPORT]);

void xbox_SetUsbPadStateHook(int (*fn)(uint8_t report[XBOX_USB_PAD_REPORT]))
{
    g_pad_state = fn;
}

/* Control-transfer state.
 *
 * A control transfer spans several TDs and the title is free to add them in
 * more than one pass, so the answer to a SETUP has to outlive the TD that
 * carried it. There is one device on one port, so one of these is enough. */
static struct {
    uint8_t  address;
    uint8_t  set_address;      /* pending: applied when the status stage retires */
    uint8_t  set_address_valid;
    uint8_t  configuration;
    uint8_t  stalled;          /* the SETUP was refused; the rest of it stalls */
    uint8_t  data[XBOX_USB_CTRL_MAX];
    uint32_t data_len;
    uint32_t data_off;
} g_dev;

void xbox_UsbDeviceReset(void)
{
    memset(&g_dev, 0, sizeof(g_dev));
}

uint8_t xbox_UsbDeviceAddress(void)      { return g_dev.address; }
uint8_t xbox_UsbDeviceConfiguration(void) { return g_dev.configuration; }

static int usb_copy(const uint8_t *src, uint32_t len, uint8_t *out,
                    uint32_t out_max, uint32_t *out_len)
{
    if (len > out_max)
        len = out_max;
    memcpy(out, src, len);
    *out_len = len;
    return 0;
}

/* Split out so the wLength clamp below applies to every reply on one path. */
static int usb_control_request(const uint8_t setup[8], uint8_t *out,
                               uint32_t out_max, uint32_t *out_len)
{
    uint8_t  bmRequestType = setup[0];
    uint8_t  bRequest      = setup[1];
    uint16_t wValue        = (uint16_t)(setup[2] | (setup[3] << 8));

    *out_len = 0;

    /* Standard requests addressed to the device itself. */
    if ((bmRequestType & 0x7Fu) == 0x00u) {
        switch (bRequest) {
        case 0x00:                      /* GET_STATUS */
            if (out_max < 2) return -1;
            out[0] = 0x00;              /* bus powered, no remote wakeup */
            out[1] = 0x00;
            *out_len = 2;
            return 0;
        case 0x05:                      /* SET_ADDRESS */
            g_dev.set_address = (uint8_t)(wValue & 0x7Fu);
            g_dev.set_address_valid = 1;
            return 0;
        case 0x06:                      /* GET_DESCRIPTOR */
            switch (wValue >> 8) {
            case 0x01:
                return usb_copy(USB_DEVICE_DESC, sizeof(USB_DEVICE_DESC),
                                out, out_max, out_len);
            case 0x02:
                return usb_copy(USB_CONFIG_DESC, sizeof(USB_CONFIG_DESC),
                                out, out_max, out_len);
            default:
                /* Including STRING: this device declares no string indices,
                 * so a request for one is a driver bug, not a gap here. */
                break;
            }
            break;
        case 0x08:                      /* GET_CONFIGURATION */
            if (out_max < 1) return -1;
            out[0] = g_dev.configuration;
            *out_len = 1;
            return 0;
        case 0x09:                      /* SET_CONFIGURATION */
            g_dev.configuration = (uint8_t)(wValue & 0xFFu);
            return 0;
        case 0x01:                      /* CLEAR_FEATURE */
        case 0x03:                      /* SET_FEATURE */
            return 0;
        default:
            break;
        }
    }

    /* Standard interface and endpoint requests that cost nothing to satisfy. */
    if ((bmRequestType & 0x60u) == 0x00u && (bmRequestType & 0x1Fu) != 0x00u) {
        if (bRequest == 0x00) {         /* GET_STATUS */
            if (out_max < 2) return -1;
            out[0] = 0x00;
            out[1] = 0x00;
            *out_len = 2;
            return 0;
        }
        if (bRequest == 0x0A) {         /* GET_INTERFACE */
            if (out_max < 1) return -1;
            out[0] = 0x00;
            *out_len = 1;
            return 0;
        }
        if (bRequest == 0x0B)           /* SET_INTERFACE */
            return 0;
    }

    /* GET_REPORT, the class request that actually carries the pad.
     *
     * Measured in claude-usb-transfer-service-12 and -13, immediately after
     * the XID descriptor: A1 01 00 01 00 00 14 00 -- device-to-host, type
     * CLASS, recipient INTERFACE, report type 1 (input), report id 0, twenty
     * bytes. XID polls the controller over the control pipe rather than
     * waiting on the interrupt endpoint, so this, not the periodic list, is
     * where a button press reaches the title. */
    if ((bmRequestType & 0x60u) == 0x20u && (bmRequestType & 0x1Fu) == 0x01u
            && bRequest == 0x01 && (wValue >> 8) == 0x01) {
        return xbox_UsbInterruptIn(1, out, out_max, out_len);
    }

    /* XID requests, addressed to the interface.
     *
     * JSRF asks for the XID descriptor as 0xC1 0x06 0x00 0x42 0x00 0x00 0x10
     * 0x00 -- measured in claude-usb-exp-a2, the request straight after
     * SET_CONFIGURATION. bmRequestType 0xC1 is device-to-host, type VENDOR,
     * recipient INTERFACE: XID rides on vendor requests, not class ones, which
     * is why this cannot live in the standard block above.
     *
     *   0x06 GET_DESCRIPTOR, wValue 0x4200: the XID descriptor.
     *   0x01 GET_CAPABILITIES, wValue 0x0100 input / 0x0200 output. The reply
     *   is a report-shaped mask with a 1 bit for every field the pad drives,
     *   which for a standard controller is all of them bar the leading report
     *   id and length. Not yet observed from this title. */
    if ((bmRequestType & 0x60u) == 0x40u && (bmRequestType & 0x1Fu) == 0x01u) {
        if (bRequest == 0x06 && (wValue >> 8) == 0x42)
            return usb_copy(USB_XID_DESC, sizeof(USB_XID_DESC),
                            out, out_max, out_len);
        if (bRequest == 0x01) {
            uint32_t len = (wValue >> 8) == 0x02
                         ? 6u : (uint32_t)XBOX_USB_PAD_REPORT;
            if (len > out_max) return -1;
            memset(out, 0xFF, len);
            out[0] = 0x00;
            out[1] = (uint8_t)len;
            *out_len = len;
            return 0;
        }
    }

    if (g_trace) {
        fprintf(stderr,
                "  [USB] unhandled setup %02X %02X %02X %02X %02X %02X %02X %02X"
                " -> STALL\n",
                setup[0], setup[1], setup[2], setup[3],
                setup[4], setup[5], setup[6], setup[7]);
        fflush(stderr);
    }
    return -1;
}

int xbox_UsbControlRequest(const uint8_t setup[8], uint8_t *out,
                           uint32_t out_max, uint32_t *out_len)
{
    uint16_t wLength = (uint16_t)(setup[6] | (setup[7] << 8));
    int rc = usb_control_request(setup, out, out_max, out_len);

    /* A device never returns more than the host asked for. The address-0 probe
     * relies on exactly this: it requests eight bytes of an eighteen-byte
     * descriptor because eight is all it knows the endpoint can carry. */
    if (rc == 0 && *out_len > wLength)
        *out_len = wLength;
    return rc;
}

int xbox_UsbInterruptIn(uint8_t endpoint, uint8_t *out, uint32_t out_max,
                        uint32_t *out_len)
{
    uint8_t report[XBOX_USB_PAD_REPORT];

    *out_len = 0;
    if (endpoint != 1)
        return -1;
    if (out_max < XBOX_USB_PAD_REPORT)
        return -1;

    /* Neutral unless the backend says otherwise, and never a refusal.
     *
     * This device is on the port because the runtime put it there, so from the
     * title's side a controller is plugged in and the honest report for "the
     * host has no gamepad attached" is one with nothing pressed. Refusing
     * instead would leave a title that had already enumerated the pad waiting
     * on an endpoint that never answers. */
    memset(report, 0, sizeof(report));
    report[0] = 0x00;                     /* report id */
    report[1] = XBOX_USB_PAD_REPORT;      /* report length */
    if (g_pad_state)
        (void)g_pad_state(report);

    /* Report a button the first few times it appears, and again whenever the
     * pressed set changes. The question this answers -- does a press actually
     * reach the title -- is otherwise invisible: the pad is polled a hundred
     * times a second and every poll looks alike. */
    if (g_trace) {
        static uint8_t last_buttons, last_a;
        static unsigned shown;
        if ((report[2] != last_buttons || report[4] != last_a) && shown < 16) {
            shown++;
            last_buttons = report[2];
            last_a = report[4];
            fprintf(stderr, "  [USB-PAD] buttons=%02X A=%02X B=%02X X=%02X"
                    " Y=%02X LT=%02X RT=%02X lx=%d ly=%d\n",
                    report[2], report[4], report[5], report[6], report[7],
                    report[10], report[11],
                    (int)(int16_t)(report[12] | (report[13] << 8)),
                    (int)(int16_t)(report[14] | (report[15] << 8)));
            fflush(stderr);
        }
    }

    memcpy(out, report, XBOX_USB_PAD_REPORT);
    *out_len = XBOX_USB_PAD_REPORT;
    return 0;
}

/* ================================================================
 * The host controller
 * ================================================================ */

/* Retire one transfer descriptor against the device, returning its condition
 * code. `td` points at the descriptor in guest RAM and is updated in place. */
static uint32_t ohci_run_td(const xbox_ohci_service *s, uint32_t ed_flags,
                            uint32_t *td)
{
    uint32_t flags = td[0];
    uint32_t cbp   = td[1];
    uint32_t be    = td[3];
    uint32_t pid   = TD_PID(flags);
    uint32_t want  = cbp ? (be >= cbp ? be - cbp + 1u : 0u) : 0u;
    uint32_t moved = 0;
    uint32_t cc    = XBOX_OHCI_CC_NOERROR;
    uint8_t *buf   = want ? (uint8_t *)ohci_at(s, cbp, want) : NULL;

    if (want && !buf) {
        cc = XBOX_OHCI_CC_DEVICENOTRESP;     /* the title pointed off the map */
        goto writeback;
    }

    if (ED_FA(ed_flags) != g_dev.address) {
        /* Nothing answers at that address. Real hardware retries and then
         * reports this; completing it immediately keeps a driver that is
         * waiting on the done queue from waiting for ever. */
        cc = XBOX_OHCI_CC_DEVICENOTRESP;
        goto writeback;
    }

    if (pid == TD_PID_SETUP) {
        uint8_t setup[8];
        if (want < 8 || !buf) {
            cc = XBOX_OHCI_CC_STALL;
            goto writeback;
        }
        memcpy(setup, buf, 8);
        g_dev.data_len = 0;
        g_dev.data_off = 0;
        g_dev.set_address_valid = 0;
        g_dev.stalled = 0;
        if (xbox_UsbControlRequest(setup, g_dev.data, sizeof(g_dev.data),
                                   &g_dev.data_len) != 0) {
            /* A device may not stall the SETUP transaction itself; the refusal
             * shows up on the stage that follows it. */
            g_dev.stalled = 1;
            g_dev.data_len = 0;
        }
        moved = 8;
    } else if (g_dev.stalled) {
        cc = XBOX_OHCI_CC_STALL;
        goto writeback;
    } else if (pid == TD_PID_IN) {
        uint32_t left = g_dev.data_len - g_dev.data_off;
        if (ED_EN(ed_flags) != 0) {
            /* An interrupt-IN endpoint: the pad report, not a control reply. */
            uint32_t len = 0;
            uint8_t report[XBOX_USB_PAD_REPORT];
            if (xbox_UsbInterruptIn((uint8_t)ED_EN(ed_flags), report,
                                    sizeof(report), &len) != 0)
                return XBOX_OHCI_CC_NOTACCESSED;   /* NAK: leave it queued */
            if (len > want) len = want;
            if (len && buf) memcpy(buf, report, len);
            moved = len;
        } else {
            if (left > want)
                left = want;
            if (left && buf)
                memcpy(buf, g_dev.data + g_dev.data_off, left);
            g_dev.data_off += left;
            moved = left;
            /* A packet shorter than the TD asked for ends the data stage.
             *
             * OHCI calls this DataUnderrun unless the TD allowed rounding, and
             * halts the endpoint. This model deliberately does not, and the
             * reason is the configuration descriptor below rather than
             * anything about the host controller: XPP splits its 80-byte
             * request into ten eight-byte TDs and sets bufferRounding on the
             * last one only, so a device with fewer than 80 bytes to give
             * underruns in the middle of that chain. Measured in
             * claude-usb-transfer-service-10 and -11: XPP treats the halt as a
             * failed enumeration, tears the control list down and resets the
             * port, for ever. Completing the short read cleanly instead --
             * claude-usb-exp-a2 and -a3 -- carries it through
             * SET_CONFIGURATION and on to the XID descriptor.
             *
             * So this compensates for a descriptor set that is shorter than
             * the real controller's, and it is the descriptor that wants
             * correcting once a real dump exists, not this line. */
            if (moved < want && !TD_ROUNDING(flags))
                cc = XBOX_OHCI_CC_NOERROR;
        }
    } else {                                  /* TD_PID_OUT */
        /* Either the status stage of a device-to-host transfer, or rumble data
         * on EP2. Nothing consumes the latter yet; accepting it is honest,
         * since the device would. */
        moved = want;
    }

writeback:
    /* OHCI: on a fully satisfied buffer CBP is zeroed, otherwise it advances
     * to the first byte not transferred. */
    td[1] = (moved >= want) ? 0u : cbp + moved;
    td[0] = (flags & 0x0FFFFFFFu) | (cc << 28);
    return cc;
}

/* A control transfer's status stage is the zero-length TD that follows its
 * data. Recognising it is what lets SET_ADDRESS take effect at the right
 * moment: the device must still answer at its old address until the host has
 * seen the status stage complete. */
static void ohci_end_of_transfer(uint32_t pid, uint32_t want)
{
    if (pid == TD_PID_SETUP || want != 0)
        return;
    if (g_dev.set_address_valid) {
        g_dev.address = g_dev.set_address;
        g_dev.set_address_valid = 0;
    }
    g_dev.stalled = 0;
    g_dev.data_len = 0;
    g_dev.data_off = 0;
}

unsigned xbox_OhciServiceList(xbox_ohci_service *s)
{
    uint32_t ed_va;
    uint32_t done;
    unsigned eds = 0;
    unsigned retired = 0;

    if (!s || !s->ram || !s->ram_size)
        return 0;

    done = s->done_head;
    s->eds_walked = 0;
    s->tds_retired = 0;

    /* `eds` counts at the top rather than in the increment clause: an ED that
     * ends the walk -- the last on the list, or one that points at itself --
     * was still walked, and the count is what the caller reports. */
    for (ed_va = s->head_ed & ~0xFu; ed_va && eds < OHCI_MAX_EDS; ) {
        uint32_t *ed = (uint32_t *)ohci_at(s, ed_va, 16);
        uint32_t flags, tail, head, next, td_va, carry;
        unsigned tds = 0;

        if (!ed)
            break;
        eds++;
        flags = ed[0];
        tail  = ed[1] & ~0xFu;
        head  = ed[2];
        next  = ed[3] & ~0xFu;
        td_va = head & ~0xFu;
        carry = (head & ED_HEAD_CARRY) ? 1u : 0u;

        if ((flags & ED_SKIP) || (head & ED_HEAD_HALTED))
            goto advance;

        while (td_va && td_va != tail && tds < OHCI_MAX_TDS) {
            uint32_t *td = (uint32_t *)ohci_at(s, td_va, 16);
            uint32_t pid, want, toggle, cc, td_next;

            if (!td)
                break;
            pid  = TD_PID(td[0]);
            want = td[1] ? (td[3] >= td[1] ? td[3] - td[1] + 1u : 0u) : 0u;
            toggle = TD_TOGGLE(td[0]);
            td_next = td[2] & ~0xFu;

            cc = ohci_run_td(s, flags, td);
            if (cc == XBOX_OHCI_CC_NOTACCESSED) {
                /* The device NAKed. The TD stays where it is, at the head of
                 * the endpoint, exactly as hardware would leave it. */
                break;
            }

            /* Prepend to the done queue: HcDoneHead names the most recently
             * retired TD and each NextTD walks back towards the oldest, which
             * is the order the driver reverses when it processes the list. */
            td[2] = done;
            done = td_va;
            retired++;
            tds++;

            /* The carry bit records the toggle the next transaction on this
             * endpoint should use. Control transfers force DATA0 on every
             * SETUP so it is not load-bearing here, but an interrupt endpoint
             * runs off it for the life of the connection. */
            carry = ((toggle & 2u) ? (toggle & 1u) : carry) ^ 1u;

            if (cc != XBOX_OHCI_CC_NOERROR) {
                /* A retired TD is retired whether or not it succeeded: it has
                 * already been unlinked and put on the done queue above, so
                 * HeadP advances past it exactly as on the success path and
                 * only gains the Halted bit. The driver finds the failure on
                 * the done queue, by its condition code.
                 *
                 * Leaving HeadP on the failing TD instead hands the driver the
                 * same descriptor twice -- once from the queue and once from
                 * the endpoint -- and XPP faults on the second look, measured
                 * in claude-usb-transfer-service-08 where an 80-byte
                 * GET_DESCRIPTOR(CONFIGURATION) short-reads at 32 and the
                 * DataUnderrun TD came back double. */
                head = td_next | ED_HEAD_HALTED | (carry ? ED_HEAD_CARRY : 0u);
                ed[2] = head;
                goto advance;
            }

            ohci_end_of_transfer(pid, want);
            td_va = td_next;
            head = td_va | (carry ? ED_HEAD_CARRY : 0u);
            ed[2] = head;
        }

advance:
        if (next == ed_va)
            break;                  /* a list that points at itself */
        ed_va = next;
    }

    s->eds_walked = eds;
    s->tds_retired = retired;
    s->published = 0;

    /* A done queue the driver has not acknowledged yet is not ours to replace.
     *
     * WritebackDoneHead still set means the previous queue is still the
     * driver's to process, so hardware keeps retiring TDs into HcDoneHead and
     * publishes them all at the next writeback. Overwriting instead loses that
     * queue outright, and the driver goes on to free transfer descriptors it
     * was never told about -- measured in claude-usb-transfer-service-04,
     * where XPP submits SET_ADDRESS from its completion DPC before it
     * acknowledges the interrupt for the transfer before it, and then faults
     * dereferencing 0xFFFFFFF0 once the two queues have been confused. */
    if (s->intr_status & XBOX_OHCI_INTR_WDH) {
        s->done_head = done;
        if (g_trace && retired) {
            fprintf(stderr, "  [OHCI-SVC] head=%08X eds=%u retired=%u done=%08X"
                    " deferred addr=%u cfg=%u\n",
                    s->head_ed, eds, retired, done,
                    g_dev.address, g_dev.configuration);
            fflush(stderr);
        }
        return retired;
    }

    /* Nothing waiting, either just retired or held over from a deferred
     * writeback. A pass that retires nothing and has nothing pending is the
     * common case: the driver rang a doorbell for a list it had emptied. */
    if (!done) {
        s->done_head = 0;
        return retired;
    }

    /* Publish the done queue. The HCCA copy is what the driver reads; bit 0 of
     * it tells the driver whether anything besides WritebackDoneHead is also
     * pending, so it knows whether HcInterruptStatus is worth a read. */
    {
        uint8_t *hcca = (uint8_t *)ohci_at(s, s->hcca & ~0xFFu,
                                           XBOX_OHCI_HCCA_SIZE);
        uint32_t other = s->intr_status & s->intr_enable & ~XBOX_OHCI_INTR_WDH
                                        & ~XBOX_OHCI_INTR_MIE;
        if (hcca) {
            uint32_t *fn = (uint32_t *)(hcca + XBOX_OHCI_HCCA_FRAME_NUMBER);
            uint32_t *dh = (uint32_t *)(hcca + XBOX_OHCI_HCCA_DONE_HEAD);
            *fn = (*fn & 0xFFFF0000u) | (s->frame_number & 0xFFFFu);
            *dh = done | (other ? 1u : 0u);
        }
    }
    s->intr_status |= XBOX_OHCI_INTR_WDH;
    s->published = 1;
    /* HcDoneHead is emptied by the writeback: the queue now belongs to the
     * driver, and a TD retired after this starts a fresh one. */
    s->done_head = 0;

    if (g_trace) {
        fprintf(stderr, "  [OHCI-SVC] head=%08X eds=%u retired=%u done=%08X"
                " published addr=%u cfg=%u\n",
                s->head_ed, eds, retired, done,
                g_dev.address, g_dev.configuration);
        fflush(stderr);
    }
    return retired;
}
