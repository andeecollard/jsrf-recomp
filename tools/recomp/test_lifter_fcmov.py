"""FCMOVcc: the branchless half of the x87 fminf/fmaxf idiom.

MSVC compiles fminf/fmaxf on a P6 target to

    fld dword ptr [esp+4]      ; st0 = a
    fld dword ptr [esp+8]      ; st0 = b, st1 = a
    fcom st(1)
    xor eax, eax
    fnstsw ax
    test ah, 1                 ; C0: st0 < st1, i.e. b < a
    fcmove st(0), st(1)        ; fmin: keep a when b >= a
    fxch st(1)
    fstp st(0)
    ret

The lifter handled every instruction in that sequence except the conditional
move, which fell through to the generic FPU case and became a bare
`/* FPU: fcmove st(0), st(1) */` comment. With the move gone the routine always
returns its second argument: JSRF's fminf (sub_0014C870) returned the 1.0f
clamp and its fmaxf (sub_0014C850) returned the 0.0f clamp, so the colour
packer sub_000A4CF0 wrote zero into all four channels of every object it
packed, on every frame.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import FCMOVCC_TO_JCC, Lifter, lift_basic_block


def _test_ah_1():
    """`test ah, 1` -- the flag setter the fmin/fmax idiom uses."""
    insn = Instruction(0, 3, "test", "ah, 1", "f6c401")
    insn.operands = [Operand(type="reg", reg="ah"),
                     Operand(type="imm", imm=1)]
    return insn


def _fcmov(mnemonic, src="st(1)"):
    insn = Instruction(3, 2, mnemonic, f"st(0), {src}", "")
    insn.operands = [Operand(type="reg", reg="st(0)"),
                     Operand(type="reg", reg=src)]
    return insn


class FcmovLifterTest(unittest.TestCase):
    def test_fcmove_moves_st1_into_st0_when_the_test_was_zero(self):
        lifted, _ = lift_basic_block(
            Lifter(),
            BasicBlock(start=0, instructions=[_test_ah_1(), _fcmov("fcmove")]))
        generated = "\n".join(lifted)

        self.assertIn(
            "if (TEST_Z(_fa, _fb)) { fp_top() = fp_st1(); }"
            " /* fcmove st(0), st(1) */",
            generated,
        )

    def test_fcmovne_takes_the_inverted_condition(self):
        lifted, _ = lift_basic_block(
            Lifter(),
            BasicBlock(start=0,
                       instructions=[_test_ah_1(), _fcmov("fcmovne")]))
        generated = "\n".join(lifted)

        self.assertIn(
            "if (TEST_NZ(_fa, _fb)) { fp_top() = fp_st1(); }"
            " /* fcmovne st(0), st(1) */",
            generated,
        )

    def test_the_source_register_is_the_explicit_operand_not_st0(self):
        # Capstone reports FCMOVcc with both operands, st(0) first. Reading
        # operands[0] would move st(0) onto itself -- a silent no-op, which is
        # exactly how `fxch st(i)` was broken before it was fixed.
        lifted, _ = lift_basic_block(
            Lifter(),
            BasicBlock(start=0,
                       instructions=[_test_ah_1(), _fcmov("fcmove", "st(3)")]))
        generated = "\n".join(lifted)

        self.assertIn("fp_top() = g_fp_stack[(g_fp_top + 3) & 7];", generated)

    def test_every_condition_form_is_translated(self):
        for mnemonic in sorted(FCMOVCC_TO_JCC):
            with self.subTest(mnemonic=mnemonic):
                lifted, _ = lift_basic_block(
                    Lifter(),
                    BasicBlock(start=0,
                               instructions=[_test_ah_1(), _fcmov(mnemonic)]))
                generated = "\n".join(lifted)
                self.assertIn(f"/* {mnemonic} st(0), st(1) */", generated)
                self.assertIn("fp_top() = fp_st1();", generated)
                self.assertNotIn("FPU: " + mnemonic, generated)

    def test_an_untracked_flag_setter_is_reported_not_dropped(self):
        # No flag setter precedes it, so the condition is unknown. The output
        # must say so; a bare `/* FPU: ... */` comment reads as "handled".
        self.assertEqual(
            Lifter().lift_instruction(_fcmov("fcmove")),
            ["/* FPU: fcmove st(0), st(1)"
             " - UNTRANSLATED CONDITIONAL MOVE, no tracked flag setter */"],
        )

    def test_fmin_sequence_returns_the_smaller_argument(self):
        """Whole-idiom check against the two JSRF sites.

        fmin keeps a when b >= a (ZF set by `test ah, 1`), then fxch/fstp
        discards the other operand. Both halves have to be present for the
        sequence to mean anything.
        """
        fld_a = Instruction(0, 4, "fld", "dword ptr [esp + 4]", "")
        fld_a.operands = [Operand(type="mem", mem_base="esp", mem_disp=4,
                                  mem_size=4)]
        fld_b = Instruction(4, 4, "fld", "dword ptr [esp + 8]", "")
        fld_b.operands = [Operand(type="mem", mem_base="esp", mem_disp=8,
                                  mem_size=4)]
        fcom = Instruction(8, 2, "fcom", "st(1)", "")
        fcom.operands = [Operand(type="reg", reg="st(1)")]
        # `xor eax, eax` sets flags, but `test ah, 1` is the last setter before
        # the move, so the condition still comes from the compare bits.
        xor = Instruction(10, 2, "xor", "eax, eax", "33c0")
        xor.operands = [Operand(type="reg", reg="eax"),
                        Operand(type="reg", reg="eax")]
        fnstsw = Instruction(12, 2, "fnstsw", "ax", "")
        fnstsw.operands = [Operand(type="reg", reg="ax")]
        fxch = Instruction(15, 2, "fxch", "st(0), st(1)", "")
        fxch.operands = [Operand(type="reg", reg="st(0)"),
                         Operand(type="reg", reg="st(1)")]
        fstp = Instruction(17, 2, "fstp", "st(0)", "")
        fstp.operands = [Operand(type="reg", reg="st(0)")]

        lifted, _ = lift_basic_block(
            Lifter(),
            BasicBlock(start=0, instructions=[
                fld_a, fld_b, fcom, xor, fnstsw, _test_ah_1(),
                _fcmov("fcmove"), fxch, fstp]))
        generated = "\n".join(lifted)

        self.assertIn("g_fp_cmp = RECOMP_FCMP(fp_top(), fp_st1());", generated)
        self.assertIn("eax = (eax & 0xFFFF0000u)", generated)
        move_at = generated.index("/* fcmove st(0), st(1) */")
        swap_at = generated.index("/* fxch st(0), st(1) */")
        self.assertLess(move_at, swap_at)


if __name__ == "__main__":
    unittest.main()
