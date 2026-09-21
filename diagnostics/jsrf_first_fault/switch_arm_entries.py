#!/usr/bin/env python3
"""The switch arms that have no body, and the registration they need.

WHY THIS EXISTS. On 21 Sep 2026 a player session produced the first [ITAIL]
failure this project has ever logged:

    [ITAIL] Unresolved tail jump to 0x00075EB3 (1 times)
    [ITAIL]   THE EPILOGUE NEVER RAN. esp is still inside the jumping
    [ITAIL]   function's frame, so `esp += 4` has just returned its caller
    [ITAIL]   through a local variable

0x00075EB3 is arm 0 of the jump table at 0x00076610, dispatched from
sub_00075E90, inside CreateFullRoboyMenu. It is not a missing function and it
is not a missing table lookup: it is an address with no ENTRY POINT. The
lookup in RECOMP_ITAIL is fine; there is simply nothing for it to find, so the
failure path runs `g_esp += 4; g_eax = 0` and eats the caller's return address
off a frame that still holds 0x2D0 bytes of locals. That is the G20
stack-corruption class, and switch_arm_audit.py counts 445 arm slots (437
distinct addresses) standing in exactly the same place.

THE GAP THIS CLOSES. recover_midfunction_entries.py already knows how to build
a body for "an address that lies at an instruction boundary inside an
already-detected function" -- that is precisely this shape, and it is a
mandatory post-pass in regenerate.sh. But it only ever looks at STUBS: names in
recomp_stubs_unresolved.c, which exist because something statically CALLS the
address. An arm reached only through an unresolved jump table is called by
nothing, so it never becomes a stub, so the recovery pass never sees it. The
machinery and the address list have been in the same directory for two days
without a wire between them. This is the wire.

control_flow_gate.py already walks the tables out of the XBE, with the tight
bound switch_arm_audit.py calibrated by hand, so the walk is imported rather
than rewritten. An audit and a repair that disagree about which arms exist is
the worst of the available outcomes: the audit would keep reporting a number
the repair had already acted on, or the reverse, and neither would be wrong on
its own terms.

WHY A DECLARATION AND A BODY ARE NOT ENOUGH, which is the part that is easy to
get wrong. A recovered STUB is reachable because its caller has a static
`RECOMP_ABI_CALL(0x..u, sub_..)` in it -- checked 21 Sep 2026: not one of the
155 names in recomp_stubs_unresolved.c appears in recomp_dispatch.c, and they
run anyway. An arm has no such call site. Its only route in is
RECOMP_ITAIL -> recomp_lookup(va), which searches g_recomp_table. A body
nothing links to is dead code and the [ITAIL] failure repeats unchanged, so
every arm recovered here is REGISTERED: a row in g_recomp_table, and the
g_recomp_table_size beside it bumped to match.

That size literal is why registration lives here as its own tested function.
recover_icall_entries.py appends rows to the same array and does not touch
g_recomp_table_size -- rows past the declared count are invisible to both the
binary search and the flat-table build, so they would register nothing while
looking completely correct. It is not run by regenerate.sh, so nothing has
paid for that yet. Do not copy it; that is what `register_dispatch` is for.

WHAT THIS DOES NOT DO. It does not seed addresses into functions.json, and
must never be changed to. The runtime's own [ITAIL] advice says so in as many
words -- "DO NOT seed this address -- that truncates its container and corrupts
every caller instead" -- and regenerate.sh carries the same warning from the
other side, which is why RECOMP_SEED_INTERIOR demotes interior seeds by
default. Seeding 0x00075EB3 would end sub_00075E90 at the dispatching jmp and
guarantee the table can never resolve. A recovered entry leaves the container
untouched and adds a second way in; that is the whole difference.

    python3 diagnostics/jsrf_first_fault/switch_arm_entries.py \\
        --gen build-macos/jsrf-first-fault/gen \\
        --xbe "$JSRF_GAME_DIR/default.xbe"

prints the plan and changes nothing. recover_midfunction_entries.py is what
acts on it, in the same run and immediately after its stub pass, because the
stub pass is what CREATES several of these dispatches (sub_00075E90 is a
recovered stub) and the arm list is not complete until it has finished.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
for _p in (HERE, ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import control_flow_gate

WINDOW = control_flow_gate.WINDOW

# A row of g_recomp_table, and the count the lookup trusts over the row count.
DISPATCH_ROW_RE = re.compile(r"^\s*\{ 0x([0-9A-Fa-f]+)u, \(recomp_func_t\)"
                             r"([A-Za-z_][A-Za-z0-9_]*) \},\s*$", re.M)
TABLE_RE = re.compile(
    r"(static const recomp_entry_t g_recomp_table\[\] = \{\n)(.*?)(\n\};)", re.S)
SIZE_RE = re.compile(r"(static const size_t g_recomp_table_size = )(\d+)(;)")
FLAT_BASE_RE = re.compile(r"(static const uint32_t g_flat_base = 0x)([0-9A-Fa-f]+)(u;)")
FLAT_SPAN_RE = re.compile(r"(static const uint32_t g_flat_span = 0x)([0-9A-Fa-f]+)(u;)")


def registered_addresses(dispatch_text):
    """Guest VAs already in g_recomp_table."""
    return {int(va, 16) for va, _ in DISPATCH_ROW_RE.findall(dispatch_text)}


def select(gen_dir, xbe_path, window=WINDOW, sources=None, image=None,
           already_named=(), defined_by_this_run=()):
    """Arms with no body of their own: {arm VA: [table VA, ...]}, plus stats.

    The three filters, in the order they are cheapest to apply:

      * the arm is in the tight-bound arm list of a table that some dispatch
        in this tree could NOT resolve. A table resolved at every site already
        turned into gotos and its arms are labels, not entries;
      * nothing already defines `sub_XXXXXXXX` anywhere in the tree. This is
        what keeps the pass off work the stub recovery has already done --
        several arms are stubs as well, because something calls them too, and
        recover_midfunction_entries.py gets to those first;
      * nothing has already claimed the name in this run (`already_named`),
        which covers the bodies the stub pass pulled in by reference and has
        not written out yet.

    `defined_by_this_run` is the exception to the second filter and it is
    there for one reason, found 21 Sep 2026 by regenerating. The scanned text
    has to include the bodies the stub phase pulled in BY REFERENCE, because
    sub_00075E90 -- the only dispatcher of the table 0x00075EB3 is an arm of
    -- is one of them, and a scan without it cannot see that table at all.
    But putting those bodies in the text also makes their names `defined`,
    which would drop them from the selection, and an arm that has a body and
    no row in g_recomp_table is exactly as unreachable as one with neither.
    So they are handed in here and exempted: seen as text, not counted as
    somebody else's work.

    Registration is filtered separately, by the caller, against
    registered_addresses() -- an address can be defined and not registered
    (every recovered stub is) and the two questions have different answers.
    """
    measures, _todo, owners_of_table, defined, _sites = control_flow_gate.scan_gen(
        gen_dir, sources=sources)
    _am, _rows, arms_of = control_flow_gate.walk_arms(
        xbe_path, owners_of_table, defined, window, image=image)

    known = (set(defined) | set(already_named)) - set(defined_by_this_run)
    tables_of = {}
    slots = 0
    for table, arms in sorted(arms_of.items()):
        for arm in arms:
            slots += 1
            if ("sub_%08X" % arm) in known:
                continue
            tables_of.setdefault(arm, set()).add(table)
    stats = {
        "unresolved_tables": len(owners_of_table),
        "arm_slots": slots,
        "arm_slots_no_body": sum(
            1 for arms in arms_of.values() for a in arms
            if ("sub_%08X" % a) not in known),
        "arms_no_body": len(tables_of),
    }
    return {arm: sorted(t) for arm, t in sorted(tables_of.items())}, stats


def register_dispatch(dispatch_text, entries):
    """Add (VA, name) rows to g_recomp_table and keep every invariant it has.

    Four of them, and each one silently produces a table that looks right:

      * SORTED. recomp_lookup binary-searches when the flat table is not built,
        so one row out of order makes an arbitrary set of neighbours
        unfindable -- not the new one, the neighbours.
      * COUNTED. g_recomp_table_size is a literal the generator emits; the
        lookup and the flat-table build both trust it over the array's real
        length. Rows past it are dead.
      * SPANNED. recomp_dispatch_init indexes the flat table by
        (va - g_flat_base) and rejects anything at or past g_flat_span. An
        address outside the span resolves through the binary search and not
        through the flat table -- which is not wrong, but it is a silent
        divergence between two paths that are meant to be identical by
        construction, and it would write outside the calloc'd block on the
        build loop. Widen both rather than allow it.
      * UNIQUE. A VA already present is left exactly as it was. Re-running the
        recovery over a tree it has already patched must be a no-op, because
        regenerate.sh is not the only thing that runs it.
    """
    match = TABLE_RE.search(dispatch_text)
    if not match:
        raise AssertionError("no g_recomp_table in this dispatch unit")
    rows = [line for line in match.group(2).splitlines() if line.strip()]
    for line in rows:
        # Refuse rather than sort around it. The sort key below reads the VA
        # out of every line, so anything else in the array -- a comment, a
        # conditional, an entry in some future shape -- would either crash
        # here or be silently moved somewhere it changes meaning.
        if not DISPATCH_ROW_RE.match(line):
            raise AssertionError("unrecognised row in g_recomp_table: %r" % line)
    present = registered_addresses(dispatch_text)
    added = []
    for va, name in sorted(entries):
        if va in present:
            continue
        present.add(va)
        rows.append("    { 0x%08Xu, (recomp_func_t)%s }," % (va, name))
        added.append((va, name))
    if not added:
        return dispatch_text, []

    rows.sort(key=lambda line: int(DISPATCH_ROW_RE.match(line).group(1), 16))
    out = dispatch_text[:match.start(2)] + "\n".join(rows) + dispatch_text[match.end(2):]

    size = SIZE_RE.search(out)
    if not size:
        raise AssertionError("no g_recomp_table_size beside g_recomp_table")
    out = SIZE_RE.sub(lambda m: m.group(1) + str(len(rows)) + m.group(3), out, count=1)

    base_m, span_m = FLAT_BASE_RE.search(out), FLAT_SPAN_RE.search(out)
    if base_m and span_m:
        base = int(base_m.group(2), 16)
        span = int(span_m.group(2), 16)
        lo = min(base, min(present))
        hi = max(base + span, max(present) + 1)
        if (lo, hi - lo) != (base, span):
            out = FLAT_BASE_RE.sub(
                lambda m: "%s%08X%s" % (m.group(1), lo, m.group(3)), out, count=1)
            out = FLAT_SPAN_RE.sub(
                lambda m: "%s%08X%s" % (m.group(1), hi - lo, m.group(3)), out, count=1)
    return out, added


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--xbe", default=None,
                    help="default: $JSRF_GAME_DIR/default.xbe if set")
    ap.add_argument("--window", type=lambda v: int(v, 0), default=WINDOW)
    ap.add_argument("--verbose", action="store_true",
                    help="one line per arm, with the tables that reach it")
    args = ap.parse_args()

    xbe = args.xbe
    if not xbe and os.environ.get("JSRF_GAME_DIR"):
        xbe = os.path.join(os.environ["JSRF_GAME_DIR"], "default.xbe")
    if not xbe or not os.path.exists(xbe):
        sys.exit("the arm walk needs the XBE: pass --xbe or set JSRF_GAME_DIR")

    tables_of, stats = select(args.gen, xbe, args.window)
    dispatch_path = os.path.join(args.gen, "recomp_dispatch.c")
    registered = (registered_addresses(open(dispatch_path).read())
                  if os.path.exists(dispatch_path) else set())

    print("switch arms with no entry point in %s" % args.gen)
    for k in ("unresolved_tables", "arm_slots", "arm_slots_no_body", "arms_no_body"):
        print("    %-24s %6d" % (k, stats[k]))
    print("    %-24s %6d   (already in g_recomp_table; nothing to do)"
          % ("of those registered", sum(1 for a in tables_of if a in registered)))
    if args.verbose:
        for arm, tables in tables_of.items():
            print("    0x%08X  <- %s" % (
                arm, ", ".join("0x%08X" % t for t in tables)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
