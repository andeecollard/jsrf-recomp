"""Add/remove opt-in read-only probes in an existing generated checkout.

Does not regenerate guest code or change branches, registers, or guest memory.
"""
import argparse
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--remove', action='store_true')
p.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2] / 'build-macos/jsrf-first-fault/gen')
a = p.parse_args()
f = a.gen / 'recomp_0000.c'
s = f.read_text()
points = {
    '00013A80': 'ecx',
    '00011083': 'esi',
    '00013220': 'MEM32(esi + 0x87EC)',
    '00025DD0': 'MEM32(esp + 4)',
}
for pc, obj in points.items():
    label = f'loc_{pc}: ;'
    call = f'\n    jsrf_startup_probe(0x{pc}u, {obj}); /* STARTUP_OBSERVATION */'
    assert s.count(label) == 1, f'missing/ambiguous {label}'
    s = s.replace(label + call, label)
    if not a.remove:
        s = s.replace(label, label + call)
f.write_text(s)
print(f'{"Removed" if a.remove else "Installed"} {len(points)} startup observation sites in {f}')
