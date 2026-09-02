import unittest

from .disasm import BasicBlock, Instruction
from .lifter import Lifter, lift_basic_block


def _lift(*instructions):
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=list(instructions)))
    return "\n".join(lifted)


class DirectionFlagLifterTest(unittest.TestCase):
    """EFLAGS.DF decides which way the string instructions walk.

    It used to lift to a comment, so every one of them walked forwards. The
    instruction that exposed it is MSVC's strrchr, which is `std` followed by
    `repne scasb` from the terminator *backwards*; scanning forwards ran off
    the end of the string and returned a pointer into whatever followed it.
    """

    def test_cld_and_std_assign_the_flag(self):
        generated = _lift(
            Instruction(0, 1, "std", "", "fd"),
            Instruction(1, 1, "cld", "", "fc"),
        )

        self.assertIn("g_df = 1;", generated)
        self.assertIn("g_df = 0;", generated)

    def test_repne_scasb_steps_by_the_direction_flag(self):
        generated = _lift(Instruction(0, 2, "repne scasb", "", "f2ae"))

        self.assertIn("RECOMP_DF_STEP(1)", generated)
        # The whole point: no unconditional forward step survives.
        self.assertNotIn("edi++;", generated)

    def test_rep_movsb_keeps_the_block_copy_only_for_the_forward_case(self):
        generated = _lift(Instruction(0, 2, "rep movsb", "", "f3a4"))

        # Forwards is still one memcpy...
        self.assertIn("if (!g_df) { memcpy(", generated)
        # ...and backwards must not be, because memcpy cannot express it.
        self.assertIn("else {", generated)
        self.assertIn("MEM8(edi - _i) = MEM8(esi - _i)", generated)

    def test_unprefixed_string_ops_step_by_the_direction_flag(self):
        for mnemonic, size in (("movsb", 1), ("stosw", 2), ("lodsd", 4)):
            with self.subTest(mnemonic=mnemonic):
                generated = _lift(Instruction(0, 1, mnemonic, "", "a4"))
                self.assertIn(f"RECOMP_DF_STEP({size})", generated)


if __name__ == "__main__":
    unittest.main()
