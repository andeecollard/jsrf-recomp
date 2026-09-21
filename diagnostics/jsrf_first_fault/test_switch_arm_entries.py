#!/usr/bin/env python3
"""The switch-arm recovery is selected, registered, and actually wired in.

WHAT THIS GUARDS, and why each part is here rather than left to the eye.

recover_midfunction_entries.py has been able to build a body for a
mid-function entry since 19 Sep 2026, and regenerate.sh has run it as a
mandatory post-pass for just as long. It still could not repair 0x00075EB3,
because it only ever looked at stubs and an arm reached through an unresolved
jump table is not one. Nothing failed. The pass ran, reported success, and the
445 arm slots switch_arm_audit.py counts stayed exactly where they were. The
player found the hole, in a live session, as a black screen.

A hole with no failing test is the shape of this whole defect, so the tests
below are chosen for the ways it can silently come back:

  SELECTION      an arm with no body is picked, an arm with one is not, and
                 the tight window still rejects the over-read the table walk
                 is known to produce. If this breaks, the pass quietly
                 recovers nothing and prints 0.
  THE STUB FILE  recomp_stubs_unresolved.c is scanned WITH the rest, through
                 the in-memory text the stub phase is about to write. The arm
                 that started this lives behind a dispatch in a recovered stub
                 body (sub_00075E90), so a version that scans the tree on disk
                 finds a strict subset and looks fine doing it.
  REGISTRATION   a body with no row in g_recomp_table is dead code, and the
                 [ITAIL] failure repeats unchanged. The size literal is
                 checked on its own because recover_icall_entries.py appends
                 rows without updating it -- rows past the declared count are
                 invisible to both the binary search and the flat-table build,
                 which is a registration that registers nothing.
  THE WIRE       recover_midfunction_entries.py must still CALL the arm phase.
                 Everything else here can pass while the pass is never run,
                 which is precisely the state the last two days were in.

Hermetic by construction: a synthetic gen tree and a synthetic image, no XBE
and no capstone, so it runs under whatever interpreter CMake found. The real
tree is checked too when one is to hand -- that half asserts the actual
address out of the actual log -- and is skipped, loudly, when it is not.
"""
import argparse
import ast
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
for _p in (HERE, ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import control_flow_gate
import switch_arm_entries

FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
    return condition


def fake_image(tables, text=(0x00001000, 0x00100000)):
    """(read32, text_lo, text_hi) over a dict of {VA: [word, ...]}.

    The walk's rules are what is under test -- stop at the first word outside
    .text, keep only arms within WINDOW of the owner, cap at 64 -- and none of
    them is a fact about the XBE container. Building a 2 MB file to exercise
    them would test the parser instead, and would have to be checked in.
    """
    words = {}
    for base, values in tables.items():
        for i, value in enumerate(values):
            words[base + 4 * i] = value

    def read32(va):
        return words.get(va, 0x00000000)

    return read32, text[0], text[1]


UNIT = """\
void sub_%(owner)08X(void)
{
    g_seh_ebp = ebp; RECOMP_ITAIL(MEM32(eax * 4 + 0x%(table)X)); return;
}
"""

DISPATCH = """\
static const recomp_entry_t g_recomp_table[] = {
%(rows)s
};

static const size_t g_recomp_table_size = %(count)d;

static const uint32_t g_flat_base = 0x%(base)08Xu;
static const uint32_t g_flat_span = 0x%(span)08Xu;
"""


def write_tree(directory, units, stubs="", registered=()):
    for name, text in units.items():
        with open(os.path.join(directory, name), "w") as f:
            f.write(text)
    with open(os.path.join(directory, "recomp_stubs_unresolved.c"), "w") as f:
        f.write(stubs)
    rows = "\n".join("    { 0x%08Xu, (recomp_func_t)sub_%08X }," % (va, va)
                     for va in sorted(registered))
    base = min(registered) if registered else 0x1000
    span = (max(registered) - base + 1) if registered else 1
    with open(os.path.join(directory, "recomp_dispatch.c"), "w") as f:
        f.write(DISPATCH % {"rows": rows, "count": len(registered),
                            "base": base, "span": span})


def test_selection():
    """An arm with no body is selected; one with a body, or a manual one, is not."""
    with tempfile.TemporaryDirectory() as gen:
        # 0x1040 has a body of its own. 0x1010 and 0x1080 do not. 0xDEADBEEF
        # is outside .text and stops the walk, which is how a table's end is
        # found at all -- the words after it are ordinary code.
        write_tree(gen, {
            "recomp_0000.c": UNIT % {"owner": 0x1000, "table": 0x2000}
                             + "\nvoid sub_00001040(void)\n{\n    return;\n}\n",
        })
        image = fake_image({0x2000: [0x1010, 0x1040, 0x1080, 0xDEADBEEF]})
        arms, stats = switch_arm_entries.select(gen, None, image=image)

        check(sorted(arms) == [0x1010, 0x1080],
              "selection: expected [0x1010, 0x1080], got %s"
              % ["0x%08X" % a for a in sorted(arms)])
        check(arms.get(0x1010) == [0x2000],
              "selection: 0x1010 should name the table that reaches it")
        check(stats["arm_slots"] == 3 and stats["arm_slots_no_body"] == 2,
              "selection: slot counts wrong: %s" % stats)

        # already_named is how the manual overrides -- which define the same
        # sub_XXXXXXXX symbols the generator does -- stay out. Recovering one
        # of those is a duplicate definition, not a repair.
        arms, _ = switch_arm_entries.select(gen, None, image=image,
                                            already_named={"sub_00001010"})
        check(sorted(arms) == [0x1080],
              "selection: already_named did not exclude sub_00001010")


def test_tight_window_rejects_the_over_read():
    """The walk reads past the table; the window is what throws that away."""
    with tempfile.TemporaryDirectory() as gen:
        write_tree(gen, {"recomp_0000.c": UNIT % {"owner": 0x1000, "table": 0x2000}})
        # 0x1010 is a real arm. 0x40000 is a valid .text address sitting after
        # the table -- the next function's bytes, read as a word. Only the
        # distance from the owner tells them apart.
        image = fake_image({0x2000: [0x1010, 0x00040000, 0xDEADBEEF]})
        arms, _ = switch_arm_entries.select(gen, None, image=image)
        check(sorted(arms) == [0x1010],
              "window: the over-read 0x00040000 was selected as an arm: %s"
              % ["0x%08X" % a for a in sorted(arms)])

        # ... and the window is the ONLY thing rejecting it, so a wider one
        # takes it. If this stops holding, the bound has stopped being applied
        # rather than the over-read having gone away.
        arms, _ = switch_arm_entries.select(gen, None, image=image, window=0x80000)
        check(sorted(arms) == [0x1010, 0x00040000],
              "window: widening it did not admit the over-read, so the tight"
              " bound is no longer what rejects it")


def test_the_stub_file_is_scanned_through_the_override():
    """The arm that started this is behind a dispatch in a RECOVERED stub."""
    stub_body = UNIT % {"owner": 0x1200, "table": 0x3000}
    image = fake_image({0x3000: [0x1210, 0xDEADBEEF]})
    with tempfile.TemporaryDirectory() as gen:
        # On disk the stub file is still the pre-recovery placeholder: no
        # dispatch in it, so no arm to find. This is the tree a pass that ran
        # BEFORE the stub phase -- or one that re-read the directory -- sees.
        write_tree(gen, {"recomp_0000.c": "/* nothing */\n"},
                   stubs="void sub_00001200(void) { g_esp += 4; }\n")
        arms, _ = switch_arm_entries.select(gen, None, image=image)
        check(arms == {},
              "ordering: the un-recovered tree should expose no arms, got %s"
              % ["0x%08X" % a for a in arms])

        # With the stub phase's in-memory result handed in, the dispatch it is
        # about to write is visible and its arm is found. Same directory.
        arms, _ = switch_arm_entries.select(
            gen, None, image=image,
            sources={os.path.join(gen, "recomp_stubs_unresolved.c"): stub_body})
        check(sorted(arms) == [0x1210],
              "ordering: the recovered stub body's arm was not found through"
              " `sources`: %s" % ["0x%08X" % a for a in sorted(arms)])


def test_registration():
    """A row, in order, counted, and idempotent."""
    with tempfile.TemporaryDirectory() as gen:
        write_tree(gen, {"recomp_0000.c": "/* nothing */\n"},
                   registered=(0x1000, 0x1100, 0x1200))
        dispatch = open(os.path.join(gen, "recomp_dispatch.c")).read()

        updated, added = switch_arm_entries.register_dispatch(
            dispatch, [(0x1080, "sub_00001080"), (0x1010, "sub_00001010")])
        check(len(added) == 2, "registration: expected 2 rows, got %d" % len(added))

        rows = [int(m.group(1), 16) for m in
                switch_arm_entries.DISPATCH_ROW_RE.finditer(
                    switch_arm_entries.TABLE_RE.search(updated).group(2))]
        check(rows == sorted(rows),
              "registration: g_recomp_table is not sorted, so recomp_lookup's"
              " binary search will miss rows: %s" % ["0x%X" % r for r in rows])
        check(rows == [0x1000, 0x1010, 0x1080, 0x1100, 0x1200],
              "registration: wrong rows: %s" % ["0x%X" % r for r in rows])

        # THE SIZE LITERAL. recomp_lookup and recomp_dispatch_init both trust
        # it over the array's real length, so a row past it is not registered
        # at all -- it is dead code that looks exactly like a fix.
        size = int(switch_arm_entries.SIZE_RE.search(updated).group(2))
        check(size == len(rows),
              "registration: g_recomp_table_size is %d for %d rows -- the last"
              " %d are invisible to recomp_lookup" % (size, len(rows),
                                                      len(rows) - size))

        again, added_again = switch_arm_entries.register_dispatch(
            updated, [(0x1080, "sub_00001080"), (0x1010, "sub_00001010")])
        check(added_again == [] and again == updated,
              "registration: re-running is not a no-op, so a second recovery"
              " pass would duplicate every row")

        check(switch_arm_entries.registered_addresses(updated)
              == {0x1000, 0x1010, 0x1080, 0x1100, 0x1200},
              "registration: registered_addresses disagrees with the rows it"
              " just wrote")


def test_registration_widens_the_flat_table():
    """recomp_dispatch_init indexes by (va - base) and bounds on span."""
    with tempfile.TemporaryDirectory() as gen:
        write_tree(gen, {"recomp_0000.c": "/* nothing */\n"},
                   registered=(0x1000, 0x1100))
        dispatch = open(os.path.join(gen, "recomp_dispatch.c")).read()
        updated, _ = switch_arm_entries.register_dispatch(
            dispatch, [(0x0900, "sub_00000900"), (0x9000, "sub_00009000")])
        base = int(switch_arm_entries.FLAT_BASE_RE.search(updated).group(2), 16)
        span = int(switch_arm_entries.FLAT_SPAN_RE.search(updated).group(2), 16)
        check(base == 0x0900 and base + span == 0x9001,
              "flat table: base/span 0x%X/0x%X does not cover the new rows,"
              " so recomp_dispatch_init would write outside its calloc"
              % (base, span))


def test_the_arm_phase_is_wired_into_the_mandatory_post_pass():
    """Everything above can pass while nothing ever runs it.

    regenerate.sh invokes recover_midfunction_entries.py and nothing else, so
    that file calling the arm phase is the entire difference between a repair
    and a library. Asserted against the parsed source rather than by importing
    it: importing drags in the translator and capstone, which the interpreter
    running this test is not required to have.
    """
    source = open(os.path.join(HERE, "recover_midfunction_entries.py")).read()
    tree = ast.parse(source)
    defined = {node.name for node in ast.walk(tree)
               if isinstance(node, ast.FunctionDef)}
    check("recover_switch_arms" in defined,
          "wiring: recover_midfunction_entries.py no longer defines"
          " recover_switch_arms")

    main = next((n for n in ast.walk(tree)
                 if isinstance(n, ast.FunctionDef) and n.name == "main"), None)
    called = {n.func.id for n in ast.walk(main) if main
              and isinstance(n, ast.Call) and isinstance(n.func, ast.Name)}
    check("recover_switch_arms" in called,
          "wiring: main() does not call recover_switch_arms, so regenerate.sh"
          " runs the stub phase only and every switch arm stays unresolved --"
          " which is the exact state that produced the 0x00075EB3 [ITAIL]")

    check("register_dispatch" in source,
          "wiring: the recovery no longer registers arms in g_recomp_table;"
          " the bodies it writes would be unreachable dead code")


def test_against_the_real_tree(gen, xbe):
    """0x00075EB3, out of the player's log, through the table at 0x00076610."""
    if not gen or not os.path.isdir(gen) or not xbe or not os.path.exists(xbe):
        print("  real tree: SKIPPED (need --gen with a generated tree and an"
              " XBE via --xbe or $JSRF_GAME_DIR)")
        return
    if not os.path.exists(os.path.join(gen, "recomp_dispatch.c")):
        print("  real tree: SKIPPED (%s has no recomp_dispatch.c)" % gen)
        return
    arms, stats = switch_arm_entries.select(gen, xbe)
    target = 0x00075EB3
    if not check(target in arms,
                 "real tree: 0x%08X -- the address in the first [ITAIL]"
                 " failure this project ever logged -- is not selected for"
                 " recovery. %d arms were." % (target, len(arms))):
        return
    check(0x00076610 in arms[target],
          "real tree: 0x%08X is selected, but not through the table at"
          " 0x00076610 that sub_00075E90 dispatches on: %s"
          % (target, ["0x%08X" % t for t in arms[target]]))
    registered = switch_arm_entries.registered_addresses(
        open(os.path.join(gen, "recomp_dispatch.c")).read())
    check(target not in registered or stats["arms_no_body"] == 0,
          "real tree: 0x%08X is registered but still selected" % target)
    print("  real tree: 0x%08X selected via %s; %d arms with no entry point"
          " across %d unresolved tables"
          % (target, ", ".join("0x%08X" % t for t in arms[target]),
             stats["arms_no_body"], stats["unresolved_tables"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gen", default=None,
                    help="a generated tree to check the real addresses against;"
                         " the hermetic tests need nothing")
    ap.add_argument("--xbe", default=None,
                    help="default: $JSRF_GAME_DIR/default.xbe if set")
    args = ap.parse_args()

    xbe = args.xbe
    if not xbe and os.environ.get("JSRF_GAME_DIR"):
        xbe = os.path.join(os.environ["JSRF_GAME_DIR"], "default.xbe")

    test_selection()
    test_tight_window_rejects_the_over_read()
    test_the_stub_file_is_scanned_through_the_override()
    test_registration()
    test_registration_widens_the_flat_table()
    test_the_arm_phase_is_wired_into_the_mandatory_post_pass()
    test_against_the_real_tree(args.gen, xbe)

    if FAILURES:
        print("FAIL: %d check(s)" % len(FAILURES))
        for failure in FAILURES:
            print("  " + failure)
        return 1
    print("switch-arm recovery: selection, ordering, registration and wiring"
          " all hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
