/**
 * OHCI transfer service and the one USB device behind it.
 *
 * JSRF's statically linked XPP peripheral library drives the MCPX OHCI
 * registers directly: it resets the root port, builds endpoint and transfer
 * descriptors in guest RAM, publishes HcControlHeadED and rings
 * ControlListFilled. Nothing upstream models that -- upstream's gap analysis
 * records "USB/OHCI gamepad | N/A | Bypassed via XInput" -- so a title that
 * never calls the host input shim waits forever.
 *
 * This is the other half: enough host controller to run the lists the title
 * actually builds, and enough device to answer as an Xbox gamepad.
 *
 * Everything here is a pure function over a caller-supplied RAM window and a
 * caller-supplied copy of the operational registers. That is deliberate. The
 * live caller is a SIGSEGV handler (see the MCPX write trap in
 * xbox_memory_layout.c), so the service must not allocate, must not lock and
 * must not touch process state; and a schedule can then be built in a test
 * buffer and run without a guest at all.
 */
#ifndef XBOX_USB_OHCI_H
#define XBOX_USB_OHCI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HcInterruptStatus / HcInterruptEnable bits this service touches. */
#define XBOX_OHCI_INTR_SO    0x00000001u   /* SchedulingOverrun */
#define XBOX_OHCI_INTR_WDH   0x00000002u   /* WritebackDoneHead */
#define XBOX_OHCI_INTR_SF    0x00000004u   /* StartOfFrame */
#define XBOX_OHCI_INTR_RHSC  0x00000040u   /* RootHubStatusChange */
#define XBOX_OHCI_INTR_MIE   0x80000000u   /* MasterInterruptEnable */

/* Transfer-descriptor condition codes (TD dword0 bits 28-31). */
#define XBOX_OHCI_CC_NOERROR         0x0u
#define XBOX_OHCI_CC_STALL           0x4u
#define XBOX_OHCI_CC_DEVICENOTRESP   0x5u
#define XBOX_OHCI_CC_DATAUNDERRUN    0xDu
#define XBOX_OHCI_CC_NOTACCESSED     0xEu

/* HCCA layout: 32 interrupt-ED pointers, frame number, then DoneHead. */
#define XBOX_OHCI_HCCA_FRAME_NUMBER  0x80u
#define XBOX_OHCI_HCCA_DONE_HEAD     0x84u
#define XBOX_OHCI_HCCA_SIZE          0x100u

/**
 * One pass of the host controller over a descriptor list.
 *
 * `ram` is the host address that guest address 0 maps to; every guest pointer
 * in a descriptor is bounds-checked against `ram_size` before it is followed,
 * because these come from the title and a bad one would fault inside a signal
 * handler.
 *
 * Register writebacks are returned in the struct rather than applied. On the
 * live path the operational registers sit behind an mprotect guard that only
 * the trap owner may lift, so the caller stores them; in a test there are no
 * registers at all.
 */
typedef struct {
    uint8_t  *ram;            /* host base of guest address 0 */
    uint32_t  ram_size;
    /* Optional address resolver, used in place of the flat ram/ram_size
     * window when set.
     *
     * The flat window is only the truth while every descriptor lives in the
     * low 64 MB. It stopped being so when the contiguous allocator began
     * returning addresses in the 0x80000000 window, which is separately backed
     * storage rather than an alias of that RAM -- so neither following the
     * address nor masking its high bit reaches the right bytes, and the only
     * thing that does is the runtime's own translation. Left NULL, the flat
     * window is used, which is what a test with no guest wants. */
    void *(*resolve)(uint32_t va, uint32_t bytes);
    uint32_t  hcca;           /* HcHCCA, guest address; 0 disables done-queue publication */
    uint32_t  head_ed;        /* HcControlHeadED or the periodic-list head */
    uint32_t  done_head;      /* in/out: HcDoneHead */
    uint32_t  intr_status;    /* in/out: HcInterruptStatus */
    uint32_t  intr_enable;    /* in: HcInterruptEnable, for the HCCA DoneHead bit 0 */
    uint32_t  frame_number;   /* in: HcFmNumber, written to the HCCA */
    unsigned  eds_walked;     /* out: diagnostics */
    unsigned  tds_retired;    /* out: diagnostics */
    /* out: a done queue was written back this pass, so `done_head` and
     * `intr_status` must be applied to the registers. This is NOT the same as
     * having retired a TD: a writeback deferred behind an unacknowledged
     * interrupt retires TDs and publishes nothing, and the pass that finally
     * publishes it retires none. */
    unsigned  published;
} xbox_ohci_service;

