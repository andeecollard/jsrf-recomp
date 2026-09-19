#!/usr/bin/env python3
"""Every control-flow edge in the generated tree, classified, and gated.

WHY THIS EXISTS. On 19 Sep 2026 two defects chased separately for weeks -- the
ADX freeze and the skating crash -- turned out to be one switch arm the lifter
could not place, falling through to RECOMP_ITAIL, whose failure path popped
four bytes off a frame that still held its locals. The instruments that found
it (switch_arm_audit.py, switch_arm_causes.py) answer one question well: which
switch dispatches are unresolved and why. This asks the wider one the goals
file kept circling: does the generated tree ACCOUNT for every transfer of
control, and can the numbers go backwards without anyone noticing?

Every edge shape the lifter emits is named below, with what it means. A shape
this file does not know is reported as UNCLASSIFIED and fails the gate: a new
lifter output must be classified deliberately, not counted as fine by default.

  translated, statically:
    direct_call         RECOMP_ABI_CALL(va, sub_X)    call to a translated body
    direct_tail         g_seh_ebp = ebp; sub_X(); return;   jmp to another function
    goto                goto loc_X                     every intra-function branch
    switch_resolved     { uint32_t _jt = ...           a jump table lifted to gotos
    switch_arms_goto    if (_jt == 0x..) goto          its arms

  resolved at RUNTIME through the dispatch table (correct when the target is a
  known function start; otherwise the failure path runs):
    icall               RECOMP_ICALL*(_icall_target    call through a pointer
    itail_vtable        RECOMP_ITAIL(MEM32(reg [+ disp])) tail jump through a slot
    itail_abs           RECOMP_ITAIL(MEM32(abs))       tail jump through a global
    itail_reg           RECOMP_ITAIL(reg)              tail jump through a register
    switch_default      RECOMP_ITAIL(_jt)              a resolved switch's
                                                       fall-through for a value
                                                       outside its table

  intentional replacements:
    icall_static        RECOMP_ICALL*(0x..u            a call rewritten to a fixed VA
    itail_manual        RECOMP_ITAIL(0x..u)            tail jump to a manual function
    ignored_priv        RECOMP-IGNORED-PRIV            a privileged/port instruction,
                                                       ignored on purpose, says so

  UNRESOLVED -- the lifter could not place the destination:
    switch_unresolved   RECOMP_ITAIL(MEM32(reg * 4 + table))
                        a jump table whose arms lie outside the function the
                        lifter was given. This is the G20 defect's shape. With
                        the XBE available the arms are walked and the ones with
                        no translated body counted (switch_arm_audit's rule).
    unresolved_stubs    bodies in recomp_stubs_unresolved.c: call targets the
                        detector never defined, so the call reaches a stub
    todo                /* TODO: mnemonic */           an instruction with no
                        translation. `todo_silent` is the subset with no
                        RECOMP_UNIMPL marker beside it, i.e. a no-op nothing
                        can report at runtime -- every one of them before
                        19 Sep 2026, none after the next regeneration.
    cond_indirect       if (cond) { /* jcc: - indirect */ }   a conditional
                        branch with no target: an EMPTY if. Silent.
    jmp_no_target       /* jmp: no target */           silent.

  plus the detector's side of the same question:
    functions           bodies in functions.json
    coverage_bytes      unique bytes of the image inside some function extent.
                        1,613,483 on the 19 Sep 12:45 gen. The goals file
                        quotes 1,614,033 for the pre-merge trees; the method
                        there was not recorded, so this one is: the union of
                        [start, end) over functions.json.

THE GATE. A baseline JSON holds the last accepted numbers. Counts of anything
unresolved or silent must not RISE; coverage and resolved switches must not
FALL. A regeneration that moves a number the wrong way fails the suite and
says which number. Accept a new state deliberately with --write-baseline, and
say why in the commit. The baseline records which gen it was taken on; a
mismatch is printed, not failed, because the rule is that the numbers hold
ACROSS regenerations -- that is the whole point.

Two things this does not do, stated so a green run is not over-read. It does
not say whether an edge is ever TAKEN: that is the runtime's job ([ITAIL],
[ICALL], [UNIMPL] lines). And it grades the lifter's output against itself,
not against the guest: it cannot see a branch translated to the wrong place,
only one translated to nowhere.

    python3 diagnostics/jsrf_first_fault/control_flow_gate.py \\
        --gen build-macos/jsrf-first-fault/gen \\
        --functions build-macos/jsrf-first-fault/disasm/functions.json \\
        --baseline diagnostics/jsrf_first_fault/control_flow_baseline.json

The switch-arm walk needs the XBE and takes it from --xbe or
$JSRF_GAME_DIR/default.xbe; without either that part is SKIPPED, printed as
such, and its baseline numbers are neither checked nor rewritten.
"""
import argparse
import collections
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)

