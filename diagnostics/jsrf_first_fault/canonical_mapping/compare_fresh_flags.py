"""Compare branch conditions in isolated baseline/current translation outputs."""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[3] / 'build-macos/jsrf-first-fault/flags-comparison'


def scan(folder):
    result = {}
    for file in sorted(folder.glob('recomp_*.c')):
        function = block = None
        index = 0
        for line in file.read_text().splitlines():
            m = re.match(r'void (sub_[0-9A-F]+)\(void\)', line)
            if m:
                function = m[1]; block = function[4:]; index = 0
            m = re.match(r'loc_([0-9A-F]+):', line)
            if m:
                block = m[1]; index = 0
            if function and line.lstrip().startswith('if ('):
                key = f'{function}:{block}:{index}'
                result[key] = line.strip()
                index += 1
    return result


def main():
    before = scan(ROOT / 'baseline/gen')
    after = scan(ROOT / 'current/gen')
    if not before or not after:
        raise SystemExit('Both translations must have generated source')
    changed = []
    for key in sorted(before.keys() & after.keys()):
        if before[key] != after[key]:
            changed.append({'site': key, 'before': before[key], 'after': after[key]})
    unresolved = lambda value: 'UNRESOLVED FLAGS' in value
    summary = {
        'baseline_branches': len(before), 'current_branches': len(after),
        'baseline_unresolved_markers': sum(map(unresolved,before.values())),
        'current_unresolved_markers': sum(map(unresolved,after.values())),
        'resolved': [v for v in changed if unresolved(v['before']) and not unresolved(v['after'])],
        'newly_unresolved': [v for v in changed if not unresolved(v['before']) and unresolved(v['after'])],
        'other_changes': [v for v in changed if unresolved(v['before']) == unresolved(v['after'])],
        'baseline_only_sites': sorted(before.keys()-after.keys()),
        'current_only_sites': sorted(after.keys()-before.keys()),
        'remaining': [{'site':k,'condition':v} for k,v in after.items() if unresolved(v)],
    }
    path=Path(__file__).with_name('fresh_flags_comparison.json')
    path.write_text(json.dumps(summary,indent=2)+'\n')
    for key,value in summary.items():
        print(key, len(value) if isinstance(value,list) else value)


if __name__=='__main__':
    main()
