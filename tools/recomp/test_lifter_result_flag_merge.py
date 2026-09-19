"""A jcc joined from flag setters of different kinds.

MSVC lowers a signed remainder by a power of two as a mask that keeps the sign
bit, then a sign-extension of the remainder on the negative path only:

    and eax, 0x800007FF
    jns  L
    dec  eax
    or   eax, 0xFFFFF800
    inc  eax
L:  jne  ...

The branch at L asks one question -- is the remainder zero -- and both edges
answer it in ZF.  They just answer with different instructions, an ``and`` on
one and an ``inc`` on the other, so the merge used to give up and the branch
fell to the dead ``_flags`` fallback, which is a constant zero.

JSRF is where that mattered.  sub_001403B0 is wxCiReqRd, the WXCI/XB DVD
sector-read request; it runs this idiom twice, on the read length and on the
seek position, at 0x001404DB and 0x001404F1.  With the second ``je`` forced
not-taken every read request the title issued fell into
"E0109152:illegal seek position." and returned zero.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block, MERGED_RESULT_SETTER
from .translator import _merge_predecessor_flag_states


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _lift_join(states, jcc, target=0x00140508, address=0x001404F1):
    incoming = _merge_predecessor_flag_states(states)
    jump = Instruction(address, 2, jcc, hex(target), "7415")
    jump.jump_target = target
    lifter = Lifter()
    lifter.func_start = 0x001403B0
    lifter.func_end = 0x00140540
    lifted, _ = lift_basic_block(
        lifter,
        BasicBlock(start=address, instructions=[jump]),
        flag_state=incoming,
    )
    return incoming, "\n".join(lifted)


class ResultFlagMergeTest(unittest.TestCase):
    def test_wxcireqrd_seek_check_resolves(self):
        """0x001404F1: `and edi, mask` joined with `inc edi`."""
        incoming, generated = _lift_join(
            [("and", [_reg("edi"), _imm(0x800007FF)]),
             ("inc", [_reg("edi")])],
            "je",
        )

        self.assertEqual(incoming[0], MERGED_RESULT_SETTER)
        # G19: the join reads the snapshot both edges published, not live edi.
        # A `mov edi, ...` between either setter and this branch used to make
        # it ask about the wrong value; _fa cannot be clobbered that way.
        self.assertIn("if ((_fa == 0)) goto loc_00140508;", generated)
        self.assertNotIn("_flags", generated)

    def test_wxcireqrd_length_check_resolves(self):
        """0x001404DB: the same join on eax, read by `jne`."""
        _, generated = _lift_join(
            [("and", [_reg("eax"), _imm(0x800007FF)]),
             ("inc", [_reg("eax")])],
            "jne",
            target=0x00140524,
            address=0x001404DB,
        )

        self.assertIn("if ((_fa != 0)) goto loc_00140524;", generated)
        self.assertNotIn("_flags", generated)

    def test_sign_condition_uses_the_operand_width(self):
        """SF is the destination's top bit, not bit 31 of a widened byte."""
        _, generated = _lift_join(
            [("and", [_reg("al"), _imm(0x7F)]), ("dec", [_reg("al")])],
            "js",
        )

        self.assertIn("(int8_t)", generated)
        self.assertNotIn("_flags", generated)

    def test_different_destinations_stay_unknown(self):
        self.assertIsNone(_merge_predecessor_flag_states(
            [("and", [_reg("eax"), _imm(1)]), ("inc", [_reg("ecx")])]))

    def test_carry_condition_stays_unknown(self):
        """`and` clears CF, `inc` leaves it alone: the edges disagree."""
        incoming, generated = _lift_join(
            [("and", [_reg("edi"), _imm(0x800007FF)]),
             ("inc", [_reg("edi")])],
            "jb",
        )

        self.assertEqual(incoming[0], MERGED_RESULT_SETTER)
        self.assertIn("_flags", generated)

    def test_non_writing_setter_stays_unknown(self):
        """cmp writes no destination, so reading it back is the wrong value."""
        self.assertIsNone(_merge_predecessor_flag_states(
            [("cmp", [_reg("eax"), _imm(1)]), ("inc", [_reg("eax")])]))

    def test_undefined_flag_setter_stays_unknown(self):
        """imul leaves ZF and SF undefined."""
        self.assertIsNone(_merge_predecessor_flag_states(
            [("imul", [_reg("eax"), _reg("ecx")]), ("inc", [_reg("eax")])]))


if __name__ == "__main__":
    unittest.main()
