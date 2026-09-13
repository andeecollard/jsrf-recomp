"""EFLAGS snapshots carried through a control-flow merge.

The loader routine at JSRF 0x0013D3D2 compares the same logical quantities
through two paths, using different registers on each path, then joins at one
``jle``.  Both compares publish their result through the lifter's shared
``_fa/_fb/_fas/_fbs`` snapshot, so the joined branch can consume that snapshot
even though the static operand objects differ.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import (Lifter, lift_basic_block, _make_condition,
                     MERGED_ZF_CMP, MERGED_ZF_TEST)
from .translator import _merge_predecessor_flag_states


def _cmp_state(lhs, rhs, width=4):
    def operand(name):
        if name.startswith("mem:"):
            return Operand(type="mem", mem_base=name[4:], mem_size=width)
        return Operand(type="reg", reg=name)

    return "cmp", [operand(lhs), operand(rhs)]


class FlagMergeLifterTest(unittest.TestCase):
    def test_diamond_cmp_merge_uses_the_runtime_snapshot(self):
        incoming = _merge_predecessor_flag_states([
            _cmp_state("eax", "edx"),
            _cmp_state("edx", "ecx"),
        ])
        jump = Instruction(0x0013D3F3, 2, "jle", "0x13d3fb", "7e06")
        jump.jump_target = 0x0013D3FB

        lifter = Lifter()
        lifter.func_start = 0x0013D300
        lifter.func_end = 0x0013D500
        lifted, _ = lift_basic_block(
            lifter,
            BasicBlock(start=jump.address, instructions=[jump]),
            flag_state=incoming,
        )
        generated = "\n".join(lifted)

        self.assertIn(
            "if (CMP_LE(_fas, _fbs)) goto loc_0013D3FB;", generated)
        self.assertNotIn("_flags", generated)

    def test_incompatible_flag_setters_remain_unknown(self):
        incoming = _merge_predecessor_flag_states([
            _cmp_state("eax", "edx"),
            ("test", [Operand(type="reg", reg="eax"),
                      Operand(type="reg", reg="eax")]),
        ])

        self.assertIsNone(incoming)

    def test_different_snapshot_widths_keep_zf_and_drop_the_rest(self):
        """Width disagreement costs the width-sensitive conditions, not ZF.

        Each predecessor masked _fa/_fb to its OWN width where its compare
        happened, so at the join the pair is correctly masked whichever path
        ran and `_fa == _fb` is that path's ZF. What genuinely needs the width
        is the sign: js/jns and the signed ordered conditions cast the
        subtraction back to the operand width, and there is no single width to
        cast to here.

        This used to return None outright, which cost sub_00130FD0 five `je`
        branches -- each an 8-bit `cmp byte ptr [...], dl` joined with a 32-bit
        `cmp [eax-0x80], edx`, every one of them emitted as constant false.
        """
        incoming = _merge_predecessor_flag_states([
            _cmp_state("mem:esi", "eax", width=2),
            _cmp_state("mem:edi", "ecx", width=4),
        ])

        self.assertIsNotNone(incoming)
        self.assertEqual(incoming[0], MERGED_ZF_CMP)

        # ZF is answerable from the snapshot.
        for jcc, macro in (("je", "CMP_EQ"), ("jne", "CMP_NE")):
            with self.subTest(jcc=jcc):
                cond = _make_condition(jcc, incoming[0], incoming[1])
                self.assertIsNotNone(cond)
                self.assertIn(f"{macro}(_fa, _fb)", cond[0])

        # Everything that needs the width, or a flag other than ZF, must still
        # refuse rather than pick one predecessor's answer.
        for jcc in ("js", "jns", "jl", "jge", "jg", "jle", "jb", "jae"):
            with self.subTest(jcc=jcc):
                self.assertIsNone(
                    _make_condition(jcc, incoming[0], incoming[1]))

    def test_a_test_join_answers_zf_with_the_test_macro(self):
        incoming = _merge_predecessor_flag_states([
            ("test", [Operand(type="mem", mem_base="esi", mem_size=1),
                      Operand(type="reg", reg="al")]),
            ("test", [Operand(type="reg", reg="ecx"),
                      Operand(type="reg", reg="ecx")]),
        ])

        self.assertEqual(incoming[0], MERGED_ZF_TEST)
        cond = _make_condition("je", incoming[0], incoming[1])
        self.assertIn("TEST_Z(_fa, _fb)", cond[0])


if __name__ == "__main__":
    unittest.main()
