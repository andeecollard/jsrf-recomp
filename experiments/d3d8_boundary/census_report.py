"""G33: read the two census arms and score the gates.

Gate 1: the off arm behaves like the shipped build (tutorial reached, 0 faults);
        the on arm reaches the same scene.
Gate 2: per-frame game calls for every entry point that fired.
Gate 3: every guest function that stored into the D3D ring lies in the D3D section.
"""
import argparse, json, re

D3D_LO, D3D_HI = 0x0018CB40, 0x0019A040
ap = argparse.ArgumentParser()
ap.add_argument('--off', required=True)
ap.add_argument('--on', required=True)
ap.add_argument('--entries', required=True)
a = ap.parse_args()

def scene_and_faults(path):
    text = open(path, errors='replace').read()
    live = re.findall(r'\[JSRF-SCENE\] root=.*?live=(\d+)', text)
    frames = re.findall(r'\[FRAME\].*', text)
    return (max(map(int, live)) if live else None, text.count('FIRST GUEST FAULT'),
            frames[-1] if frames else '(no [FRAME] line)', text)

ok = True
lo, fo, fro, _ = scene_and_faults(a.off)
ln, fn, frn, on_text = scene_and_faults(a.on)
print('gate 1  off arm: max live=%s faults=%d\n        %s' % (lo, fo, fro))
print('        on  arm: max live=%s faults=%d\n        %s' % (ln, fn, frn))
g1 = lo is not None and lo >= 61 and fo == 0 and ln is not None and ln >= 61 and fn == 0
print('gate 1 %s (both arms must reach live>=61 with 0 guest faults)' % ('PASS' if g1 else 'FAIL'))
ok &= g1

# last complete census block (exit if present, else last periodic)
blocks = re.split(r'(?=\[D3D8-CENSUS\] (?:periodic|exit) frames=)', on_text)
blocks = [b for b in blocks if b.startswith('[D3D8-CENSUS] ')]
if not blocks:
    print('gate 2 FAIL: no [D3D8-CENSUS] report in the on arm'); ok = False
else:
    last = blocks[-1]
    frames = int(re.search(r'frames=(\d+)', last).group(1))
    rows = re.findall(r'\[D3D8-CENSUS\] 0x([0-9A-F]{8}) (\S+)\s+game=(\d+) internal=(\d+)', last)
    print('gate 2 PASS: %d entry points fired over %d swaps (%s report)' %
          (len(rows), frames, 'exit' if 'exit frames' in last[:40] else 'periodic'))
    print('   %-10s %-44s %12s %10s %10s' % ('address', 'name', 'game', 'per swap', 'internal'))
    for ad, nm, g, i in sorted(rows, key=lambda r: -int(r[2])):
        print('   0x%s %-44s %12s %10.1f %10s' % (ad, nm[:44], g, int(g) / max(frames, 1), i))
    fired = {int(r[0], 16) for r in rows}
    never = [e for e in json.load(open(a.entries)) if int(e['address'], 16) not in fired]
    print('   never called in this run: %d entry points' % len(never))

tally = re.split(r'(?=\[RING-TALLY\] (?:periodic|exit) ring=)', on_text)
tally = [t for t in tally if t.startswith('[RING-TALLY] ') and ' ring=' in t[:60]]
if not tally:
    print('gate 3 FAIL: no [RING-TALLY] report in the on arm'); ok = False
else:
    last = tally[-1]
    head = last.splitlines()[0]
    fns = re.findall(r'function=0x([0-9A-F]{8}) stores=(\d+) block_bytes=(\d+)', last)
    outside = [(f, s, b) for f, s, b in fns if not (D3D_LO <= int(f, 16) < D3D_HI)]
    print('gate 3 %s: %s' % ('PASS' if fns and not outside else 'FAIL', head))
    print('   ring writers: %d functions, %d outside the D3D section' % (len(fns), len(outside)))
    for f, s, b in outside:
        print('   OUTSIDE 0x%s stores=%s block_bytes=%s' % (f, s, b))
    ok &= bool(fns) and not outside
print('OVERALL', 'PASS' if ok else 'FAIL')
