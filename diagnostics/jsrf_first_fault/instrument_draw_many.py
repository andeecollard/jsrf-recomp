"""Install/remove read-only drawManyDefault observations, including alias copies."""
import argparse
import re
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2] / 'build-macos/jsrf-first-fault/gen')
p.add_argument('--remove', action='store_true')
a = p.parse_args()
# Stage IDs: visit sorted/direct, virtual dispatch sites, post-transform exclusion.
sites = {'000110D0': 0, '000111B1': 0, '000110F0': 1,
         '00011179': 1, '00011186': 1, '000111EC': 1, '000111F9': 1,
         '000110EB': 2, '000111D0': 2}
updates = {}
counts = dict.fromkeys(sites, 0)
for f in a.gen.glob('recomp_*.c'):
    old = f.read_text()
    s = re.sub(r'\n    jsrf_draw_many_probe\([^\n]+/\* DRAW_MANY_OBSERVATION \*/', '', old)
    for pc, stage in sites.items():
        label = f'loc_{pc}: ;'
        counts[pc] += s.count(label)
        if not a.remove:
            s = s.replace(label, label + f'\n    jsrf_draw_many_probe(0x{pc}u, {stage}, esi, esp); /* DRAW_MANY_OBSERVATION */')
    updates[f] = (old, s)
if any(v == 0 for v in counts.values()):
    raise SystemExit(f'Missing anchors; no files changed: {counts}')
for f, (old, s) in updates.items():
    if old != s:
        f.write_text(s)
print(('Removed' if a.remove else 'Installed'), counts)
