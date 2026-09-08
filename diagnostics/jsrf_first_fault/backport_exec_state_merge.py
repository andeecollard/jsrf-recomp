"""Preserve the incoming NZ condition at US JSRF 0x153A9.

Eight incoming edges: three TEST AH,1 paths (0x1520C, 0x1527B,
0x152BB) and five CMP [ebx+0x10],3..7 paths. TEST uses an AND;
CMP uses inequality. A common CMP_NE expression is incorrect.
The condition is published on each edge before the shared JNE.
"""
import argparse
from pathlib import Path
import re

OLD = ('    if (_flags /* jne: not equal / not zero */) '
       'goto loc_000153BC;')
NEW = ('    if (_flags) goto loc_000153BC; '
       '/* resolved incoming NZ at 0x153A9 */')
ARMS = {0x1520C: '((_fa & _fb) != 0)',
        0x1527B: '((_fa & _fb) != 0)',
        0x152BB: '((_fa & _fb) != 0)',
        0x15311: '(_fa != _fb)', 0x15333: '(_fa != _fb)',
        0x15356: '(_fa != _fb)', 0x15375: '(_fa != _fb)',
        0x153A5: '(_fa != _fb)'}


def patch(text):
    for address, condition in ARMS.items():
        pattern = rf'(loc_{address:08X}: ;\n)(.*?)(?=\nloc_[0-9A-F]+: ;)'
        matches = list(re.finditer(pattern, text, re.S))
        if len(matches) != 1:
            raise ValueError(f'expected one predecessor {address:X}')
        m = matches[0]
        body = m.group(2)
        assignment = f'    _flags = {condition}; /* 0x153A9 incoming NZ */'
        if assignment in body:
            continue
        expected = 'test HI8(eax), 1 (8-bit)' if address < 0x15300 else 'cmp MEM32(ebx + 0x10),'
        if expected not in body:
            raise ValueError(f'unexpected flag setter at {address:X}')
        if address == 0x153A5:
            body = body.rstrip() + '\n' + assignment + '\n'
        else:
            target = '    goto loc_000153A9;'
            if body.count(target) != 1:
                raise ValueError(f'unexpected successor at {address:X}')
            body = body.replace(target, assignment + '\n' + target)
        text = text[:m.start(2)] + body + text[m.end(2):]
    pattern = r'(loc_000153A9: ;\n)(.*?)(\nloc_000153AB: ;)'
    matches = list(re.finditer(pattern,text,re.S))
    if len(matches) != 1:
        raise ValueError('expected one 0x153A9 branch')
    m = matches[0]; body = m.group(2)
    if NEW not in body:
        wrong = ('    if (CMP_NE(_fa, _fb)) goto loc_000153BC; '
                 '/* jne: not equal / not zero */')
        candidates = [old for old in (OLD, wrong) if body.count(old) == 1]
        if len(candidates) != 1:
            raise ValueError('unexpected 0x153A9 branch')
        body = '\n'.join(l for l in body.split('\n')
                         if 'jsrf_unresolved_flag_probe(' not in l)
        body = body.replace(candidates[0], NEW)
        text = text[:m.start(2)] + body + text[m.end(2):]
    return text


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
        print('0x153A9 mixed NZ merge:', 'PASS' if new == old else 'MISSING')
        return int(new != old)
    if new != old:
        path.write_text(new)
    print('0x153A9 mixed NZ merge applied')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
