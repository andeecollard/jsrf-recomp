#!/usr/bin/env python3
"""WHY could the lifter not resolve each switch table? One cause per table.

switch_arm_audit.py counts the damage: how many `jmp [reg*4 + table]`
dispatches fell through to RECOMP_ITAIL, and how many of their arms have no
translated body. This asks the next question, which is the one you need before
you can fix any of them: WHAT, specifically, stopped each one.

They do not all break the same way, and that matters, because the fixes are
different and one of them is dangerous. Measured on JSRF 19 Sep 2026, three
distinct diseases were already known by name and each needed its own repair:

  0x00114B66  the table that carved its owner had TWO entries, and
              engine.resync_jump_tables(min_entries=3) records nothing below
              three -- so the arm was never recognised as an arm, stayed a
              function start, and truncated sub_00114A80 at its own switch.
              Fixed in the detector (short_jump_tables).

  0x000A5B8C  the owner does not exist. The real __thiscall prologue at
              0x000A5B60 was never detected, so the dispatch at 0x000A5B85
              lives inside a tail-jump fragment that starts 0x30 bytes late
              and the arms are simply outside anything. Fixed by seeding the
              PROLOGUE -- a real function start, not an arm.

  0x001063CF  the owner is clamped by a neighbour that is not real.
              0x0010651F is the second of two consecutive `fstp st(0)`, an
              x87 cleanup pair with two entry points, reached only by a `jmp`
              from inside sub_001063A0 itself. _pass_tail_jump_targets minted
              it as a function only because a vtable-seeded ARM had already
              truncated the owner, making that internal jump look like a tail
              jump. The seed is dropped later; the function it caused is not.

So the classification below is not decoration. `short-table` means the
detector fix already covers it. `arms-below-owner` means a missing function
START and is safe to seed. `clamped-by-neighbour` means some OTHER pass minted
a start that is not a function, and the detection_method column names which
pass to go and look at. Only what is left over after those argues for changing
lifter.py's `_analyze_switch_table` rule, and that rule currently lifts 484
switches correctly, so it should be argued for with a number rather than
assumed.

    /usr/bin/python3 -m diagnostics.jsrf_first_fault.switch_arm_causes \\
        --xbe "$JSRF_GAME_DIR/default.xbe" \\
        --gen build-macos/jsrf-first-fault/gen \\
        --functions build-macos/jsrf-first-fault/disasm/functions.json

READ THE CAVEAT FROM switch_arm_audit.py, WHICH APPLIES HERE UNCHANGED. A jump
table is followed by ordinary code and the next function's bytes routinely read
as a valid .text address, so walking a table to the first non-.text word
over-reads it. Arms are taken within --window bytes of the owner's start, which
is true of every real switch arm and false of most over-read. A table left with
fewer than two plausible arms is reported as `not-a-table` rather than counted
as a defect, because that is what an over-read looks like from here and calling
it a bug would inflate every number below.
"""
import argparse
import collections
import glob
import json
import os
import re
import struct
import sys

WINDOW = 0x2000

DEF_RE = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)", re.M)
ITAIL_RE = re.compile(r"RECOMP_ITAIL\(MEM32\([a-z]+ \* 4 \+ 0x([0-9A-F]+)\)\)")

# The bound resync_jump_tables uses: a switch never jumps out of its own
# section, so the table stops at the first word that does. Mirrored rather than
# imported so this tool needs no disassembly pass to run.
RESYNC_MIN_ENTRIES = 3


def load_image(xbe_path):
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
    from tools.xbe_parser.xbe_parser import XBEParser

    xbe = XBEParser(xbe_path).parse()
    data = open(xbe_path, "rb").read()
    sections = [(s.virtual_addr, s.virtual_size, s.raw_addr, s.name)
                for s in xbe.sections]

    def read32(va):
        for va0, vsize, raw, _ in sections:
            if va0 <= va < va0 + vsize:
                off = raw + (va - va0)
                if off + 4 <= len(data):
                    return struct.unpack_from("<I", data, off)[0]
        return None

    def home(va):
        for va0, vsize, raw, name in sections:
            if va0 <= va < va0 + vsize:
                return va0, va0 + vsize
        return None

    text = next(s for s in sections if s[3] == ".text")
    return read32, home, text[0], text[0] + text[1]


def scan_gen(gen_dir):
    """Defined bodies, and every RECOMP_ITAIL through a scaled table."""
    defined = set()
    sites = []
    for path in sorted(glob.glob(os.path.join(gen_dir, "*.c"))):
        src = open(path, errors="ignore").read()
        defined.update(DEF_RE.findall(src))
        owners = [(m.start(), m.group(1)) for m in DEF_RE.finditer(src)]
        for m in ITAIL_RE.finditer(src):
            owner = None
            for start, name in owners:
                if start < m.start():
                    owner = name
                else:
                    break
            sites.append((owner, int(m.group(1), 16)))
    return defined, sites


