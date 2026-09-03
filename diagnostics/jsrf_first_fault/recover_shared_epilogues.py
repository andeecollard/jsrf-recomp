"""Apply the recompiler's shared-return recovery to existing generated stubs.

This preserves the diagnostic checkout's timing patches and instrumentation.
A full tools.recomp run performs the same recovery before emitting C/dispatch.
"""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp.config import configure_from_xbe
from tools.recomp.shared_epilogue import recover_shared_epilogue
from tools.recomp.translator import FunctionTranslator


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, default=ROOT.parent / "Jet Set Radio Future (US)/default.xbe")
    parser.add_argument("--output", type=Path, default=ROOT / "build-macos/jsrf-first-fault")
    args = parser.parse_args()
    configure_from_xbe(str(args.xbe))
    db = {int(f["start"], 16): dict(f, end=int(f["end"], 16))
          for f in json.loads((args.output / "disasm/functions.json").read_text())}
    translator = FunctionTranslator(args.xbe.read_bytes(), db)
    path = args.output / "gen/recomp_stubs_unresolved.c"
    original = path.read_text()
    recovered = []

    def replace(match):
        name, hex_address = match.group(1, 2)
        code = recover_shared_epilogue(translator, int(hex_address, 16), name)
        if code:
            recovered.append(hex_address)
            return code
        return match.group(0)

    patched = re.sub(r"^void (sub_([0-9A-F]{8}))\(void\) \{ g_esp \+= \d+; /\* 0x[0-9A-F]{8}: [^\n]* \*/ \}$",
                     replace, original, flags=re.MULTILINE)
    if patched != original:
        backup = path.with_suffix(".c.before-shared-epilogues")
        version = 1
        while backup.exists():
            backup = path.with_suffix(f".c.before-shared-epilogues.{version}")
            version += 1
        backup.write_text(original)
        path.write_text(patched)
        manifest = args.output / "shared_epilogues.json"
        previous = json.loads(manifest.read_text()) if manifest.exists() else []
        manifest.write_text(json.dumps(sorted(set(previous + recovered)), indent=2) + "\n")
    print(f"Recovered {len(recovered)} shared epilogues in {path}")


if __name__ == "__main__":
    main()
