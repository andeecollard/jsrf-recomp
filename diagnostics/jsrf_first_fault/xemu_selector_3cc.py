#!/usr/bin/env python3
"""Read CPlayer +0x3CC on xemu, and check whether +0xE50 is stable there.

WHY THIS FIELD. sub_00080340, CPlayer's exec virtual, dispatches every frame on
[this+0x3CC]:

    0x00080716  mov eax, [esi+0x3CC]
                == 0 -> 0x0008073A
                == 1 -> sub_00092690
                == 2 -> sub_00092750

sub_00092750 sets [this+0x11C] := 0x80 (anim 128) and [this+0xE50] := 0x0F
(state 15), and it is the ONLY site in .text that writes 15 to +0xE50. On our
side the tutorial CPlayers read state 15 / anim 128 and the pose is frozen;
during the healthy intro the same field reads +0x3CC == 0 and the pose moves.

WHAT WOULD SETTLE IT. On xemu the previous handover measured e50 = 23 and 25
while Corn was visibly animating. If that is STABLE there -- never 15 --
sub_00092750 does not run on hardware, so +0x3CC is not 2 there, and the fault
is whatever computes +0x3CC. If xemu also reads 2 and 15, the dispatch is right
and the fault is downstream.

The trap this script exists to avoid: comparing a field's VALUE is not comparing
how often it is WRITTEN. The previous handover compared e50 against xemu, found
23 and 25 on both sides, and crossed the state band off the list -- but on our
side that value is rewritten every frame by two fighting paths, which reads
identical to a value written once. So this samples repeatedly and reports the
DISTRIBUTION, not one reading.

POSITIVE CONTROL. It also hashes the transform block at +0xCE0..+0xDDC each
sample. If that hash never changes, xemu is not animating either and the whole
comparison is void -- so the hash moving is what makes a stable e50 meaningful.

Usage, with xemu ALREADY AT THE CORN TUTORIAL and launched with a GDB stub:

    /Applications/xemu.app/Contents/MacOS/xemu -s
    SAMPLES=8 GAP=1.5 /usr/bin/python3 \
        diagnostics/jsrf_first_fault/xemu_selector_3cc.py

Read-only. XemuRSP resumes the guest on every exit path; stranding it costs a
manual replay of the whole tutorial.
"""
import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xemu_rsp import XemuRSP                                    # noqa: E402

ROOT_PTR = 0x0022FCE0
ROOT_VA = 0x005E3A70
IDS_OFF = 0x98
POSE_OFF = 0xCE0
POSE_LEN = 252


def fnv1a(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def sample(idx):
    out = {}
    with XemuRSP() as x:
        via = x.u32(x.read(ROOT_PTR, 4))
        # Same low-16 rule the recomp uses: the root literal moves between runs,
        # its low 16 bits do not, and a bare range check once accepted a pointer
        # 0x2BE00 bytes off with every field then read from the wrong object.
        if via is None or (via & 0xFFFF) != (ROOT_VA & 0xFFFF):
            print("root pointer %s rejected (low16 != %04X); is the title past "
                  "the logos?" % (("%08X" % via) if via else "unreadable",
                                  ROOT_VA & 0xFFFF))
            return None
        root = via
        seq_obj = x.u32(x.read(root + IDS_OFF, 4))
        seq = x.u32(x.read(seq_obj + 0x48, 4)) if seq_obj else None
        live = x.u32(x.read(root + 0x87E8, 4))
        print("[%d] root=%08X live=%s seq=%s" % (idx, root, live, seq))
        for oid in (44, 45):
            p = x.u32(x.read(root + IDS_OFF + oid * 4, 4))
            if not p:
                print("  id=%d not present" % oid)
                continue
            e50 = x.u32(x.read(p + 0xE50, 4))
            s11c = x.u32(x.read(p + 0x11C, 4))
            sel = x.u32(x.read(p + 0x3CC, 4))
            e6c = x.u32(x.read(p + 0xE6C, 4))
            blk = x.read(p + POSE_OFF, POSE_LEN)
            ph = fnv1a(blk) if blk else None
            print("  id=%d this=%08X  +0x3CC=%s  e50=%s  11c=%s  e6c=%s"
                  "  pose=%s"
                  % (oid, p, sel, e50, s11c, e6c,
                     ("%08X" % ph) if ph is not None else "?"))
            out[oid] = (sel, e50, s11c, ph)
    return out


def main():
    n = int(os.environ.get("SAMPLES", "8"))
    gap = float(os.environ.get("GAP", "1.5"))
    seen = []
    for i in range(n):
        if i:
            time.sleep(gap)
        r = sample(i)
        if r is None:
            return 1
        seen.append(r)

    print("\n=== distribution over %d samples ===" % len(seen))
    for oid in (44, 45):
        rows = [s[oid] for s in seen if oid in s]
        if not rows:
            continue
        sel = Counter(r[0] for r in rows)
        e50 = Counter(r[1] for r in rows)
        s11c = Counter(r[2] for r in rows)
        poses = [r[3] for r in rows]
        moved = len(set(poses)) > 1
        print("  id=%d" % oid)
        print("    +0x3CC : %s" % dict(sel))
        print("    e50    : %s" % dict(e50))
        print("    11c    : %s" % dict(s11c))
        print("    pose   : %d distinct of %d samples -> %s"
              % (len(set(poses)), len(poses),
                 "ANIMATING" if moved else "STATIC (control FAILED - a stable "
                 "e50 proves nothing if xemu is not animating either)"))
    print("\nOurs at the tutorial, for comparison: +0x3CC=2, e50=15, 11c=128,")
    print("pose byte-identical across 16 consecutive reports.")
    print("Ours during the healthy intro:        +0x3CC=0, pose moving.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
