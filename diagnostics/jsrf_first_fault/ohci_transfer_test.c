/* Focused regression coverage for the OHCI control-list service.
 *
 * The first case is the schedule JSRF actually built, transcribed from
 * build-macos/jsrf-first-fault/render-investigation/codex-usb-transfer-dump-04
 * lines 1649-1655: a control ED at 0x009E29A0 with MaxPacketSize 8 on address
 * 0 endpoint 0, and three TDs -- SETUP carrying 80 06 00 01 00 00 08 00, an
 * eight-byte IN, and a zero-length OUT status stage. Everything else here
 * exercises a rule that schedule does not reach: bounds, stalls, addressing,
 * done-queue order and interrupt gating.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xbox_usb_ohci.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* A guest RAM window big enough to hold the addresses the capture uses. */
#define RAM_SIZE 0x00A00000u
static uint8_t ram[RAM_SIZE];

#define ED_VA    0x009E29A0u
#define TD_SETUP 0x009E3FD0u
#define TD_IN    0x009E3F90u
#define TD_OUT   0x009E3F70u
#define TD_TAIL  0x009E3F50u
#define BUF_SETUP 0x009E3FB0u
#define BUF_IN    0x002648DCu
#define HCCA_VA   0x009E4400u

static uint32_t *at(uint32_t va) { return (uint32_t *)(ram + va); }

static void put_ed(uint32_t va, uint32_t flags, uint32_t tail, uint32_t head,
                   uint32_t next)
{
    at(va)[0] = flags;
    at(va)[1] = tail;
    at(va)[2] = head;
    at(va)[3] = next;
}

static void put_td(uint32_t va, uint32_t flags, uint32_t cbp, uint32_t next,
                   uint32_t be)
{
    at(va)[0] = flags;
    at(va)[1] = cbp;
    at(va)[2] = next;
    at(va)[3] = be;
}

/* Rebuild the captured control schedule with `setup` in the SETUP TD and an
 * IN data stage of `in_len` bytes. */
static void build_control(const uint8_t setup[8], uint32_t in_len)
{
    memset(ram, 0, RAM_SIZE);
    put_ed(ED_VA, 0x00080000u, TD_TAIL, TD_SETUP, 0);
    put_td(TD_SETUP, 0xE2E00000u, BUF_SETUP, TD_IN, BUF_SETUP + 7u);
    if (in_len)
        put_td(TD_IN, 0xE3F00000u, BUF_IN, TD_OUT, BUF_IN + in_len - 1u);
    else
        put_td(TD_IN, 0xE3F00000u, 0, TD_OUT, 0);
    put_td(TD_OUT, 0xE3080000u, 0, TD_TAIL, 0);
    memcpy(ram + BUF_SETUP, setup, 8);
}

static xbox_ohci_service run(uint32_t hcca, uint32_t intr_enable)
{
    xbox_ohci_service s;
    memset(&s, 0, sizeof(s));
    s.ram = ram;
    s.ram_size = RAM_SIZE;
    s.hcca = hcca;
    s.head_ed = ED_VA;
    s.intr_enable = intr_enable;
    s.frame_number = 0x1234;
    xbox_OhciServiceList(&s);
    return s;
}

static uint32_t td_cc(uint32_t va) { return at(va)[0] >> 28; }

/* ---------------------------------------------------------------- */

