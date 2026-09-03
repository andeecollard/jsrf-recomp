import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class StringCompareLifterTest(unittest.TestCase):
    def test_repne_scasb_scans_until_equal(self):
        scan = Instruction(
            0, 2, "repne scasb", "al, byte ptr es:[edi]", "f2ae")

        lifted = Lifter().lift_instruction(scan)
        generated = "\n".join(lifted)

        self.assertIn("while (ecx != 0)", generated)
        self.assertIn("_flags = (LO8(eax) == MEM8(edi));", generated)
        self.assertIn("edi += _st; ecx--;", generated)
        self.assertIn("if (_flags) break;", generated)
        self.assertLess(
            generated.index("edi += _st; ecx--;"),
            generated.index("if (_flags) break;"),
        )

    def test_repe_cmpsb_compares_bytes_and_feeds_equal_jump(self):
        compare = Instruction(
            0, 2, "repe cmpsb", "byte ptr [esi], byte ptr es:[edi]",
            "f3a6")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=1),
            Operand(type="mem", mem_base="edi", mem_size=1),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = (MEM8(esi) == MEM8(edi));", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        self.assertIn("if (!_flags) break;", generated)
        self.assertIn("if ((_flags != 0)) goto loc_00000010;", generated)

    def test_dword_compare_compares_dwords_and_feeds_equal_jump(self):
        """The dword width used to lift to a comment while cmpsb worked."""
    def test_dword_compare_sets_the_flags_its_jcc_reads(self):
        compare = Instruction(
            0, 2, "repe cmpsd", "dword ptr [esi], dword ptr es:[edi]",
            "f3a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=4),
            Operand(type="mem", mem_base="edi", mem_size=4),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertNotIn("TODO", generated)
        self.assertNotIn("- string compare, ecx iterations", generated)
        # Was a bare comment, so the jcc after it read whatever the previous
        # instruction had left in the flags. It compares four bytes at a time
        # and steps esi/edi by four, and the following je reads _flags.
        self.assertIn("_flags = (MEM32(esi) == MEM32(edi));", generated)
        self.assertIn("esi += _st; edi += _st; ecx--;", generated)
        # The step is four bytes, in whichever direction EFLAGS.DF says.
        self.assertIn("RECOMP_DF_STEP(4)", generated)
        self.assertIn("if (!_flags) break;", generated)
        self.assertIn("if ((_flags != 0)) goto loc_00000010;", generated)

    def test_setcc_after_dword_compare_reads_the_compare_not_the_xor(self):
        """The JSRF QueryInterface shape: xor / repe cmpsd / sete.

        While `repe cmpsd` set no flag state, the `sete` resolved its ZF
        against the preceding `xor edx, edx` and so was always 1 -- every GUID
        compared equal, and CDirectSound::QueryInterface returned the primary
        interface for every IID it was asked for.
        """
        clear = Instruction(0, 2, "xor", "edx, edx", "33d2")
        clear.operands = [Operand(type="reg", reg="edx"),
                          Operand(type="reg", reg="edx")]
        compare = Instruction(
            2, 2, "repe cmpsd", "dword ptr [esi], dword ptr es:[edi]", "f3a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=4),
            Operand(type="mem", mem_base="edi", mem_size=4),
        ]
        setcc = Instruction(4, 3, "sete", "dl", "0f94c2")
        setcc.operands = [Operand(type="reg", reg="dl")]
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter,
            BasicBlock(start=0, instructions=[clear, compare, setcc]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = (MEM32(esi) == MEM32(edi));", generated)
        self.assertNotIn("(edx == 0)", generated)
        self.assertIn("SET_LO8(edx, ((_flags != 0)) ? 1 : 0); /* sete */",
                      generated)

    def test_dword_scan_is_implemented(self):
        scan = Instruction(0, 2, "repne scasd", "eax, dword ptr es:[edi]",
                           "f2af")
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start, lifter.func_end = 0, 0x20
        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[scan, jump]))
        generated = "\n".join(lifted)

        self.assertNotIn("string scan, ecx iterations", generated)
        self.assertIn("_flags = (eax == MEM32(edi));", generated)
        # The stride is RECOMP_DF_STEP(4), not a literal 4: EFLAGS.DF decides
        # which way a string instruction walks, and MSVC's strrchr scans
        # backwards with `std; repne scas`. Asserting the literal would pin
        # the forward-only bug back in place.
        self.assertIn("int32_t _st = RECOMP_DF_STEP(4);", generated)
        self.assertIn("edi += _st; ecx--;", generated)
        self.assertIn("if (_flags) break;", generated)
        # The je reads the flag the loop set, in whichever equivalent form
        # the emitter picks -- what matters is that it reads _flags and not
        # a stale _fa/_fb snapshot left by some earlier compare.
        jcc = generated.splitlines()[-1]
        self.assertIn("_flags", jcc)
        self.assertNotIn("_fa", jcc)


if __name__ == "__main__":
    unittest.main()
