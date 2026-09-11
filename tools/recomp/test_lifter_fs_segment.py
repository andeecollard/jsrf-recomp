import unittest

from .disasm import Disassembler
from .lifter import Lifter


class FsSegmentLifterTest(unittest.TestCase):
    def _decode(self, raw, address=0x1000):
        disassembler = Disassembler()
        native = next(disassembler._cs.disasm(bytes.fromhex(raw), address))
        return disassembler._decode_instruction(native)

    def test_disassembler_retains_fs_override(self):
        instruction = self._decode("64a128000000")  # mov eax, fs:[0x28]

        self.assertEqual("fs", instruction.operands[1].mem_seg)
        self.assertEqual(0x28, instruction.operands[1].mem_disp)

    def test_fs_read_uses_thread_segment_accessor(self):
        instruction = self._decode("64a128000000")

        self.assertEqual(
            ["eax = FS_MEM32(0x28);"],
            Lifter().lift_instruction(instruction),
        )

    def test_fs_write_uses_thread_segment_accessor(self):
        instruction = self._decode("64890d00000000")  # mov fs:[0], ecx

        self.assertEqual(
            ["RECOMP_MEM_WRITE32(0x00001000u, 0x00000000u, "
             "g_fs_base + (uint32_t)(0), ecx);"],
            Lifter().lift_instruction(instruction),
        )

    def test_flat_memory_stays_flat(self):
        instruction = self._decode("a128000000")  # mov eax, [0x28]

        self.assertEqual(
            ["eax = MEM32(0x28);"],
            Lifter().lift_instruction(instruction),
        )

    def test_signed_fs_load_keeps_both_segment_and_extension(self):
        instruction = self._decode("640fbe0524000000")  # movsx eax, fs:[0x24]

        self.assertEqual(
            ["eax = (uint32_t)(int32_t)FS_SMEM8(0x24);"],
            Lifter().lift_instruction(instruction),
        )


if __name__ == "__main__":
    unittest.main()