static void test_captured_get_descriptor(void)
{
    /* 80 06 00 01 00 00 08 00 -- GET_DESCRIPTOR(DEVICE), first eight bytes. */
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };
    xbox_ohci_service s;
    const uint8_t *got;

    xbox_UsbDeviceReset();
    build_control(setup, 8);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE | XBOX_OHCI_INTR_WDH);

    check(s.tds_retired == 3, "the captured schedule retires all three TDs");
    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_NOERROR, "SETUP completes NoError");
    check(td_cc(TD_IN) == XBOX_OHCI_CC_NOERROR, "IN completes NoError");
    check(td_cc(TD_OUT) == XBOX_OHCI_CC_NOERROR, "status completes NoError");

    got = ram + BUF_IN;
    check(got[0] == 0x12 && got[1] == 0x01,
          "the reply is an eighteen-byte DEVICE descriptor");
    check(got[2] == 0x10 && got[3] == 0x01, "bcdUSB is 1.10");
    check(got[7] == 0x08,
          "bMaxPacketSize0 is 8, matching the MaxPacketSize in the title's ED");
    /* The probe asks for eight bytes of an eighteen-byte descriptor, which is
     * the whole point of it: eight is all the driver yet knows the endpoint
     * can carry. idVendor starts at byte 8 and must not appear. */
    check(ram[BUF_IN + 8] == 0x00,
          "wLength 8 is honoured: nothing past the eighth byte is written");

    /* HeadP reaching TailP is how the driver knows the endpoint is idle. */
    check((at(ED_VA)[2] & ~0xFu) == TD_TAIL, "the ED head advances to its tail");
    check((at(ED_VA)[2] & 1u) == 0, "a clean transfer leaves the ED unhalted");

    /* Done queue: HcDoneHead names the last TD retired and each NextTD walks
     * back towards the first. */
    check(at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] == TD_OUT,
          "the HCCA done head names the most recently retired TD");
    check((at(TD_OUT)[2] & ~0xFu) == TD_IN && (at(TD_IN)[2] & ~0xFu) == TD_SETUP,
          "the done queue is chained newest to oldest");
    check(at(TD_SETUP)[2] == 0, "the done queue terminates at the oldest TD");
    check((at(HCCA_VA)[XBOX_OHCI_HCCA_FRAME_NUMBER / 4] & 0xFFFFu) == 0x1234u,
          "the HCCA frame number is published with the done queue");

    check((s.intr_status & XBOX_OHCI_INTR_WDH) != 0,
          "WritebackDoneHead is raised");
    check(s.done_head == 0, "HcDoneHead is emptied by the writeback");
}

static void test_hcca_done_head_low_bit(void)
{
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };
    xbox_ohci_service s;

    /* Bit 0 of the HCCA done head means "something other than WDH is also
     * pending", so the driver knows whether to read HcInterruptStatus. */
    xbox_UsbDeviceReset();
    build_control(setup, 8);
    memset(&s, 0, sizeof(s));
    s.ram = ram;
    s.ram_size = RAM_SIZE;
    s.hcca = HCCA_VA;
    s.head_ed = ED_VA;
    s.intr_status = XBOX_OHCI_INTR_RHSC;
    s.intr_enable = XBOX_OHCI_INTR_RHSC | XBOX_OHCI_INTR_MIE;
    xbox_OhciServiceList(&s);
    check((at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] & 1u) == 1u,
          "an enabled unrelated interrupt sets the done-head low bit");

    xbox_UsbDeviceReset();
    build_control(setup, 8);
    memset(&s, 0, sizeof(s));
    s.ram = ram;
    s.ram_size = RAM_SIZE;
    s.hcca = HCCA_VA;
    s.head_ed = ED_VA;
    s.intr_status = XBOX_OHCI_INTR_RHSC;
    s.intr_enable = XBOX_OHCI_INTR_MIE;      /* RHSC pending but masked */
    xbox_OhciServiceList(&s);
    check((at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] & 1u) == 0u,
          "a masked interrupt does not set the done-head low bit");
}

static void test_set_address_takes_effect_after_status(void)
{
    static const uint8_t set_addr[8] = { 0x00, 0x05, 0x02, 0x00,
                                         0x00, 0x00, 0x00, 0x00 };
    static const uint8_t get_desc[8] = { 0x80, 0x06, 0x00, 0x01,
                                         0x00, 0x00, 0x12, 0x00 };
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    check(xbox_UsbDeviceAddress() == 0, "the device starts at address 0");

    /* SET_ADDRESS 2: SETUP then a zero-length IN status stage. The whole
     * transfer runs at the old address; only afterwards does the new one
     * apply. */
    build_control(set_addr, 0);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 3, "SET_ADDRESS retires its chain");
    check(xbox_UsbDeviceAddress() == 2,
          "the address changes once the status stage retires");

    /* The old ED, still addressing 0, must now find nothing. */
    build_control(get_desc, 18);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_DEVICENOTRESP,
          "address 0 no longer answers after SET_ADDRESS");
    check((at(ED_VA)[2] & 1u) == 1u, "an error halts the endpoint");

    /* Re-addressed, the same request succeeds and returns all 18 bytes. */
    build_control(get_desc, 18);
    at(ED_VA)[0] = 0x00080000u | 2u;         /* FunctionAddress 2 */
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 3, "the re-addressed endpoint completes");
    check(ram[BUF_IN] == 0x12 && ram[BUF_IN + 17] == 0x01,
          "the full eighteen-byte descriptor is delivered");
    check(ram[BUF_IN + 8] == 0x5E && ram[BUF_IN + 9] == 0x04 &&
          ram[BUF_IN + 10] == 0x89 && ram[BUF_IN + 11] == 0x02,
          "the device identifies as Microsoft 0x045E:0x0289");

    /* And a port reset puts it back to the default address. */
    xbox_UsbDeviceReset();
    check(xbox_UsbDeviceAddress() == 0, "a port reset restores address 0");
}

