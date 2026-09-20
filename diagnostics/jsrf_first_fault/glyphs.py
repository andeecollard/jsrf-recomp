#!/usr/bin/env python3
"""Reconstruct what the renderer actually drew, from a RECOMP_GLYPH_DUMP log.

One [GLYPH] draw header plus its q-lines is one submitted batch: a set of
character quads sampling ONE font page. The question G2 asks is how a line of
text is split across those batches and in what order they are submitted, so the
grouping here is by SUBMISSION, never by screen position -- sorting the quads of
a line back into reading order is what would hide the fault.
"""
import re
import sys
from collections import defaultdict

DRAW = re.compile(r'\[GLYPH\] draw: t=([\d.]+)s (\d+) quads, tex0 ([0-9A-F]+) '
                  r'(\d+)x(\d+)')
QUAD = re.compile(r'\[GLYPH\]  q(\d+) screen x\s*([-\d.]+)\.\.\s*([-\d.]+) '
                  r'y\s*([-\d.]+)\.\.\s*([-\d.]+)\s+uv \S+\s+texel\s*'
                  r'([-\d.]+),\s*([-\d.]+)\.\.\s*([-\d.]+),\s*([-\d.]+)')

CELL_W, CELL_H, COLS = 21.0, 34.0, 12


def cell_of(u0, v0):
    return int(round(v0 / CELL_H)), int(round(u0 / CELL_W))


def draws(path):
    batch = None
    for line in open(path, errors='replace'):
        m = DRAW.search(line)
        if m:
            if batch:
                yield batch
            batch = {'t': float(m.group(1)), 'quads': int(m.group(2)),
                     'tex': m.group(3), 'w': int(m.group(4)), 'q': []}
            continue
        m = QUAD.search(line)
        if m and batch is not None:
            x0, x1, y0, y1 = (float(m.group(i)) for i in range(2, 6))
            u0, v0, u1, v1 = (float(m.group(i)) for i in range(6, 10))
            row, col = cell_of(u0, v0)
            batch['q'].append({'i': int(m.group(1)), 'x0': x0, 'x1': x1,
                               'y0': y0, 'y1': y1, 'u0': u0, 'v0': v0,
                               'u1': u1, 'v1': v1, 'row': row, 'col': col})
    if batch:
        yield batch


def main():
    path = sys.argv[1]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    pages = defaultdict(int)
    for d in draws(path):
        pages[d['tex']] += len(d['q'])
        if not (lo <= d['t'] <= hi) or not d['q']:
            continue
        ys = sorted({round(q['y0']) for q in d['q']})
        print('t=%.2f tex0 %s %2d quads  y=%s' % (
            d['t'], d['tex'], len(d['q']),
            ','.join(str(y) for y in ys[:6])))
        for q in d['q']:
            print('   q%02d x %7.1f..%7.1f y %7.1f  cell r%d c%-2d  '
                  'texel %5.1f,%5.1f..%5.1f,%5.1f' % (
                      q['i'], q['x0'], q['x1'], q['y0'], q['row'], q['col'],
                      q['u0'], q['v0'], q['u1'], q['v1']))
    print('# page totals:', dict(pages), file=sys.stderr)


if __name__ == '__main__':
    main()
