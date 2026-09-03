"""MMX moves must move bytes, not become comments.

D3D's block copy (JSRF sub_00198FD0) is, per 64-byte iteration:

    movq   mm0..mm7, [esi+n]
    movntq [edi+n], mm0..mm7

`movq` was in the SSE mnemonic list, so an MMX form fell out of that handler as
"/* SSE: movq mm0, ... */"; `movntq` had no rule and became a TODO. The memcpy
therefore copied its rep-movsd prologue and epilogue and silently dropped every
64-byte block in between.
"""

import unittest

from .disasm import Disassembler, Instruction, Operand
from .lifter import Lifter


def _mm(n):
    return Operand(type="reg", reg=f"mm{n}")


def _mem(base, disp=0):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=8)


def _insn(mnemonic, operands, op_str=""):
    insn = Instruction(0, 4, mnemonic, op_str, "")
    insn.operands = operands
    return insn


class MmxLifterTest(unittest.TestCase):
    def test_capstone_representation_matches_jsrf_d3d_block_copy(self):
        """0x00199008 and 0x00199027 in JSRF's D3D section."""
        cs = Disassembler()._cs
        load = list(cs.disasm(bytes.fromhex("0f6f06"), 0x00199008))
        store = list(cs.disasm(bytes.fromhex("0fe707"), 0x00199027))
        self.assertEqual("movq", load[0].mnemonic)
        self.assertEqual("mm0, qword ptr [esi]", load[0].op_str)
        self.assertEqual("movntq", store[0].mnemonic)
        self.assertEqual("qword ptr [edi], mm0", store[0].op_str)

    def test_mmx_load_reads_64_bits_of_memory(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn("movq", [_mm(0), _mem("esi")], "mm0, qword ptr [esi]")))

        self.assertNotIn("TODO", generated)
        self.assertNotIn("SSE:", generated)
        self.assertIn("mm0 = MMX_MEM(esi);", generated)

    def test_movntq_stores_64_bits(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn("movntq", [_mem("edi", 8), _mm(1)],
                  "qword ptr [edi + 8], mm1")))

        self.assertNotIn("TODO", generated)
        self.assertIn("MMX_STORE(edi + 8, mm1);", generated)

    def test_register_to_register_move(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn("movq", [_mm(2), _mm(3)], "mm2, mm3")))

        self.assertIn("mm2 = mm3;", generated)

    def test_xmm_movq_is_still_handled_by_the_sse_path(self):
        """The MMX rule must not steal movq from SSE."""
        xmm = Operand(type="reg", reg="xmm0")
        generated = "\n".join(Lifter().lift_instruction(
            _insn("movq", [xmm, _mem("esi")], "xmm0, qword ptr [esi]")))

        self.assertNotIn("MEM64", generated)

    def test_emms_and_femms_are_not_todos(self):
        for m in ("emms", "femms"):
            generated = "\n".join(Lifter().lift_instruction(_insn(m, [], "")))
            self.assertNotIn("TODO", generated, m)


if __name__ == "__main__":
    unittest.main()
