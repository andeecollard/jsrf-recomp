"""Compare a lift WAV (48 kHz stereo) against a reference decode (any rate):
align by cross-correlation, then report per-second correlation, gain and the
lag drift -- which says whether the game's audio is the file's audio, and if
not, whether it is late, early, skipping or corrupted.

    python3 compare_to_reference.py ours.wav ref.wav [--from 15] [--seconds 40]
"""
import argparse, struct, numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('ours'); ap.add_argument('ref')
ap.add_argument('--from', dest='t0', type=float, default=15)
ap.add_argument('--seconds', type=float, default=40)
a = ap.parse_args()

def load(p):
    b = open(p, 'rb').read()
    ch, rate = struct.unpack_from('<HI', b, 22)
    x = np.frombuffer(b[44:], dtype='<i2').astype(np.float64)
    x = x[:len(x) // ch * ch].reshape(-1, ch)
    return x, rate
o, orate = load(a.ours); r, rrate = load(a.ref)
# reference to the lift's rate, linear
t = np.arange(int(len(r) * orate / rrate)) * rrate / orate
r48 = np.stack([np.interp(t, np.arange(len(r)), r[:, c]) for c in range(r.shape[1])], 1)
R = orate
oo = o[int(a.t0 * R):int((a.t0 + a.seconds) * R), 0]
rr = r48[:, 0]
# coarse alignment on the first 3 s of reference music against ours
snip = rr[int(1 * R):int(4 * R)]
def best_lag(sig, snip, lo, hi, step):
    best = (-2, 0)
    for L in range(lo, hi, step):
        seg = sig[L:L + len(snip)]
        if len(seg) < len(snip): break
        c = np.corrcoef(seg, snip)[0, 1]
        if c > best[0]: best = (c, L)
    return best
c, L = best_lag(oo, snip[::8], 0, len(oo) - len(snip), 8 * 48) if False else (None, None)
# do it on 8x-decimated signals, then refine
od, sd = oo[::8], snip[::8]
c, Ld = best_lag(od, sd, 0, len(od) - len(sd), 1)
c, L = best_lag(oo, snip, max(0, Ld * 8 - 64), Ld * 8 + 64, 1)
start_ours = L - int(1 * R)       # ours index of reference t=0
print('alignment: reference t=0 is ours t=%.3f s (corr %.3f on the 3 s snippet)' % (a.t0 + start_ours / R, c))
print('%6s %6s %8s %8s %s' % ('ref s', 'corr', 'gain', 'lag ms', ''))
for s in range(0, int(a.seconds) - 2):
    i0 = start_ours + s * R
    if i0 < 0 or i0 + R > len(oo): continue
    ref = rr[s * R:(s + 1) * R]
    # local lag search +-60 ms: a skip or stall shows as the lag moving
    best = (-2, 0)
    for d in range(-int(0.06 * R), int(0.06 * R) + 1, 24):
        seg = oo[i0 + d:i0 + d + R]
        if len(seg) < R or i0 + d < 0: continue
        cc = np.corrcoef(seg, ref)[0, 1]
        if cc > best[0]: best = (cc, d)
    seg = oo[i0 + best[1]:i0 + best[1] + R]
    g = np.std(seg) / (np.std(ref) + 1e-9)
    flag = '' if best[0] > 0.95 else ('  <-- DIFFERENT' if best[0] < 0.7 else '  <-- degraded')
    print('%6d %6.3f %8.3f %8.1f%s' % (s, best[0], g, best[1] * 1000 / R, flag))
