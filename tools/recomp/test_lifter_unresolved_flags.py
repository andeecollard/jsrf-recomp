"""A jcc with no flag state must say so, not look like ordinary output.

When no usable flag state reaches a conditional branch the lifter emits the
function-local fallback `_flags`. The translator initialises it to zero and
only the rep-string and xadd paths ever write it, so for a jcc it is a constant
zero: the branch is never taken. That is not a conservative choice, it is an
arbitrary one, and it is silent.

It cost JSRF a boot. The `jle` at 0x0013D3F3 is reached from two predecessors
that both `cmp`, but named different registers, so the older translator
discarded the state; the branch could never be taken, the loader recorded
status -1 and the title opened JSRF_FATAL.ERR after the anti-graffiti screen.
`_merge_predecessor_flag_states` fixes that case and
`tools/recomp/test_lifter_flag_merge.py` covers it, but the fallback still
exists for the cases it cannot merge -- 110 of them survive in the preserved
JSRF tree.

So the emitted code is unchanged (there is no correct conservative answer for a
branch) and the marker is the deliverable: UNRESOLVED FLAGS is greppable, which
is what `diagnostics/jsrf_first_fault/audit_unresolved_flags.py` counts.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _jcc(mnemonic="jne", target=0x00401000):
    insn = Instruction(0, 6, mnemonic, f"0x{target:x}", "")
    insn.operands = [Operand(type="imm", imm=target)]
    insn.jump_target = target
    return insn   # is_cond_jump is a property of the mnemonic


class UnresolvedFlagsTest(unittest.TestCase):
    def test_a_jcc_with_no_flag_state_is_marked(self):
        # Lifted on its own, so nothing has set the flags.
        out = Lifter().lift_instruction(_jcc())
        self.assertEqual(len(out), 1)
        self.assertIn("UNRESOLVED FLAGS", out[0])
        self.assertIn("branch never taken", out[0])
        # The behaviour itself must not change: still the _flags fallback.
        # A bare Lifter has no function context, so the target reads as
        # external and the branch becomes a conditional tail call rather than
        # a goto -- either form is the same fallback.
        self.assertIn("if (_flags", out[0])
        self.assertIn("sub_00401000", out[0])

    def test_the_marker_names_the_condition_it_could_not_resolve(self):
        for mnemonic in ("je", "jne", "jle", "jbe", "jp", "jnp"):
            with self.subTest(mnemonic=mnemonic):
                out = Lifter().lift_instruction(_jcc(mnemonic))[0]
                self.assertIn(f"{mnemonic}:", out)
                self.assertIn("UNRESOLVED FLAGS", out)

    def test_setcc_and_cmovcc_fallbacks_are_marked_too(self):
        setcc = Instruction(0, 3, "sete", "al", "")
        setcc.operands = [Operand(type="reg", reg="al")]
        out = Lifter().lift_instruction(setcc)[0]
        self.assertIn("UNRESOLVED FLAGS", out)
        self.assertIn("always 0", out)

        cmov = Instruction(0, 3, "cmovne", "eax, ecx", "")
        cmov.operands = [Operand(type="reg", reg="eax"),
                         Operand(type="reg", reg="ecx")]
        out = Lifter().lift_instruction(cmov)[0]
        self.assertIn("UNRESOLVED FLAGS", out)
        self.assertIn("never moves", out)

    def test_a_resolved_jcc_carries_no_marker(self):
        # The ordinary path: a cmp reaches the jcc, so a real condition is
        # emitted and nothing should be flagged.
        from .disasm import BasicBlock
        from .lifter import lift_basic_block

        cmp_insn = Instruction(0, 3, "cmp", "eax, 4", "")
        cmp_insn.operands = [Operand(type="reg", reg="eax"),
                             Operand(type="imm", imm=4)]
        jcc = _jcc("jne")
        jcc.address = 3
        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[cmp_insn, jcc]))
        generated = "\n".join(lifted)
        self.assertIn("CMP_NE(_fa, _fb)", generated)
        self.assertNotIn("UNRESOLVED FLAGS", generated)


if __name__ == "__main__":
    unittest.main()