static void test_configuration_and_config_descriptor(void)
{
    static const uint8_t get_cfg[8]  = { 0x80, 0x06, 0x00, 0x02,
                                         0x00, 0x00, 0x20, 0x00 };
    static const uint8_t set_cfg[8]  = { 0x00, 0x09, 0x01, 0x00,
                                         0x00, 0x00, 0x00, 0x00 };

    xbox_UsbDeviceReset();
    build_control(get_cfg, 0x20);
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(ram[BUF_IN] == 0x09 && ram[BUF_IN + 1] == 0x02,
          "the configuration descriptor leads the set");
    check(ram[BUF_IN + 2] == 0x20 && ram[BUF_IN + 3] == 0x00,
          "wTotalLength covers interface and both endpoints");
    check(ram[BUF_IN + 14] == 0x58 && ram[BUF_IN + 15] == 0x42,
          "the interface declares the XID class and gamepad subclass");
    check(ram[BUF_IN + 20] == 0x81 && ram[BUF_IN + 21] == 0x03,
          "EP1 IN is an interrupt endpoint");

    check(xbox_UsbDeviceConfiguration() == 0, "the device starts unconfigured");
    build_control(set_cfg, 0);
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(xbox_UsbDeviceConfiguration() == 1, "SET_CONFIGURATION is recorded");
}

static void test_unsupported_request_stalls(void)
{
    /* GET_DESCRIPTOR(STRING, index 1). This device declares no string
     * indices, so it must stall rather than invent one. */
    static const uint8_t setup[8] = { 0x80, 0x06, 0x01, 0x03,
                                      0x09, 0x04, 0xFF, 0x00 };
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    build_control(setup, 8);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);

    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_NOERROR,
          "a device may not stall the SETUP transaction itself");
    check(td_cc(TD_IN) == XBOX_OHCI_CC_STALL,
          "the refusal appears on the stage after the SETUP");
    check(s.tds_retired == 2, "a halted endpoint retires nothing further");
    check((at(ED_VA)[2] & 1u) == 1u, "a stall halts the endpoint");
    /* The failed TD is retired like any other -- unlinked and put on the done
     * queue -- so HeadP advances past it. Leaving HeadP on it would hand the
     * driver the same descriptor from two places at once. */
    check((at(ED_VA)[2] & ~0xFu) == TD_OUT,
          "HeadP advances past the failed TD, which the done queue now owns");
    check(at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] == TD_IN,
          "the failed TD is the head of the done queue");
    check(td_cc(TD_OUT) == XBOX_OHCI_CC_NOTACCESSED,
          "the status TD behind the halt is untouched");
}

