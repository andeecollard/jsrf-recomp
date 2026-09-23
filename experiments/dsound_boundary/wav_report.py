"""Per-second report on a 48 kHz stereo 16-bit WAV from the DSOUND lift
(RECOMP_DSOUND_LIFT_WAV) or the shadow (RECOMP_DSOUND_SHADOW_WAV).

For each second: RMS, peak, clipped samples, and DROPOUTS -- runs of exact
digital silence of 5 ms or more inside audio that is otherwise playing, which
is what a starved stream sounds like. A summary follows. Pure Python, no numpy.

    python3 experiments/dsound_boundary/wav_report.py <file.wav> [--every 5]
"""
import argparse, array, math, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument('wav')
ap.add_argument('--every', type=int, default=1, help='print every Nth second')
a = ap.parse_args()
f = open(a.wav, 'rb')
h = f.read(44)
if h[:4] != b'RIFF' or h[8:12] != b'WAVE':
    sys.exit('not a WAV')
ch, rate, bits = struct.unpack_from('<HIH', h, 22)[0], struct.unpack_from('<I', h, 24)[0], struct.unpack_from('<H', h, 34)[0]
if (ch, rate, bits) != (2, 48000, 16):
    sys.exit('expected 48 kHz stereo 16-bit, got %d ch %d Hz %d bit' % (ch, rate, bits))
tot = dict(seconds=0, silent_seconds=0, clipped=0, dropouts=0, peak=0)
DROP = 240   # frames: 5 ms
sec = 0
print('%5s %7s %6s %6s %5s' % ('t', 'rms', 'peak', 'clip', 'drop'))
while True:
    d = f.read(48000 * 4)
    if len(d) < 4:
        break
    s = array.array('h'); s.frombytes(d[:len(d) - len(d) % 4])
    n = len(s)
    rms = math.sqrt(sum(x * x for x in s) / n)
    peak = max(max(s), -min(s))
    clip = sum(1 for x in s if x >= 32767 or x <= -32768)
    drops, run = 0, 0
    if rms > 50:
        for i in range(0, n, 2):
            if s[i] == 0 and s[i + 1] == 0:
                run += 1
            else:
                if run >= DROP: drops += 1
                run = 0
    tot['seconds'] += 1; tot['clipped'] += clip; tot['dropouts'] += drops
    tot['peak'] = max(tot['peak'], peak); tot['silent_seconds'] += rms <= 50
    if sec % a.every == 0 or clip or drops:
        print('%5d %7.0f %6d %6d %5d%s' % (sec, rms, peak, clip, drops, '  <-- ' + ', '.join(
            w for w, c in (('clipping', clip), ('dropouts', drops)) if c) if clip or drops else ''))
    sec += 1
print('summary: %(seconds)d s, %(silent_seconds)d silent, peak %(peak)d, %(clipped)d clipped samples, '
      '%(dropouts)d dropouts (>=5 ms of digital silence inside playing audio)' % tot)
