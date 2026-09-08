"""lock xadd / lock cmpxchg are InterlockedIncrement and friends.

Both used to fall through to a TODO comment: a reference count that never
moved and a compare-and-swap that never swapped. That was harmless while every
guest thread ran synchronously, and stopped being harmless once the runtime
began spawning real host threads for a title's workers.

Lowering them to a plain read-modify-write would not do either: a racing
sequence is the exact bug these instructions exist to prevent.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _mem_ecx():
    return Operand(type="mem", mem_base="ecx", mem_index=None, mem_scale=1,
                   mem_disp=0, mem_size=4)


def _lift(mnemonic, src_reg="eax"):
    insn = Instruction(0, 3, mnemonic, "dword ptr [ecx], %s" % src_reg, "0fc101")
    insn.operands = [_mem_ecx(), Operand(type="reg", reg=src_reg)]
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[insn]))
    return chr(10).join(lifted)


class AtomicLiftTest(unittest.TestCase):
    def test_xadd_is_atomic_and_returns_the_old_value(self):
        generated = _lift("lock xadd", "edx")
        self.assertIn("RECOMP_ATOMIC_XADD32", generated)
        self.assertIn("_old", generated)
        self.assertNotIn("TODO", generated)

    def test_cmpxchg_is_a_real_compare_and_swap(self):
        generated = _lift("lock cmpxchg", "edx")
        self.assertIn("RECOMP_ATOMIC_CMPXCHG32", generated)
        # eax is only replaced when the comparand did not match.
        self.assertIn("if (!_cx_ok) eax = _cx_old;", generated)
        self.assertNotIn("TODO", generated)

    def test_cmpxchg_preserves_the_outcome_when_touching_eax(self):
        # The ZF a following jz reads must come from the saved exchange outcome,
        # not from eax after it may have been overwritten.
        generated = _lift("lock cmpxchg", "edx")
        outcome_at = generated.index("_cx_ok")
        flags_at = generated.index("_flags = _cx_ok;")
        eax_at = generated.index("if (!_cx_ok) eax = _cx_old;")
        self.assertLess(outcome_at, eax_at)
        self.assertGreater(flags_at, eax_at)

    def test_lock_prefixed_forms_lift_the_same_way(self):
        for m in ("lock xadd", "lock cmpxchg"):
            with self.subTest(mnemonic=m):
                generated = _lift(m, "edx")
                self.assertNotIn("TODO", generated)
                self.assertIn("RECOMP_ATOMIC_", generated)