static void test_short_read_completes_cleanly(void)
{
    /* GET_DESCRIPTOR(CONFIGURATION) for 80 bytes, which is what JSRF asks --
     * measured in claude-usb-transfer-service-08. The device has only 32, so
     * the data stage ends short.
     *
     * OHCI would call that DataUnderrun and halt the endpoint. This model
     * completes it cleanly instead, because XPP treats a mid-chain halt as a
     * failed enumeration and resets the port for ever; see the note in
     * ohci_run_td. The endpoint must be left runnable, or the transfers that
     * follow never happen. */
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x02,
                                      0x00, 0x00, 0x50, 0x00 };
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    build_control(setup, 40);            /* five eight-byte IN TDs' worth */
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);

    check(ram[BUF_IN] == 0x09 && ram[BUF_IN + 1] == 0x02,
          "the configuration set is delivered up to its real length");
    check(ram[BUF_IN + 31] == 0x04 && ram[BUF_IN + 32] == 0x00,
          "exactly thirty-two bytes are written, and no more");
    check(td_cc(TD_IN) == XBOX_OHCI_CC_NOERROR,
          "a short read is not reported as an error");
    check((at(ED_VA)[2] & 1u) == 0u, "and does not halt the endpoint");
    check(s.tds_retired == 3, "so the status stage retires with the rest");
}

static void test_enumeration_sequence(void)
{
    /* The whole conversation JSRF actually has, in order, measured in
     * claude-usb-exp-a2 and -a3. Each step is the request the title issues
     * once the one before it has completed. */
    static const uint8_t dev8[8]   = { 0x80, 0x06, 0x00, 0x01, 0, 0, 0x08, 0 };
    static const uint8_t setaddr[8]= { 0x00, 0x05, 0x01, 0x00, 0, 0, 0x00, 0 };
    static const uint8_t cfg80[8]  = { 0x80, 0x06, 0x00, 0x02, 0, 0, 0x50, 0 };
    static const uint8_t setcfg[8] = { 0x00, 0x09, 0x01, 0x00, 0, 0, 0x00, 0 };
    static const uint8_t xid[8]    = { 0xC1, 0x06, 0x00, 0x42, 0, 0, 0x10, 0 };
    uint32_t addressed = 0x00080000u | 1u;

    xbox_UsbDeviceReset();

    build_control(dev8, 8);
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(ram[BUF_IN] == 0x12, "1: the address-0 device probe answers");

    build_control(setaddr, 0);
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(xbox_UsbDeviceAddress() == 1, "2: SET_ADDRESS(1) takes effect");

    build_control(cfg80, 40);
    at(ED_VA)[0] = addressed;
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(ram[BUF_IN] == 0x09 && ram[BUF_IN + 2] == 0x20,
          "3: the configuration set answers at the new address");

    build_control(setcfg, 0);
    at(ED_VA)[0] = addressed;
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(xbox_UsbDeviceConfiguration() == 1, "4: SET_CONFIGURATION(1) sticks");

    build_control(xid, 16);
    at(ED_VA)[0] = addressed;
    run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(ram[BUF_IN] == 0x10 && ram[BUF_IN + 1] == 0x42,
          "5: the XID class descriptor answers");
    check(ram[BUF_IN + 6] == XBOX_USB_PAD_REPORT,
          "and declares the twenty-byte input report");
}

static void test_deferred_writeback(void)
{
    /* The driver submits from its completion DPC, before it acknowledges the
     * interrupt for the transfer before it. Hardware will not overwrite a done
     * queue the driver still owes an acknowledge for; it keeps retiring into
     * HcDoneHead and publishes the lot at the next writeback. */
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };
    xbox_ohci_service s;
    uint32_t stale = 0x00ABCDE0u;

    xbox_UsbDeviceReset();
    build_control(setup, 8);
    at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] = stale;

    memset(&s, 0, sizeof(s));
    s.ram = ram;
    s.ram_size = RAM_SIZE;
    s.hcca = HCCA_VA;
    s.head_ed = ED_VA;
    s.intr_status = XBOX_OHCI_INTR_WDH;   /* not yet acknowledged */
    s.intr_enable = XBOX_OHCI_INTR_MIE;
    xbox_OhciServiceList(&s);

    check(s.tds_retired == 3, "TDs still retire while a writeback is owed");
    check(s.published == 0, "nothing is published over an unacknowledged queue");
    check(at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] == stale,
          "the driver's queue is left exactly as it was");
    check(s.done_head == TD_OUT,
          "the retired TDs accumulate in HcDoneHead instead");

    /* The driver acknowledges; the next pass publishes what was held over,
     * even though it retires nothing of its own. */
    s.intr_status = 0;
    s.head_ed = 0;                        /* an empty list, as after a doorbell */
    xbox_OhciServiceList(&s);
    check(s.tds_retired == 0, "the flushing pass retires nothing");
    check(s.published == 1, "the held-over queue is published once it can be");
    check(at(HCCA_VA)[XBOX_OHCI_HCCA_DONE_HEAD / 4] == TD_OUT,
          "and it reaches the HCCA intact");
    check(s.done_head == 0, "HcDoneHead empties, so it is not published twice");
    check((s.intr_status & XBOX_OHCI_INTR_WDH) != 0,
          "the writeback raises its interrupt");
}

