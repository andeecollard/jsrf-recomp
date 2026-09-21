"""Translate the entry points discovery missed: stubs, and switch arms.

Function discovery splits some routines -- a switch whose cases became
separate functions, a block only ever reached by an indirect jump -- and any
entry it did not detect became a stub whose whole body is `g_esp += 4`. That
stub consumes the return address and nothing else, so the pushes the real
entry's epilogue would have popped stay on the guest stack and the caller
restores its callee-saved registers from the wrong slots.

recover_shared_epilogues.py repairs the subset of those stubs that are pure
return sequences. This repairs the rest of the same failure: an address that
lies at an instruction boundary inside an already-detected function is real
code, and translating it from there to the end of the function that owns it
produces the body it should have had. That is how the generator would emit it
if discovery had found the entry.

An address that falls in the gap between two detected functions is a whole
function discovery missed, not a fragment of one, and is recovered the same
way with the next detected function as its bound. Anything past its return is
unreachable and costs only compiled bytes.

A recovered body can jump backwards, to an address inside its owner that is
earlier than the entry. That becomes a call to another mid-function entry, and
that entry needs a body too -- so recovery closes over what it references
rather than rejecting the entry that needed it.

Deliberately narrow otherwise. An address that does not fall on an instruction
boundary is not recovered, because that means the owner was decoded
differently and the disagreement has to be understood first, and a body that
reaches a label it does not define is rejected rather than patched around.

THE SECOND PHASE, added 21 Sep 2026. Everything above was true of STUBS only
-- addresses something statically CALLS, which is how they got a placeholder
to replace in the first place. An address reached only through a jump table the
lifter could not place is called by nothing, never becomes a stub, and so was
invisible to a pass that was otherwise built exactly for it. That is not a
theoretical hole: it is the first [ITAIL] failure this project has logged,
0x00075EB3 in CreateFullRoboyMenu, on the player's session this morning, and
switch_arm_audit.py counts 445 arm slots standing in the same place.

So after the stub phase, the arm phase asks switch_arm_entries.py which arms
still have no body and recovers those the same way, by the same rules, with
the same refusals. Two differences, both forced:

  * it runs AFTER the stub phase and scans the tree that phase is about to
    write, because recovered stub bodies are themselves a source of unresolved
    dispatches -- sub_00075E90, which owns the table 0x00075EB3 comes from, is
    a recovered stub. Ordering them the other way finds a strict subset.

    "The tree that phase is about to write" has to mean ALL of it, which the
    first version of this did not: sub_00075E90 is reached by a backward jump
    from a recovered stub, so it lands in `extra` and was appended after the
    arm walk had already run. The first real regeneration, 21 Sep 2026, then
    saw 52 tables and 424 arms instead of 53 and 437, and the 13 it missed
    included 0x00075EB3 -- the address the whole pass exists for, absent from
    the tree while every printed number looked healthy. The walk now reads the
    patched text, the by-reference bodies and the arms recovered so far, and
    repeats until a round adds nothing.
  * an arm is REGISTERED in g_recomp_table, which a stub never needs. A stub
    is reached by a static call to its name; an arm is reached only by
    recomp_lookup, so a body with no row is dead code. switch_arm_entries.py's
    register_dispatch does it and documents the four invariants involved.
"""
import argparse
import bisect
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))
from tools.recomp.config import configure_from_xbe
from tools.recomp.translator import FunctionTranslator
from tools.recomp import manual_scan
import switch_arm_entries

STUB = re.compile(
    r"^void (sub_([0-9A-F]{8}))\(void\) \{ "
    r'(?:static int _seen; if \(!_seen\) \{ _seen = 1; '
    r'recomp_stub_ran\(0x\2u, "[^"\n]*"\); \} )?'
    r"g_esp \+= \d+; /\* 0x\2: [^\n]* \*/ \}$",
    re.MULTILINE)

# Hand-written guest bodies use the same sub_XXXXXXXX names the generator does,
# so recovering an address one of them already defines is a link error, not a
# repair. --exclude-manual keeps the recompiler off them for the same reason;
# this is the same list, read through the same scanner so the two cannot drift.
MANUAL_SOURCES = ("jsrf_manual_overrides.c", "recomp_manual.c")


def dangling_labels(code):
    """Labels the body jumps to but never defines."""
    defined = set(re.findall(r"^(loc_[0-9A-F]{8}): ;", code, re.MULTILINE))
    used = set(re.findall(r"goto (loc_[0-9A-F]{8});", code))
    return sorted(used - defined)