# Same rule as switch_arm_audit: an arm belongs to its own function, and 8 KB
# is well above JSRF's largest translated function.
WINDOW = 0x2000

DEF_RE = re.compile(r"^void (sub_[0-9A-F]{8})\(void\)", re.M)

# Every RECOMP_ITAIL / RECOMP_ICALL occurrence, so the classified sum can be
# checked against the total.
ITAIL_ANY = re.compile(r"RECOMP_ITAIL\(")
ICALL_ANY = re.compile(r"RECOMP_ICALL(?:_SAFE)?\(")

REG = r"e(?:[a-d]x|[sd]i|[bs]p)"
SHAPES = [
    ("direct_call", re.compile(r"RECOMP_ABI_CALL\(0x[0-9A-F]+u, sub_[0-9A-F]{8}\)")),
    ("direct_tail", re.compile(r"g_seh_ebp = ebp; sub_[0-9A-F]{8}\(\); return;")),
    ("goto", re.compile(r"\bgoto loc_[0-9A-F]{8};")),
    ("switch_resolved", re.compile(r"\{ uint32_t _jt = ")),
    ("switch_arms_goto", re.compile(r"if \(_jt == 0x[0-9A-F]{8}u\) goto")),
    ("icall", re.compile(r"RECOMP_ICALL(?:_SAFE)?\(_icall_target")),
    ("icall_static", re.compile(r"RECOMP_ICALL(?:_SAFE)?\(0x[0-9A-F]+u")),
    ("switch_unresolved", re.compile(r"RECOMP_ITAIL\(MEM32\(" + REG + r" \* 4 \+ 0x[0-9A-F]+\)\)")),
    ("switch_default", re.compile(r"RECOMP_ITAIL\(_jt\)")),
    ("itail_vtable", re.compile(r"RECOMP_ITAIL\(MEM32\(" + REG + r"(?: \+ (?:0x[0-9A-F]+|\d+))?\)\)")),
    ("itail_abs", re.compile(r"RECOMP_ITAIL\(MEM32\(0x[0-9A-F]+\)\)")),
    ("itail_reg", re.compile(r"RECOMP_ITAIL\(" + REG + r"\)")),
    ("itail_manual", re.compile(r"RECOMP_ITAIL\(0x[0-9A-F]+u\)")),
    ("ignored_priv", re.compile(r"RECOMP-IGNORED-PRIV")),
    ("todo", re.compile(r"/\* TODO: [a-z0-9]+")),
    ("cond_indirect", re.compile(r" - indirect \*/ \}")),
    ("jmp_no_target", re.compile(r"/\* jmp: no target \*/")),
]
TODO_RE = re.compile(r"^(.*)/\* TODO: ([a-z0-9]+)", re.M)
UNRESOLVED_TABLE_RE = re.compile(
    r"RECOMP_ITAIL\(MEM32\(" + REG + r" \* 4 \+ 0x([0-9A-F]+)\)\)")

# The classes whose count must not go UP, and the ones that must not go DOWN.
MUST_NOT_RISE = ["switch_unresolved", "switch_unresolved_tables",
                 "switch_arms_no_body", "unresolved_stubs", "todo",
                 "todo_silent", "cond_indirect", "jmp_no_target",
                 "unclassified_itail", "unclassified_icall"]
MUST_NOT_FALL = ["coverage_bytes", "switch_resolved"]


def read_manifest(gen_dir):
    out = {}
    path = os.path.join(gen_dir, "GENERATION_MANIFEST.txt")
    if os.path.exists(path):
        for line in open(path):
            if "=" in line and not line.startswith("#"):
                k, v = line.rstrip("\n").split("=", 1)
                out[k] = v
    return out