/**
 * Run every ED on the list at `s->head_ed`, retiring the transfer descriptors
 * between HeadP and TailP against the device model below.
 *
 * Returns the number of TDs retired. Whether the caller must write registers
 * back is `s->published`, not the return value -- see that field.
 */
unsigned xbox_OhciServiceList(xbox_ohci_service *s);

/* ================================================================
 * The device: one Xbox gamepad on root-hub port 1
 * ================================================================ */

/** Longest control response this model produces (the configuration set). */
#define XBOX_USB_CTRL_MAX 64

/** The Xbox gamepad's interrupt-IN report is fixed at 20 bytes. */
#define XBOX_USB_PAD_REPORT 20

/**
 * Answer a USB control request.
 *
 * `setup` is the eight-byte setup packet exactly as it sits in the SETUP TD's
 * buffer. On success the response, which may be empty for a status-only
 * request such as SET_ADDRESS, is written to `out` and its length to
 * `out_len`; the caller clamps that to the request's wLength.
 *
 * Returns 0 when the device answers and -1 when it would stall. An unhandled
 * request stalls rather than inventing a reply: a wrong descriptor is far
 * harder to see in a trace than a stall on a request that names itself.
 */
int xbox_UsbControlRequest(const uint8_t setup[8], uint8_t *out,
                           uint32_t out_max, uint32_t *out_len);

/**
 * Fill the interrupt-IN report for `endpoint` (1 for the gamepad).
 *
 * Returns 0 with `out_len` set, or -1 if the endpoint is not one this device
 * has. The report is sourced from the pad-state hook when one is installed and
 * is otherwise all-neutral, which is a controller that is plugged in with
 * nothing pressed.
 */
int xbox_UsbInterruptIn(uint8_t endpoint, uint8_t *out, uint32_t out_max,
                        uint32_t *out_len);

/**
 * Supply live pad state to the interrupt endpoint.
 *
 * A hook rather than a direct call so xbox_kernel keeps no link dependency on
 * xbox_input, the same arrangement xbox_SetApuMmioWriteHook uses. The callee
 * fills 20 bytes in Xbox gamepad report order and returns non-zero if a
 * controller is present.
 */
void xbox_SetUsbPadStateHook(int (*fn)(uint8_t report[XBOX_USB_PAD_REPORT]));

/* The pad's output report -- the XID rumble packet, six bytes: report id 0,
 * length 6, left motor and right motor as little-endian 16-bit speeds. It
 * reaches the device two ways and both land here: as the data stage of a
 * SET_REPORT class request on the control pipe, and as a packet on the
 * interrupt OUT endpoint. The hook is how the runtime turns it into rumble on
 * the host pad; without one the report is consumed and forgotten, which is
 * still a completed transfer rather than a stalled one. */
void xbox_SetUsbPadRumbleHook(void (*fn)(uint16_t left, uint16_t right));
int xbox_UsbOutputReport(const uint8_t *data, uint32_t len);

/**
 * Forget the device's addressing and configuration state.
 *
 * A port reset returns a USB device to the default address, so the trap calls
 * this when the title resets the root port; without it a second enumeration
 * pass would find a device that still answers only at its old address.
 */
void xbox_UsbDeviceReset(void);

/** Current USB device address, 0 until the title assigns one. */
uint8_t xbox_UsbDeviceAddress(void);

/**
 * Resolve the service's diagnostic settings once, from ordinary code.
 *
 * The service itself runs inside the MCPX signal handler, where getenv is not
 * async-signal-safe. Calling this during memory-layout init keeps the lookup
 * off that path; it is safe to call more than once and safe never to call, in
 * which case tracing stays off.
 */
void xbox_UsbOhciInit(void);

/** Configuration selected by SET_CONFIGURATION, 0 when unconfigured. */
uint8_t xbox_UsbDeviceConfiguration(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_USB_OHCI_H */
