"""Translate placeholder stubs that are mid-function entry points.

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
"""
import argparse
import bisect
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp.config import configure_from_xbe
from tools.recomp.translator import FunctionTranslator

STUB = re.compile(
    r"^void (sub_([0-9A-F]{8}))\(void\) \{ g_esp \+= \d+; /\* 0x[0-9A-F]{8}: [^\n]* \*/ \}$",
    re.MULTILINE)


def dangling_labels(code):
    """Labels the body jumps to but never defines."""
    defined = set(re.findall(r"^(loc_[0-9A-F]{8}): ;", code, re.MULTILINE))
    used = set(re.findall(r"goto (loc_[0-9A-F]{8});", code))
    return sorted(used - defined)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path,
                        default=ROOT.parent / "Jet Set Radio Future (US)/default.xbe")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-macos/jsrf-first-fault")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    configure_from_xbe(str(args.xbe))
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16))
          for f in json.loads((args.output / "disasm/functions.json").read_text())}
    starts = sorted(db)
    translator = FunctionTranslator(args.xbe.read_bytes(), db)

    path = args.output / "gen/recomp_stubs_unresolved.c"
    header = (args.output / "gen/recomp_funcs.h").read_text()
    original = path.read_text()

    recovered, skipped, extra = [], {}, {}

    def bounds_of(address):
        """(owner start or None, end address) for code at this address."""
        index = bisect.bisect_right(starts, address) - 1
        if index >= 0 and starts[index] < address < db[starts[index]]["end"]:
            return starts[index], db[starts[index]]["end"]
        following = bisect.bisect_right(starts, address)
        if following >= len(starts):
            return None, None
        return None, starts[following]

    def translate(address, name):
        """Body for this entry, or a string explaining why there is none."""
        start, end = bounds_of(address)
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
            if re.search(rf"\bvoid {re.escape(callee)}\(void\);", header):
                continue
            if callee in extra:
                continue
            callee_address = int(callee[len("sub_"):], 16)
            extra[callee] = None          # claim it before recursing
            body = translate(callee_address, callee)
            if not body.startswith("/*"):
                del extra[callee]
                return f"calls {callee}, which cannot be recovered ({body})"
            extra[callee] = body
        return ("/* Recovered entry at 0x%08X, translated through 0x%08X. */\n%s"
                % (address, end, code.rstrip()))

    def replace(match):
        name, hex_address = match.group(1, 2)
        result = translate(int(hex_address, 16), name)
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
          f" (plus {len(extra)} they reference)")

    if args.dry_run or patched == original:
        return

    header_path = args.output / "gen/recomp_funcs.h"
    for name in sorted(extra):
        patched += ("\n/* Reached by a backward jump from a recovered entry. */\n"
                    + extra[name].rstrip() + "\n")
        header += f"\nvoid {name}(void);\n"
    if extra:
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


if __name__ == "__main__":
    main()
