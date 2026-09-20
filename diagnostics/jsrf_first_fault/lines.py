#!/usr/bin/env python3
"""Turn a RECOMP_GLYPH_DUMP log back into the text the renderer submitted.

The cell->character map is the guest's own, recovered from the descriptor at
0x1F8B48 and checked against a line whose content is known: 84 cells per page on
a 12x7 grid, codes 0x21..0x75, and ONE code with no cell. Which one is fixed by
two observations in the same frame -- 'T' (0x54) sits at index 51, so the gap is
above it, and 'u' (0x75) sits at page 0's last cell 83, so the gap is at 0x60.
That makes the map exact for every printable character the game uses.

Quads are reported in SUBMISSION order and grouped by the draw that carried
them, because the order is the thing under test; the y column is what separates
two lines that one draw drew together.
"""
import sys
from collections import OrderedDict
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from glyphs import draws

GAP = 0x60          # the code with no cell


def char_of(page, row, col):
    index = page * 84 + row * 12 + col
    code = index + 0x21 if index + 0x21 < GAP else index + 0x22
    return chr(code) if 0x20 <= code < 0x7F else '?'


def main():
    path = sys.argv[1]
    lo = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    hi = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
    page_of = {}
    for d in draws(path):
        if not (lo <= d['t'] <= hi) or not d['q']:
            continue
        # Page identity is not in the draw: it is which of the two 256x256
        # atlases this address is, learned in first-seen order, which is page 0
        # first because page 0 carries almost every character.
        if d['tex'] not in page_of:
            page_of[d['tex']] = len(page_of)
        page = page_of[d['tex']]
        lines = OrderedDict()
        for q in d['q']:
            lines.setdefault(round(q['y0']), []).append(q)
        parts = []
        for y, qs in lines.items():
            text = ''.join(char_of(page, q['row'], q['col'])
                           for q in sorted(qs, key=lambda q: q['x0']))
            parts.append('y%-4d x%.0f..%.0f %r' % (
                y, min(q['x0'] for q in qs), max(q['x1'] for q in qs), text))
        print('t=%.2f page%d tex %s %2d quads | %s' % (
            d['t'], page, d['tex'], len(d['q']), '  ||  '.join(parts)))


if __name__ == '__main__':
    main()
