#!/usr/bin/env python3
"""Disassemble one guest function out of the XBE, by VA.

Small on purpose: a full tools.disasm pass is minutes of CPU, and an audio
sweep that is scoring dropouts at 50 ms cannot have minutes of CPU taken from
under it. This reads the bytes for one address range and stops.
"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
sys.path.insert(0, '/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration')
from pathlib import Path
from tools.disasm.loader import load_image

# Written by `python3 -m tools.xbe_parser <default.xbe> --json <this>`
# and kept beside this script, so these instruments outlive the session
# scratchpad they were written in.


XBE = '/Users/andrewcollard/Library/Application Support/JSRF/game/default.xbe'


def _analysis():
    """The XBE analysis JSON, generated on demand.

    Not committed: it is derived from the game binary, which this repository
    deliberately does not vendor. Regenerating costs a second and means these
    instruments work in a fresh clone without a manual step.
    """
    out = Path(__file__).with_name('default_analysis.json')
    if not out.exists():
        import subprocess
        subprocess.run([sys.executable, '-m', 'tools.xbe_parser', XBE,
                        '--json', str(out)],
                       cwd=str(Path(__file__).resolve().parents[2]),
                       check=True, stdout=subprocess.DEVNULL)
    return str(out)


def main():
    va = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 320
    img = load_image(XBE, _analysis())
    data = img.read_bytes_at_va(va, n)
    if not data:
        print('no bytes at %08X' % va)
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False
    for i in md.disasm(data, va):
        print('%08X  %-22s %s' % (i.address, i.mnemonic, i.op_str))


if __name__ == '__main__':
    main()
