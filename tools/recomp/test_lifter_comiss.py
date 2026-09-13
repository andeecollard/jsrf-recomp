"""COMISS sets ZF, PF and CF together on NaN, and C's operators do not.

    unordered  ZF=1 PF=1 CF=1        less     ZF=0 PF=0 CF=1
    equal      ZF=1 PF=0 CF=0        greater  ZF=0 PF=0 CF=0

So `je` after a compare against NaN is TAKEN on hardware, while the plain C
`a == b` it used to be emitted as is false; and `jne` is NOT taken, while C's
`a != b` is true. The two conditions were inverted for NaN in opposite
directions, which is the worst shape for this kind of bug -- a test that only
exercised ordered operands passes either way.

What it cost: JSRF's ray-plane intersection sub_0014C100 ends

    comiss xmm0, xmm7          ; the dot product against zero
    je     -> return 0         ; degenerate, no intersection

and is handed a movement segment that is zero-length whenever a character is
standing still, which normalises to NaN. Emitted as a bare `==` the branch fell
through and the function reported a HIT with a NaN intersection point. That made
CSysDeathWarpManager (0x00024AE0) death-warp both CPlayers on every frame of the
Corn tutorial: CPlayer +0x3CC latched the warp kind, the exec virtual dispatched
to the respawn handler sub_00092750 every frame, and the animation was re-armed
before it could ever advance. Measured against xemu, which reads +0x3CC=0 and a
stable state 23/25 while the same scene animates correctly.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _comiss():
    insn = Instruction(0, 3, "comiss", "xmm0, xmm7", "0f2fc7")
    insn.operands = [Operand(type="reg", reg="xmm0"),
                     Operand(type="reg", reg="xmm7")]
    return insn


def _jcc(mnemonic, target=0x100):
    insn = Instruction(3, 2, mnemonic, hex(target), "")
    insn.operands = [Operand(type="imm", imm=target)]
    return insn


def _lift(jcc):
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[_comiss(), _jcc(jcc)]))
    return "\n".join(lifted)


UNORDERED = "(_fca != _fca || _fcb != _fcb)"


class ComissNaNTest(unittest.TestCase):
    def test_je_fires_on_unordered(self):
        # ZF is set by a NaN operand, so this branch is taken. This is the
        # exact site that froze the Corn tutorial.
        self.assertIn(f"(_fca == _fcb || {UNORDERED})", _lift("je"))

    def test_jne_does_not_fire_on_unordered(self):
        # C's `!=` is true for NaN; ZF-clear is not. Without the guard this is
        # wrong in the opposite direction to `je`.
        self.assertIn(f"(_fca != _fcb && !{UNORDERED})", _lift("jne"))

    def test_cf_conditions_include_unordered(self):
        self.assertIn(f"(_fca < _fcb || {UNORDERED})", _lift("jb"))
        self.assertIn(f"(_fca <= _fcb || {UNORDERED})", _lift("jbe"))

    def test_ordered_only_conditions_stay_plain(self):
        # ja is !CF && !ZF and jae is !CF; both are already false for NaN
        # because CF is set, and C agrees. These must NOT gain the guard.
        self.assertIn("(_fca > _fcb)", _lift("ja"))
        self.assertNotIn(UNORDERED, _lift("ja"))
        self.assertIn("(_fca >= _fcb)", _lift("jae"))
        self.assertNotIn(UNORDERED, _lift("jae"))

    def test_parity_branches_test_unorderedness_not_a_constant(self):
        # These were hardcoded to 0 and 1, which silently discards every
        # NaN check the game makes through the parity flag.
        jp = _lift("jp")
        self.assertIn(UNORDERED, jp)
        self.assertNotIn("0 /* jp", jp)
        jnp = _lift("jnp")
        self.assertIn(f"(!{UNORDERED})", jnp)
        self.assertNotIn("1 /* jnp", jnp)

    def test_synonyms_match_their_primary_form(self):
        # Compare the emitted CONDITION only. The trailing comment names the
        # mnemonic ("je: equal / zero" vs "jz: zero"), so a whole-line compare
        # would fail on wording rather than on semantics.
        def condition(jcc):
            line = next(l for l in _lift(jcc).splitlines()
                        if l.startswith("if ("))
            return line[:line.index("{ /*")]

        # jc, jnc and jnbe are deliberately absent: capstone normalises them
        # to jb, jae and ja, so the branch layer never emits them and asking
        # for one yields `/* TODO: jc */`. Listing them here would assert on a
        # form the lifter is not required to produce.
        for primary, synonyms in (("jb", ("jnae",)),
                                  ("jae", ("jnb",)),
                                  ("jbe", ("jna",)),
                                  ("je", ("jz",)),
                                  ("jne", ("jnz",))):
            for syn in synonyms:
                with self.subTest(jcc=syn):
                    self.assertEqual(condition(syn), condition(primary))


if __name__ == "__main__":
    unittest.main()