static void test_bounds_and_list_safety(void)
{
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };
    xbox_ohci_service s;

    /* A TD buffer pointing past the end of RAM must be reported, not
     * followed: this walk runs inside a signal handler. */
    xbox_UsbDeviceReset();
    build_control(setup, 8);
    put_td(TD_IN, 0xE3F00000u, RAM_SIZE - 2u, TD_OUT, RAM_SIZE + 16u);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(td_cc(TD_IN) == XBOX_OHCI_CC_DEVICENOTRESP,
          "a buffer that runs off the end of RAM is refused");

    /* An ED whose NextED points at itself must not spin. */
    xbox_UsbDeviceReset();
    build_control(setup, 8);
    put_ed(ED_VA, 0x00080000u, TD_TAIL, TD_SETUP, ED_VA);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.eds_walked == 1, "a self-referential ED list terminates");

    /* A TD ring must not spin either. */
    xbox_UsbDeviceReset();
    build_control(setup, 8);
    put_td(TD_SETUP, 0xE2E00000u, BUF_SETUP, TD_SETUP, BUF_SETUP + 7u);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired <= 64, "a self-referential TD chain terminates");

    /* A skipped ED is left entirely alone -- which is the state the capture
     * shows at the moment HcControlHeadED is published. */
    xbox_UsbDeviceReset();
    build_control(setup, 8);
    put_ed(ED_VA, 0x00084000u, TD_TAIL, TD_SETUP, 0);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 0, "a skipped ED retires nothing");
    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_NOTACCESSED,
          "a skipped ED's TDs keep their NotAccessed condition code");
    check((s.intr_status & XBOX_OHCI_INTR_WDH) == 0,
          "an empty pass raises no interrupt");

    /* A head of zero is the list being empty, not an error. */
    xbox_UsbDeviceReset();
    memset(&s, 0, sizeof(s));
    s.ram = ram;
    s.ram_size = RAM_SIZE;
    s.head_ed = 0;
    check(xbox_OhciServiceList(&s) == 0, "an empty list is a no-op");
}

static uint8_t g_hook_report[XBOX_USB_PAD_REPORT];
static int g_hook_present = 1;

static int pad_hook(uint8_t report[XBOX_USB_PAD_REPORT])
{
    if (!g_hook_present)
        return 0;
    memcpy(report, g_hook_report, XBOX_USB_PAD_REPORT);
    return 1;
}

