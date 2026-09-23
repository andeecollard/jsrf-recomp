#!/usr/bin/env python3
"""Score metal_hwtex_test dumps: software (off) arm against hardware (on) arm.

Per case, over a 256x256 R5G6B5 target: pixels whose worst channel differs by
more than one step, and the largest difference in steps. Gate, per case:
at most 1% of pixels beyond one step and none beyond --max-steps.
"""
import argparse, struct, sys

FORMATS = ['DXT1', 'DXT3', 'RGBA8']; MINS = [1, 2, 4, 6]; GEO = ['magnified', 'minified', 'projective']
ap = argparse.ArgumentParser()
ap.add_argument('off'); ap.add_argument('on')
ap.add_argument('--max-steps', type=int, default=4)
ap.add_argument('--max-frac', type=float, default=0.01)
a = ap.parse_args()
A, B = open(a.off, 'rb').read(), open(a.on, 'rb').read()
size = 256 * 256 * 2
if len(A) != len(B) or len(A) % size:
    sys.exit('dump sizes differ or are not whole targets: %d %d' % (len(A), len(B)))
names = [(f, m, r, g, b) for f in FORMATS for m in MINS for r in (0, 1) for g in GEO for b in (0, 1)]
fail = 0; worst_frac = 0.0; worst_step = 0
for i in range(len(A) // size):
    pa = struct.unpack('<%dH' % (size // 2), A[i*size:(i+1)*size])
    pb = struct.unpack('<%dH' % (size // 2), B[i*size:(i+1)*size])
    over = 0; mx = 0
    for x, y in zip(pa, pb):
        if x == y: continue
        d = max(abs((x >> 11) - (y >> 11)), abs(((x >> 5) & 63) - ((y >> 5) & 63)) // 2, abs((x & 31) - (y & 31)))
        if d > 1: over += 1
        mx = max(mx, d)
    frac = over / (size // 2); worst_frac = max(worst_frac, frac); worst_step = max(worst_step, mx)
    ok = frac <= a.max_frac and mx <= a.max_steps
    if not ok: fail += 1
    f, m, r, g, b = names[i] if i < len(names) else ('?',)*5
    if not ok or '-v' in sys.argv:
        print('%s %-5s min=%s %-6s %-10s %s  beyond-1-step=%.3f%%  max=%d' %
              ('FAIL' if not ok else 'ok  ', f, m, 'repeat' if r else 'clamp', g, 'blend' if b else '     ', 100*frac, mx))
print('cases=%d failed=%d worst beyond-1-step=%.3f%% worst max-step=%d' % (len(A)//size, fail, 100*worst_frac, worst_step))
sys.exit(1 if fail else 0)