def scan_gen(gen_dir):
    """Counts per shape over the translated units, the stub file separately."""
    counts = collections.Counter()
    todo_by_mnemonic = collections.Counter()
    todo_silent = 0
    owners_of_table = collections.defaultdict(set)
    defined = set()
    itail_total = icall_total = 0
    # The stub file is scanned with the rest: its "Recovered entry" bodies
    # carry real dispatches (one of JSRF's 152 unresolved switches lives
    # there), and switch_arm_audit counts them, so the two must agree.
    units = sorted(glob.glob(os.path.join(gen_dir, "*.c")))
    if not units:
        sys.exit("no generated units in %s" % gen_dir)
    for path in units:
        src = open(path, errors="ignore").read()
        defined.update(DEF_RE.findall(src))
        for name, rx in SHAPES:
            counts[name] += len(rx.findall(src))
        itail_total += len(ITAIL_ANY.findall(src))
        icall_total += len(ICALL_ANY.findall(src))
        for m in TODO_RE.finditer(src):
            todo_by_mnemonic[m.group(2)] += 1
            if "RECOMP_UNIMPL(" not in m.group(1):
                todo_silent += 1
        # Which function owns each unresolved dispatch: the nearest body
        # start above it in the file.
        starts = [(m.start(), m.group(1)) for m in DEF_RE.finditer(src)]
        for m in UNRESOLVED_TABLE_RE.finditer(src):
            owner = None
            for at, name in starts:
                if at < m.start():
                    owner = name
                else:
                    break
            owners_of_table[int(m.group(1), 16)].add(owner)
    stub_path = os.path.join(gen_dir, "recomp_stubs_unresolved.c")
    stub_names = set()
    if os.path.exists(stub_path):
        stub_names = set(DEF_RE.findall(open(stub_path, errors="ignore").read()))
    stubs = len(stub_names)
    scan_gen.stub_names = stub_names
    itail_classified = sum(counts[k] for k in
                           ("switch_unresolved", "switch_default", "itail_vtable",
                            "itail_abs", "itail_reg", "itail_manual"))
    icall_classified = counts["icall"] + counts["icall_static"]
    measures = dict(counts)
    measures["todo_silent"] = todo_silent
    measures["unresolved_stubs"] = stubs
    measures["switch_unresolved_tables"] = len(owners_of_table)
    measures["unclassified_itail"] = itail_total - itail_classified
    measures["unclassified_icall"] = icall_total - icall_classified
    measures["itail_total"] = itail_total
    measures["icall_total"] = icall_total
    return measures, dict(todo_by_mnemonic), owners_of_table, defined


def walk_arms(xbe_path, owners_of_table, defined, window):
    """switch_arm_audit's tight-bound count: arms within WINDOW of the owner,
    and how many of those have no translated body."""
    from tools.xbe_parser.xbe_parser import XBEParser
    import struct
    xbe = XBEParser(xbe_path).parse()
    data = open(xbe_path, "rb").read()
    sections = [(s.virtual_addr, s.virtual_size, s.raw_addr, s.name)
                for s in xbe.sections]
    text = next(s for s in sections if s[3] == ".text")
    text_lo, text_hi = text[0], text[0] + text[1]

    def read32(va):
        for va0, vsize, raw, _ in sections:
            if va0 <= va < va0 + vsize:
                off = raw + (va - va0)
                if off + 4 <= len(data):
                    return struct.unpack_from("<I", data, off)[0]
        return None

    arms_total = arms_missing = tables_hit = 0
    rows = []
    arms_of = {}
    for table, owners in sorted(owners_of_table.items()):
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
        tight = [a for a in arms if anchor <= a < anchor + window]
        missing = [a for a in tight if ("sub_%08X" % a) not in defined]
        arms_total += len(tight)
        arms_missing += len(missing)
        if missing:
            tables_hit += 1
        rows.append((table, sorted(o or "?" for o in owners), len(tight), len(missing)))
        arms_of[table] = tight
    return {"switch_arms_tight": arms_total,
            "switch_arms_no_body": arms_missing,
            "switch_tables_with_missing_arm": tables_hit}, rows, arms_of


