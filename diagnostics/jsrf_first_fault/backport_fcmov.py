"""Backport the FCMOVcc lifter fix into the existing generated checkout.

`tools/recomp/lifter.py` dropped FCMOVcc: it fell through to the generic x87
case and became a bare `/* FPU: fcmovcc ... */` comment. That is fixed there and
covered by `tools/recomp/test_lifter_fcmov.py`, but the checked-out generated
tree under `build-macos/jsrf-first-fault/gen` predates the fix and a full JSRF
regeneration is a non-goal right now (it would also discard the recovered
mid-function and startup entries alongside it). This rewrites only the two
affected statements, to exactly what the fixed lifter now emits for them.

The two sites are the whole of JSRF's fminf and fmaxf:

    sub_0014C870  fminf(a, b)   fcmove  st(0), st(1)   -- ZF from `test ah, 1`
    sub_0014C850  fmaxf(a, b)   fcmovne st(0), st(1)   -- ZF from `test ah, 1`

Without the move each returns its second argument. The colour packer
sub_000A4CF0 clamps every channel with fminf(c, 1.0f) then fmaxf(c, 0.0f) and
writes the result back, so it stored 0.0f into all four channels of every
object it packed and returned a packed colour of 0.

Idempotent: re-running finds the fixed form and reports it. Read-only with
--check. Regenerating the tree makes this script a no-op, which is the point.
"""
import argparse
import sys
from pathlib import Path

# mnemonic -> (condition the fixed lifter emits after `test ah, 1`, guest fn)
SITES = {
    'fcmove': ('TEST_Z(_fa, _fb)', 'sub_0014C870 (fminf)'),
    'fcmovne': ('TEST_NZ(_fa, _fb)', 'sub_0014C850 (fmaxf)'),
}

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--check', action='store_true',
               help='report without writing; exit 1 if any site is unfixed')
p.add_argument('--gen', type=Path,
               default=Path(__file__).resolve().parents[2]
               / 'build-macos/jsrf-first-fault/gen')
a = p.parse_args()

f = a.gen / 'recomp_0007.c'
s = f.read_text()
unfixed = 0
for mnemonic, (cond, who) in SITES.items():
    old = f'/* FPU: {mnemonic} st(0), st(1) */'
    new = (f'if ({cond}) {{ fp_top() = fp_st1(); }}'
           f' /* {mnemonic} st(0), st(1) */')
    if new in s:
        print(f'{mnemonic}: already fixed in {who}')
        continue
    if s.count(old) != 1:
        sys.exit(f'{mnemonic}: expected exactly one {old!r} in {f}, '
                 f'found {s.count(old)}')
    unfixed += 1
    print(f'{mnemonic}: {"unfixed" if a.check else "fixing"} in {who}')
    s = s.replace(old, new)

if a.check:
    sys.exit(1 if unfixed else 0)
if unfixed:
    f.write_text(s)
print(f'{unfixed} site(s) rewritten in {f}')
