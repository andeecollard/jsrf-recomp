"""Decode a CRI ADX file (standard, unencrypted, type 3, 4-bit) to a WAV,
independently of the title -- the reference to compare the game's own decode
against (G46, the intro garble). Algorithm as vgmstream's coding_CRI_ADX.

    python3 experiments/dsound_boundary/adx_decode.py in.adx out.wav [--seconds N]
"""
import argparse, array, math, struct

ap = argparse.ArgumentParser()
ap.add_argument('adx'); ap.add_argument('wav')
ap.add_argument('--seconds', type=float, default=0)
ap.add_argument('--no-plus-one', action='store_true', help='scale without the +1')
a = ap.parse_args()
b = open(a.adx, 'rb').read()
magic, off, enc, bs, bits, ch, rate, tot, hp = struct.unpack('>HHBBBBIIH', b[:18])
assert magic == 0x8000 and enc == 3 and bits == 4, 'not a standard ADX'
data = off + 4
x = math.sqrt(2) - math.cos(2 * math.pi * hp / rate)
y = math.sqrt(2) - 1
z = (x - math.sqrt((x + y) * (x - y))) / y
c1 = int(math.floor(z * 8192)); c2 = int(math.floor(z * z * -4096))
spb = (bs - 2) * 2
n = tot if not a.seconds else min(tot, int(a.seconds * rate))
blocks = (n + spb - 1) // spb
out = array.array('h', bytes(2 * ch * blocks * spb))
hist = [[0, 0] for _ in range(ch)]
p = data
for blk in range(blocks):
    for c in range(ch):
        fr = b[p:p + bs]; p += bs
        if len(fr) < bs: break
        scale = ((fr[0] << 8) | fr[1]) + (0 if a.no_plus_one else 1)
        h1, h2 = hist[c]
        base = blk * spb
        for i in range(spb):
            nib = (fr[2 + i // 2] >> (4 if i % 2 == 0 else 0)) & 0xF
            if nib >= 8: nib -= 16
            s = nib * scale + ((c1 * h1 + c2 * h2) >> 12)
            s = 32767 if s > 32767 else -32768 if s < -32768 else s
            out[(base + i) * ch + c] = s
            h2, h1 = h1, s
        hist[c] = [h1, h2]
out = out[:n * ch]
with open(a.wav, 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + len(out) * 2) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, ch, rate, rate * ch * 2, ch * 2, 16) + b'data' + struct.pack('<I', len(out) * 2))
    f.write(out.tobytes())
print('%s: %d ch %d Hz, %.1f s decoded, coef %d %d' % (a.adx, ch, rate, n / rate, c1, c2))
