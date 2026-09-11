#!/usr/bin/env python3
"""Find object fields that identify a character in xemu but not in the recomp.

The Corn tutorial breakthrough was noticing a pattern by eye: fields holding
DISTINCT values for the two CPlayers in xemu while holding the SAME value for
both in the recomp. That is the signature of two objects constructed with one
shared identity, and it is mechanically detectable across every offset instead
of being spotted by hand.

Four classes are reported:

  IDENTITY-LOST   distinct in xemu, identical here  <- the signature of interest
  IDENTITY-GAINED identical in xemu, distinct here  <- the inverse, also a bug
  BOTH-DISTINCT   distinct on both sides            <- healthy per-character data
  VALUE-DIFF      identical on both sides, but the values differ

Pointer-valued fields are noisy because the heaps sit at different bases, so an
offset whose values look like guest addresses on either side is flagged rather
than trusted.

Usage: signature_scan.py <xemu_a.bin> <xemu_b.bin> <recomp_a.bin> <recomp_b.bin>
"""
import struct
import sys


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def looks_like_pointer(v):
    return 0x00010000 <= v < 0x08000000


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    xa, xb, ra, rb = (open(p, "rb").read() for p in sys.argv[1:5])
    span = min(len(xa), len(xb), len(ra), len(rb)) & ~3

    lost, gained, both, valdiff = [], [], [], []
    for off in range(0, span, 4):
        x1, x2 = u32(xa, off), u32(xb, off)
        r1, r2 = u32(ra, off), u32(rb, off)
        xd, rd = x1 != x2, r1 != r2
        ptr = any(looks_like_pointer(v) for v in (x1, x2, r1, r2))
        row = (off, x1, x2, r1, r2, ptr)
        if xd and not rd:
            lost.append(row)
        elif rd and not xd:
            gained.append(row)
        elif xd and rd:
            both.append(row)
        elif (x1, x2) != (r1, r2):
            valdiff.append(row)

    def show(title, rows, limit=40):
        print(f"\n=== {title}: {len(rows)} offsets ===")
        for off, x1, x2, r1, r2, ptr in rows[:limit]:
            mark = "  (pointer-ish)" if ptr else ""
            print(f"  +{off:#06x}  xemu {x1:#010x} / {x2:#010x}"
                  f"   recomp {r1:#010x} / {r2:#010x}{mark}")
        if len(rows) > limit:
            print(f"  ... {len(rows) - limit} more")

    print(f"compared {span} bytes ({span // 4} dwords) of each object")
    show("IDENTITY-LOST  (distinct in xemu, SAME here)", lost)
    show("IDENTITY-GAINED (same in xemu, distinct here)", gained)
    print(f"\n=== BOTH-DISTINCT: {len(both)} offsets (healthy per-character data) ===")
    print(f"=== VALUE-DIFF:    {len(valdiff)} offsets ===")

    solid = [r for r in lost if not r[5]]
    print(f"\nIDENTITY-LOST excluding pointer-ish values: {len(solid)}")
    for off, x1, x2, r1, r2, _ in solid[:60]:
        print(f"  +{off:#06x}  xemu {x1:>10} / {x2:<10}  recomp {r1:>10} / {r2}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
