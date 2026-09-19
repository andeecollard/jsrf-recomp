"""FIST/FISTP must produce the integer indefinite, like the SSE path already does.

The float -> int audit that fixed CVTSS2SI stopped at SSE and never reached
x87, so `fist`/`fistp` was still `(int_type)llrint(fp_top())` afterwards. That
is wrong twice over:

  * x86 stores the "integer indefinite" -- the destination's own most-negative
    value -- when the source is NaN, infinite, or out of range. llrint on
    AArch64 saturates instead. Measured on this host: `(int32_t)llrint(NaN)` is
    0x00000000, `(int32_t)llrint(+inf)` is 0xFFFFFFFF, `(int32_t)llrint(3e9)`
    is 0xB2D05E00 -- x86 stores 0x80000000 for all three. A guest that tests
    its converted value against the sentinel is told the conversion succeeded.
  * llrint returns long long, and narrowing that to int16_t/int32_t is
    undefined behaviour for exactly the out-of-range inputs the sentinel is
    about -- so the second defect hides inside the first.

Six sites in JSRF reach this: 1 `fist dword`, 4 `fistp dword`, and one
`fistp qword` inside `sub_0017C3E8`, which is MSVC's `_ftol2` -- 432 call sites
across 129 functions, i.e. *the* (int)float conversion for the whole image.

The 16- and 64-bit helpers are new, so this file also runs them: a Python test
that only compares emitted text cannot tell a correct bound from one that
rounds, and the int64 bound is the kind that does.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]


def _store(mnemonic, size):
    operand = Operand(type="mem", mem_base="esp", mem_disp=0x10, mem_size=size,
                      insn_address=0x00123456, function_address=0x00123000)
    insn = Instruction(0x00123456, 4, mnemonic, "[esp + 0x10]", "")
    insn.operands = [operand]
    return "\n".join(Lifter().lift_instruction(insn))


class FistHelperSelectionTest(unittest.TestCase):
    def test_each_destination_width_gets_its_own_helper(self):
        # The per-width RECOMP_F2I*_ROUND helpers were replaced at the merge
        # with upstream's recomp_fist, which takes the width as an argument
        # and additionally honours the guest's x87 rounding-control bits
        # instead of assuming nearest-even. The property this test exists for
        # is unchanged: the destination's width must reach the conversion, so
        # that an out-of-range value yields THAT width's integer indefinite.
        for mnemonic in ("fist", "fistp"):
            for size, bits in ((2, 16), (4, 32), (8, 64)):
                with self.subTest(mnemonic=mnemonic, size=size):
                    out = _store(mnemonic, size)
                    self.assertIn(
                        f"recomp_fist(fp_top(), g_fp_control_word, {bits})",
                        out)
                    # and the store is made at that width, through the
                    # traced-write macro rather than a raw pointer store.
                    self.assertIn(f"RECOMP_MEM_WRITE{bits}(", out)

    def test_no_llrint_and_no_bare_narrowing_cast_survives(self):
        # The defect this guards is converting the x87 value with a bare C
        # cast or with llrint: both disagree with x86 exactly at NaN, the
        # infinities and the range bounds -- llrint saturates on AArch64, and
        # narrowing its long long is undefined for the very values the
        # integer-indefinite sentinel is about.
        #
        # A cast of recomp_fist's RESULT is not that defect: it has already
        # clamped to the destination width's indefinite (see recomp_fist in
        # templates/runtime/recomp_types.h), so the cast is exact by
        # construction. What must not appear is a cast of the raw value.
        for size in (2, 4, 8):
            with self.subTest(size=size):
                out = _store("fistp", size)
                self.assertNotIn("llrint", out)
                for cast in ("(int16_t)fp_top()", "(int32_t)fp_top()",
                             "(int64_t)fp_top()", "(int16_t)llrint",
                             "(int32_t)llrint", "(int64_t)llrint"):
                    self.assertNotIn(cast, out)
                self.assertIn("recomp_fist(fp_top(), g_fp_control_word,", out)

    def test_fist_does_not_pop_and_fistp_does(self):
        # The helper swap must not disturb the stack discipline around it.
        self.assertNotIn("fp_pop()", _store("fist", 4))
        self.assertIn("fp_pop();", _store("fistp", 4))

    def test_truncating_helper_is_never_used_here(self):
        # FIST rounds under the x87 control word; it has no truncating form.
        for size in (2, 4, 8):
            self.assertNotIn("TRUNC", _store("fistp", size))


class FistIndefiniteRuntimeTest(unittest.TestCase):
    """Run the helpers. NaN, the infinities and the range bounds are where
    llrint and a C cast disagree with x86, so they are the whole point."""

    def test_helpers_yield_the_integer_indefinite(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        source = r'''
#include <stdint.h>
#include <math.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

int main(void) {
    const double nan_v = (double)NAN;
    const double inf_v = (double)INFINITY;

    /* 16-bit: the indefinite is 0x8000, not int32's 0x80000000 narrowed. */
    if (RECOMP_F2I16_ROUND(nan_v)  != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(inf_v)  != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(-inf_v) != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(40000.0)  != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(-40000.0) != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(32767.0) != 32767)  return __LINE__;
    if (RECOMP_F2I16_ROUND(-32768.0) != -32768) return __LINE__;
    /* 32767.5 rounds to 32768, which the destination cannot hold. */
    if (RECOMP_F2I16_ROUND(32767.5) != (int16_t)0x8000) return __LINE__;
    if (RECOMP_F2I16_ROUND(-1.5) != -2) return __LINE__;  /* nearest-EVEN */
    if (RECOMP_F2I16_ROUND(2.5)  !=  2) return __LINE__;
    if (RECOMP_F2I16_ROUND(3.5)  !=  4) return __LINE__;
    if (RECOMP_F2I16_ROUND(1234.0) != 1234) return __LINE__;

    /* 32-bit: the existing helper, exercised here for the first time. */
    if (RECOMP_F2I_ROUND(nan_v)  != RECOMP_INT_INDEFINITE) return __LINE__;
    if (RECOMP_F2I_ROUND(inf_v)  != RECOMP_INT_INDEFINITE) return __LINE__;
    if (RECOMP_F2I_ROUND(-inf_v) != RECOMP_INT_INDEFINITE) return __LINE__;
    if (RECOMP_F2I_ROUND(3000000000.0)  != RECOMP_INT_INDEFINITE) return __LINE__;
    if (RECOMP_F2I_ROUND(-3000000000.0) != RECOMP_INT_INDEFINITE) return __LINE__;
    if (RECOMP_F2I_ROUND(2147483647.0) != 2147483647) return __LINE__;
    if (RECOMP_F2I_ROUND(-2147483648.0) != (-2147483647 - 1)) return __LINE__;
    if (RECOMP_F2I_ROUND(-1.5) != -2) return __LINE__;
    if (RECOMP_F2I_ROUND(2.5)  !=  2) return __LINE__;

    /* 64-bit, where the bound is the interesting part: 2^63 is exact as a
       double and is the correct EXCLUSIVE upper bound, and 2^63 - 1024 is the
       largest double below it. A bound that rounded would admit 2^63 and then
       convert it, which is undefined behaviour. */
    if (RECOMP_F2I64_ROUND(nan_v)  != RECOMP_INT64_INDEFINITE) return __LINE__;
    if (RECOMP_F2I64_ROUND(inf_v)  != RECOMP_INT64_INDEFINITE) return __LINE__;
    if (RECOMP_F2I64_ROUND(-inf_v) != RECOMP_INT64_INDEFINITE) return __LINE__;
    if (RECOMP_F2I64_ROUND(9223372036854775808.0) != RECOMP_INT64_INDEFINITE)
        return __LINE__;
    if (RECOMP_F2I64_ROUND(-9223372036854775808.0) != RECOMP_INT64_INDEFINITE)
        return __LINE__;   /* -2^63 IS representable, and is also the sentinel */
    if (RECOMP_F2I64_ROUND(9223372036854774784.0) != 9223372036854774784LL)
        return __LINE__;
    if (RECOMP_F2I64_ROUND(1e30) != RECOMP_INT64_INDEFINITE) return __LINE__;
    if (RECOMP_F2I64_ROUND(-1.5) != -2) return __LINE__;
    if (RECOMP_F2I64_ROUND(2.5)  !=  2) return __LINE__;
    if (RECOMP_F2I64_ROUND(4294967296.0) != 4294967296LL) return __LINE__;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="fist-runtime-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe), "-lm"],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode,
                             f"check on line {ran.returncode} failed")


if __name__ == "__main__":
    unittest.main()
