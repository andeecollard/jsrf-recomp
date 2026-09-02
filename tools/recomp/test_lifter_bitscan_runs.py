"""
Bit-scan lifting, in the form that RUNS in this environment.

tools/recomp/test_lifter_bit_scan.py covers the same ground and is the
upstream file, but it is pytest-style and imports tools.recomp.disasm, which
needs capstone -- and the interpreter that has pytest here does not have
capstone, while the one that has capstone does not have pytest. It therefore
errors during collection and asserts nothing.

This is the unittest-style twin, run by the same discovery the rest of
tools/recomp uses:

    /usr/bin/python3 -m unittest discover -s tools/recomp -t . -p "test_*.py"

It replaces an earlier local implementation that emitted BSF32/BSR32 helpers.
Upstream's inline form was kept on the merge because it handles the operand
width (a 16-bit bsf must scan 16 bits, not 32); these assertions are written
against that form so the two cannot drift apart silently.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Instruction, Operand  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def _reg(name):
    """Register width comes from the NAME (see lifter._REG_WIDTH), not a field:
    "ax" is two bytes, "eax" four."""
    return Operand(type="reg", reg=name)


def _lift(mnemonic, op_str, ops):
    insn = Instruction(0, 3, mnemonic, op_str, "0fbcc1")
    insn.operands = ops
    return "\n".join(Lifter().lift_instruction(insn))


class BitScanLifting(unittest.TestCase):

    def test_bsf_scans_upward_from_zero(self):
        out = _lift("bsf", "eax, ecx", [_reg("eax"), _reg("ecx")])
        self.assertNotIn("TODO", out)
        self.assertIn("_bs_index = 0;", out)
        self.assertIn("++_bs_index", out)
        self.assertIn("eax = _bs_index;", out)

    def test_bsr_scans_downward_from_the_top_bit(self):
        out = _lift("bsr", "esi, edx", [_reg("esi"), _reg("edx")])
        self.assertIn("_bs_index = 31;", out)
        self.assertIn("--_bs_index", out)
        self.assertIn("esi = _bs_index;", out)

    def test_zero_source_leaves_the_destination_alone(self):
        # x86 sets ZF and does NOT write the destination when the source is
        # zero. The Xbox D3D Log2 helper is a bare `bsf eax, ecx`, so inventing
        # a value here would feed a wrong surface size into
        # MmAllocateContiguousMemoryEx -- the original symptom was JSRF asking
        # for 0x08000000 bytes, twice the console's RAM.
        out = _lift("bsf", "eax, ecx", [_reg("eax"), _reg("ecx")])
        self.assertIn("if (_bs_value != 0)", out)
        write_pos = out.index("eax = _bs_index;")
        guard_pos = out.index("if (_bs_value != 0)")
        self.assertLess(guard_pos, write_pos,
                        "the destination write must sit inside the zero guard")

    def test_sixteen_bit_source_is_masked_to_sixteen_bits(self):
        # The reason upstream's implementation was kept over the local one:
        # a 16-bit scan must not see the high half of the register.
        out = _lift("bsr", "ax, cx", [_reg("ax"), _reg("cx")])
        self.assertIn("(uint16_t)", out)
        self.assertIn("_bs_index = 15;", out)

    def test_zero_flag_is_published_for_a_following_branch(self):
        lifter = Lifter()
        lifter.needs_flags = True
        insn = Instruction(0, 3, "bsf", "eax, ecx", "0fbcc1")
        insn.operands = [_reg("eax"), _reg("ecx")]
        out = "\n".join(lifter.lift_instruction(insn))
        self.assertIn("_flags = (_bs_value == 0);", out)


if __name__ == "__main__":
    unittest.main()
