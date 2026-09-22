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

THE GATE, IN TWO HALVES.

TOTALS. A baseline JSON holds the last accepted numbers. Counts of anything
unresolved or silent must not RISE; coverage must not FALL. Accept a new state
deliberately with --write-baseline, and say why in the commit. The baseline
records which gen it was taken on; a mismatch is printed, not failed, because
the rule is that the numbers hold ACROSS regenerations -- that is the whole
point.

ADDRESSES, WHICH ARE THE HALF THAT CAN ACTUALLY SEE A REGRESSION. Totals move
for two unrelated reasons and cannot tell them apart. On 21 Sep 2026 a
regeneration picked up 135 new icall observations, which re-carved functions
and merged spurious splits; switch_resolved fell 487 -> 481 and the gate failed
on it, while every unresolved measure IMPROVED. The fall was duplicate
dispatches disappearing. A ratio would have passed that -- and would equally
have passed six sites genuinely going unresolved while six duplicates vanished.

So the identity of a site is its GUEST ADDRESS, which survives re-carving
because it is a fact about the binary and not about how the lifter chopped it
up:

  switch sites   the jump TABLE's address, recorded with how many dispatches
                 reached it resolved and how many unresolved. FAILS when a
                 table's unresolved count RISES -- which is the "previously
                 resolved site is now unresolved" case, and also the case
                 where an already-unresolved table acquires another one.
  todo sites     the address in RECOMP_UNIMPL(..., 0xVA), with its mnemonic.
                 FAILS on a new ADDRESS. "todo 122 -> 123" went into a handover
                 as "the one genuine negative and is unexplained" because the
                 number was all there was; the address and the instruction are
                 both right there in the generated line.
  stub sites     the sub_XXXXXXXX names in recomp_stubs_unresolved.c. FAILS on
                 a new one: that is a call target the detector never defined.

Tables and sites that DISAPPEAR, and counts that fall, are reported and never
failed -- that is what re-carving does, and it is not a regression. A gen with
no site record in its baseline skips this half and says so; write one with
--write-baseline.

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
ICALL_ANY = re.compile(r"RECOMP_ICALL(?:_SAFE)?(?:_AT)?\(")

