"""x86 edges a C operator does not reproduce: conversion rounding, and shifts.

Found auditing for more of the shape that froze the Corn tutorial -- an x86
instruction emitted as the nearest C operator, where C disagrees at an edge.
See test_lifter_comiss.py for the original.

  * CVTSS2SI ROUNDS under MXCSR (default nearest-even); only CVTTSS2SI
    truncates. Both were emitted as `(int32_t)x`, so every CVTSS2SI silently
    became a truncation -- wrong for ordinary values, not just at the edges.
    The PACKED path already distinguished them, which is what marks the scalar
    collapse as an oversight rather than a decision.

  * A float -> int cast of NaN, an infinity, or an out-of-range value is
    UNDEFINED BEHAVIOUR in C, and AArch64 saturates (NaN gives 0, +inf gives
    INT_MAX) where x86 yields the integer indefinite 0x80000000.

  * x86 masks a shift count to 5 bits, so `shl eax, 32` is a no-op. C leaves
    a shift of 32 or more undefined and the compiler may assume it cannot
    happen. 339 sites in JSRF take their count from cl.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _insn(mnemonic, text, operands, size=4):
    insn = Instruction(0, size, mnemonic, text, "")
    insn.operands = operands
    return insn


def _lift_one(insn):
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[insn]))
    return "\n".join(lifted)


def _cvt(mnemonic):
    return _lift_one(_insn(mnemonic, "eax, xmm0",
                           [Operand(type="reg", reg="eax"),
                            Operand(type="reg", reg="xmm0")]))


def _shift(mnemonic, count_operand):
    return _lift_one(_insn(mnemonic, f"eax, {count_operand[1]}",
                           [Operand(type="reg", reg="eax"),
                            count_operand[0]]))


CL = (Operand(type="reg", reg="cl"), "cl")
IMM4 = (Operand(type="imm", imm=4), "4")


class ConversionRoundingTest(unittest.TestCase):
    def test_cvttss2si_truncates(self):
        self.assertIn("RECOMP_F2I_TRUNC(", _cvt("cvttss2si"))

    def test_cvtss2si_rounds_rather_than_truncating(self):
        out = _cvt("cvtss2si")
        self.assertIn("RECOMP_F2I_ROUND(", out)
        self.assertNotIn("RECOMP_F2I_TRUNC(", out)

    def test_the_two_forms_are_not_emitted_identically(self):
        # The whole bug was that they were. Guard the distinction directly.
        self.assertNotEqual(_cvt("cvtss2si"), _cvt("cvttss2si"))

    def test_double_forms_match_their_single_counterparts(self):
        self.assertIn("RECOMP_F2I_ROUND(", _cvt("cvtsd2si"))
        self.assertIn("RECOMP_F2I_TRUNC(", _cvt("cvttsd2si"))

    def test_no_bare_cast_survives(self):
        # A bare `(int32_t)` is the undefined-behaviour form this replaced.
        for m in ("cvtss2si", "cvttss2si", "cvtsd2si", "cvttsd2si"):
            with self.subTest(mnemonic=m):
                self.assertNotIn("(int32_t)xmm0", _cvt(m))


class ShiftCountMaskingTest(unittest.TestCase):
    def test_register_count_is_masked(self):
        for m in ("shl", "shr", "sar"):
            with self.subTest(mnemonic=m):
                self.assertIn("& 31", _shift(m, CL))

    def test_immediate_count_is_masked_too(self):
        # Harmless for a small immediate (it folds), and it keeps the emission
        # uniform so a future reader cannot conclude only one form is guarded.
        self.assertIn("& 31", _shift("shl", IMM4))

    def test_sar_keeps_its_signed_shift(self):
        out = _shift("sar", CL)
        self.assertIn("(int32_t)", out)
        self.assertIn("& 31", out)


if __name__ == "__main__":
    unittest.main()
