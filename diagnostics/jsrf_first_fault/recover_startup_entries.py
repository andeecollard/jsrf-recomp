"""Translate verified missing startup switch entries without full regeneration.

The same entry addresses feed regeneration through startup_entries.json.
Existing generated function bodies, timing patches and probes are preserved.
"""
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp.config import configure_from_xbe, va_to_file_offset
from tools.recomp.translator import FunctionTranslator


def main():
    output = ROOT / "build-macos/jsrf-first-fault"
    xbe = ROOT.parent / "Jet Set Radio Future (US)/default.xbe"
    configure_from_xbe(str(xbe))
    raw = xbe.read_bytes()
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16))
          for f in json.loads((output / "disasm/functions.json").read_text())}
    translator = FunctionTranslator(raw, db)
    entries = json.loads((Path(__file__).with_name("startup_entries.json")).read_text())
    dispatch_path = output / "gen/recomp_dispatch.c"
    header_path = output / "gen/recomp_funcs.h"
    bodies_path = output / "gen/recomp_stubs_unresolved.c"
    dispatch, header, bodies = [p.read_text() for p in (dispatch_path, header_path, bodies_path)]
    generated = []
    for entry in entries:
        start, end = int(entry["start"], 16), int(entry["end"], 16)
        name = f"sub_{start:08X}"
        slot = int(entry["table"], 16) + 4 * entry["index"]
        assert struct.unpack_from("<I", raw, va_to_file_offset(slot))[0] == start, entry
        owner_start, owner = max(((a, f) for a, f in db.items() if a <= start < f["end"]), key=lambda item: item[0])
        assert end <= owner["end"], entry
        decoded = translator.disasm.disassemble_function(translator._read_func_bytes(owner_start, owner["end"]), owner_start, owner["end"])
        assert start in {insn.address for insn in decoded}, entry
        if re.search(rf"\{{ 0x{start:08X}u,", dispatch):
            continue
        code = translator.translate_function(start, {"name": name, "end": end})
        assert code and "TODO:" not in code and "UNIMPLEMENTED" not in code
        generated.append((start, name, code))
    if not generated:
        print("Verified startup entries already registered")
        return
    # All called routines must already exist; this path never invents stubs.
    for name in translator.lifter.referenced_calls.values():
        assert re.search(rf"\bvoid {re.escape(name)}\(void\)", header), name
    match = re.search(r"(static const recomp_entry_t g_recomp_table\[\] = \{\n)(.*?)(\n\};)", dispatch, re.S)
    assert match
    rows = match.group(2).splitlines()
    for address, name, code in generated:
        rows.append(f"    {{ 0x{address:08X}u, (recomp_func_t){name} }},")
        declaration = f"void {name}(void);"
        if declaration not in header:
            header += "\n" + declaration + "\n"
        bodies += "\n/* Verified startup switch entry. */\n" + code
    rows.sort(key=lambda line: int(re.search(r"0x([0-9A-F]+)u", line).group(1), 16))
    dispatch = dispatch[:match.start(2)] + "\n".join(rows) + dispatch[match.end(2):]
    # Retain every pre-recovery file for inspection/reversal.
    for path in (dispatch_path, header_path, bodies_path):
        backup = path.with_suffix(path.suffix + ".before-startup-entries")
        assert not backup.exists(), backup
        backup.write_text(path.read_text())
    bodies_path.write_text(bodies)
    header_path.write_text(header)
    dispatch_path.write_text(dispatch)
    print("Registered startup entries: " + ", ".join(f"{a:08X}" for a, _, _ in generated))


if __name__ == "__main__":
    main()
