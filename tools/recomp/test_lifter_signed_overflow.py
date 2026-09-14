"""Signed conditions after add/sub are SF != OF, and at the operand's width.

Two defects in one place, both in the fused jcc that follows an `add` or a
`sub`:

  * `jl` was emitted as `result < 0`, which is SF. x86's `jl` is SF != OF and
    `jle` is ZF || SF != OF, so the emitted form is wrong on every add or sub
    that overflows -- `add eax, 1` at 0x7FFFFFFF leaves SF=1 *and* OF=1, so the
    branch is NOT taken, where `(int32_t)eax < 0` says it is. The dec/inc
    branch had already been fixed this way; its siblings were missed.

  * The condition was then evaluated at 32 bits over operands that may be 8 or
    16. `sub al, bl` writes through SET_LO8 and reads back through LO8, which
    zero-extends, so `(int32_t)LO8(eax) < 0` compares 0..255 against zero:
    `js` after an 8-bit subtraction could never be true, at any site in the
    image. MEM8/MEM16 are uint8_t/uint16_t, so a byte or word memory
    destination has it too.

The sweep below is the real test: every condition, every width, against x86's
own ZF/SF/OF definitions computed in arithmetic wide enough to lose nothing.
The three named cases after it are the specific disagreements, kept separate so
a failure says which defect came back.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction, Operand
from .lifter import Lifter, _make_condition

_ROOT = Path(__file__).resolve().parents[2]

_WIDTHS = ((1, "al", "bl"), (2, "ax", "bx"), (4, "eax", "ebx"))
_JCCS = (("js", "J_S"), ("jns", "J_NS"), ("jl", "J_L"),
         ("jge", "J_GE"), ("jle", "J_LE"), ("jg", "J_G"))

# Chosen so that every width sees zero, both signs, and its own extremes, and
# so the 8- and 16-bit cases carry junk in the bits above them: a condition
# that reads the whole 32-bit register instead of the sub-register fails here
# rather than passing on operands that happen to be small.
_VALUES = (0x00000000, 0x00000001, 0x00000002, 0x0000007F, 0x00000080,
           0x000000FF, 0x00007FFF, 0x00008000, 0x0000FFFF, 0x7FFFFFFF,
           0x80000000, 0xFFFFFFFF, 0xDEADBE80, 0x12345678)


def _ops(lo, hi):
    return [Operand(type="reg", reg=lo), Operand(type="reg", reg=hi)]


def _lifted_write(mnemonic, lo, hi):
    insn = Instruction(0, 2, mnemonic, f"{lo}, {hi}", "", operands=_ops(lo, hi))
    return " ".join(Lifter().lift_instruction(insn))


def _condition(jcc, mnemonic, lo, hi):
    made = _make_condition(jcc, mnemonic, _ops(lo, hi))
    assert made is not None, f"{mnemonic} + {jcc} at {lo} refused a condition"
    return made[0]


class SignedConditionEmissionTest(unittest.TestCase):
    def test_add_no_longer_answers_signed_conditions_with_the_sign_bit(self):
        for jcc in ("jl", "jge", "jle", "jg"):
            with self.subTest(jcc=jcc):
                cond = _condition(jcc, "add", "eax", "ebx")
                # SF alone, in each of the shapes it used to take.
                self.assertNotIn("((int32_t)eax < 0)", cond)
                self.assertNotIn("((int32_t)eax >= 0)", cond)
                self.assertNotIn("((int32_t)eax <= 0)", cond)
                self.assertNotIn("((int32_t)eax > 0)", cond)
                # The exact sum has to be computed somewhere wider than the
                # operands, or OF is not recoverable at all.
                self.assertIn("int64_t", cond)

    def test_sign_conditions_cast_back_to_the_operand_width(self):
        for mnemonic in ("add", "sub"):
            for width, lo, _hi in _WIDTHS:
                cast = {1: "(int8_t)", 2: "(int16_t)"}.get(width, "(int32_t)")
                for jcc in ("js", "jns"):
                    with self.subTest(mnemonic=mnemonic, reg=lo, jcc=jcc):
                        self.assertIn(cast, _condition(jcc, mnemonic, lo, "bl"))

    def test_sub_refuses_rather_than_guessing_without_a_source_operand(self):
        # OF is unknowable from the result alone; the old fallback answered SF
        # anyway. One operand means no answer.
        one = [Operand(type="reg", reg="eax")]
        for jcc in ("jl", "jge", "jle", "jg"):
            with self.subTest(jcc=jcc):
                self.assertIsNone(_make_condition(jcc, "sub", one))


class SignedConditionRuntimeTest(unittest.TestCase):
    def _build_and_run(self, source):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        with tempfile.TemporaryDirectory(prefix="signed-overflow-") as tmp:
            tmp = Path(tmp)
            src, exe = tmp / "test.c", tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(_ROOT / "templates" / "runtime"),
                 str(src), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)

    def test_every_condition_matches_x86_at_every_width(self):
        cases, table = [], []
        for mnemonic in ("add", "sub"):
            for width, lo, hi in _WIDTHS:
                write = _lifted_write(mnemonic, lo, hi)
                for jcc, code in _JCCS:
                    name = f"case_{mnemonic}_{width}_{jcc}"
                    cases.append(
                        f"static int {name}(uint32_t a, uint32_t b) {{\n"
                        f"    eax = a; ebx = b;\n"
                        f"    {write}\n"
                        f"    return ({_condition(jcc, mnemonic, lo, hi)}) "
                        f"? 1 : 0;\n"
                        f"}}")
                    table.append(
                        f'    {{ {name}, {code}, {int(mnemonic == "add")}, '
                        f'{width}, "{mnemonic} {lo},{hi} {jcc}" }},')

        source = _HARNESS.replace("/*CASES*/", "\n\n".join(cases)) \
                         .replace("/*TABLE*/", "\n".join(table)) \
                         .replace("/*VALUES*/",
                                  ", ".join(f"0x{v:08X}u" for v in _VALUES))
        self._build_and_run(source)

    def test_the_three_disagreements_that_motivated_this(self):
        source = _NAMED.replace(
            "/*SUB8_WRITE*/", _lifted_write("sub", "al", "bl")).replace(
            "/*SUB8_JL*/", _condition("jl", "sub", "al", "bl")).replace(
            "/*SUB8_JS*/", _condition("js", "sub", "al", "bl")).replace(
            "/*ADD32_WRITE*/", _lifted_write("add", "eax", "ebx")).replace(
            "/*ADD32_JL*/", _condition("jl", "add", "eax", "ebx")).replace(
            "/*SUB32_WRITE*/", _lifted_write("sub", "eax", "ebx")).replace(
            "/*SUB32_JL*/", _condition("jl", "sub", "eax", "ebx"))
        self._build_and_run(source)


_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
static uint32_t eax, ebx;

enum { J_S, J_NS, J_L, J_GE, J_LE, J_G };

static int64_t sx(uint32_t v, int w) {
    if (w == 1) return (int8_t)v;
    if (w == 2) return (int16_t)v;
    return (int32_t)v;
}

static uint32_t narrow(uint32_t v, int w) {
    if (w == 1) return v & 0xFFu;
    if (w == 2) return v & 0xFFFFu;
    return v;
}

/* x86's own definitions. The exact value is computed in int64, where neither
   operand width can overflow, so OF is "the truncated result is not the exact
   one" rather than a sign-juggling rule that could be wrong the same way the
   code under test was. */
static int reference(int code, int is_add, int w, uint32_t a, uint32_t b) {
    int64_t exact = is_add ? sx(a, w) + sx(b, w) : sx(a, w) - sx(b, w);
    uint32_t result = narrow(is_add ? a + b : a - b, w);
    int zf = (result == 0);
    int sf = (sx(result, w) < 0);
    int of = (sx(result, w) != exact);
    switch (code) {
    case J_S:  return sf;
    case J_NS: return !sf;
    case J_L:  return sf != of;
    case J_GE: return sf == of;
    case J_LE: return zf || (sf != of);
    case J_G:  return !zf && (sf == of);
    }
    return -1;
}

/*CASES*/

struct probe {
    int (*fn)(uint32_t, uint32_t);
    int code;
    int is_add;
    int width;
    const char *what;
};

static const struct probe probes[] = {
/*TABLE*/
};

static const uint32_t values[] = { /*VALUES*/ };

int main(void) {
    size_t p, i, j;
    int failures = 0;
    for (p = 0; p < sizeof probes / sizeof probes[0]; p++) {
        for (i = 0; i < sizeof values / sizeof values[0]; i++) {
            for (j = 0; j < sizeof values / sizeof values[0]; j++) {
                uint32_t a = values[i], b = values[j];
                int got = probes[p].fn(a, b);
                int want = reference(probes[p].code, probes[p].is_add,
                                     probes[p].width, a, b);
                if (got != want) {
                    printf("%s  a=0x%08X b=0x%08X  got %d want %d\n",
                           probes[p].what, a, b, got, want);
                    if (++failures > 20) return 1;
                }
            }
        }
    }
    return failures != 0;
}
'''


_NAMED = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
static uint32_t eax, ebx;

int main(void) {
    /* 1. 8-bit sub, overflowing: al = -128, bl = 1 leaves 0x7F with SF=0 and
       OF=1, so jl IS taken. Evaluated at 32 bits over the zero-extended LO8
       the reconstruction compared 0x80 against 1 as positives and said no. */
    eax = 0xDEADBE80u; ebx = 0x0BADF00Du | 0x01u;
    ebx = (ebx & 0xFFFFFF00u) | 0x01u;
    /*SUB8_WRITE*/
    if (!(/*SUB8_JL*/)) return __LINE__;

    /* 2. 8-bit sub, plain borrow: 0 - 1 = 0xFF, SF=1, so js is taken. With
       LO8 zero-extending, (int32_t)0xFF < 0 was false for every possible
       result -- this branch could not be taken at any 8-bit site. */
    eax = 0xDEADBE00u; ebx = 0x0BADF001u;
    /*SUB8_WRITE*/
    if (!(/*SUB8_JS*/)) return __LINE__;

    /* 3. 32-bit add, overflowing: 0x7FFFFFFF + 1 leaves SF=1 AND OF=1, so
       SF != OF is false and jl is NOT taken. `result < 0` said it was. */
    eax = 0x7FFFFFFFu; ebx = 1u;
    /*ADD32_WRITE*/
    if (/*ADD32_JL*/) return __LINE__;

    /* And the case that was already right, so the fix cannot have broken it:
       INT_MIN - 1 overflows the other way, SF=0 OF=1, jl taken. */
    eax = 0x80000000u; ebx = 1u;
    /*SUB32_WRITE*/
    if (!(/*SUB32_JL*/)) return __LINE__;
    return 0;
}
'''


if __name__ == "__main__":
    unittest.main()
