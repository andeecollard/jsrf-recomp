# How CrossOver exposes a controller

**CORRECTED 2026-09-12, later.** Two pads have now been measured on both hosts
and the picture is better than this note first said:

| pad | macOS (SDL) | Windows (CrossOver) |
|---|---|---|
| DualShock 4 (054C:09CC) | works | works, but only via the DirectInput fallback added in 0d60248 -- XInput reports 1167 |
| Xbox Series X wired, PowerA (20D6:2062) | works, "Xbox Series X Controller" | **works natively via XInput**, ports 0 and 1 report CONNECTED |

So an Xbox-layout pad is the better choice for the CrossOver build: it needs no
fallback and lands on the path the code always had.

A claim in an earlier draft of this file was WRONG and is retracted: that the
PowerA unit speaks GIP and so cannot appear as a HID gamepad on macOS. It does
appear -- `ioreg -c IOHIDDevice` shows `"Product" = "Controller"` with
PrimaryUsagePage 1, PrimaryUsage 5, which is Generic Desktop / Game Pad. The
reading that produced the wrong conclusion was a single `0 joystick(s) seen`
line taken before SDL had hotplugged the device; the very next run showed
`port 0: Xbox Series X Controller (opened)`, 844 of 844 polls connected. One
sample, before a hotplug had settled, is not a measurement.


Date: 2026-09-12 (Europe/London)
Measured with a DualShock 4 attached to the Mac by USB.

## Short version

CrossOver exposes the pad correctly. **XInput is the wrong API for a
DualShock 4 — on real Windows as much as under Wine** — and the Windows branch
of `src/input/xinput_device.c` uses nothing else.

## What is actually there

The bottle has the whole Wine input stack: `winebus.sys`, `winehid.sys`,
`winexinput.sys`, `hidclass.sys`, and `xinput1_1..1_4`, `xinput9_1_0`,
`dinput`, `dinput8`.

`winebus` has already enumerated the pad. From the bottle registry:

    System\CurrentControlSet\Enum\WINEBUS\VID_054C&PID_09CC\...
    System\CurrentControlSet\Enum\HID\VID_054C&PID_09CC\...   Class = HIDClass

`054C:09CC` is Sony, DualShock 4 second generation (CUH-ZCT2). It is
registered under the HID device-interface GUID
`{4D1E55B2-F16F-11CF-88CB-001111000030}`. There is **no `WINEXINPUT` entry
anywhere in the bottle**, so the XInput shim never attached to it.

(The other device, `VID_845E&PID_0001`, is Wine's virtual mouse -- it carries
`GUID_DEVINTERFACE_MOUSE`, not a gamepad interface. Not a pad.)

## Measured, not inferred

Two probes, built with the clang/mingw toolchain and run in the bottle. Both
are kept at `C:\jsrf\xiprobe.exe` and `C:\jsrf\diprobe.exe` with `run_xi.bat`
and `run_di.bat` beside them, so this is one command to re-check.

**XInput** -- 4 ports, 3 rounds, every single call:

    XInputGetState -> 1167   (ERROR_DEVICE_NOT_CONNECTED)

**DirectInput8** -- same bottle, same moment:

    DirectInput8Create -> 0x00000000
    device 0: "Wireless Controller"  type=0x00010215
    total attached game controllers: 1

`0x00010215` is `DIDEVTYPE_HID | DI8DEVTYPE_GAMEPAD` with subtype 2. So
DirectInput sees the pad, correctly classified as a gamepad, and XInput sees
nothing.

## Why, and why it is not a Wine bug

XInput only ever supported Xbox-family controllers. A DualShock 4 is not an
XInput device on real Windows either -- it needs DS4Windows, Steam Input or
similar to be translated into one. Wine's `winexinput.sys` attaches to devices
`winebus` flags `is_gamepad`, and its IOHID/hidraw backend presents the DS4 with
its own report descriptor rather than as a synthesised Xbox pad. That is the
same answer Windows itself gives.

macOS is unaffected because the POSIX branch of the harness goes through SDL,
which carries DS4 mappings in its controller database. That is the whole reason
one host sees a pad and the other does not -- not the OHCI path, not the guest.

## What to do about it

In rough order of effort:

1. **Plug in an Xbox-layout pad.** Zero code. An Xbox 360/One/Series controller
   is a real XInput device and the existing Windows path should find it. This
   is also the cheapest way to test the rest of the input chain, which is still
   unproven on Windows.
2. **Add a DirectInput8 fallback** to the Windows branch of
   `src/input/xinput_device.c`, used when `XInputGetState` returns
   `ERROR_DEVICE_NOT_CONNECTED` on every port. The device is already there and
   already classified as a gamepad; it needs enumerating, acquiring and its
   axes mapping to `XBOX_INPUT_STATE`. This is what makes the Windows build
   work with the pad that is actually on the desk.
3. Steam Input or DS4Windows in the bottle. Works, but adds a dependency to
   every future run and hides the real gap.

## Do not conclude from this

That Windows input is fixed once a pad is visible. `polls=0` on Windows was
measured with NO pad attached and with `RECOMP_USB` unset, and the OHCI service
path has separately been found never to link (see
`CLAUDE_HANDOVER_2026-09-12_TLS_IS_THE_BOTTLENECK.txt` and the note in
kernel_bridge.c where `xbox_AllocThreadTib` would go). Those are three
different problems and only this one is now understood.
