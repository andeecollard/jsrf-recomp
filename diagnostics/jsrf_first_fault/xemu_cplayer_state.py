#!/usr/bin/env python3
"""Read the two CPlayers' state fields from xemu, in the recomp's own schema.

The recomp's probe at 0x00011083 prints, for global object ids 44 and 45:

    [CORN] id=45 ... e50=25 r2d0=04846280 r38=00000000   <== POSE SKIPPED

and the whole tutorial question now turns on whether xemu reads the same at the
same moment. sub_00080340 -- CPlayer's exec virtual, vtable slot 1 of
0x001CCFF8 -- short-circuits on both of these:

    0x00080340:  if ([this+0xE50] <= 0x1A) skip the timer path
    0x00080407:  if ([[this+0x2D0]+0x38] == 0) skip the pose writes

On our side both are taken every frame from the moment the tutorial scene
exists, so the characters never move. Which end to work from depends entirely
on what xemu has here:

  * xemu also shows e50 in 0x14..0x1A -> the state is RIGHT and the fault is in
    what the state's handler does. Keep descending from sub_000A76E0.
  * xemu shows e50 outside that band -> the state was CHOSEN wrongly, and the
    0x09/0x0A setter family is where it should have gone instead.

Usage, with xemu already at the Corn tutorial and launched with a GDB stub:

    /usr/bin/python3 diagnostics/jsrf_first_fault/xemu_cplayer_state.py

Read-only. XemuRSP resumes the guest on every exit path, including exceptions;
stranding it costs a manual replay of the whole tutorial.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xemu_rsp import XemuRSP                                    # noqa: E402

ROOT_PTR = 0x0022FCE0
ROOT_VA = 0x005E3A70
IDS_OFF = 0x98
FIELDS = (("e50", 0xE50), ("e60", 0xE60), ("e68", 0xE68),
          ("s11c", 0x11C), ("flags", 0x04), ("own_id", 0x08))


def main():
    with XemuRSP() as x:
        via = x.u32(x.read(ROOT_PTR, 4))
        # The same low-16 rule the recomp uses. The root literal moves between
        # runs; its low 16 bits do not. A bare range check once accepted a
        # pointer 0x2BE00 bytes off and every field read came from the wrong
        # object.
        if via is None or (via & 0xFFFF) != (ROOT_VA & 0xFFFF):
            print("root pointer %s rejected (low16 is not %04X); is the title "
                  "past the logos?" % (("%08X" % via) if via else "unreadable",
                                       ROOT_VA & 0xFFFF))
            return 1
        root = via
        print("root=%08X live=%s" % (root, x.u32(x.read(root + 0x87E8, 4))))
        for oid in (44, 45):
            p = x.u32(x.read(root + IDS_OFF + oid * 4, 4))
            if not p:
                print("  id=%d  not present" % oid)
                continue
            vals = {n: x.u32(x.read(p + off, 4)) for n, off in FIELDS}
            r2d0 = x.u32(x.read(p + 0x2D0, 4))
            r38 = x.u32(x.read(r2d0 + 0x38, 4)) if r2d0 else None
            skipped = (vals["e50"] is not None and 0x14 <= vals["e50"] <= 0x1A)
            print("  id=%d this=%08X e50=%s e60=%s e68=%s s11c=%s"
                  " r2d0=%08X r38=%s%s%s"
                  % (oid, p, vals["e50"], vals["e60"], vals["e68"],
                     ("%08X" % vals["s11c"]) if vals["s11c"] is not None else "?",
                     r2d0 or 0,
                     ("%08X" % r38) if r38 is not None else "?",
                     "   <== e50 IN THE SKIP BAND" if skipped else "",
                     "   <== POSE SKIPPED" if r38 == 0 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
