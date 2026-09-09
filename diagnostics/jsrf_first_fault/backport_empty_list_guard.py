"""Restore the empty-list guard the 0x00014885 fallthrough drops.

The guest function at 0x00014870 removes entries from an 8-slot array that
lives at `this+0x70`, with its count at `this+0xB0`:

    00014870  push ecx/ebx/ebp/esi
    00014874  mov  esi, ecx              ; this
    00014876  mov  eax, [esi+0xB0]       ; count
    0001487C  xor  ebp, ebp
    0001487E  cmp  eax, ebp              ; count vs 0
    00014880  push edi
    00014881  mov  [esp+0x10], ebp       ; index = 0
    00014885  jbe  0x00014909            ; count == 0 -> do nothing
    0001488B  lea  ebx, [esi+0x70]       ; ...otherwise walk the array

The recompiler split those ten instructions into three functions --
sub_00014870, sub_00014881 and sub_00014885 -- chained by fallthrough calls.
Guest registers survive that hand-off because they are globals; the lifter's
`_fa`/`_fb` flag snapshot does not, because it is a local of each generated
function. So the `jbe` at 0x00014885 lands on the `_flags` fallback, which is
a constant zero, and the guard is never taken.

The body then runs on an empty list and calls sub_000147A0(this, 0), whose
tail-closing loop is

    for (i = index; i < count - 1; i++) a[i] = a[i+1];

compiled with unsigned compares. With count == 0 the bound is 0xFFFFFFFF, so
it copies each dword down one slot for as far as guest memory is mapped. That
was measured overwriting the kernel import thunk table at 0x001C3F60: every
entry ended up holding the next entry's synthetic dispatch address, so the
title's call to ObReferenceObjectByHandle through 0x001C4034 arrived at
ExQueryNonVolatileSetting and faulted on a kernel function pointer where the
Type argument belonged.

The condition is recovered here rather than in the lifter because this tree is
not regenerated; carrying flags across a fallthrough is the general fix, and
the audit_unresolved_flags ratchet counts the sites still waiting for it.
`ebp` is zero at the comparison, so `count <= 0` unsigned is `count == 0`.
"""
import argparse
from pathlib import Path
import sys

UNGUARDED = (
    ('    if (_flags /* jbe: below or equal (unsigned <=) */) '
     'goto loc_00014909;'),
    ('    if (_flags /* jbe: below or equal (unsigned <=) - UNRESOLVED FLAGS, '
     'branch never taken */) goto loc_00014909;'),
)
GUARDED = ('    /* cmp eax, ebp / jbe, with ebp zeroed at 0x0001487C: the '
           'empty-list\n'
           '     * guard. eax still holds MEM32(esi + 0xB0) from 0x00014876; '
           'the flags\n'
           '     * that set this branch were computed in sub_00014870 and do '
           'not cross\n'
           '     * the fallthrough. */\n'
           '    if ((eax == 0u)) goto loc_00014909;')


def patch(text):
    start = text.index('void sub_00014885(void)')
    end = text.find('\nvoid sub_', start + 1)
    if end < 0:
        end = len(text)
    body = text[start:end]
    if GUARDED in body:
        return text
    candidates = [old for old in UNGUARDED if body.count(old) == 1]
    if len(candidates) != 1:
        raise ValueError('sub_00014885 differs from the expected pre-fix form')
    return text[:start] + body.replace(candidates[0], GUARDED) + text[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--gen', type=Path, default=Path(__file__).resolve().parents[2]
                        / 'build-macos/jsrf-first-fault/gen')
    args = parser.parse_args()
    path = args.gen / 'recomp_0000.c'
    old = path.read_text()
    new = patch(old)
    if args.check:
        print('empty-list guard at 0x00014885: '
              + ('PASS' if new == old else 'MISSING'))
        return int(new != old)
    if new != old:
        path.write_text(new)
    print('empty-list guard installed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
