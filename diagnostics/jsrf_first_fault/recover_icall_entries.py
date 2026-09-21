"""Translate indirect-call targets the function detector never found.

A function whose address is only ever taken as a data immediate has no call
site to detect it by, so it can fall in the gap between two detected functions
and never be translated. An indirect call to it then fails to resolve at
runtime -- RECOMP_ICALL prints "Failed to resolve VA", the call does nothing,
and whatever the guest expected it to do silently does not happen.

That is not hypothetical here. 0x0013D840 is a tail-call thunk for the hook
installed at 0x00261588; the disassembler even recorded the xref that reaches
it ("XREF: 0x0013E42A (data_imm)") without promoting it to an entry. It failed
to resolve three times per run, each during the title BGM open.

This is the sibling of recover_startup_entries.py: same registration, but for
an address that is unowned rather than inside an existing function. The extent
is decoded rather than declared, so a mistaken address cannot register a body
that runs off into whatever follows it.

icall_entries.json also feeds a full regeneration through --seed-functions.

THE ROWS WERE INVISIBLE, fixed 21 Sep 2026. Registration here appended rows to
g_recomp_table and never touched the `g_recomp_table_size` literal beside it --
and that literal, not the array's real length, is what recomp_lookup's binary
search and recomp_dispatch_init's flat-table build both loop to. Every row this
script added past the declared count was therefore dead: the body existed, the
row existed, the address still failed to resolve, and nothing said so. Nothing
has paid for it because regenerate.sh does not run this script, which is
exactly the state a latent bug is in before it costs a day.

The same array has four invariants -- sorted, counted, spanned, unique --
and switch_arm_entries.register_dispatch keeps all four in one tested place.
Use it rather than restating any of them here.
"""
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(HERE))
from tools.recomp.config import configure_from_xbe
from tools.recomp.translator import FunctionTranslator
import switch_arm_entries

TERMINATORS = ("ret", "retn", "jmp", "int3", "ud2", "hlt")


def extent(translator, start, ceiling):
    """Decode from start to its terminator, never past the next function.

    A conditional branch forward keeps the body open: 0x0013D840's `je` skips
    over its own tail jump to the `ret` beyond it, so stopping at the first
    terminator would truncate the function and drop the return.
    """
    window = translator._read_func_bytes(start, ceiling)
    decoded = translator.disasm.disassemble_function(window, start, ceiling)
    assert decoded, f"nothing decoded at 0x{start:08X}"
    furthest = start
    for insn in decoded:
        target = insn.jump_target
        if target is not None and start <= target < ceiling:
            furthest = max(furthest, target)
        end = insn.address + insn.size
        if insn.mnemonic in TERMINATORS and insn.address >= furthest:
            return end
    raise AssertionError(f"0x{start:08X} has no terminator before 0x{ceiling:08X}")


def main():
    output = ROOT / "build-macos/jsrf-first-fault"
    xbe = ROOT.parent / "Jet Set Radio Future (US)/default.xbe"
    configure_from_xbe(str(xbe))
    raw = xbe.read_bytes()
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16))
          for f in json.loads((output / "disasm/functions.json").read_text())}
    translator = FunctionTranslator(raw, db)
    entries = json.loads(Path(__file__).with_name("icall_entries.json").read_text())

    dispatch_path = output / "gen/recomp_dispatch.c"
    header_path = output / "gen/recomp_funcs.h"
    bodies_path = output / "gen/recomp_stubs_unresolved.c"
    dispatch, header, bodies = [p.read_text()
                                for p in (dispatch_path, header_path, bodies_path)]
    generated = []
    for entry in entries:
        start = int(entry["start"], 16)
        name = f"sub_{start:08X}"
        # Unowned is the whole point: an address inside a detected function is
        # a mid-function entry and belongs to recover_startup_entries.py.
        owner = [a for a, f in db.items() if a <= start < f["end"]]
        assert not owner, f"0x{start:08X} is inside {owner}"
        following = sorted(a for a in db if a > start)
        assert following, entry
        end = extent(translator, start, following[0])
        if re.search(rf"\{{ 0x{start:08X}u,", dispatch):
            continue
        code = translator.translate_function(start, {"name": name, "end": end})
        assert code and "TODO:" not in code and "UNIMPLEMENTED" not in code
        generated.append((start, end, name, code))

    if not generated:
        print("Indirect-call entries already registered")
        return
    # Everything the new bodies call must already exist; never invent a stub.
    for name in translator.lifter.referenced_calls.values():
        assert re.search(rf"\bvoid {re.escape(name)}\(void\)", header), name

    for start, end, name, code in generated:
        declaration = f"void {name}(void);"
        if declaration not in header:
            header += "\n" + declaration + "\n"
        bodies += ("\n/* Indirect-call target the detector never found; "
                   f"0x{start:08X}-0x{end:08X}. */\n" + code)
    # Sorted, counted, spanned, unique -- see the note at the top of the file.
    # The count is the one that used to be missed, and it is the one that
    # decides whether any of this is reachable at all.
    dispatch, added = switch_arm_entries.register_dispatch(
        dispatch, [(start, name) for start, _end, name, _code in generated])
    assert len(added) == len(generated), (
        "rows refused by register_dispatch: %r" % (
            sorted({n for _s, _e, n, _c in generated} - {n for _v, n in added}),))

    for path in (dispatch_path, header_path, bodies_path):
        backup = path.with_suffix(path.suffix + ".before-icall-entries")
        if not backup.exists():
            backup.write_text(path.read_text())
    bodies_path.write_text(bodies)
    header_path.write_text(header)
    dispatch_path.write_text(dispatch)
    print("Registered indirect-call entries: "
          + ", ".join(f"{a:08X}-{e:08X}" for a, e, _, _ in generated))


if __name__ == "__main__":
    main()