static void test_interrupt_in_report(void)
{
    static const uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01,
                                      0x00, 0x00, 0x08, 0x00 };
    uint32_t len = 0;
    uint8_t out[XBOX_USB_PAD_REPORT];
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    xbox_SetUsbPadStateHook(NULL);
    check(xbox_UsbInterruptIn(1, out, sizeof(out), &len) == 0 &&
          len == XBOX_USB_PAD_REPORT,
          "with no backend the pad reports a neutral twenty-byte report");
    check(out[0] == 0x00 && out[1] == XBOX_USB_PAD_REPORT,
          "the report carries its id and length");
    check(xbox_UsbInterruptIn(3, out, sizeof(out), &len) == -1,
          "an endpoint this device does not have is refused");

    g_hook_present = 0;
    xbox_SetUsbPadStateHook(pad_hook);
    check(xbox_UsbInterruptIn(1, out, sizeof(out), &len) == 0 &&
          out[2] == 0x00 && out[4] == 0x00,
          "a backend with no gamepad still reports a neutral pad, never a refusal");

    memset(g_hook_report, 0, sizeof(g_hook_report));
    g_hook_report[0] = 0x00;
    g_hook_report[1] = XBOX_USB_PAD_REPORT;
    g_hook_report[2] = 0x10;                   /* Start */
    g_hook_report[4] = 0xFF;                   /* A held */
    g_hook_present = 1;
    xbox_SetUsbPadStateHook(pad_hook);
    check(xbox_UsbInterruptIn(1, out, sizeof(out), &len) == 0 &&
          out[2] == 0x10 && out[4] == 0xFF,
          "the backend's button state reaches the report");

    /* An interrupt ED whose endpoint is 1 delivers that report through the
     * list walker, not the control path. */
    xbox_UsbDeviceReset();
    memset(ram, 0, RAM_SIZE);
    put_ed(ED_VA, 0x00080000u | (1u << 7), TD_TAIL, TD_SETUP, 0);
    put_td(TD_SETUP, 0xE3F00000u, BUF_IN, TD_TAIL,
           BUF_IN + XBOX_USB_PAD_REPORT - 1u);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 1, "the interrupt endpoint retires its TD");
    check(ram[BUF_IN + 1] == XBOX_USB_PAD_REPORT && ram[BUF_IN + 4] == 0xFF,
          "the pad report lands in the TD's buffer");

    /* And the same state through GET_REPORT, which is how XID actually polls
     * the pad -- a class request on the control pipe, not the interrupt
     * endpoint. Measured in claude-usb-transfer-service-12. */
    {
        static const uint8_t get_report[8] = { 0xA1, 0x01, 0x00, 0x01,
                                               0x00, 0x00, 0x14, 0x00 };
        xbox_UsbDeviceReset();
        build_control(get_report, XBOX_USB_PAD_REPORT);
        s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
        check(s.tds_retired == 3, "GET_REPORT completes as an ordinary read");
        check(ram[BUF_IN + 1] == XBOX_USB_PAD_REPORT &&
              ram[BUF_IN + 2] == 0x10 && ram[BUF_IN + 4] == 0xFF,
              "and carries the pressed buttons to the title");
    }
    xbox_SetUsbPadStateHook(NULL);
}

/* ---- the output report: rumble ---------------------------------------- */

static unsigned g_rumble_calls;
static uint16_t g_rumble_left, g_rumble_right;

static void rumble_hook(uint16_t left, uint16_t right)
{
    g_rumble_calls++;
    g_rumble_left = left;
    g_rumble_right = right;
}

static void test_set_report_rumble(void)
{
    /* 21 09 00 02 00 00 06 00 -- SET_REPORT(output), the XID rumble packet on
     * the control pipe. Before 21 Sep 2026 this stalled, XPP tore the pad down
     * on the stall, and the first car hit of a session put up "please
     * reconnect the controller to port 3". */
    static const uint8_t set_report[8] = { 0x21, 0x09, 0x00, 0x02,
                                           0x00, 0x00, 0x06, 0x00 };
    static const uint8_t packet[6] = { 0x00, 0x06, 0x34, 0x12, 0x78, 0x56 };
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    xbox_SetUsbPadRumbleHook(rumble_hook);
    g_rumble_calls = 0;

    /* SETUP, a six-byte OUT data stage, a zero-length IN status stage. */
    memset(ram, 0, RAM_SIZE);
    put_ed(ED_VA, 0x00080000u, TD_TAIL, TD_SETUP, 0);
    put_td(TD_SETUP, 0xE2E00000u, BUF_SETUP, TD_IN, BUF_SETUP + 7u);
    put_td(TD_IN, 0xE3080000u, BUF_IN, TD_OUT, BUF_IN + 5u);     /* OUT data */
    put_td(TD_OUT, 0xE3F00000u, 0, TD_TAIL, 0);                  /* IN status */
    memcpy(ram + BUF_SETUP, set_report, 8);
    memcpy(ram + BUF_IN, packet, 6);

    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 3, "SET_REPORT retires all three TDs");
    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_NOERROR, "its SETUP completes");
    check(td_cc(TD_IN) == XBOX_OHCI_CC_NOERROR,
          "its OUT data stage completes instead of stalling");
    check(td_cc(TD_OUT) == XBOX_OHCI_CC_NOERROR, "and so does its status stage");
    check((at(ED_VA)[2] & 1u) == 0u, "the control endpoint is not halted");
    check(g_rumble_calls == 1, "the host rumble hook is called once");
    check(g_rumble_left == 0x1234 && g_rumble_right == 0x5678,
          "with the little-endian motor speeds from the packet");

    /* The status stage is what completes it: the hook must not fire on the
     * data stage alone, or a driver that splits the packet would rumble on
     * half of one. */
    xbox_UsbDeviceReset();
    g_rumble_calls = 0;
    memset(ram, 0, RAM_SIZE);
    put_ed(ED_VA, 0x00080000u, TD_TAIL, TD_SETUP, 0);
    put_td(TD_SETUP, 0xE2E00000u, BUF_SETUP, TD_IN, BUF_SETUP + 7u);
    put_td(TD_IN, 0xE3080000u, BUF_IN, TD_TAIL, BUF_IN + 5u);
    memcpy(ram + BUF_SETUP, set_report, 8);
    memcpy(ram + BUF_IN, packet, 6);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 2 && g_rumble_calls == 0,
          "no status stage yet, no rumble yet");
    put_td(TD_OUT, 0xE3F00000u, 0, TD_TAIL, 0);
    at(ED_VA)[2] = TD_OUT;
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 1 && g_rumble_calls == 1,
          "the status stage on a later pass completes it");

    /* A class request this device still does not know keeps stalling: the
     * fix is for the report, not a blanket yes. */
    {
        static const uint8_t set_idle[8] = { 0x21, 0x0A, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0x00 };
        xbox_UsbDeviceReset();
        build_control(set_idle, 0);
        s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
        check(td_cc(TD_IN) == XBOX_OHCI_CC_STALL,
              "an unknown class request still stalls");
    }
    xbox_SetUsbPadRumbleHook(NULL);
}

