#!/usr/bin/env python3
"""Capture the CPlayer +0x9E4 index provenance from xemu, whole.

The index that selects a CPlayer's per-frame behaviour is built entirely from
guest state and one image-resident table:

    edi = *( 0x2104A0 + [this+0xE6C]*96 + (0 if [this+0xE70]==0 else 0x30) + 0xC )
    then  [this+0x9E4] = edi

xemu supplies 233 and 234 for the two CPlayers; the recomp supplies 61 for
both. Every input to that expression is captured here in one pass, so the
comparison never needs another manual playthrough.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xemu_rsp import XemuRSP

TABLE = 0x002104A0
STRIDE = 96
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("xemu_index_chain.json")

with XemuRSP() as x:
    root = x.u32(x.read(0x0022FCE0, 4))
    mgr = x.read(root, 0x8840)
    result = {"root": root, "live": x.u32(mgr, 0x87E8), "objects": {}}
    print(f"root={root:#010x} live={result['live']}")
    for oid in (44, 45):
        a = x.u32(mgr, 0x98 + oid * 4)
        blob = x.read(a, 0x2000)
        if blob is None or len(blob) < 0x1148:
            print(f"  id {oid}: short read"); continue
        Path(f"/tmp/xemu_cplayer_{oid}.bin").write_bytes(blob)
        e6c, e70 = x.u32(blob, 0xE6C), x.u32(blob, 0xE70)
        rec = TABLE + e6c * STRIDE + (0 if e70 == 0 else 0x30)
        row = x.read(rec, 0x30)
        idx = x.u32(row, 0xC) if row and len(row) >= 0x10 else None
        result["objects"][oid] = {
            "address": a, "vtable": x.u32(blob), "e6c": e6c, "e70": e70,
            "record_va": rec, "record": list(row[:0x30]) if row else None,
            "table_idx_field": idx, "actual_9E4": x.u32(blob, 0x9E4),
            "e54": x.u32(blob, 0xE54),
        }
        print(f"  id {oid} @ {a:#010x}  +0xE6C={e6c}  +0xE70={e70:#x}  "
              f"record@{rec:#010x} +0xC={idx}  actual +0x9E4={x.u32(blob,0x9E4)}")
    # A slice of the table itself, to compare contents side to side.
    tab = x.read(TABLE, STRIDE * 64)
    if tab:
        Path("/tmp/xemu_index_table.bin").write_bytes(tab)
        result["table_bytes"] = len(tab)
        print(f"  saved {len(tab)} bytes of the table at {TABLE:#010x}")

OUT.write_text(json.dumps(result, indent=2))
print(f"wrote {OUT}")
