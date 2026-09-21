#!/usr/bin/env python3
"""Merge XbSymbolDatabase's XDK names into the shape symbolize.py already reads.

Two databases name disjoint halves of this title. KeybadeBlox' decompilation
names the GAME (CMGameGL, TextRenderer_MAYBE, CActMan); XbSymbolDatabase names
the XDK that the game was statically linked against (D3D8, DSOUND, XAPILIB),
which is why every address at 0x0019xxxx read as bare hex until now. Neither
covers the other's ground, so the merge is a union with the decompilation
winning any tie.

OUT is symbolize.py's TSV shape, so `--symbols` takes it directly.

WHAT THIS SCRIPT USED TO THROW AWAY, AND WHY THERE ARE NOW TWO OUTPUTS
---------------------------------------------------------------------
The decompilation's table has two kinds of row. `func` rows carry a calling
convention, an inline flag and a parameter list, so they are eight fields or
more. `data` rows carry a type and nothing else, so they are exactly five.
The keep test used to be `len(f) > 6`, a field-count proxy for "is this a
function" -- and it dropped all 405 `data` rows without printing anything.
405 rows is 23% of the table.

They are not merged into OUT, because symbolize.py rejects any row whose
second field is not `func` (symbolize.py:55): putting them there would grow
the file and change nothing. They go to DATA_OUT instead, verbatim in the
decompilation's own five-field shape, for the consumers that want data
extents rather than function names -- see
docs/jsrf/plans/PLAN_2026-09-21_JUMP_TABLE_GROUND_TRUTH.md for what the
jump-table subset is and is not worth.

Every row of the decompilation's table is now counted and reported by kind,
including kinds this script has never seen, so the next silent drop is a line
of output rather than a discovery three sessions later.

LICENCE: the decompilation publishes no licence file and its readme disallows
LLM use in that repository. Both outputs live in ~/jsrf-build/, OUTSIDE this
repo, and nothing derived from it is ever committed here.
"""
import collections
import sys

DECOMP = '/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv'
XDK = '/Users/andrewcollard/jsrf-build/jsrf-xbsymbols.txt'
OUT = '/Users/andrewcollard/jsrf-build/jsrf-symbols-merged.tsv'
DATA_OUT = '/Users/andrewcollard/jsrf-build/jsrf-symbols-data.tsv'


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

    seen = collections.Counter()      # every decomp row, by kind
    kept = collections.Counter()      # what each output actually received
    data_rows = {}
    skipped = []                      # rows routed nowhere, with a reason
    for line in open(DECOMP, errors='replace'):
        line = line.rstrip('\n')
        if not line.strip():
            continue
        f = line.split('\t')
        kind = f[1] if len(f) > 1 else '(no kind field)'
        seen[kind] += 1
        if not f[0].startswith('0x'):
            skipped.append((kind, len(f), 'first field is not an address', line[:60]))
            continue
        va = int(f[0], 16)
        # The merged file keeps EXACTLY the rows it kept before: symbolize.py
        # reads seven fields and a `func` in the second, so this predicate is
        # left alone on purpose.
        if len(f) > 6:
            rows[va] = line               # the game's own names win
            kept['%s -> merged' % kind] += 1
        elif kind == 'data':
            data_rows[va] = line
            kept['data -> sidecar'] += 1
        else:
            skipped.append((kind, len(f), 'kind is not data and row is too '
                            'short for symbolize.py', line[:60]))

    with open(OUT, 'w') as out:
        for va in sorted(rows):
            out.write(rows[va] + '\n')
    with open(DATA_OUT, 'w') as out:
        for va in sorted(data_rows):
            out.write(data_rows[va] + '\n')

    total = sum(seen.values())
    print('%s: %d rows' % (DECOMP, total))
    for kind, n in sorted(seen.items(), key=lambda kv: -kv[1]):
        print('    %-20s seen %5d' % (kind, n))
    for dest, n in sorted(kept.items()):
        print('    %-20s kept %5d' % (dest, n))
    print('    %-20s      %5d' % ('DROPPED', len(skipped)))
    if skipped:
        print('  DROPPED ROWS -- nothing consumes these. Route them or widen a'
              ' filter:')
        shown = collections.Counter((k, r) for k, _, r, _ in skipped)
        for (kind, reason), n in shown.most_common():
            print('    %5d  kind=%-10s %s' % (n, kind, reason))
        for kind, nf, reason, text in skipped[:3]:
            print('      e.g. (%d fields) %s' % (nf, text))
    accounted = sum(kept.values()) + len(skipped)
    if accounted != total:
        sys.exit('row accounting is wrong: %d of %d rows went somewhere'
                 % (accounted, total))

    print('%d XDK + %d decompilation func -> %d rows in %s'
          % (xdk, kept.get('func -> merged', 0), len(rows), OUT))
    print('%d decompilation data rows -> %s' % (len(data_rows), DATA_OUT))


if __name__ == '__main__':
    main()
