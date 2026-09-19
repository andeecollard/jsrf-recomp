"""SHLD/SHRD against x86's count rules, swept rather than argued.

Both defects here are live. JSRF runs `shrd eax, edx, cl` twice in its own
text section, at 0x0017D2AA and 0x0017D54A, and `cl` is a runtime count.

    the count is masked to five bits by the hardware, and was not here
    a count of ZERO is a no-op on x86, and the old expression was UB for it

The second is the one that produced a wrong value rather than merely
undefined behaviour. `shrd` built

    dst = (dst >> cnt) | (src << (32 - cnt))

so cnt == 0 shifted left by 32. C leaves that undefined and the compiler may
do as it likes; on this target the variable shift takes its amount modulo 32,
shifts by zero, and hands back `src` whole -- so the write came out
`dst | src` where x86 changes nothing at all.

The sweep below compiles the lifter's own emitted statement and runs it
against a reference computed in 64-bit arithmetic, over every count from 0 to
40 -- which covers zero, the ordinary range, the 5-bit wrap at 32, and beyond
it -- and over values chosen so the two answers differ.

NEGATIVE CONTROL, run rather than imagined: substituting the old unguarded
expression back makes the sweep fail, and on the count=0 cases specifically.
That is what says these numbers come from the check and not from the
arithmetic happening to agree.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]

_VALUES = (0x00000000, 0x00000001, 0x80000000, 0xFFFFFFFF,
           0x12345678, 0xDEADBEEF, 0x0000FFFF, 0xFFFF0000)
_COUNTS = tuple(range(0, 41))

_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
uint32_t eax, edx, ecx;
uint32_t _fa, _fb;
int32_t _fas, _fbs;

static uint32_t emitted(uint32_t d, uint32_t s, uint32_t c)
{
    eax = d; edx = s; ecx = c;
    /*BODY*/
    return eax;
}

/* x86: the count is masked to 5 bits; a masked count of zero does nothing. */
static uint32_t reference(uint32_t d, uint32_t s, uint32_t c)
{
    uint32_t n = c & 31u;
    if (n == 0) return d;
    /*REF*/
}

int main(void)
{
    static const uint32_t vals[] = { /*VALUES*/ };
    int failures = 0;
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (unsigned j = 0; j < sizeof vals / sizeof vals[0]; j++)
            for (uint32_t c = 0; c <= 40; c++) {
                uint32_t got = emitted(vals[i], vals[j], c);
                uint32_t want = reference(vals[i], vals[j], c);
                if (got != want) {
                    failures++;
                    if (failures <= 12)
                        printf("d=0x%08X s=0x%08X c=%u  got 0x%08X want 0x%08X\n",
                               vals[i], vals[j], c, got, want);
                }
            }
    if (!failures) printf("ok\n");
    return failures ? 1 : 0;
}
'''

_REF = {
    # SHLD: dst's low bits filled from src's high bits.
    "shld": "    return (uint32_t)(((uint64_t)d << n) | ((uint64_t)s >> (32 - n)));",
    # SHRD: dst's high bits filled from src's low bits.
    "shrd": "    return (uint32_t)((d >> n) | ((uint64_t)s << (32 - n)));",
}


def _reg(n):
    return Operand(type="reg", reg=n)


def _body(m):
    ops = [_reg("eax"), _reg("edx"), _reg("cl")]
    insn = Instruction(0, 3, m, "eax, edx, cl", "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


class DoubleShiftTest(unittest.TestCase):
    def _build_and_run(self, body, m):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        src = (_HARNESS.replace("/*BODY*/", body)
                       .replace("/*REF*/", _REF[m])
                       .replace("/*VALUES*/",
                                ", ".join("0x%08Xu" % v for v in _VALUES)))
        with tempfile.TemporaryDirectory(prefix="double-shift-") as tmp:
            tmp = Path(tmp)
            c, exe = tmp / "t.c", tmp / "t"
            c.write_text(src, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(c), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_matches_x86_at_every_count(self):
        for m in ("shld", "shrd"):
            with self.subTest(mnemonic=m):
                ran = self._build_and_run(_body(m), m)
                self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)

    def test_a_zero_count_changes_nothing(self):
        """The case the old expression got wrong, pinned on its own."""
        for m in ("shld", "shrd"):
            with self.subTest(mnemonic=m):
                body = _body(m)
                self.assertIn("_c &&", body,
                              "the zero-count guard is what makes a count of "
                              "0 a no-op instead of a shift by the full width")

    def test_the_count_is_masked_to_five_bits(self):
        for m in ("shld", "shrd"):
            with self.subTest(mnemonic=m):
                self.assertIn("& 31u", _body(m))

    def test_negative_control_the_old_expression_is_caught(self):
        """Put the pre-fix statement back; the sweep must fail on count 0."""
        old = {
            "shld": "eax = (eax << (LO8(ecx))) | (edx >> (32 - (LO8(ecx))));",
            "shrd": "eax = (eax >> (LO8(ecx))) | (edx << (32 - (LO8(ecx))));",
        }
        for m in ("shld", "shrd"):
            with self.subTest(mnemonic=m):
                ran = self._build_and_run(old[m], m)
                self.assertEqual(
                    1, ran.returncode,
                    "the unguarded expression should disagree with x86 and "
                    "did not:\n" + ran.stdout + ran.stderr)
                self.assertIn(" c=0 ", ran.stdout,
                              "it should disagree at a count of zero:\n"
                              + ran.stdout)


if __name__ == "__main__":
    unittest.main()
