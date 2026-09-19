#!/usr/bin/env python3
"""Which switch arms did the lifter fail to translate, and how many are there?

WHY THIS EXISTS. A `jmp dword ptr [reg*4 + table]` is a C switch. The lifter
turns it into a chain of gotos -- but only when it can prove the table's arms
lie inside the function being lifted (lifter.py `_analyze_switch_table`). When
it cannot, the dispatch falls through to `RECOMP_ITAIL`, an indirect tail jump
that must be resolved at RUNTIME against the function database. An arm that is
only reachable through its own table is never a known function start, so the
lookup fails, and RECOMP_ITAIL's failure path is

    recomp_icall_fail_log(va); g_esp += 4; g_eax = 0;

followed by `return`. That is the correct emulation of a REAL tail jump, whose
caller has already run its epilogue and left esp on the return address. It is
badly wrong for a switch, where esp is still the function's whole frame below
that: the epilogue never runs, and the generated function returns with the
guest esp short by locals + saved registers. Callers then pop ebx/esi/edi --
globals in this ABI -- from the wrong slots, and the damage surfaces somewhere
else entirely, as a caller walking a structure through a register it never set.

The proof case, 19 Sep 2026: sub_000A5B60 (0x50 of locals, 4 pushes, 0x60 in
all) dispatches through the table at 0x000A60A4, whose five arms all sit past
its carved end of 0x000A5B8C. Arm 0x000A5B8C is taken once per session, the
lookup fails, and the walk that the player's crash happens inside resumes
0x60 bytes low. All four of the player's crash logs carry exactly one
"[ICALL] Failed to resolve VA 0x000A5B8C", 6-13 lines before the fault.

So the question this answers is not "is there a bug" but "how much of the
title is standing on one". Run it against a gen tree:

    python3 -m diagnostics.jsrf_first_fault.switch_arm_audit \\
        --xbe "$JSRF_GAME_DIR/default.xbe" --gen build-macos/jsrf-first-fault/gen

READ THE CAVEAT BEFORE QUOTING THE NUMBER. A jump table is followed by
ordinary code, and the next function's bytes routinely read as a valid .text
address, so walking the table until the first non-.text word OVERRUNS it --
the same over-read lifter.py's own comment warns about. Two bounds are
printed. The loose one walks to the first non-.text word; the tight one also
requires the arm to sit within WINDOW bytes of the dispatching function's
start, which is true of every real switch arm and false of most over-read.
Quote the tight one. Both were calibrated by hand against 0x000A60A4 (5 arms)
and 0x00114F90 (3 arms), where the tight bound is exact.
"""
import argparse
import glob
import os
import re
import struct
import sys

# An arm belongs to its own function. 8 KB is generous for JSRF, whose largest
# translated function is under 3 KB, and it is the difference between counting
# a table and counting the code after it.
WINDOW = 0x2000

OWNER_RE = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)", re.M)
DEF_RE = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)", re.M)
ITAIL_RE = re.compile(r"RECOMP_ITAIL\(MEM32\([a-z]+ \* 4 \+ 0x([0-9A-F]+)\)\)")


def load_image(xbe_path):
    """Guest VA -> bytes, for the .text section and for reading tables."""
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
    from tools.xbe_parser.xbe_parser import XBEParser

    xbe = XBEParser(xbe_path).parse()
    data = open(xbe_path, "rb").read()
    sections = [(s.virtual_addr, s.virtual_size, s.raw_addr, s.name)
                for s in xbe.sections]
    text = next(s for s in sections if s[3] == ".text")

    def read32(va):
        for va0, vsize, raw, _ in sections:
            if va0 <= va < va0 + vsize:
                off = raw + (va - va0)
                if off + 4 <= len(data):
                    return struct.unpack_from("<I", data, off)[0]
        return None

    return read32, text[0], text[0] + text[1]


def scan_gen(gen_dir):
    """Every RECOMP_ITAIL through a scaled table, and every defined body."""
    defined = set()
    sites = []
    for path in sorted(glob.glob(os.path.join(gen_dir, "*.c"))):
        src = open(path, errors="ignore").read()
        defined.update(DEF_RE.findall(src))
        owners = [(m.start(), m.group(1)) for m in OWNER_RE.finditer(src)]
        for m in ITAIL_RE.finditer(src):
            owner = None
            for start, name in owners:
                if start < m.start():
                    owner = name
                else:
                    break
            sites.append((owner, int(m.group(1), 16), os.path.basename(path)))
    return defined, sites


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--window", type=lambda v: int(v, 0), default=WINDOW)
    ap.add_argument("--verbose", action="store_true",
                    help="one line per table with a missing arm")
    args = ap.parse_args()

    read32, text_lo, text_hi = load_image(args.xbe)
    defined, sites = scan_gen(args.gen)
    if not sites:
        print("no RECOMP_ITAIL-through-a-table sites: either this gen tree is "
              "not JSRF's, or the lifter no longer emits that shape")
        return 1

    tables = {}
    for owner, table, _ in sites:
        tables.setdefault(table, set()).add(owner)

    loose_arms = loose_missing = tight_arms = tight_missing = 0
    rows = []
    for table, owners in sorted(tables.items()):
        # Every owner of a table starts at or before its own arms, so the
        # lowest owner start is the window anchor.
        starts = [int(o[4:], 16) for o in owners if o]
        anchor = min(starts) if starts else table
        arms, va = [], table
        while True:
            value = read32(va)
            if value is None or not (text_lo <= value < text_hi):
                break
            arms.append(value)
            va += 4
            if len(arms) >= 64:
                break
        tight = [a for a in arms if anchor <= a < anchor + args.window]
        miss_loose = [a for a in arms if "sub_%08X" % a not in defined]
        miss_tight = [a for a in tight if "sub_%08X" % a not in defined]
        loose_arms += len(arms)
        loose_missing += len(miss_loose)
        tight_arms += len(tight)
        tight_missing += len(miss_tight)
        if miss_tight:
            rows.append((table, sorted(owners), len(tight), len(miss_tight)))

    print("switch arms the lifter could not resolve")
    print("  dispatch sites (RECOMP_ITAIL through a scaled table): %d"
          % len(sites))
    print("  distinct tables: %d   distinct owning functions: %d"
          % (len(tables), len({o for o, _, _ in sites if o})))
    print("  arms, tight bound (within 0x%X of the owner): %d, of which %d "
          "have NO translated body" % (args.window, tight_arms, tight_missing))
    print("  arms, loose bound (table read to the first non-.text word): "
          "%d, of which %d have NO translated body" % (loose_arms, loose_missing))
    print("  tables with at least one unreachable arm: %d of %d"
          % (len(rows), len(tables)))
    if args.verbose:
        print()
        for table, owners, n, m in rows:
            print("  0x%06X  %-40s arms %2d  missing %2d"
                  % (table, ",".join(owners)[:40], n, m))
    # Reported, not enforced. The count is a property of a gen tree, and a
    # build is not wrong for having one -- it is wrong for nobody knowing.
    return 0


if __name__ == "__main__":
    sys.exit(main())
