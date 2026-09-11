#!/usr/bin/env python3
"""Which regions of a CPlayer does xemu write per frame, at the current scene?

The recomp updates a CPlayer's HIGH region (above +0xC94) in the Corn tutorial
and never touches the low ~2.5 KB (+0x0000..+0x09F4), while in ordinary
gameplay it writes both. That comparison is recomp-vs-recomp; this takes the
same measurement on the oracle so the differential is against xemu AT THE SAME
SCENE, which is the only version of it that can settle the framing.

Usage: xemu_region_delta.py [seconds] [out.json]
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xemu_rsp import XemuRSP

SPAN = 0x2000
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0


def sample(x, ids):
    root = x.u32(x.read(0x0022FCE0, 4))
    mgr = x.read(root, 0x8840)
    out = {}
    for oid in ids:
        a = x.u32(mgr, 0x98 + oid * 4)
        if 0x10000 <= a < 0x08000000:
            blob = x.read(a, SPAN)
            if blob:
                out[oid] = (a, blob)
    return root, out


def runs(offs):
    out = []
    for o in offs:
        if out and o == out[-1][1] + 4:
            out[-1][1] = o
        else:
            out.append([o, o])
    return out


with XemuRSP() as x:
    root, first = sample(x, (44, 45, 46))
    print(f"root={root:#010x}  CPlayers={[hex(v[0]) for v in first.values()]}")
    x.resume()
    time.sleep(SECONDS)
    # re-enter the stopped state for the second read
    x.sock.sendall(b"\x03")
    time.sleep(0.25)
    x._recv(5.0)
    x._resumed = False
    _, second = sample(x, tuple(first))

result = {"root": root, "seconds": SECONDS, "objects": {}}
LOW_HI = 0x09F4
for oid, (a, b1) in first.items():
    if oid not in second:
        continue
    b2 = second[oid][1]
    n = min(len(b1), len(b2))
    ch = [i for i in range(0, n, 4) if b1[i:i + 4] != b2[i:i + 4]]
    low = [o for o in ch if o <= LOW_HI]
    ce0 = [o for o in ch if 0xCE0 <= o < 0xD20]
    result["objects"][oid] = {"address": a, "changed": len(ch),
                              "low_region": len(low), "ce0_block": len(ce0),
                              "runs": runs(ch)[:40]}
    print(f"\n  CPlayer {oid} @ {a:#010x}: {len(ch)} dwords changed in {SECONDS:g}s")
    print(f"    +0x0000..+0x09F4 (the region the recomp never writes): "
          f"{len(low)} dwords")
    print(f"    +0xCE0 transform block: {len(ce0)}/16")
    for lo, hi in runs(ch)[:14]:
        print(f"      +{lo:#06x}..+{hi:#06x}  ({(hi - lo) // 4 + 1})")

if len(sys.argv) > 2:
    Path(sys.argv[2]).write_text(json.dumps(result, indent=2))
    print(f"\nwrote {sys.argv[2]}")
