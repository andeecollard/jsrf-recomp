"""Install bounded function-entry counters in existing generated C; --remove reverses.

No regeneration. Every anchor is validated before any file is written, so a
missed label leaves the tree untouched rather than half-instrumented.
"""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                    / 'build-macos/jsrf-first-fault/gen')
parser.add_argument('--remove', action='store_true')
parser.add_argument('--skip-mismatched', action='store_true',
               help='skip VAs whose label is missing or not unique and report '
                    'them, instead of refusing the whole set. Necessary when '
                    'arming hundreds of sites: one non-unique label should not '
                    'block the other five hundred. Each site installed is still '
                    'validated, so nothing lands at a label that did not match.')
parser.add_argument('--va', action='append', default=None,
                    help='guest VA to count, repeatable (default: the +0xCE0 writers)')
parser.add_argument('--va-file', type=Path, default=None,
                    help='file of guest VAs, one per line -- avoids shells that '
                         'do not word-split an expanded argument list')
parser.add_argument('--stackarg', action='append', default=[],
                    help='VA:OFFSET -- count and record the guest dword at '
                         '[esp+OFFSET] on entry, for a stack-passed argument')
parser.add_argument('--derefarg', action='append', default=[],
                    help='VA:STACKOFF:FIELD -- record the guest dword at '
                         '[[esp+STACKOFF]+FIELD] on entry, i.e. a field of a '
                         'pointer-valued argument, sampled AT CALL TIME rather '
                         'than later when the block may have been reused')
parser.add_argument('--ecxfield', action='append', default=[],
                    help='VA:FIELD -- record the guest dword at [ecx+FIELD] on '
                         'entry, i.e. a field of `this` BEFORE the function '
                         'runs. Brackets which call changes a field: the setter '
                         'is entered with the old value while later callers see '
                         'the new one.')
parser.add_argument('--ecxpair', action='append', default=[],
                    help='VA:FIELD -- record BOTH `this` and [this+FIELD] on '
                         'entry; one without the other is ambiguous')
parser.add_argument('--arg', action='append', default=[],
                    help='guest VA to count AND record `this` (ecx) for, repeatable')
args = parser.parse_args()

# Defaults are the two functions that form CPlayer +0xCE0 statically, plus
# CActMan::drawOne as a positive control: a run where drawOne is also zero is
# a broken instrument, not a finding about the other two.
from_file = ([l.split('#')[0].strip() for l in args.va_file.read_text().splitlines()]
             if args.va_file else [])
from_file = [v for v in from_file if v]
vas = ((args.va or from_file or ['00094AB0', '00080340', '00012580'])
       + from_file if args.va else (from_file or args.va
       or ['00094AB0', '00080340', '00012580'])) + list(args.arg)
stackset = {}
for spec in args.stackarg:
    va, _, off = spec.partition(':')
    stackset[va.upper().removeprefix('0X')] = int(off or '4', 0)
derefset = {}
for spec in args.derefarg:
    va, _, rest = spec.partition(':')
    soff, _, foff = rest.partition(':')
    derefset[va.upper().removeprefix('0X')] = (int(soff or '4', 0), int(foff or '0', 0))
ecxset = {}
for spec in args.ecxfield:
    va, _, foff = spec.partition(':')
    ecxset[va.upper().removeprefix('0X')] = int(foff or '0', 0)
pairset = {}
for spec in args.ecxpair:
    va, _, foff = spec.partition(':')
    pairset[va.upper().removeprefix('0X')] = int(foff or '0', 0)
vas = list(dict.fromkeys(vas + list(stackset) + list(derefset) + list(ecxset)
                         + list(pairset)))
argset = {v.upper().removeprefix('0X') for v in args.arg}
files = {p: p.read_text() for p in args.gen.glob('recomp_*.c')}
updates = {}
skipped = []
for va in vas:
    va = va.upper().removeprefix('0X')
    label = f'loc_{va}: ;'
    matches = [p for p, text in files.items() if label in text]
    if len(matches) != 1 or files[matches[0]].count(label) != 1:
        found = sum(t.count(label) for t in files.values())
        if args.skip_mismatched:
            skipped.append(f'{label.strip()} found {found}')
            continue
        raise SystemExit(f'Expected exactly one {label}, found {found}; '
                         f'no files changed. --skip-mismatched installs the rest')
    path = matches[0]
    text = updates.get(path, files[path])
    text = re.sub(r'\n    [^\n]* /\* FUNC_HIT:' + va + r' \*/', '', text)
    if not args.remove:
        if va in pairset:
            call = (f'jsrf_func_arg2(0x{va}u, ecx, '
                    f'MEM32(ecx + {pairset[va]}u));')
        elif va in ecxset:
            call = f'jsrf_func_arg(0x{va}u, MEM32(ecx + {ecxset[va]}u));'
        elif va in derefset:
            so, fo = derefset[va]
            call = (f'jsrf_func_arg(0x{va}u, '
                    f'MEM32(MEM32(esp + {so}u) + {fo}u));')
        elif va in stackset:
            # At the entry label esp still points at the return address, so a
            # stack-passed argument sits at [esp+4] before any push.
            call = f'jsrf_func_arg(0x{va}u, MEM32(esp + {stackset[va]}u));'
        elif va in argset:
            call = f'jsrf_func_arg(0x{va}u, ecx);'
        else:
            call = f'jsrf_func_hit(0x{va}u);'
        text = text.replace(label, label + f'\n    {call} /* FUNC_HIT:{va} */')
    updates[path] = text
for path, text in updates.items():
    if text != files[path]:
        path.write_text(text)
print(f'{"Removed" if args.remove else "Installed"} {len(vas)} counter(s): {", ".join(vas)}')
if skipped:
    print(f'skipped {len(skipped)} site(s): ' + ', '.join(skipped[:8])
          + (' ...' if len(skipped) > 8 else ''))
