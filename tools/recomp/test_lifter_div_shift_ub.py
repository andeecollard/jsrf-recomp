"""Two places the lifter emitted C the standard leaves undefined.

Both survive ordinary traffic and misbehave exactly at the edges, which is the
shape that gets blamed on something else months later.

  shl CF   The shift count is masked to 5 bits, as x86 requires, but the carry
           bit index for a LEFT shift is (width - count). On a byte operand
           with a register count of 31 that is 8 - 31 = -23, and shifting by a
           negative amount is undefined -- at -O2 the compiler may assume it
           cannot happen and optimise around the branch that reaches it. x86
           leaves CF undefined once the count passes the operand width, so
           declining to compute it there is exact, not a compromise.

  div/idiv A zero divisor is undefined in C. AArch64's division yields zero;
           x86-64 faults. So a guest divide by zero is silent on the primary
           host and crashes the Windows differential oracle -- a divergence on
           the host whose job is to check the other one. idiv has a second
           case, INT64_MIN / -1, whose quotient does not fit.
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
from tools.recomp import lifter as L

src = open(L.__file__).read()

fails = []
def want(name, cond):
    if not cond:
        fails.append(name)

# ── shl: the carry computation must refuse counts past the operand width ──
m = re.search(r"bit = f\"\(\{cnt\}\) - 1\".*?\n(.*?)\n", src, re.S)
want("shl carry line found", m is not None)
if m:
    guard = m.group(1)
    want("shl carry guards the count against the width",
         "<= {w}" in guard or "<= \" + str(w)" in guard or "{cnt}) <= {w}" in guard)

# ── div / idiv: both must guard before dividing ──
want("div guards a zero divisor",
     re.search(r'elif m == "div".*?if \(_divisor\)', src, re.S) is not None)
want("idiv guards zero and INT64_MIN / -1",
     re.search(r'elif m == "idiv".*?_divisor == -1 && _dividend == INT64_MIN',
               src, re.S) is not None)
want("div still divides when the divisor is non-zero",
     "_dividend / _divisor" in src)

# ── the arithmetic the guards protect, checked independently ──
# A negative shift is the thing being avoided; confirm the index really does go
# negative for the narrow case, or the guard is guarding nothing.
for width, count in ((8, 31), (16, 31), (8, 9)):
    want("shl bit index is negative for w=%d cnt=%d" % (width, count),
         width - count < 0)
want("shl bit index is fine for a 32-bit operand", all(
    32 - c >= 0 for c in range(1, 32)))

if fails:
    print("FAIL")
    for f in fails:
        print("   ", f)
    sys.exit(1)
print("div/shift UB: shl carry guarded against negative indices, div and idiv "
      "guard the cases x86 faults on; %d narrow-shift indices confirmed negative"
      % 3)
