"""cmpxchg must exchange, not just compare.

DSOUND's sub_001A1F31 atomically swaps a pending-event mask to zero with the
standard retry idiom:

    eax = [m]                  ; snapshot
  L: cmpxchg [m], edx          ; swap in 0 if [m] is still eax
    jne L                      ; retry with the reloaded eax
    or  [acc], eax             ; drain what we took

Lifted to a comment this never cleared the mask, and because nothing reloaded
eax the loop could not terminate once another thread touched the word.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base, size=4):
    return Operand(type="mem", mem_base=base, mem_size=size)


def _insn(address, mnemonic, operands, op_str="", raw=""):
    insn = Instruction(address, 3, mnemonic, op_str, raw)
    insn.operands = operands
    return insn


class CmpxchgLifterTest(unittest.TestCase):
    def test_capstone_representation_matches_jsrf_dsound(self):
        """0x001A1F4B in JSRF's DSOUND section."""
        from .disasm import Disassembler
        decoded = list(Disassembler()._cs.disasm(bytes.fromhex("0fb111"),
                                                 0x001A1F4B))
        self.assertEqual(1, len(decoded))
        self.assertEqual("cmpxchg", decoded[0].mnemonic)
        self.assertEqual("dword ptr [ecx], edx", decoded[0].op_str)

    def test_memory_cmpxchg_exchanges_and_reloads_the_accumulator(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn(0, "cmpxchg", [_mem("ecx"), _reg("edx")],
                  "dword ptr [ecx], edx")))

        self.assertNotIn("TODO", generated)
        # compares the accumulator with the destination
        self.assertIn("_cx_acc = (uint32_t)(eax)", generated)
        self.assertIn("_cx_ok = (_cx_old == _cx_acc)", generated)
        # writes the source in on success ...
        self.assertIn("if (_cx_ok) MEM32(_cx_addr) = _cx_src;", generated)
        # ... and reloads the accumulator on failure, which is what lets the
        # caller's retry loop terminate
        self.assertIn("if (!_cx_ok) eax = _cx_old;", generated)

    def test_zf_is_the_exchange_outcome_not_a_stale_operand_compare(self):
        """The old resolver compared operands AFTER they had been written."""
        c = _insn(0, "cmpxchg", [_mem("ecx"), _reg("edx")],
                  "dword ptr [ecx], edx")
        j = Instruction(3, 2, "jne", "0x0", "75fb")
        j.jump_target = 0
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[c, j]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = _cx_ok;", generated)
        self.assertIn("if ((_flags == 0)) goto loc_00000000;", generated)
        self.assertNotIn("== eax)) goto", generated)

    def test_lock_prefix_uses_the_atomic_helper(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn(0, "lock cmpxchg", [_mem("ecx"), _reg("edx")],
                  "dword ptr [ecx], edx")))

        self.assertIn("RECOMP_ATOMIC_CMPXCHG32(_cx_addr, _cx_acc, _cx_src,"
                      " &_cx_ok)", generated)

    def test_byte_width_uses_the_low_accumulator(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn(0, "cmpxchg", [_mem("ecx", 1), _reg("dl")],
                  "byte ptr [ecx], dl")))

        self.assertIn("_cx_acc = (uint8_t)(LO8(eax))", generated)
        self.assertIn("SET_LO8(eax, _cx_old);", generated)

    def test_register_destination(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn(0, "cmpxchg", [_reg("ebx"), _reg("edx")], "ebx, edx")))

        self.assertNotIn("TODO", generated)
        self.assertIn("_cx_old = (uint32_t)(ebx)", generated)
        self.assertIn("if (_cx_ok) ebx = _cx_src;", generated)

    def test_lock_requires_a_memory_destination(self):
        generated = "\n".join(Lifter().lift_instruction(
            _insn(0, "lock cmpxchg", [_reg("ebx"), _reg("edx")], "ebx, edx")))

        self.assertIn("LOCK requires a memory destination", generated)


if __name__ == "__main__":
    unittest.main()
