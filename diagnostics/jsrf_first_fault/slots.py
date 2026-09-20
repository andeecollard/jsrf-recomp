#!/usr/bin/env python3
"""Name the function pointers in a vtable, given its address."""
import struct, sys
sys.path.insert(0, '/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration')
from pathlib import Path
from tools.disasm.loader import load_image

# Written by `python3 -m tools.xbe_parser <default.xbe> --json <this>`
# and kept beside this script, so these instruments outlive the session
# scratchpad they were written in.


XBE = '/Users/andrewcollard/Library/Application Support/JSRF/game/default.xbe'
TABLES = ['/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv',
          '/Users/andrewcollard/jsrf-build/jsrf-xbsymbols.txt']


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
    for path in TABLES:
        for line in open(path, errors='replace'):
            if '=' in line and '0x' in line:
                n, _, a = line.partition('=')
                try: out.setdefault(int(a.strip(), 16), n.strip())
                except ValueError: pass
            else:
                f = line.rstrip('\n').split('\t')
                if len(f) > 6 and f[0].startswith('0x') and f[6]:
                    out[int(f[0], 16)] = f[6]
    return out


def main():
    base = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    img = load_image(XBE, _analysis())
    sym = names()
    data = img.read_bytes_at_va(base, count * 4)
    if not data:
        print('nothing at 0x%08X' % base); return
    for i in range(count):
        fn, = struct.unpack_from('<I', data, i * 4)
        print('  +0x%03X -> 0x%08X  %s' % (i * 4, fn, sym.get(fn, '')))


if __name__ == '__main__':
    main()
