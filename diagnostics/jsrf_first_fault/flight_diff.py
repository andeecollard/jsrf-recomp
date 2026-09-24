#!/usr/bin/env python3
"""Read a RECOMP_FLIGHT_FRAMES dump (flight-N/) and name what flickers.

For each pair of neighbouring frames: how much of the picture changed, and
which draws were present in one and missing in the other. A draw is keyed by
its state (textures, formats, combiner, control, blend, depth, primitive and
vertex count), not its draw number, which advances every frame. A flicker is a
key that comes and goes while the frames around it are otherwise steady.

    flight_diff.py <flight-dir> [--top N]
"""
import argparse, collections, glob, os, re, struct


def draws(path):
    out = []
    for line in open(path):
        if line.startswith('#'):
            continue
        f = line.split()
        if len(f) < 12:
            continue
        # key: prim count mode tex0 fmt0 tex1 fmt1 cw0 ctl blend zfunc
        out.append(tuple(f[1:12]))
    return out


def bmp_pixels(path):
    with open(path, 'rb') as fh:
        data = fh.read()
    off = struct.unpack_from('<I', data, 10)[0]
    return data[off:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    ds = sorted(glob.glob(os.path.join(a.dir, 'draws-*.txt')))
    lists = [collections.Counter(draws(p)) for p in ds]
    seen = collections.Counter()
    for c in lists:
        for k in c:
            seen[k] += 1
    n = len(lists)
    print('%d frames; %d distinct draw states' % (n, len(seen)))
    # States present in most frames but missing from a few: the flicker candidates.
    flick = [(k, v) for k, v in seen.items() if 0.2 * n <= v < n]
    flick.sort(key=lambda kv: -kv[1])
    print('\nstates present in some frames but not all (prim count mode tex0 fmt0 tex1 fmt1 cw0 ctl blend zfunc):')
    for k, v in flick[:a.top]:
        missing = [i for i, c in enumerate(lists) if k not in c]
        runs = []
        for i in missing:
            if runs and i == runs[-1][1] + 1:
                runs[-1][1] = i
            else:
                runs.append([i, i])
        print('  in %3d/%d frames, missing at %s   %s' % (
            v, n, ','.join('%d' % r[0] if r[0] == r[1] else '%d-%d' % tuple(r) for r in runs[:10]),
            ' '.join(k)))
    print('\nframe-to-frame picture change (fraction of bytes differing), largest first:')
    bmps = sorted(glob.glob(os.path.join(a.dir, 'frame-*.bmp')))
    prev, changes = None, []
    for i, p in enumerate(bmps):
        px = bmp_pixels(p)
        if prev is not None and len(prev) == len(px):
            diff = sum(1 for x, y in zip(prev[::7], px[::7]) if x != y) / max(1, len(px[::7]))
            gone = [k for k in lists[i - 1] if k not in lists[i]] if i < len(lists) else []
            came = [k for k in lists[i] if k not in lists[i - 1]] if i < len(lists) else []
            changes.append((diff, i, gone, came))
        prev = px
    for diff, i, gone, came in sorted(changes, reverse=True)[:a.top]:
        print('  %4d->%4d  %.3f  -%d +%d draws' % (i - 1, i, diff, len(gone), len(came)))
        for k in gone[:4]:
            print('        gone:', ' '.join(k))
        for k in came[:4]:
            print('        came:', ' '.join(k))


if __name__ == '__main__':
    main()
