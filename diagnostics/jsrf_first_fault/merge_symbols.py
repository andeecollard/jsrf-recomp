#!/usr/bin/env python3
"""Merge XbSymbolDatabase's XDK names into the shape symbolize.py already reads.

Two databases name disjoint halves of this title. KeybadeBlox' decompilation
names the GAME (CMGameGL, TextRenderer_MAYBE, CActMan); XbSymbolDatabase names
the XDK that the game was statically linked against (D3D8, DSOUND, XAPILIB),
which is why every address at 0x0019xxxx read as bare hex until now. Neither
covers the other's ground, so the merge is a union with the decompilation
winning any tie.

Output is symbolize.py's TSV shape, so `--symbols` takes it directly.
"""
import sys

DECOMP = '/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv'
XDK = '/Users/andrewcollard/jsrf-build/jsrf-xbsymbols.txt'
OUT = '/Users/andrewcollard/jsrf-build/jsrf-symbols-merged.tsv'


def main():
    rows = {}
    for line in open(XDK, errors='replace'):
        if '=' not in line:
            continue
        name, _, addr = line.partition('=')
        name, addr = name.strip(), addr.strip()
        if not addr.startswith('0x'):
            continue
        rows[int(addr, 16)] = '%s\tfunc\tundefined\t__cdecl\tnotinline\tnofixup\t%s\tnocomment' % (addr, name)
    xdk = len(rows)
    kept = 0
    for line in open(DECOMP, errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) > 6 and f[0].startswith('0x'):
            rows[int(f[0], 16)] = line.rstrip('\n')   # the game's own names win
            kept += 1
    with open(OUT, 'w') as out:
        for va in sorted(rows):
            out.write(rows[va] + '\n')
    print('%d XDK + %d decompilation -> %d rows in %s' % (xdk, kept, len(rows), OUT))


if __name__ == '__main__':
    main()