static void test_interrupt_out_rumble(void)
{
    /* The same packet on the interrupt OUT endpoint the configuration
     * descriptor advertises (EP2), which is the other way a driver sends it. */
    static const uint8_t packet[6] = { 0x00, 0x06, 0xFF, 0xFF, 0x00, 0x00 };
    xbox_ohci_service s;

    xbox_UsbDeviceReset();
    xbox_SetUsbPadRumbleHook(rumble_hook);
    g_rumble_calls = 0;
    memset(ram, 0, RAM_SIZE);
    /* endpoint 2, direction OUT (bits 11-12 = 01), MPS 32 */
    put_ed(ED_VA, 0x00200000u | (2u << 7) | (1u << 11), TD_TAIL, TD_SETUP, 0);
    put_td(TD_SETUP, 0xE3080000u, BUF_IN, TD_TAIL, BUF_IN + 5u);
    memcpy(ram + BUF_IN, packet, 6);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 1, "the interrupt OUT TD retires");
    check(td_cc(TD_SETUP) == XBOX_OHCI_CC_NOERROR, "without error");
    check(g_rumble_calls == 1 && g_rumble_left == 0xFFFF && g_rumble_right == 0,
          "and the packet reaches the host rumble hook");

    /* No hook installed: the packet is still consumed, never refused. */
    xbox_SetUsbPadRumbleHook(NULL);
    at(ED_VA)[2] = TD_SETUP;
    put_td(TD_SETUP, 0xE3080000u, BUF_IN, TD_TAIL, BUF_IN + 5u);
    s = run(HCCA_VA, XBOX_OHCI_INTR_MIE);
    check(s.tds_retired == 1 && td_cc(TD_SETUP) == XBOX_OHCI_CC_NOERROR,
          "with no host hook the report is accepted and dropped");
}

int main(void)
{
    test_captured_get_descriptor();
    test_hcca_done_head_low_bit();
    test_set_address_takes_effect_after_status();
    test_configuration_and_config_descriptor();
    test_unsupported_request_stalls();
    test_short_read_completes_cleanly();
    test_enumeration_sequence();
    test_deferred_writeback();
    test_bounds_and_list_safety();
    test_interrupt_in_report();
    test_set_report_rumble();
    test_interrupt_out_rumble();

    if (failures) return 1;
    puts("ohci_transfer_test: all checks passed");
    return 0;
}
