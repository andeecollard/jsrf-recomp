"""bsf/bsr must produce a value, not a comment.

The Xbox D3D library's Log2 helper is a single `bsf eax, ecx` (JSRF has it at
0x00192B10). Unhandled, it lifted to `/* TODO: bsf eax, ecx */` and the helper
returned whatever eax already held, so every surface size derived from
log2(width)+log2(height)+log2(depth) was garbage.
"""

import unittest

from .disasm import Disassembler, Instruction, Operand
from .lifter import Lifter


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base, size=4):
    return Operand(type="mem", mem_base=base, mem_size=size)


def _insn(mnemonic, operands, op_str=""):
    insn = Instruction(0, 4, mnemonic, op_str, "")
    insn.operands = operands
    return insn


class BitScanLifterTest(unittest.TestCase):
    def test_capstone_representation_matches_jsrf_d3d_log2(self):
        """0x00192B19 in JSRF's D3D section, the body of the Log2 helper."""
        decoded = list(Disassembler()._cs.disasm(bytes.fromhex("0fbcc1"),
                                                 0x00192B19))
        self.assertEqual(1, len(decoded))
        self.assertEqual("bsf", decoded[0].mnemonic)
        self.assertEqual("eax, ecx", decoded[0].op_str)

    def test_bsf_scans_the_source_into_the_destination(self):
        generated = "\n".join(
            Lifter().lift_instruction(_insn("bsf", [_reg("eax"), _reg("ecx")],
                                            "eax, ecx")))

        self.assertNotIn("TODO", generated)
        self.assertIn("BSF32(ecx)", generated)
        self.assertIn("eax =", generated)

    def test_bsr_uses_the_reverse_helper(self):
        generated = "\n".join(
            Lifter().lift_instruction(_insn("bsr", [_reg("edx"), _reg("esi")],
                                            "edx, esi")))

        self.assertNotIn("TODO", generated)
        self.assertIn("BSR32(esi)", generated)
        self.assertIn("edx =", generated)

    def test_zero_source_leaves_the_destination_unmodified(self):
        """x86 sets ZF and does not write the destination when src is 0.

        Without the guard the helper would loop off the end of the word, and
        any value written would be one the hardware never produces.
        """
        generated = "\n".join(
            Lifter().lift_instruction(_insn("bsf", [_reg("eax"), _reg("ecx")],
                                            "eax, ecx")))

        self.assertIn("if ((ecx) != 0)", generated)

    def test_memory_source_is_supported(self):
        generated = "\n".join(
            Lifter().lift_instruction(
                _insn("bsf", [_reg("eax"), _mem("ebx")],
                      "eax, dword ptr [ebx]")))

        self.assertNotIn("TODO", generated)
        self.assertIn("BSF32(MEM32(ebx))", generated)


if __name__ == "__main__":
    unittest.main()
