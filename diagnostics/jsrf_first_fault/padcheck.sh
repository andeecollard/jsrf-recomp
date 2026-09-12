#!/bin/sh
# Is a pad visible to the two things that matter, right now?
#
# Both hosts read the pad through macOS's HID layer -- the POSIX build via SDL,
# the CrossOver build via Wine's winebus/IOHID backend -- so if macOS is not
# exposing a Generic Desktop / Game Pad interface, neither build can see one
# however healthy the USB connection looks. A controller sitting on the bus
# without that interface reads exactly like a controller that is unplugged,
# which has now cost two runs.
#
# Takes a second. Run it before a four minute boot, not after.
printf 'USB bus:\n'
ioreg -p IOUSB -l -w 0 2>/dev/null | grep -E '"USB Product Name"' \
  | sed 's/.*= /  /' | grep -viE 'hub|keyboard|trackpad' || printf '  (nothing)\n'

printf 'HID gamepad interfaces (Generic Desktop / Game Pad):\n'
found=$(ioreg -c IOHIDDevice -r -a 2>/dev/null | python3 -c '
import sys, plistlib
def walk(n, out):
    for x in (n if isinstance(n, list) else [n]):
        if not isinstance(x, dict): continue
        if x.get("PrimaryUsagePage") == 1 and x.get("PrimaryUsage") == 5:
            out.append("  %s  vid=0x%04X pid=0x%04X" % (
                x.get("Product") or "(unnamed)",
                x.get("VendorID") or 0, x.get("ProductID") or 0))
        if "IORegistryEntryChildren" in x: walk(x["IORegistryEntryChildren"], out)
try:
    out = []; walk(plistlib.loads(sys.stdin.buffer.read()), out); print("\n".join(out))
except Exception: pass
' 2>/dev/null)
if [ -n "$found" ]; then
    printf '%s\n' "$found"
    printf '\n  -> both builds should see a pad.\n'
else
    printf '  (none)\n'
    printf '\n  -> NEITHER build can see a pad. Unplug and replug it; some pads\n'
    printf '     only present their HID interface after a button press, and a\n'
    printf '     direct port works where a hub does not.\n'
fi
