"""Backport the INC/DEC flag snapshot at JSRF's measured ADX overrun.

At 0x14519F DEC ECX sets ZF for JNE at 0x1451B6, but MOV at 0x1451A6
reloads ECX with the input pointer. The old lifter tested that pointer and
never ended the 16-iteration sample loop. Only this observed site is changed;
the reusable lifter fix applies to other INC/DEC consumers on regeneration.
"""
import argparse
from pathlib import Path
import sys

# Kept byte-for-byte equal to the lifter output by test_lifter_inc_dec_flags.
# The build-tree check needs no Capstone installation.
DECREMENT = ('ecx--;\n    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; '
             '_fas = (int32_t)(int32_t)_fa;'
             ' /* dec result snapshot; CF unchanged */')
CONDITION = '(_fa != 0)'


def patch(text):
    start = text.index('loc_00145181: ;')
    end = text.index('loc_001451BC: ;', start)
    block = text[start:end]
    decrement = DECREMENT
    condition = CONDITION
    old_branch = 'if ((ecx != 0)) goto loc_00145000;'
    new_branch = f'if ({condition}) goto loc_00145000;'
    if decrement in block and new_branch in block:
        return text
    if block.count('ecx--;') != 1 or block.count(old_branch) != 1:
        raise ValueError('ADX loop differs from the expected pre-fix or fixed form')
    block = block.replace('ecx--;', decrement).replace(old_branch, new_branch)
    return text[:start] + block + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'build-macos/jsrf-first-fault/gen')
    args = parser.parse_args()
    # Find the chunk holding the loop rather than naming one. --split packs a
    # fixed number of functions per file, so any change in how many functions
    # are discovered shifts every later address into a different chunk: this
    # site moved from recomp_0006.c to recomp_0007.c when a function-boundary
    # fix stopped splitting table-reached functions. backport_result_flag_merge
    # already scans for its site for the same reason.
    path = next((p for p in sorted(args.gen.glob('recomp_[0-9]*.c'))
                 if 'loc_00145181: ;' in p.read_text()), None)
    if path is None:
        raise SystemExit('ADX loop site 0x00145181 is in no generated chunk')
    old = path.read_text()
    new = patch(old)
    if args.check:
        print('ADX loop flag snapshot: ' + ('PASS' if new == old else 'MISSING'))
        return int(new != old)
    if new != old:
        path.write_text(new)
    print('ADX loop flag snapshot installed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
