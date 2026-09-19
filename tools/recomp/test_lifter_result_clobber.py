"""G19: a result-setter's flags, read after something overwrote its destination.

The whole family -- and/or/xor, add/sub, adc/sbb, neg, the shifts -- WRITES its
destination, and the jcc that reads its flags can be several instructions, or
several basic blocks, later. The lifter used to rebuild the condition at the
branch by re-reading that destination. Anything in between could have replaced
it: a mov, a pop, a lea, a reloaded loop pointer.

inc/dec were fixed for this once already, and the note in _make_condition says
why: JSRF's ADX loop does `dec ecx`, reloads ecx with its input pointer, then
`jne`, and testing the live ecx made a sixteen-iteration loop run forever. The
rest of the family had the identical hole. This is the test for closing it.

    and eax, 0x0F        ; ZF answers "were the low four bits clear"
    mov eax, 0x99        ; the answer is now unobtainable from eax
    jne taken

x86 branches on the flags the `and` left, so with a = 0x10 the `and` gives 0 and
`jne` is NOT taken -- whatever the `mov` put there afterwards. Reading live eax
asks about 0x99, which is nonzero, and takes the branch every time regardless of
a. The sweep below pins that across both answers and at all three widths, so a
condition that reads the destination fails on the half of the inputs where the
result and the clobber disagree.

The last case is the cross-block form, which is the shape G19 was found in:
setter and branch in different basic blocks, with the clobber between them.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block, _make_condition
from .translator import _merge_predecessor_flag_states

_ROOT = Path(__file__).resolve().parents[2]

_HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;
uint32_t eax, ebx, ecx;
uint32_t _fa, _fb;
int32_t _fas, _fbs;
int _cf;

/*CASES*/

static const uint32_t values[] = { /*VALUES*/ };

int main(void) {
    size_t i;
    int failures = 0;
    for (i = 0; i < sizeof values / sizeof values[0]; i++) {
/*CALLS*/
    }
    if (!failures) printf("ok\n");
    return failures ? 1 : 0;
}
'''

_VALUES = (0x00000000, 0x00000001, 0x0000000F, 0x00000010, 0x0000007F,
           0x00000080, 0x000000FF, 0x00001000, 0x0000FFFF, 0x7FFFFFFF,
           0x80000000, 0xFFFFFFFF, 0xDEADBE80, 0x12345678)

# (width, destination register, how C reads it back, the mask the `and` uses)
_WIDTHS = ((1, "al", "LO8(eax)", 0x0F),
           (2, "ax", "LO16(eax)", 0x0FFF),
           (4, "eax", "eax", 0x0000000F))

# What the clobber writes. Deliberately nonzero and positive, so a condition
# that reads it instead of the result gets `jne` taken and `js` not taken --
# the opposite of the right answer for a good half of the sweep.
_CLOBBER = 0x99


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _lift(mnemonic, operands, op_str=""):
    insn = Instruction(0, 3, mnemonic, op_str, "", operands=operands)
    return " ".join(Lifter().lift_instruction(insn))


class ResultClobberTest(unittest.TestCase):
    """The condition must not mention the destination it is about."""

    def test_condition_does_not_read_the_live_destination(self):
        for width, lo, dest_read, mask in _WIDTHS:
            for mnemonic, ops in (
                    ("and", [_reg(lo), _imm(mask)]),
                    ("or", [_reg(lo), _imm(0)]),
                    ("xor", [_reg(lo), _imm(mask)]),
                    ("add", [_reg(lo), _imm(1)]),
                    ("sub", [_reg(lo), _imm(1)]),
                    ("neg", [_reg(lo)]),
                    ("shl", [_reg(lo), _reg("cl")]),
                    ("sar", [_reg(lo), _reg("cl")])):
                for jcc in ("je", "jne", "js", "jns"):
                    made = _make_condition(jcc, mnemonic, ops)
                    if made is None:
                        continue
                    with self.subTest(w=width, m=mnemonic, jcc=jcc):
                        self.assertNotIn(dest_read, made[0])
                        self.assertIn("_fa", made[0])

    def test_the_setter_publishes_what_the_branch_reads(self):
        """Every setter whose condition reads _fa must also write it."""
        for width, lo, _dest, mask in _WIDTHS:
            for mnemonic, ops in (("and", [_reg(lo), _imm(mask)]),
                                  ("or", [_reg(lo), _imm(1)]),
                                  ("add", [_reg(lo), _imm(1)]),
                                  ("sub", [_reg(lo), _imm(1)]),
                                  ("neg", [_reg(lo)]),
                                  ("adc", [_reg(lo), _imm(1)]),
                                  ("sbb", [_reg(lo), _imm(1)]),
                                  ("shl", [_reg(lo), _reg("cl")]),
                                  ("shr", [_reg(lo), _reg("cl")]),
                                  ("sar", [_reg(lo), _reg("cl")])):
                with self.subTest(w=width, m=mnemonic):
                    self.assertIn("_fa =", _lift(mnemonic, ops))

    def test_cross_block_join_reads_the_snapshot(self):
        """The shape G19 was found in: setter and branch in different blocks."""
        incoming = _merge_predecessor_flag_states(
            [("and", [_reg("eax"), _imm(0x0F)]), ("sub", [_reg("eax"), _imm(1)])])
        jump = Instruction(0x1000, 2, "jne", "0x1100", "7415")
        jump.jump_target = 0x1100
        lifter = Lifter()
        lifter.func_start, lifter.func_end = 0x0F00, 0x1200
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0x1000, instructions=[jump]),
            flag_state=incoming)
        joined = "\n".join(lifted)
        self.assertIn("_fa", joined)
        self.assertNotIn("(eax !=", joined)


class ResultClobberRuntimeTest(unittest.TestCase):
    """Compile the lifter's own output and run it against x86's answer."""

    def _source(self):
        cases, calls = [], []
        for width, lo, dest_read, mask in _WIDTHS:
            write = _lift("and", [_reg(lo), _imm(mask)], f"{lo}, {mask:#x}")
            clobber = _lift("mov", [_reg(lo), _imm(_CLOBBER)],
                            f"{lo}, {_CLOBBER:#x}")
            for jcc, op in (("je", "=="), ("jne", "!="),
                            ("js", "<"), ("jns", ">=")):
                made = _make_condition(jcc, "and", [_reg(lo), _imm(mask)])
                if made is None:
                    continue
                name = f"case_{width}_{jcc}"
                # x86's own answer, computed from the AND's result at this
                # width, in a form no clobber can reach.
                sign = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[width]
                ref = (f"(({sign})(uint32_t)(a & {mask:#x}u) {op} 0)"
                       if jcc in ("js", "jns")
                       else f"((uint32_t)(a & {mask:#x}u) {op} 0)")
                cases.append(
                    f"static int {name}(uint32_t a) {{\n"
                    f"    eax = a; ecx = 1; _cf = 0;\n"
                    f"    {write}\n"
                    f"    {clobber}\n"
                    f"    return ({made[0]}) ? 1 : 0;\n"
                    f"}}\n"
                    f"static int ref_{name}(uint32_t a) {{ return {ref} ? 1 : 0; }}")
                calls.append(
                    f"        {{ uint32_t a = values[i];\n"
                    f"          int got = {name}(a), want = ref_{name}(a);\n"
                    f"          if (got != want) {{ failures++;\n"
                    f'            printf("and+mov+{jcc} w{width} a=0x%08X got %d want %d\\n",'
                    f" a, got, want); }} }}")
        return (_HARNESS
                .replace("/*CASES*/", "\n\n".join(cases))
                .replace("/*CALLS*/", "\n".join(calls))
                .replace("/*VALUES*/", ", ".join(f"0x{v:08X}u" for v in _VALUES)))

    def _build_and_run(self, source):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        with tempfile.TemporaryDirectory(prefix="result-clobber-") as tmp:
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

    def test_the_branch_survives_a_clobbered_destination(self):
        ran = self._build_and_run(self._source())
        self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)

    def test_negative_control_reading_the_destination_fails(self):
        """The old expression, substituted back, must be caught by this sweep.

        Without this the suite could pass because the sweep is blind rather
        than because the fix works.
        """
        source = self._source()
        # Put the pre-G19 expression back: read the live destination.
        broken = (source
                  .replace("((int8_t)(_fa)", "((int8_t)(LO8(eax))")
                  .replace("((int16_t)(_fa)", "((int16_t)(LO16(eax))")
                  .replace("((int32_t)(_fa)", "((int32_t)(eax)")
                  .replace("(_fa == 0)", "(LO8(eax) == 0)")
                  .replace("(_fa != 0)", "(LO8(eax) != 0)"))
        self.assertNotEqual(source, broken, "the substitution matched nothing")
        ran = self._build_and_run(broken)
        self.assertEqual(1, ran.returncode,
                         "reading the live destination should disagree with "
                         "x86 once a mov has overwritten it, and did not:\n"
                         + ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()


class DeclarationCoverageTest(unittest.TestCase):
    """Every mnemonic that EMITS the snapshot must be DECLARED for.

    These two lists live in different files and drifted the moment the fix was
    written: `sal` shares _lift_shift with `shl`, so it emits `_fa = ...`, but
    it is not in _RESULT_ZF_SF_SETTERS -- it never becomes a flag setter, so
    nothing reads it -- and a function whose only member of the family was a
    `sal` would not have compiled. Nothing in the unit tests would have caught
    that; it needed a real function shaped that way.

    So this walks every mnemonic the dispatcher knows and checks the two ends
    agree: if lifting it mentions `_fa =`, the declaration gate must fire for a
    function containing it.
    """

    # One plausible operand shape per mnemonic the dispatcher routes.
    _CANDIDATES = {
        "add": 2, "sub": 2, "and": 2, "or": 2, "xor": 2, "adc": 2, "sbb": 2,
        "inc": 1, "dec": 1, "neg": 1, "not": 1,
        "shl": 2, "sal": 2, "shr": 2, "sar": 2, "rol": 2, "ror": 2,
        "rcl": 2, "rcr": 2, "shld": 3, "shrd": 3,
        "mov": 2, "lea": 2, "imul": 2, "test": 2, "cmp": 2,
    }

    def test_every_emitter_is_declared_for(self):
        src = (Path(__file__).resolve().parent / "translator.py").read_text()
        gate = src.split("uint32_t _fa = 0, _fb = 0;")[0].rsplit("if any(", 1)[1]
        from .lifter import _RESULT_ZF_SF_SETTERS
        named = set(_RESULT_ZF_SF_SETTERS)
        for token in gate.replace('"', " ").replace(",", " ").split():
            named.add(token)

        for mnemonic, argc in sorted(self._CANDIDATES.items()):
            ops = [_reg("eax"), _reg("ebx"), _reg("cl")][:argc]
            if mnemonic in ("shl", "sal", "shr", "sar", "rol", "ror",
                            "rcl", "rcr"):
                ops = [_reg("eax"), _reg("cl")]
            try:
                lifted = _lift(mnemonic, ops)
            except Exception:                      # not a shape it accepts
                continue
            if "_fa =" not in lifted:
                continue
            with self.subTest(mnemonic=mnemonic):
                self.assertIn(
                    mnemonic, named,
                    f"{mnemonic} emits a result snapshot but the declaration "
                    f"gate in translator.py does not name it, so a function "
                    f"whose only such instruction is a {mnemonic} emits "
                    f"C that references an undeclared _fa")
