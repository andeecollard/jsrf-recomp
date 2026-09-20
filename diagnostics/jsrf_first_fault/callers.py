#!/usr/bin/env python3
"""Who calls a given guest address? Scan .text for E8 rel32 landing on it.

Direct calls only -- a vtable dispatch will not show here -- but that is the
point: the question is which code emits vertices DIRECTLY into the push buffer.
"""
import bisect, struct, sys
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
    target = int(sys.argv[1], 16)
    img = load_image(XBE, _analysis())
    sym = names()
    starts = sorted(sym)
    hits = []
    for sec in img.sections:
        if not sec.name.startswith('.text'):
            continue
        data = img.get_section_data(sec)
        base = sec.virtual_addr
        for off in range(len(data) - 5):
            if data[off] != 0xE8:
                continue
            rel, = struct.unpack_from('<i', data, off + 1)
            if base + off + 5 + rel == target:
                hits.append(base + off)
    print('%d direct call site(s) to 0x%08X' % (len(hits), target))
    for va in hits:
        i = bisect.bisect_right(starts, va) - 1
        owner = sym[starts[i]] if i >= 0 else '?'
        print('   0x%08X  in %s +0x%X' % (va, owner, va - starts[i] if i >= 0 else 0))


if __name__ == '__main__':
    main()