class EntryRecovery:
    """Translate one entry point, closing over the entries it references.

    Was a pair of closures inside main() until the arm phase needed the same
    rules and the same `extra` set. The rules are the value here: which bound
    an address gets, that it must fall on an instruction boundary in its owner,
    and that a body reaching a label it does not define is refused rather than
    patched. Two callers applying them from one place is the point.
    """

    def __init__(self, translator, db, header):
        self.translator = translator
        self.db = db
        self.starts = sorted(db)
        self.header = header
        # name -> body, for entries a recovered body calls. Shared across both
        # phases: an arm can be reached by a backward jump from a stub and vice
        # versa, and translating either one twice would define it twice.
        self.extra = {}

    def bounds_of(self, address):
        """(owner start or None, end address) for code at this address."""
        index = bisect.bisect_right(self.starts, address) - 1
        if index >= 0 and self.starts[index] < address < self.db[self.starts[index]]["end"]:
            return self.starts[index], self.db[self.starts[index]]["end"]
        following = bisect.bisect_right(self.starts, address)
        if following >= len(self.starts):
            return None, None
        return None, self.starts[following]

    def translate(self, address, name):
        """Body for this entry, or a string explaining why there is none."""
        translator = self.translator
        start, end = self.bounds_of(address)
        if end is None:
            return "past the last detected function"
        if start is not None:
            decoded = translator.disasm.disassemble_function(
                translator._read_func_bytes(start, end), start, end)
            if address not in {insn.address for insn in decoded}:
                return "not an instruction boundary in its owner"

        translator.lifter.referenced_calls.clear()
        code = translator.translate_function(address, {"name": name, "end": end})
        if not code or "TODO:" in code or "UNIMPLEMENTED" in code:
            return "incomplete translation"
        if "return;" not in code:
            return "no return path"
        missing = dangling_labels(code)
        if missing:
            return f"jumps to {missing[0]}, outside the range translated"

        for callee in list(translator.lifter.referenced_calls.values()):
            if re.search(rf"\bvoid {re.escape(callee)}\(void\);", self.header):
                continue
            if callee in self.extra:
                continue
            callee_address = int(callee[len("sub_"):], 16)
            self.extra[callee] = None          # claim it before recursing
            body = self.translate(callee_address, callee)
            if not body.startswith("/*"):
                del self.extra[callee]
                return f"calls {callee}, which cannot be recovered ({body})"
            self.extra[callee] = body
        return ("/* Recovered entry at 0x%08X, translated through 0x%08X. */\n%s"
                % (address, end, code.rstrip()))


def manual_definitions():
    """Names the hand-written guest bodies already define."""
    names = set()
    for source in MANUAL_SOURCES:
        names |= manual_scan.definition_names(str(HERE / source))
    return names


EXTRA_NOTE = "/* Reached by a backward jump from a recovered entry. */"
ARM_NOTE = ("/* Switch arm with no entry point of its own; reached"
            " only through an unresolved jump table. */")


def with_extra_bodies(text, extra):
    """`text` plus every body pulled in by reference, as main() will write it.

    One function because two callers must agree. main() appends these at the
    end; the arm phase has to SEE them before that, and a copy of the loop in
    each place is how the arm phase came to scan a tree that was missing
    sub_00075E90 -- the dispatcher of the one table this whole pass exists
    for. Whatever main() writes is what the arm walk reads.
    """
    for name in sorted(extra):
        text += "\n" + EXTRA_NOTE + "\n" + extra[name].rstrip() + "\n"
    return text


# A recovered body can open a table nobody could see before it existed, so one
# round is not the answer -- it is only the answer that happens to be right on
# a tree some earlier run already patched. Eight is regenerate.sh's bound for
# the same shape of loop and this converges in far fewer; not converging is
# reported rather than silently truncated.
ARM_ROUNDS = 8