REG = r"e(?:[a-d]x|[sd]i|[bs]p)"
SHAPES = [
    ("direct_call", re.compile(r"RECOMP_ABI_CALL\(0x[0-9A-F]+u, sub_[0-9A-F]{8}\)")),
    ("direct_tail", re.compile(r"g_seh_ebp = ebp; sub_[0-9A-F]{8}\(\); return;")),
    ("goto", re.compile(r"\bgoto loc_[0-9A-F]{8};")),
    ("switch_resolved", re.compile(r"\{ uint32_t _jt = ")),
    ("switch_arms_goto", re.compile(r"if \(_jt == 0x[0-9A-F]{8}u\) goto")),
    ("icall", re.compile(r"RECOMP_ICALL(?:_SAFE)?(?:_AT)?\(_icall_target")),
    ("icall_guarded", re.compile(r"/\* indirect call, guarded \d+ \*/")),
    ("icall_guard_arm", re.compile(r"RECOMP_ABI_CALL_G\(")),
    ("icall_static", re.compile(r"RECOMP_ICALL(?:_SAFE)?(?:_AT)?\(0x[0-9A-F]+u")),
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
# The SAME table address, on the side of the fence where the lifter placed the
# arms. Checked against the generated tree when this was written: all 481
# resolved dispatches carry it, so the two regexes between them see every
# switch site the tree has.
RESOLVED_TABLE_RE = re.compile(
    r"\{ uint32_t _jt = MEM32\(" + REG + r" \* 4 \+ 0x([0-9A-F]+)\)")
# The untranslated instruction's own address and text, which the generated line
# already carries -- `RECOMP_UNIMPL("cli", 0x0001FCD8u); /* TODO: cli */`.
TODO_SITE_RE = re.compile(
    r'RECOMP_UNIMPL\("([^"]*)",\s*0x([0-9A-F]+)u\);\s*/\* TODO:')

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


def scan_gen(gen_dir, sources=None):
    """Counts per shape over the translated units, the stub file separately.

    `sources` maps a unit's path to the text to read INSTEAD of the file. It
    exists for one caller: recover_midfunction_entries.py, which patches
    recomp_stubs_unresolved.c in memory and then has to ask this what the tree
    it is about to write looks like. Scanning the copy still on disk would be
    wrong in exactly the interesting direction -- the recovered stub bodies are
    where several of the unresolved dispatches live (sub_00075E90's table at
    0x00076610 among them), so a pre-patch scan cannot see the arms they open
    and the recovery would silently do half the job. Reading through one
    accessor also means the override cannot be applied to some files and
    forgotten on others.
    """
    sources = {os.path.abspath(k): v for k, v in (sources or {}).items()}

    def read(path):
        override = sources.get(os.path.abspath(path))
        return open(path, errors="ignore").read() if override is None else override

    counts = collections.Counter()
    todo_by_mnemonic = collections.Counter()
    todo_silent = 0
    owners_of_table = collections.defaultdict(set)
    # Sites, keyed by GUEST address. A table can be dispatched from more than
    # one lifted body -- 481 dispatches over 428 distinct tables in the 21 Sep
    # gen -- and the same table can be resolved in one copy and unresolved in
    # another, so each side is counted rather than flagged.
    switch_sites = collections.defaultdict(lambda: {"r": 0, "u": 0})
    todo_sites = collections.Counter()
    todo_text = {}
    defined = set()
    itail_total = icall_total = 0
    # The stub file is scanned with the rest: its "Recovered entry" bodies
    # carry real dispatches (one of JSRF's 152 unresolved switches lives
    # there), and switch_arm_audit counts them, so the two must agree.
    units = sorted(glob.glob(os.path.join(gen_dir, "*.c")))
    if not units:
        sys.exit("no generated units in %s" % gen_dir)
    for path in units:
        src = read(path)
        defined.update(DEF_RE.findall(src))
        for name, rx in SHAPES:
            counts[name] += len(rx.findall(src))
        itail_total += len(ITAIL_ANY.findall(src))
        icall_total += len(ICALL_ANY.findall(src))
        for m in TODO_RE.finditer(src):
            todo_by_mnemonic[m.group(2)] += 1
            if "RECOMP_UNIMPL(" not in m.group(1):
                todo_silent += 1
        for m in RESOLVED_TABLE_RE.finditer(src):
            switch_sites[int(m.group(1), 16)]["r"] += 1
        for m in UNRESOLVED_TABLE_RE.finditer(src):
            switch_sites[int(m.group(1), 16)]["u"] += 1
        for m in TODO_SITE_RE.finditer(src):
            va = int(m.group(2), 16)
            todo_sites[va] += 1
            todo_text[va] = m.group(1)
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
        stub_names = set(DEF_RE.findall(read(stub_path)))
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
    sites = {
        "switch": {"0x%08X" % va: dict(v)
                   for va, v in sorted(switch_sites.items())},
        "todo": {"0x%08X" % va: {"n": todo_sites[va], "text": todo_text[va]}
                 for va in sorted(todo_sites)},
        "stubs": sorted(stub_names),
    }
    return measures, dict(todo_by_mnemonic), owners_of_table, defined, sites


def load_image(xbe_path):
    """(read32, text_lo, text_hi) for the guest image. switch_arm_audit's."""
    from tools.xbe_parser.xbe_parser import XBEParser
    import struct
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


def walk_arms(xbe_path, owners_of_table, defined, window, image=None):
    """switch_arm_audit's tight-bound count: arms within WINDOW of the owner,
    and how many of those have no translated body.

    `image` is (read32, text_lo, text_hi) already loaded, for a caller that has
    one -- or for a test, which needs to exercise the WALK without shipping a
    2 MB XBE into the repository. The walk is the part with the rules in it
    (the tight bound, the stop at the first non-.text word, the 64-arm cap);
    parsing the XBE is not, so only the walk is worth testing and only the
    parse is worth skipping. With `image` given, `xbe_path` is unused.
    """
    read32, text_lo, text_hi = image if image else load_image(xbe_path)

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


def compare_sites(now, base):
    """(problems, notes) from the per-address records.

    A regression is a site that got WORSE at an address that existed before.
    A site that vanished, or one that is new, is re-carving: reported, never
    failed, because the lifter chopping the binary differently is not the same
    event as an arm it can no longer place."""
    problems, notes = [], []

    bsw, nsw = base.get("switch", {}), now.get("switch", {})
    worse, gone, fresh, better = [], [], [], []
    for va, cur in sorted(nsw.items()):
        old_site = bsw.get(va)
        if old_site is None:
            fresh.append((va, cur))
        elif cur["u"] > old_site["u"]:
            worse.append((va, old_site, cur))
        elif cur["u"] < old_site["u"]:
            better.append((va, old_site, cur))
    for va in sorted(bsw):
        if va not in nsw:
            gone.append((va, bsw[va]))

    for va, o, c in worse:
        problems.append(
            "switch table %s: unresolved dispatches %d -> %d"
            " (resolved %d -> %d) -- a site the lifter could place before"
            % (va, o["u"], c["u"], o["r"], c["r"]))
    if gone:
        notes.append("%d switch tables are no longer dispatched anywhere"
                     " (re-carving): %s" % (len(gone),
                     ", ".join(v for v, _ in gone[:8])
                     + (" ..." if len(gone) > 8 else "")))
    if fresh:
        nu = sum(1 for _, c in fresh if c["u"])
        notes.append("%d switch tables are new to this gen, %d of them with an"
                     " unresolved dispatch: %s" % (len(fresh), nu,
                     ", ".join(v for v, _ in fresh[:8])
                     + (" ..." if len(fresh) > 8 else "")))
    if better:
        notes.append("%d switch tables lost unresolved dispatches" % len(better))

    btd, ntd = base.get("todo", {}), now.get("todo", {})
    new_todo = [va for va in sorted(ntd) if va not in btd]
    gone_todo = [va for va in sorted(btd) if va not in ntd]
    for va in new_todo:
        problems.append("untranslated instruction is NEW at %s: `%s`"
                        " (in %d lifted bodies)"
                        % (va, ntd[va]["text"], ntd[va]["n"]))
    if gone_todo:
        notes.append("%d untranslated sites are gone: %s"
                     % (len(gone_todo), ", ".join(gone_todo[:8])
                        + (" ..." if len(gone_todo) > 8 else "")))
    dup = [va for va in sorted(ntd)
           if va in btd and ntd[va]["n"] != btd[va]["n"]]
    if dup:
        notes.append("%d untranslated sites changed how many lifted bodies"
                     " carry them (duplicate bodies, not new instructions)"
                     % len(dup))

    bst, nst = set(base.get("stubs", [])), set(now.get("stubs", []))
    for name in sorted(nst - bst):
        problems.append("new unresolved stub %s: a call target the detector"
                        " never defined" % name)
    if bst - nst:
        notes.append("%d unresolved stubs are gone" % len(bst - nst))
    return problems, notes


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
    measures, todo_by_mnemonic, owners_of_table, defined, sites = scan_gen(args.gen)
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
    for k in ("direct_call", "direct_tail", "goto", "switch_resolved", "switch_arms_goto",
              "icall_guarded", "icall_guard_arm"):
        print("    %-28s %7d" % (k, measures.get(k, 0)))
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

    # WHICH HALF OF THE UNRESOLVED DISPATCHES ALREADY HAS ITS ANSWER.
    #
    # A jump table's contents do not depend on which site reads it, so a table
    # this tree resolves at one dispatch and not at another is one where the
    # arm list is already known. Splitting the total says how much of it is
    # missing information and how much is missing plumbing -- two different
    # pieces of work, and the totals cannot tell them apart.
    #
    # MEASURED 21 Sep 2026: 72 of 142 unresolved dispatches, in 32 tables.
    # That is NOT 72 easy wins. Checked on 0x0007C9C0
    # (CActSequence::ReturnFromFullRoboyMenu, 5 arms): four functions dispatch
    # through it, the one that resolves it is the one whose body contains the
    # arms, and for the other three the arms are labels inside somebody else's
    # function -- four of the five have no body of their own to call. So the
    # blocker is switch_arms_no_body, not the table lookup, and the fix is an
    # ENTRY POINT for each arm. recover_midfunction_entries.py already builds
    # exactly that, but only for stubs -- addresses something statically calls
    # -- and an arm reached only through an unresolved table never becomes a
    # stub, so the recovery pass never sees it.
    _sw = sites["switch"]
    _u_both = sum(v["u"] for v in _sw.values() if v["u"] and v["r"])
    _n_both = sum(1 for v in _sw.values() if v["u"] and v["r"])
    _u_only = sum(v["u"] for v in _sw.values() if v["u"] and not v["r"])
    _n_only = sum(1 for v in _sw.values() if v["u"] and not v["r"])
    if _u_both or _u_only:
        print("  unresolved dispatches, split by whether the table is"
              " resolved somewhere else")
        print("    %-28s %7d   in %d tables -- arm list known, arms need an"
              " entry point" % ("table resolved elsewhere", _u_both, _n_both))
        print("    %-28s %7d   in %d tables -- arm list not known here"
              % ("resolved nowhere", _u_only, _n_only))
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
    explained = []
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
                              "switch_arms": "switch_arm_audit tight bound, window 0x%X" % args.window,
                              "sites": "guest addresses: switch tables by"
                                       " resolved/unresolved dispatch count,"
                                       " untranslated instructions by"
                                       " RECOMP_UNIMPL address, stubs by name"},
                   "measures": merged,
                   "sites": sites},
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
    # THE ADDRESS GATE FIRST, because its answer decides how to read the
    # totals below.
    base_sites = base.get("sites")
    site_problems, site_notes = [], []
    if base_sites:
        site_problems, site_notes = compare_sites(sites, base_sites)
        problems.extend(site_problems)
    else:
        print()
        print("  no site record in %s: the per-address gate is SKIPPED."
              " Write one with --write-baseline -- until then a regression"
              " that is masked by a falling total cannot be seen here."
              % args.baseline)

    for k in MUST_NOT_RISE:
        if k in measures and k in bm and measures[k] > bm[k]:
            problems.append("%s rose %d -> %d" % (k, bm[k], measures[k]))
    for k in MUST_NOT_FALL:
        if k in measures and k in bm and measures[k] < bm[k]:
            # A FALL THAT THE ADDRESSES ACCOUNT FOR IS NOT A REGRESSION. This
            # is the 487 -> 481 case: duplicate dispatches disappearing when
            # re-carving merged spurious function splits. The totals cannot
            # tell that from six sites going unresolved; the addresses can, and
            # they have just been checked. Only switch_resolved is excused this
            # way -- coverage_bytes has no per-site record to excuse it.
            if (k == "switch_resolved" and base_sites
                    and not site_problems):
                explained.append(
                    "switch_resolved fell %d -> %d, and no switch table gained"
                    " an unresolved dispatch: these are duplicate dispatches"
                    " that stopped being emitted, not sites the lifter can no"
                    " longer place" % (bm[k], measures[k]))
            else:
                problems.append("%s fell %d -> %d" % (k, bm[k], measures[k]))
    if measures["unclassified_itail"] or measures["unclassified_icall"]:
        problems.append("unclassified transfer shapes: %d itail, %d icall" % (
            measures["unclassified_itail"], measures["unclassified_icall"]))
    for n in site_notes:
        print("  note: " + n)
    for e in explained:
        print("  explained: " + e)
    if args.verbose and base_sites:
        print()
        print("  site record: %d switch tables, %d untranslated addresses,"
              " %d stubs" % (len(sites["switch"]), len(sites["todo"]),
                             len(sites["stubs"])))
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
