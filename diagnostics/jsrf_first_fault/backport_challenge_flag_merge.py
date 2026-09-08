"""Backport compatible CMP snapshot merging at US JSRF 0x15275.

0x15271 compares [ebx+0x10] with 1; 0x152EE compares it with 2 and
jumps to the same JNE. Both populate _fa/_fb. Consume the arriving snapshot,
not a re-read against one fixed constant. Do not alter the mixed CMP/TEST
merge at 0x153A9. See canonical_mapping/flags_15130.asm.
"""
import argparse
from pathlib import Path
import re

OLD = 'if (_flags /* jne: not equal / not zero */) goto loc_000153BC;'
NEW = 'if (CMP_NE(_fa, _fb)) goto loc_000153BC; /* jne: not equal / not zero */'


def patch(text):
    pattern = r'(loc_00015275: ;\n)(.*?)(\nloc_0001527B: ;)'
    matches = list(re.finditer(pattern, text, re.S))
    if len(matches) != 1:
        raise ValueError('expected one 0x15275 block')
    m = matches[0]
    body = m.group(2)
    if NEW in body:
        return text
    if body.count(OLD) != 1:
        raise ValueError('unexpected 0x15275 branch')
    # This branch is no longer unresolved; preserve all other instrumentation.
    body = '\n'.join(line for line in body.split('\n')
                     if 'jsrf_unresolved_flag_probe(' not in line)
    body = body.replace(OLD, NEW)
    return text[:m.start(2)] + body + text[m.end(2):]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--check', action='store_true')
    p.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                   / 'build-macos/jsrf-first-fault/gen')
    a = p.parse_args()
    path = a.gen / 'recomp_0000.c'
    old = path.read_text()
    new = patch(old)
    if a.check:
        print('0x15275 CMP merge:', 'PASS' if new == old else 'MISSING')
        return int(new != old)
    if new != old:
        path.write_text(new)
    print('0x15275 CMP merge applied')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
