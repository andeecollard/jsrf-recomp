#!/usr/bin/env python3
"""Diff a recomp object dump against an xemu one, field by field.

Both sides emit the same schema: xemu via xemu_objects.py over the GDB stub,
the recomp via RECOMP_OBJECT_DUMP. The two heaps sit at different bases, so a
raw pointer diff would report every object as divergent. Pointer-valued fields
are therefore compared *symbolically*: each is resolved to the id of the object
it points at, and only unresolvable pointers fall back to a base-relative
comparison. Code pointers (vtables) are compared raw, because the XBE is mapped
at the same VA on both sides and a vtable difference would be real.

Usage: compare_objects.py <recomp.json> <xemu.json>
"""
import json
import struct
import sys


def f32(bits):
    """Reinterpret a captured u32 as the float the guest stored there."""
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]

PTR_FIELDS = ["parent", "child", "sibling_before", "sibling_next",
              "draw_next", "draw_before_ptr", "draw_last_ptr"]
RAW_FIELDS = ["vtable", "flags", "stored_id", "draw_child_mask", "zsort_key",
              "zsort", "extra_44", "extra_48"]
FLOAT_FIELDS = ["fz", "tx", "ty", "tz"]
MANAGER_RAW = ["live", "skip_draw", "draw_mode",
               "state_7930", "state_7934", "state_7EC4"]
MANAGER_PTR = ["exec_root", "draw_root", "draw_root_tail",
               "draw_sort_root", "draw_sort_tail"]


def load(path):
    d = json.load(open(path))
    d["_by_id"] = {o["id"]: o for o in d["objects"]}
    d["_by_addr"] = {o["address"]: o["id"] for o in d["objects"]}
    return d


def sym(side, value):
    """Name a pointer by what it points at, so the two heaps are comparable.

    The draw links are intrusive: m_lplpDrawBeforePtr points *into* the middle
    of the previous node, at its m_lpDrawNext field, and m_lplplpDrawLastPtr
    points at a tail slot inside CActMan. Resolving only exact object bases
    would leave both as raw addresses and manufacture a diff for every object,
    so interior pointers are named as base+offset.
    """
    if value == 0:
        return "null"
    if value in side["_by_addr"]:
        return f"obj#{side['_by_addr'][value]}"
    delta = value - side["root"]
    if 0 <= delta < 0x8840:
        return f"root+{delta:#x}"
    for addr, oid in side["_by_addr"].items():
        if addr < value < addr + 0x50:
            return f"obj#{oid}+{value - addr:#x}"
    if side.get("exec_root") and 0 <= value - side["exec_root"] < 0x50:
        return f"execroot+{value - side['exec_root']:#x}"
    return f"unresolved:{value - side['root']:+#x}"


def main():
    rec, xem = load(sys.argv[1]), load(sys.argv[2])
    bad = 0

    print(f"recomp root {rec['root']:#010x}   xemu root {xem['root']:#010x}"
          f"   (delta {rec['root'] - xem['root']:+#x})")

    print("\n== manager ==")
    for f in MANAGER_RAW:
        a, b = rec.get(f), xem.get(f)
        mark = "  " if a == b else "!!"
        if a != b:
            bad += 1
        print(f" {mark} {f:16s} recomp={a!s:>12}  xemu={b!s:>12}")
    for f in MANAGER_PTR:
        a, b = sym(rec, rec.get(f, 0)), sym(xem, xem.get(f, 0))
        mark = "  " if a == b else "!!"
        if a != b:
            bad += 1
        print(f" {mark} {f:16s} recomp={a:>12}  xemu={b:>12}")

    ra = {b["bin"]: sym(rec, b["head"]) for b in rec.get("draw_sort_bins", [])}
    xa = {b["bin"]: sym(xem, b["head"]) for b in xem.get("draw_sort_bins", [])}
    if ra != xa:
        bad += 1
        print(f" !! draw_sort_bins   recomp={ra}  xemu={xa}")
    else:
        print(f"    draw_sort_bins   both {ra}")

    print("\n== id set ==")
    r_ids, x_ids = set(rec["_by_id"]), set(xem["_by_id"])
    if r_ids == x_ids:
        print(f"    identical, {len(r_ids)} ids")
    else:
        bad += 1
        print(f" !! recomp only: {sorted(r_ids - x_ids)}")
        print(f" !! xemu   only: {sorted(x_ids - r_ids)}")

    print("\n== per-object ==")
    for oid in sorted(r_ids & x_ids):
        r, x = rec["_by_id"][oid], xem["_by_id"][oid]
        diffs = []
        for f in RAW_FIELDS:
            if r.get(f) != x.get(f):
                diffs.append(f"{f}: recomp={r.get(f, 0):#010x} xemu={x.get(f, 0):#010x}")
        for f in FLOAT_FIELDS:
            if f not in r or f not in x:
                continue
            a, b = f32(r[f]), f32(x[f])
            if r[f] != x[f]:
                diffs.append(f"{f}: recomp={a:.4f} xemu={b:.4f}"
                             f"  (delta {a - b:+.4f})")
        for f in PTR_FIELDS:
            a, b = sym(rec, r[f]), sym(xem, x[f])
            if a != b:
                diffs.append(f"{f}: recomp={a} xemu={b}")
        if diffs:
            bad += 1
            print(f" !! id {oid} (vtable {r['vtable']:#010x})")
            for d in diffs:
                print(f"      {d}")
    if not bad:
        print("    every common object matches on every compared field")

    print(f"\n{bad} divergent field group(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
