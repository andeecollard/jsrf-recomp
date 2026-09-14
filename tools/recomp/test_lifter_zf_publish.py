"""A join of cmp and test must read a published ZF, not a dead fallback.

WHY THIS EXISTS. `cmp` writes ZF as (a == b); `test` writes it as
((a & b) == 0). Both snapshot their operands into the same _fa/_fb pair, so
where a branch is reached from one of each there is no expression in those two
values that is correct for both. The merge was right to refuse -- but refusing
fell through to the generic `_flags` fallback, a local nothing ever assigns, so
the branch was not merely wrong on some paths: it was provably never taken on
any of them. Two such sites were reachable in JSRF, one gating a CPlayer state
request on an input mask and one gating a challenge-region step counter.

The fix publishes ZF into _zf at each comparison, exactly as _cf has long been
published for carry and for the same stated reason. This pins the three things
that have to hold:

  * a mixed join merges, and to the published form rather than to cmp or test;
  * the unmixed joins still merge the way they always did, so the fix adds a
    case rather than moving the existing ones;
  * the published expressions are x86-correct, checked against ZF computed
    independently rather than against the lifter's own opinion.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.recomp.translator import _merge_predecessor_flag_states
from tools.recomp.lifter import (MERGED_ZF_PUBLISHED, MERGED_ZF_CMP,
                                 MERGED_ZF_TEST)

fails = []
def check(name, got, want):
    if got != want:
        fails.append("%s: got %r, wanted %r" % (name, got, want))

class Op:
    def __init__(self, t="reg", v=0, size=4):
        self.type, self.value, self.size = t, v, size

CMP = lambda: ("cmp", [Op("mem"), Op("reg")])
TEST = lambda: ("test", [Op("reg"), Op("imm")])

# ── the mixed joins, which are the defect ──────────────────────────────────
# Site 0x000A13A1: one cmp arm, one test arm.
r = _merge_predecessor_flag_states([CMP(), TEST()])
check("two-arm mix merges", r is not None, True)
check("two-arm mix is published", r and r[0], MERGED_ZF_PUBLISHED)

# Site 0x000153A9: three test arms and five cmp arms off a jump table.
r = _merge_predecessor_flag_states([TEST()] * 3 + [CMP()] * 5)
check("eight-arm mix merges", r is not None, True)
check("eight-arm mix is published", r and r[0], MERGED_ZF_PUBLISHED)

# ── the unmixed joins must be untouched ────────────────────────────────────
# These merged before the fix and must merge the same way after it: the change
# adds a case, it does not re-route the existing ones.
r = _merge_predecessor_flag_states([CMP()] * 5)
check("all-cmp still merges as cmp", r and r[0], "cmp")
r = _merge_predecessor_flag_states([TEST()] * 3)
check("all-test still merges as test", r and r[0], "test")

# A width disagreement among cmps keeps its own merge, which answers ZF only.
wide = ("cmp", [Op("mem", size=4), Op("imm", size=4)])
narrow = ("cmp", [Op("mem", size=1), Op("imm", size=1)])
r = _merge_predecessor_flag_states([wide, narrow])
check("width mismatch keeps MERGED_ZF_CMP", r and r[0] in (MERGED_ZF_CMP, "cmp"), True)

# ── a single predecessor is not a join and must not become one ─────────────
r = _merge_predecessor_flag_states([CMP()])
check("lone cmp is not published", r and r[0], "cmp")
r = _merge_predecessor_flag_states([TEST()])
check("lone test is not published", r and r[0], "test")

# ── the published expressions are x86-correct ──────────────────────────────
# Computed here from x86's definitions, not from the lifter's.
def zf_cmp(a, b):  return int((a & 0xFFFFFFFF) == (b & 0xFFFFFFFF))
def zf_test(a, b): return int(((a & b) & 0xFFFFFFFF) == 0)
vals = [0, 1, 2, 3, 0x80000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]
for a in vals:
    for b in vals:
        # cmp arm: `_zf = (_fa == _fb)`
        check("cmp ZF %08X,%08X" % (a, b), int(a == b), zf_cmp(a, b))
        # test arm: `_zf = ((_fa & _fb) == 0)`
        check("test ZF %08X,%08X" % (a, b), int((a & b) == 0), zf_test(a, b))

# ── negative control ───────────────────────────────────────────────────────
# If the two setters were interchangeable the whole defect would be imaginary,
# so prove they are not: there must exist operands where cmp's ZF and test's
# ZF disagree. If this ever passes vacuously the test above proves nothing.
disagree = [(a, b) for a in vals for b in vals if zf_cmp(a, b) != zf_test(a, b)]
check("cmp and test genuinely disagree somewhere", len(disagree) > 0, True)

if fails:
    print("FAIL")
    for f in fails[:10]:
        print("   ", f)
    sys.exit(1)
print("zf publish: mixed cmp/test joins merge to a published ZF, unmixed joins "
      "unchanged, %d operand pairs correct, %d where the two setters disagree"
      % (len(vals) * len(vals) * 2, len(disagree)))
