"""Install bounded drawOne observations in existing generated C; --remove reverses.

No regeneration. Validate every anchor before writing any file.
"""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                    / 'build-macos/jsrf-first-fault/gen')
parser.add_argument('--remove', action='store_true')
args = parser.parse_args()
points = {
    '00012580': 'jsrf_draw_one_enter(ecx, esp);',
    '000125C7': 'jsrf_draw_one_return();',
    '00011220': 'jsrf_draw_tree_count(1);',
    '00011260': 'jsrf_draw_tree_count(2);',
}
files = {p: p.read_text() for p in args.gen.glob('recomp_*.c')}
updates = {}
for pc, call in points.items():
    label = f'loc_{pc}: ;'
    matches = [p for p, text in files.items() if label in text]
    if len(matches) != 1 or files[matches[0]].count(label) != 1:
        raise SystemExit(f'Expected exactly one {label}; no files changed')
    path = matches[0]
    text = updates.get(path, files[path])
    text = re.sub(r'\n    [^\n]* /\* DRAW_ONE_OBSERVATION:' + pc + r' \*/', '', text)
    if not args.remove:
        text = text.replace(label, label + '\n    ' + call
                            + f' /* DRAW_ONE_OBSERVATION:{pc} */')
    updates[path] = text
for path, text in updates.items():
    if text != files[path]:
        path.write_text(text)
print(f'{"Removed" if args.remove else "Installed"} {len(points)} drawOne sites')
