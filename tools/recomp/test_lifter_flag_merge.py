"""EFLAGS snapshots carried through a control-flow merge.

The loader routine at JSRF 0x0013D3D2 compares the same logical quantities
through two paths, using different registers on each path, then joins at one
``jle``.  Both compares publish their result through the lifter's shared
``_fa/_fb/_fas/_fbs`` snapshot, so the joined branch can consume that snapshot
even though the static operand objects differ.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
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

    def test_different_snapshot_widths_remain_unknown(self):
        incoming = _merge_predecessor_flag_states([
            _cmp_state("mem:esi", "eax", width=2),
            _cmp_state("mem:edi", "ecx", width=4),
        ])

        self.assertIsNone(incoming)


if __name__ == "__main__":
    unittest.main()