def coverage(functions_path):
    fns = json.load(open(functions_path))
    iv = sorted((int(f["start"], 16), int(f["end"], 16)) for f in fns)
    total = 0
    cur_s, cur_e = iv[0]
    for s, e in iv[1:]:
        if s > cur_e:
            total += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    total += cur_e - cur_s
    return {"functions": len(fns), "coverage_bytes": total}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--xbe", default=None,
                    help="default: $JSRF_GAME_DIR/default.xbe if set")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--write-baseline", action="store_true",
                    help="accept the current numbers as the new baseline")
    ap.add_argument("--window", type=lambda v: int(v, 0), default=WINDOW)
    ap.add_argument("--verbose", action="store_true",
                    help="TODO sites by mnemonic and one line per unresolved table")
    args = ap.parse_args()

    xbe = args.xbe
    if not xbe and os.environ.get("JSRF_GAME_DIR"):
        cand = os.path.join(os.environ["JSRF_GAME_DIR"], "default.xbe")
        if os.path.exists(cand):
            xbe = cand

    manifest = read_manifest(args.gen)
    measures, todo_by_mnemonic, owners_of_table, defined = scan_gen(args.gen)
    measures.update(coverage(args.functions))
    arm_rows = None
    if xbe:
        arm_measures, arm_rows, _ = walk_arms(xbe, owners_of_table, defined, args.window)
        measures.update(arm_measures)

    print("control-flow edges in %s" % args.gen)
    print("  gen: translator %s, head %s, generated %s" % (
        manifest.get("translator_sha", "?"), manifest.get("git_head", "?"),
        manifest.get("generated_utc", "?")))
    print()
    print("  translated statically")
    for k in ("direct_call", "direct_tail", "goto", "switch_resolved", "switch_arms_goto"):
        print("    %-28s %7d" % (k, measures[k]))
    print("  resolved at runtime through the dispatch table")
    for k in ("icall", "itail_vtable", "itail_abs", "itail_reg", "switch_default"):
        print("    %-28s %7d" % (k, measures[k]))
    print("  intentional replacements")
    for k in ("icall_static", "itail_manual", "ignored_priv"):
        print("    %-28s %7d" % (k, measures[k]))
    print("  UNRESOLVED or SILENT")
    for k in ("switch_unresolved", "switch_unresolved_tables", "switch_arms_tight",
              "switch_arms_no_body", "switch_tables_with_missing_arm",
              "unresolved_stubs", "todo", "todo_silent", "cond_indirect",
              "jmp_no_target", "unclassified_itail", "unclassified_icall"):
        if k in measures:
            print("    %-28s %7d" % (k, measures[k]))
        else:
            print("    %-28s   SKIPPED (no XBE: pass --xbe or set JSRF_GAME_DIR)" % k)
    print("  detector")
    for k in ("functions", "coverage_bytes"):
        print("    %-28s %7d" % (k, measures[k]))
    if args.verbose:
        print()
        print("  TODO sites by mnemonic:")
        for m, n in sorted(todo_by_mnemonic.items(), key=lambda kv: -kv[1]):
            print("    %-12s %4d" % (m, n))
        if arm_rows:
            print()
            print("  unresolved tables (table, owners, tight arms, arms with no body):")
            for table, owners, n, missing in arm_rows:
                print("    0x%08X  %-40s %2d %2d" % (table, ",".join(owners)[:40], n, missing))

    if measures["unclassified_itail"] or measures["unclassified_icall"]:
        print()
        print("  UNCLASSIFIED transfer shapes are present. Name them in SHAPES"
              " before accepting this tree.")

    problems = []
    if args.write_baseline:
        old = {}
        if os.path.exists(args.baseline):
            old = json.load(open(args.baseline)).get("measures", {})
        # Keep the XBE-dependent numbers from the old baseline when this run
        # could not measure them, rather than silently dropping the gate.
        merged = dict(old)
        merged.update(measures)
        json.dump({"gen": {k: manifest.get(k) for k in
                           ("translator_sha", "runtime_types_sha", "xbe_sha",
                            "git_head", "generated_utc")},
                   "method": {"coverage_bytes": "union of [start,end) over functions.json",
                              "switch_arms": "switch_arm_audit tight bound, window 0x%X" % args.window},
                   "measures": merged},
                  open(args.baseline, "w"), indent=1, sort_keys=True)
        print()
        print("  baseline written to %s" % args.baseline)
        return 0

    if not os.path.exists(args.baseline):
        print()
        print("  no baseline at %s: run once with --write-baseline" % args.baseline)
        return 2
    base = json.load(open(args.baseline))
    bm = base.get("measures", {})
    if base.get("gen", {}).get("translator_sha") != manifest.get("translator_sha"):
        print()
        print("  note: baseline was taken on translator %s; this gen is %s."
              " The numbers must still hold." % (
                  base.get("gen", {}).get("translator_sha"),
                  manifest.get("translator_sha")))
    for k in MUST_NOT_RISE:
        if k in measures and k in bm and measures[k] > bm[k]:
            problems.append("%s rose %d -> %d" % (k, bm[k], measures[k]))
    for k in MUST_NOT_FALL:
        if k in measures and k in bm and measures[k] < bm[k]:
            problems.append("%s fell %d -> %d" % (k, bm[k], measures[k]))
    if measures["unclassified_itail"] or measures["unclassified_icall"]:
        problems.append("unclassified transfer shapes: %d itail, %d icall" % (
            measures["unclassified_itail"], measures["unclassified_icall"]))
    print()
    if problems:
        print("  GATE FAILED against %s:" % args.baseline)
        for p in problems:
            print("    " + p)
        print("  If this is a deliberate acceptance, re-run with --write-baseline"
              " and say why in the commit.")
        return 1
    print("  gate holds against %s" % args.baseline)
    return 0


if __name__ == "__main__":
    sys.exit(main())
