import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class CarryLifterTest(unittest.TestCase):
    def test_neg_carry_feeds_adjacent_sbb(self):
        neg = Instruction(0, 2, "neg", "esi", "f7de")
        neg.operands = [Operand(type="reg", reg="esi")]
        sbb = Instruction(2, 2, "sbb", "esi, esi", "19f6")
        sbb.operands = [
            Operand(type="reg", reg="esi"),
            Operand(type="reg", reg="esi"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((esi) != 0);", generated)
        self.assertIn(
            "esi = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */",
            generated,
        )

    def test_cmp_sets_carry_for_a_following_sbb(self):
        # MSVC's branchless tolower inside _stricmp:
        #     sub al, 0x41 ; cmp al, 0x1A ; sbb cl, cl ; and cl, 0x20
        # sbb wants the carry from the cmp (is-a-letter). When cmp left CF
        # alone, sbb read the sub's carry instead, no uppercase letter was
        # ever folded, and _stricmp behaved like strcmp.
        sub = Instruction(0, 2, "sub", "al, 0x41", "2c41")
        sub.operands = [
            Operand(type="reg", reg="al"),
            Operand(type="imm", imm=0x41),
        ]
        cmp = Instruction(2, 2, "cmp", "al, 0x1a", "3c1a")
        cmp.operands = [
            Operand(type="reg", reg="al"),
            Operand(type="imm", imm=0x1A),
        ]
        sbb = Instruction(4, 2, "sbb", "cl, cl", "1ac9")
        sbb.operands = [
            Operand(type="reg", reg="cl"),
            Operand(type="reg", reg="cl"),
        ]

        # needs_cf is what the translator sets for any function containing
        # adc/sbb, which is exactly the case that reads CF.
        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[sub, cmp, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)(_fa < _fb);", generated)
        # and it must land between the cmp and the sbb, not before the cmp
        cmp_at = generated.index("/* cmp ")
        cf_at = generated.index("_cf = (int)(_fa < _fb);")
        sbb_at = generated.index("sbb self (CF extend)")
        self.assertLess(cmp_at, cf_at)
        self.assertLess(cf_at, sbb_at)

    def test_test_clears_carry_for_a_following_sbb(self):
        tst = Instruction(0, 2, "test", "al, al", "84c0")
        tst.operands = [
            Operand(type="reg", reg="al"),
            Operand(type="reg", reg="al"),
        ]
        sbb = Instruction(2, 2, "sbb", "cl, cl", "1ac9")
        sbb.operands = [
            Operand(type="reg", reg="cl"),
            Operand(type="reg", reg="cl"),
        ]

        lifter = Lifter()
        lifter.needs_cf = True
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[tst, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = 0;", generated)

    def test_neg_carry_feeds_adjacent_adc(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        adc = Instruction(2, 3, "adc", "edx, 0", "83d200")
        adc.operands = [
            Operand(type="reg", reg="edx"),
            Operand(type="imm", imm=0),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, adc]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((eax) != 0);", generated)
        self.assertIn("+ (uint64_t)_cf;", generated)
        self.assertIn("edx = (uint32_t)_t;", generated)

    def test_neg_carry_feeds_sbb_across_push(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        push = Instruction(2, 1, "push", "edi", "57")
        push.operands = [Operand(type="reg", reg="edi")]
        sbb = Instruction(3, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, push, sbb]))
        generated = "\n".join(lifted)

        self.assertIn("_cf = (int)((eax) != 0);", generated)
        self.assertIn(
            "eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */",
            generated,
        )

    def test_neg_carry_not_preserved_across_flag_setter(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        add = Instruction(2, 3, "add", "ecx, 1", "83c101")
        add.operands = [
            Operand(type="reg", reg="ecx"),
            Operand(type="imm", imm=1),
        ]
        sbb = Instruction(5, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, add, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)

    def test_neg_carry_not_preserved_across_branch(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        jmp = Instruction(2, 2, "jmp", "0x10", "eb0c")
        jmp.jump_target = 0x10
        sbb = Instruction(4, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, jmp, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)

    def test_neg_carry_not_preserved_across_popfd(self):
        neg = Instruction(0, 2, "neg", "eax", "f7d8")
        neg.operands = [Operand(type="reg", reg="eax")]
        popfd = Instruction(2, 1, "popfd", "", "9d")
        sbb = Instruction(3, 2, "sbb", "eax, eax", "19c0")
        sbb.operands = [
            Operand(type="reg", reg="eax"),
            Operand(type="reg", reg="eax"),
        ]

        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[neg, popfd, sbb]))
        generated = "\n".join(lifted)

        self.assertNotIn("_cf = (int)((eax) != 0);", generated)


if __name__ == "__main__":
    unittest.main()