def section_bounded_entries(read32, home, table, cap=512):
    """How many entries resync_jump_tables would count for this table."""
    hb = home(table)
    if hb is None:
        return []
    lo, hi = hb
    out = []
    while len(out) < cap:
        v = read32(table + len(out) * 4)
        if v is None or not (lo <= v < hi):
            break
        out.append(v)
    return out


def next_start_at_or_after(starts, addr):
    """The first recorded start at or past `addr`, or None.

    AT OR PAST, not "after the owner's start", and the difference is the whole
    classification. functions.json also holds ALIASES -- second entry points
    that deliberately overlap the body they sit inside -- so "the next start
    above the owner" is routinely an alias 200 bytes into it, which is not a
    boundary and never clamped anything. The question that matters is whether
    the recorded END lands exactly on a start, because that is what
    _find_function_end's `upper` is.
    """
    import bisect
    i = bisect.bisect_left(starts, addr)
    return starts[i] if i < len(starts) else None


def classify(owner_lo, owner_hi, arms, starts, by_start, table_entries,
             alias_starts):
    """One cause per table. First match wins; the order is the argument.

    Returns (cause, detail).
    """
    import bisect

    if not arms or len(arms) < 2:
        return "not-a-table", "fewer than 2 arms survive the window"

    lowest, highest = min(arms), max(arms)

    # Asked first because it is the only one where the OWNER is wrong rather
    # than its end: an arm below the recorded start means the body the gen
    # attributes this dispatch to begins after the real function does.
    if owner_lo is not None and lowest < owner_lo:
        return ("arms-below-owner",
                "lowest arm 0x%08X is below the owner's start 0x%08X"
                % (lowest, owner_lo))

    if owner_lo is None or owner_hi is None:
        return "no-owner-in-database", "the gen names a body functions.json does not"

    if owner_hi > highest:
        return ("owner-already-covers-arms",
                "end 0x%08X is past the highest arm 0x%08X -- the lifter "
                "refused for some other reason" % (owner_hi, highest))

    # Does the recorded end land exactly on a function start? That start IS
    # _find_function_end's `upper`, so it is the thing doing the clamping.
    nxt = next_start_at_or_after(starts, owner_hi)
    clamped = nxt is not None and nxt == owner_hi

    if clamped and nxt in arms:
        f = by_start[nxt]
        return ("arm-is-a-start",
                "0x%08X is an ARM of this very table and is a function "
                "(%s) -- the carve survived" % (nxt, f["detection_method"]))

    if len(table_entries) < RESYNC_MIN_ENTRIES:
        return ("short-table",
                "%d entries: resync_jump_tables(min_entries=%d) never recorded "
                "it, so its arms were never classified"
                % (len(table_entries), RESYNC_MIN_ENTRIES))

    if clamped:
        f = by_start[nxt]
        return ("clamped-by-neighbour",
                "0x%08X (%s) is exactly the owner's end, so it is the clamp"
                % (nxt, f["detection_method"]))

    # Nothing sits exactly on the end, so the walk stopped of its own accord.
    # Split on whether extending it would have to ABSORB something, because
    # that decides how risky the repair is: walking further through open bytes
    # is cheap, swallowing a recorded function is not.
    blockers = [a for a in starts if owner_hi <= a <= highest]
    if blockers:
        b = blockers[0]
        return ("blocked-further-along",
                "%d recorded start(s) between the end 0x%08X and the highest "
                "arm 0x%08X; first is 0x%08X (%s)"
                % (len(blockers), owner_hi, highest, b,
                   by_start[b]["detection_method"]))

    n_alias = len([a for a in alias_starts if owner_hi <= a <= highest])
    return ("walk-stopped-in-open-space",
            "end 0x%08X, highest arm 0x%08X, nothing REAL between them%s -- "
            "the end-finder gave up on its own"
            % (owner_hi, highest,
               " (%d alias entr%s, which are not boundaries)"
               % (n_alias, "y" if n_alias == 1 else "ies") if n_alias else ""))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--functions", required=True,
                    help="the disasm functions.json THAT PRODUCED THIS GEN. A "
                         "different one silently reclassifies everything.")
    ap.add_argument("--window", type=lambda v: int(v, 0), default=WINDOW)
    ap.add_argument("--verbose", action="store_true",
                    help="one line per table")
    ap.add_argument("--cause", help="list only tables with this cause")
    ap.add_argument("--oracle", help=(
        "a functions.json produced by round 1 of the discovery loop with an "
        "EMPTY vtable_seeds_accum.json. That sweep carves the bodies before "
        "any accumulated thunk seed has had a chance to truncate them, so it "
        "is a free regression oracle for what the extents SHOULD be: measured "
        "19 Sep 2026, it contains none of the four spurious starts "
        "(0x0010651D, 0x0010651F, 0x00114D34, 0x00114F7D) that the shipping "
        "tree has. Given one, this reports how many tables the oracle already "
        "resolves -- i.e. how much of the backlog needs no change to "
        "lifter.py's rule at all."))
    args = ap.parse_args()

    read32, home, text_lo, text_hi = load_image(args.xbe)
    defined, sites = scan_gen(args.gen)
    if not sites:
        print("no RECOMP_ITAIL-through-a-table sites in %s" % args.gen)
        return 1

    fns = json.load(open(args.functions))
    by_start = {int(f["start"], 16): f for f in fns}
    # ALIASES ARE NOT BOUNDARIES, and counting them as such is a mistake this
    # codebase has already paid for once: functions.py's _alias_end says so in
    # as many words -- "an alias is an alternate ENTRY into a body, not a
    # boundary of one" -- and notes that counting them "cost 198 of 200
    # dispatches their arms". Mirror the detector or this tool reports a cause
    # the detector does not have.
    alias_starts = sorted(a for a, f in by_start.items()
                          if f["detection_method"] == "tail_jump_alias")
    starts = sorted(a for a, f in by_start.items()
                    if f["detection_method"] != "tail_jump_alias")

    oracle = None
    if args.oracle:
        ofns = json.load(open(args.oracle))
        obodies = sorted((int(f["start"], 16), int(f["end"], 16))
                         for f in ofns)

        def oracle_body(addr):
            import bisect
            i = bisect.bisect_right([b[0] for b in obodies], addr) - 1
            if i >= 0 and obodies[i][0] <= addr < obodies[i][1]:
                return obodies[i]
            return None

        oracle = oracle_body

    tables = collections.OrderedDict()
    for owner, table in sites:
        tables.setdefault(table, set()).add(owner)

    buckets = collections.Counter()
    blame = collections.Counter()
    owners_by_method = collections.Counter()
    oracle_verdict = collections.Counter()
    rows = []
    for table, owners in sorted(tables.items()):
        owner_starts = [int(o[4:], 16) for o in owners if o]
        owner_lo = min(owner_starts) if owner_starts else None
        owner_hi = (int(by_start[owner_lo]["end"], 16)
                    if owner_lo in by_start else None)

        entries = section_bounded_entries(read32, home, table)
        anchor = owner_lo if owner_lo is not None else table
        arms = [a for a in entries
                if text_lo <= a < text_hi and anchor <= a < anchor + args.window]
        missing = [a for a in arms if "sub_%08X" % a not in defined]

        cause, detail = classify(owner_lo, owner_hi, arms, starts, by_start,
                                 entries, alias_starts)
        buckets[cause] += 1
        if cause in ("arm-is-a-start", "clamped-by-neighbour",
                     "blocked-further-along"):
            nxt = next_start_at_or_after(starts, owner_hi)
            if nxt in by_start:
                blame[by_start[nxt]["detection_method"]] += 1
        owner_method = (by_start[owner_lo]["detection_method"]
                        if owner_lo in by_start else "?")
        owners_by_method[owner_method] += 1
        rows.append((table, owner_lo, owner_hi, len(arms), len(missing),
                     cause, detail))
        if oracle is not None and owner_lo is not None:
            body = oracle(owner_lo)
            if body is None:
                oracle_verdict["no body in the oracle either"] += 1
            else:
                # The lifter's own rule, applied to the oracle's extent:
                # truncate the arms at the first one outside the body and
                # require two.
                inside = []
                for a in entries:
                    if not (body[0] <= a < body[1]):
                        break
                    inside.append(a)
                oracle_verdict["oracle RESOLVES it (>=2 arms inside)"
                               if len(inside) >= 2
                               else "oracle does not resolve it either"] += 1

    print("why each unresolved switch table is unresolved")
    print("  gen       : %s" % args.gen)
    print("  functions : %s" % args.functions)
    print("  tables    : %d   dispatch sites: %d   arms with no body: %d"
          % (len(tables), len(sites), sum(r[4] for r in rows)))
    print()
    print("  cause                        tables")
    for cause, n in buckets.most_common():
        print("    %-26s %5d" % (cause, n))
    if blame:
        print()
        print("  where something is in the way, WHICH PASS minted it:")
        for method, n in blame.most_common():
            print("    %-26s %5d" % (method, n))
    print()
    print("  and how the OWNER itself was detected (a tail_jump_alias owner is")
    print("  a fragment of a function, not a function):")
    for method, n in owners_by_method.most_common():
        print("    %-26s %5d" % (method, n))

    if oracle_verdict:
        print()
        print("  against the round-1 oracle (empty accumulator), applying the")
        print("  lifter's own >=2-arms-inside rule to the oracle's extents:")
        for k, n in oracle_verdict.most_common():
            print("    %-42s %5d" % (k, n))

    if args.verbose or args.cause:
        print()
        for table, lo, hi, n, miss, cause, detail in rows:
            if args.cause and cause != args.cause:
                continue
            print("  0x%08X owner %s..%s arms %2d missing %2d  %s"
                  % (table,
                     "0x%08X" % lo if lo is not None else "????????",
                     "0x%08X" % hi if hi is not None else "????????",
                     n, miss, cause))
            print("      %s" % detail)

    # Reported, never enforced. The count is a property of a gen tree and it
    # changes every regeneration; a build is not wrong for having one, it is
    # wrong for nobody knowing the shape of it.
    return 0


if __name__ == "__main__":
    sys.exit(main())
