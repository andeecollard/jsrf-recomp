#!/usr/bin/env python3
"""Find a COM vtable by its QueryInterface/AddRef/Release triple, then name its
slots from the decompilation's symbol table.

The text path calls its renderer through [0x251d6c] at +0x24, +0x124, +0x128
and +0x144, and a vtable slot is the only way to turn those numbers into
functions. QI/AddRef/Release are the first three entries of any COM vtable and
all three are named, so the triple locates the table without guessing.
"""
import struct
import sys
sys.path.insert(0, '/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration')
from pathlib import Path
from tools.disasm.loader import load_image

# Written by `python3 -m tools.xbe_parser <default.xbe> --json <this>`
# and kept beside this script, so these instruments outlive the session
# scratchpad they were written in.


XBE = '/Users/andrewcollard/Library/Application Support/JSRF/game/default.xbe'
TSV = '/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv'


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


def names():
    out = {}
    for line in open(TSV, errors='replace'):
        f = line.split('\t')
        if len(f) > 6 and f[0].startswith('0x'):
            out[int(f[0], 16)] = f[6]
    return out


def main():
    want = [int(a, 16) for a in sys.argv[1:4]]      # QI, AddRef, Release
    slots = [int(a, 16) for a in sys.argv[4:]]
    img = load_image(XBE, _analysis())
    sym = names()
    for sec in img.sections:
        data = img.get_section_data(sec)
        for off in range(0, len(data) - 12, 4):
            three = struct.unpack_from('<3I', data, off)
            if sorted(three) == sorted(want):
                base = sec.virtual_addr + off
                print('vtable at 0x%08X in %s  (first three: %s)' % (
                    base, sec.name,
                    ', '.join(sym.get(v, '0x%08X' % v) for v in three)))
                for s in slots:
                    if off + s + 4 > len(data):
                        continue
                    fn = struct.unpack_from('<I', data, off + s)[0]
                    print('   +0x%03X -> 0x%08X  %s' % (
                        s, fn, sym.get(fn, '(unnamed)')))
                return
    print('no vtable found with that triple')


if __name__ == '__main__':
    main()
