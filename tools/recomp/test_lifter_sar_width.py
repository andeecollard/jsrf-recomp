"""`sar` shifts arithmetically at the OPERAND's width, not always at 32 bits.

This is a wrong VALUE, not a wrong flag, which makes it a different class of
bug from the signed-condition family it was found beside.

Every narrow read in this lifter arrives zero-extended: LO8/LO16 mask, and
MEM8/MEM16 are unsigned pointers. So the old `(int32_t)LO8(eax) >> n` was a
LOGICAL shift wearing an arithmetic cast -- the sign bits it should feed in are
zeros that were never there. `sar al, 1` with al=0x80 produced 0x40 where x86
gives 0xC0: a negative number quietly halved into a positive one, which is how
a fixed-point divide or a signed average goes wrong without crashing.

The sweep runs the lifter's OWN emitted code against a reference that does the
shift in a signed type of the right width. The negative control at the end
substitutes the old expression and asserts the sweep rejects it -- a test that
passes against both old and new code would be worth nothing.

Note deliberately NOT asserted: x86 leaves flags untouched when the masked
count is zero, and this lifter does not model that. It is pre-existing and
unrelated to the width defect, so it is excluded rather than silently folded in.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter

_ROOT = Path(__file__).resolve().parents[2]

_WIDTHS = ((1, "al", "cl"), (2, "ax", "cx"), (4, "eax", "ecx"))

# Values chosen so each width sees its own sign bit set and clear, with junk in
# the bits above it: an expression that reads the whole 32-bit register instead
# of the sub-register fails here rather than passing on small operands.
_VALUES = (0x00000000, 0x00000001, 0x0000007F, 0x00000080, 0x000000FF,
           0x00007FFF, 0x00008000, 0x0000FFFF, 0x7FFFFFFF, 0x80000000,
           0xFFFFFFFF, 0xDEADBE80, 0xDEADBE7F, 0x12345678)
_COUNTS = (1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 47, 63)
# 32, 33, 47 and 63 are upstream's addition: x86 masks the count to 5
# bits at every operand width, so they must behave as 0, 1, 15 and 31.
# Without them nothing in this file proved the mask was still applied.


def _emit(lo, hi):
    ops = [Operand(type="reg", reg=lo), Operand(type="reg", reg=hi)]
    insn = Instruction(0, 2, "sar", f"{lo}, {hi}", "", operands=ops)
    return " ".join(Lifter().lift_instruction(insn))


class SarEmissionTest(unittest.TestCase):
    def test_cast_matches_the_operand_width(self):
        for width, lo, hi in _WIDTHS:
            cast = {1: "(int8_t)", 2: "(int16_t)"}.get(width, "(int32_t)")
            with self.subTest(reg=lo):
                self.assertIn(cast, _emit(lo, hi))

    def test_narrow_sar_is_not_emitted_as_int32(self):
        # The SHIFT is what must not be widened -- that is the defect this
        # guards. Since G19 the emission also carries a result snapshot, whose
        # `_fas = (int32_t)(int8_t)(_fa)` is a sign-extension of an already
        # width-masked value and is correct at every width, so the assertion
        # looks at the shift statement rather than the whole emission.
        for _width, lo, hi in ((1, "al", "cl"), (2, "ax", "cx")):
            with self.subTest(reg=lo):
                shift = _emit(lo, hi).split("_fa =")[0]
                self.assertIn(">>", shift)          # we are looking at the shift
                self.assertNotIn("(int32_t)", shift)


_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
static uint32_t eax, ecx;
/* G19: the result-setter family publishes its flags into this pair
   where the result is computed, so a jcc reads the snapshot rather
   than a destination something may have overwritten since. */
uint32_t _fa, _fb;
int32_t _fas, _fbs;

/* x86 sar, done honestly in a signed type of the operand's own width. */
static uint32_t reference(uint32_t a, uint32_t count, int width) {
    unsigned n = count & 31u;
    if (width == 1) {
        int8_t v = (int8_t)(a & 0xFFu);
        return (uint32_t)((a & 0xFFFFFF00u) | ((uint32_t)(int32_t)(v >> n) & 0xFFu));
    }
    if (width == 2) {
        int16_t v = (int16_t)(a & 0xFFFFu);
        return (uint32_t)((a & 0xFFFF0000u) | ((uint32_t)(int32_t)(v >> n) & 0xFFFFu));
    }
    return (uint32_t)((int32_t)a >> n);
}
__CASES__

int main(void) {
    static const uint32_t values[] = { __VALUES__ };
    static const uint32_t counts[] = { __COUNTS__ };
    struct { uint32_t (*fn)(uint32_t, uint32_t); int width; const char *name; }
        table[] = { __TABLE__ };
    int failures = 0;
    for (unsigned t = 0; t < sizeof table / sizeof table[0]; t++) {
        for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
            for (unsigned j = 0; j < sizeof counts / sizeof counts[0]; j++) {
                uint32_t got = table[t].fn(values[i], counts[j]);
                uint32_t want = reference(values[i], counts[j], table[t].width);
                if (got != want) {
                    if (failures < 12)
                        printf("%s a=0x%08X n=%u: got 0x%08X want 0x%08X\n",
                               table[t].name, values[i], counts[j], got, want);
                    failures++;
                }
            }
        }
    }
    if (failures) printf("%d disagreement(s)\n", failures);
    return failures != 0;
}
'''


class SarRuntimeTest(unittest.TestCase):
    def _source(self, force_int32):
        cases, table = [], []
        for width, lo, hi in _WIDTHS:
            write = _emit(lo, hi)
            if force_int32:      # the negative control: the old expression
                write = (write.replace("(int8_t)", "(int32_t)")
                              .replace("(int16_t)", "(int32_t)"))
            name = f"case_{width}"
            cases.append(
                f"static uint32_t {name}(uint32_t a, uint32_t n) {{\n"
                f"    eax = a; ecx = n;\n"
                f"    {write}\n"
                f"    return eax;\n"
                f"}}")
            table.append(f'    {{ {name}, {width}, "sar {lo},{hi}" }},')
        return (_HARNESS
                .replace("__CASES__", "\n".join(cases))
                .replace("__TABLE__", "\n".join(table))
                .replace("__VALUES__", ", ".join(f"0x{v:08X}u" for v in _VALUES))
                .replace("__COUNTS__", ", ".join(str(c) for c in _COUNTS)))

    def _run(self, source):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        with tempfile.TemporaryDirectory(prefix="sar-width-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "t.c", tmp / "t"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_sar_matches_x86_at_every_width(self):
        ran = self._run(self._source(force_int32=False))
        self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)

    def test_negative_control_the_old_int32_form_is_rejected(self):
        # Without this, the test above could be passing for the wrong reason.
        ran = self._run(self._source(force_int32=True))
        self.assertNotEqual(0, ran.returncode,
                            "the old (int32_t) form passed the sweep -- the "
                            "sweep is not testing what it claims to")
        # And it must fail for the RIGHT reason: narrow, sign bit set.
        self.assertIn("sar al,cl a=0x00000080", ran.stdout)


if __name__ == "__main__":
    unittest.main()
