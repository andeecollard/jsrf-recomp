"""Give the shared `je` at 0x000A13A1 the ZF its two predecessors compute.

The compiler folded two comparisons onto one branch:

    000A1385  cmp  [esi+0xE6C], edi
    000A138B  jmp  0x000A13A1
    000A138D  cmp  [esi+0xE60], ebp
    000A1393  jne  0x000A13CF
    000A1395  mov  ecx, esi
    000A1397  call 0x0007F880
    000A139C  test eax, 0x80000
    000A13A1  je   0x000A13CF          <-- reached from both
    000A13A3  cmp  [esi+0xE6C], edi

So the branch has no single condition: arriving by the jump it means
`[esi+0xE6C] == edi`, and arriving by fall-through it means
`(eax & 0x80000) == 0`. The lifter resolves a conditional from the flag write
that precedes it in the same block, finds two different ones, and emits the
`_flags` fallback -- a constant zero, so the branch is never taken and the
guest always runs the 0x000A13A3 arm.

This is the one dead `_flags` fallback that executes in the reachable code as
of 5 September, measured with RECOMP_UNRESOLVED_FLAGS=1 over a 110 s run
(site 38 of sub_000A0F10, the only [UNRESOLVED-FLAG] line in the log). The
0x00014885 site that corrupted the kernel import table was the other, and it
has its own backport.

The fix is not at the branch, because the branch genuinely has no condition of
its own; it is at each predecessor, which is also what the general lifter fix
in G21 has to do. Each writes ZF into `_flags`, the form the audit already
recognises as a legitimate resolved site.
"""
import argparse
from pathlib import Path
import sys

JUMP_ARM = """    _cf = (int)(_fa < _fb);
    goto loc_000A13A1;"""
JUMP_ARM_FIXED = """    _cf = (int)(_fa < _fb);
    _flags = (_fa == _fb); /* cmp [esi+0xE6C], edi -> ZF for the je at 0xA13A1 */
    goto loc_000A13A1;"""

FALL_ARM = """    _cf = 0; /* test/cmp-logical clears CF */

loc_000A13A1: ;"""
FALL_ARM_FIXED = """    _cf = 0; /* test/cmp-logical clears CF */
    _flags = ((_fa & _fb) == 0); /* test eax, 0x80000 -> ZF for the je below */

loc_000A13A1: ;"""


def patch(text):
    start = text.index('void sub_000A0F10(void)')
    end = text.find('\nvoid sub_', start + 1)
    if end < 0:
        end = len(text)
    body = text[start:end]
    if JUMP_ARM_FIXED in body and FALL_ARM_FIXED in body:
        return text
    for old in (JUMP_ARM, FALL_ARM):
        if body.count(old) != 1:
            raise ValueError('sub_000A0F10 differs from the expected pre-fix form')
    body = body.replace(JUMP_ARM, JUMP_ARM_FIXED).replace(FALL_ARM, FALL_ARM_FIXED)
    return text[:start] + body + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'build-macos/jsrf-first-fault/gen')
    args = parser.parse_args()
    # Find the chunk the function landed in rather than naming one. The chunk
    # numbering is an artefact of how many functions preceded it, so it moves
    # on any change to function bounds -- this site went from recomp_0003.c to
    # recomp_0002.c, and the hardcoded name turned a truthful "the branch is
    # still dead" into `ValueError: substring not found`, which reads like a
    # broken script rather than a live defect.
    needle = 'void sub_000A0F10(void)'
    candidates = [p for p in sorted(args.gen.glob('recomp_*.c'))
                  if needle in p.read_text()]
    if len(candidates) != 1:
        sys.exit(f'{needle}: expected one chunk in {args.gen}, '
                 f'found {len(candidates)}')
    path = candidates[0]
    old = path.read_text()
    new = patch(old)
    if args.check:
        print('shared ZF branch at 0x000A13A1: '
              + ('PASS' if new == old else 'MISSING'))
        return int(new != old)
    if new != old:
        path.write_text(new)
    print('shared ZF branch resolved')
    return 0


if __name__ == '__main__':
    sys.exit(main())