def recover_switch_arms(recovery, gen_dir, xbe, stub_source, window,
                        rounds=ARM_ROUNDS):
    """Bodies for the arms of every table the lifter could not place.

    Returns (entries, skipped, stats): entries is [(VA, name, body or None)],
    with None meaning the body is already in recovery.extra because something
    recovered earlier called it -- it still needs a dispatch row, which is the
    whole reason an arm is different from a stub.

    ITERATED, and the reason is 0x00075EB3 itself. The text handed to the walk
    must contain every body this run will write, because an unresolved
    dispatch inside one of them is the only evidence its table exists:
    sub_00075E90 is reached by a backward jump from a recovered stub, lands in
    recovery.extra rather than in the stub text, and a single round over
    `stub_source` alone therefore saw 52 tables and 424 arms where the written
    tree has 53 and 437. The 13 it could not see included the one address the
    player's [ITAIL] named. Each round rescans with everything recovered so
    far in place and stops when a round adds nothing.
    """
    stub_path = os.path.join(gen_dir, "recomp_stubs_unresolved.c")
    manual = manual_definitions()
    entries, skipped, stats = [], {}, {}
    seen = set()
    for _round in range(rounds):
        # Everything this run will write, in the order main() writes it: the
        # patched stub text, the bodies pulled in by reference, then the arm
        # bodies recovered so far.
        prospective = with_extra_bodies(stub_source, recovery.extra)
        for _va, _name, body in entries:
            if body is not None:
                prospective += "\n" + ARM_NOTE + "\n" + body.rstrip() + "\n"
        # Only the manual definitions are excluded from SELECTION. A name this
        # run created stays in the list: it has a body, but a body is half the
        # repair and the dispatch row is the half the runtime can see, so it
        # is carried through to registration with body=None.
        tables_of, stats = switch_arm_entries.select(
            gen_dir, xbe, window=window,
            sources={stub_path: prospective},
            already_named=manual,
            defined_by_this_run=set(recovery.extra) | {n for _v, n, _b in entries})

        fresh = [arm for arm in tables_of
                 if arm not in seen and ("0x%08X" % arm) not in skipped]
        if not fresh:
            break
        for arm in fresh:
            seen.add(arm)
            name = "sub_%08X" % arm
            if name in recovery.extra:
                # Pulled in as a callee while recovering something else. The
                # body exists; only the registration is missing.
                entries.append((arm, name, None))
                continue
            result = recovery.translate(arm, name)
            if not result.startswith("/*"):
                skipped["0x%08X" % arm] = result
                continue
            entries.append((arm, name, result))
    else:
        print("WARNING: switch-arm recovery did not converge in %d rounds;"
              " a table opened by one of the last round's bodies is"
              " unrecovered." % rounds)
    stats["arms_recovered"] = len(entries)
    stats["arms_skipped"] = len(skipped)
    stats["arms_no_body"] = len(entries) + len(skipped)
    return entries, skipped, stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # Same reasoning as regenerate.sh, which invokes this: the game dump is
    # outside the tree and the sibling default is nobody else's layout.
    # JSRF_GAME_DIR is what every other script in this directory already reads.
    _game_dir = os.environ.get("JSRF_GAME_DIR")
    parser.add_argument("--xbe", type=Path,
                        default=(Path(_game_dir) / "default.xbe") if _game_dir
                        else ROOT.parent / "Jet Set Radio Future (US)/default.xbe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-macos/jsrf-first-fault")
    parser.add_argument("--dry-run", action="store_true")
    # The arm phase is on by default because the defect it repairs is live.
    # The switch is here so a bisect can separate the two phases without
    # editing the script, which is the only way anyone will ever want it off.
    parser.add_argument("--no-arms", dest="arms", action="store_false",
                        help="stub phase only: skip switch-arm recovery")
    parser.add_argument("--window", type=lambda v: int(v, 0),
                        default=switch_arm_entries.WINDOW,
                        help="tight bound for the arm walk (switch_arm_audit's)")
    args = parser.parse_args()

    configure_from_xbe(str(args.xbe))
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16))
          for f in json.loads((args.output / "disasm/functions.json").read_text())}
    translator = FunctionTranslator(args.xbe.read_bytes(), db)

    gen = args.output / "gen"
    path = gen / "recomp_stubs_unresolved.c"
    header_path = gen / "recomp_funcs.h"
    dispatch_path = gen / "recomp_dispatch.c"
    header = header_path.read_text()
    original = path.read_text()

    recovery = EntryRecovery(translator, db, header)
    recovered, skipped = [], {}

    def replace(match):
        name, hex_address = match.group(1, 2)
        result = recovery.translate(int(hex_address, 16), name)
        if not result.startswith("/*"):
            skipped[hex_address] = result
            return match.group(0)
        recovered.append(hex_address)
        return result

    patched = STUB.sub(replace, original)

    for reason in sorted(set(skipped.values())):
        count = sum(1 for value in skipped.values() if value == reason)
        print(f"  skipped {count}: {reason}")
    print(f"Recoverable mid-function entries: {len(recovered)}"
          f" (plus {len(recovery.extra)} they reference)")

    arm_entries, arm_skipped = [], {}
    if args.arms:
        arm_entries, arm_skipped, arm_stats = recover_switch_arms(
            recovery, str(gen), str(args.xbe), patched, args.window)
        for reason in sorted(set(arm_skipped.values())):
            count = sum(1 for value in arm_skipped.values() if value == reason)
            print(f"  arm skipped {count}: {reason}")
        print("Switch arms with no entry point: %d in %d unresolved tables;"
              " recovered %d, skipped %d"
              % (arm_stats["arms_no_body"], arm_stats["unresolved_tables"],
                 arm_stats["arms_recovered"], arm_stats["arms_skipped"]))

    if args.dry_run or (patched == original and not arm_entries):
        return

    # THE ROW IS THE REPAIR. A body with no row in g_recomp_table is dead code
    # and the [ITAIL] failure repeats unchanged, so this is not an optional
    # extra step -- it is the only part of the arm phase the runtime can see.
    #
    # Computed BEFORE anything is written, because the three files only make
    # sense together. register_dispatch refuses a table it does not recognise
    # rather than sorting around it, and a refusal after the bodies had landed
    # would leave a tree that compiles, links, and still faults at 0x00075EB3 --
    # a half-applied repair being the one outcome worse than none.
    dispatch = dispatch_patched = None
    added = []
    if arm_entries:
        dispatch = dispatch_path.read_text()
        dispatch_patched, added = switch_arm_entries.register_dispatch(
            dispatch, [(va, name) for va, name, _ in arm_entries])

    extra = recovery.extra
    patched = with_extra_bodies(patched, extra)
    for name in sorted(extra):
        header += f"\nvoid {name}(void);\n"
    for _va, name, body in arm_entries:
        if body is None:
            continue          # already appended above, out of `extra`
        patched += "\n" + ARM_NOTE + "\n" + body.rstrip() + "\n"
        declaration = f"void {name}(void);"
        if declaration not in header:
            header += "\n" + declaration + "\n"
    if extra or arm_entries:
        header_backup = header_path.with_suffix(".h.before-midfunction")
        version = 1
        while header_backup.exists():
            header_backup = header_path.with_suffix(f".h.before-midfunction.{version}")
            version += 1
        header_backup.write_text(header_path.read_text())
        header_path.write_text(header)

    backup = path.with_suffix(".c.before-midfunction")
    version = 1
    while backup.exists():
        backup = path.with_suffix(f".c.before-midfunction.{version}")
        version += 1
    backup.write_text(original)
    path.write_text(patched)
    manifest = args.output / "midfunction_entries.json"
    previous = json.loads(manifest.read_text()) if manifest.exists() else []
    manifest.write_text(json.dumps(sorted(set(previous + recovered)), indent=2) + "\n")
    print(f"Patched {path}, backup {backup.name}")

    if arm_entries:
        if added:
            dispatch_backup = dispatch_path.with_suffix(".c.before-switch-arms")
            version = 1
            while dispatch_backup.exists():
                dispatch_backup = dispatch_path.with_suffix(
                    f".c.before-switch-arms.{version}")
                version += 1
            dispatch_backup.write_text(dispatch)
            dispatch_path.write_text(dispatch_patched)
        # The record of which addresses were recovered as arms, cumulative and
        # separate from midfunction_entries.json so the two stay tellable
        # apart. It is also the list that must NEVER reach --seed-functions:
        # every address in it is now reachable and will start being observed
        # RESOLVED by the icall feedback, and seeding one clamps the function
        # that dispatches to it at the dispatching jump -- the loop
        # regenerate.sh's RECOMP_SEED_INTERIOR comment describes, arrived at
        # from the new direction. Recovery adds an entry point; it does not
        # move a boundary, and nothing downstream should read it as one.
        arm_manifest = args.output / "switch_arm_entries.json"
        previous = json.loads(arm_manifest.read_text()) if arm_manifest.exists() else []
        arm_manifest.write_text(json.dumps(
            sorted(set(previous + ["0x%08X" % va for va, _, _ in arm_entries])),
            indent=2) + "\n")
        print(f"Registered {len(added)} switch arms in {dispatch_path}")


if __name__ == "__main__":
    main()
