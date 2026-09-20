#!/usr/bin/env python3
"""Count text draws whose opening quads are the NEXT draw's batch.

JSRF draws a line needing both Latin pages in two passes out of one sprite
buffer. If the page-0 pass is rasterised after the guest has refilled that
buffer for the page-1 pass, the page-0 draw opens with the page-1 quads --
same screen position, same cell, one page too early. That is checkable without
a picture: compare the head of each page-0 batch against the whole of the
page-1 batch that follows it, by position AND cell, and count the matches.

A clean run scores 0. Nothing else in a text draw produces that coincidence.
"""
import sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from glyphs import draws


def key(q):
    return (round(q['x0'], 1), round(q['y0'], 1), q['row'], q['col'])


def main():
    seq = list(draws(sys.argv[1]))
    pages = {}
    for d in seq:
        pages.setdefault(d['tex'], 0)
        pages[d['tex']] += len(d['q'])
    order = sorted(pages, key=lambda t: -pages[t])
    p0 = order[0] if order else None
    p1 = order[1] if len(order) > 1 else None
    # The denominator that matters is not every text draw: it is every draw
    # that had a page-1 pass behind it to be clobbered BY. A replay that meets
    # fewer two-page lines scores fewer clobbers for a reason that is not the
    # fix, so the opportunities are counted and printed beside the failures.
    clobbered, heads, total, chances = 0, 0, 0, 0
    for i, d in enumerate(seq):
        if d['tex'] != p0:
            continue
        total += 1
        nxt = next((e for e in seq[i + 1:i + 3] if e['tex'] == p1), None)
        if not nxt or not nxt['q']:
            continue
        chances += 1
        n = len(nxt['q'])
        if [key(q) for q in d['q'][:n]] == [key(q) for q in nxt['q']]:
            clobbered += 1
            heads += n
    print('page 0 %s: %d quads over %d draws' % (p0, pages.get(p0, 0), total))
    print('page 1 %s: %d quads' % (p1, pages.get(p1, 0)))
    print('opportunities (a page-1 pass followed the draw): %d' % chances)
    print('CLOBBERED page-0 draws: %d of %d opportunities (%.2f%%),'
          ' %d glyphs lost'
          % (clobbered, chances, 100.0 * clobbered / chances if chances else 0,
             heads))


if __name__ == '__main__':
    main()
