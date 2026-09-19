"""Does the flag model agree with x86 about who writes EFLAGS?

lift_basic_block decides what a jcc's condition is about by carrying a "last
flag setter" forward, and which list a mnemonic is in decides whether that
setter survives the instruction. Three of the four answers are safe when
wrong in the cautious direction and one is not:

    FLAG_SETTERS / _EFLAGS_SETTERS   this instruction becomes the setter
    _FLAGS_UNDEFINED                 forget the setter (branch unresolved)
    _EFLAGS_PRESERVE                 carry the setter ACROSS this instruction
    (in none of them)                forget the setter, conservatively

Putting something in _EFLAGS_PRESERVE that really does write flags is the one
mistake with teeth: the branch is not left unresolved, it is resolved against
an instruction that has nothing to do with it, and the generated C looks
perfectly ordinary.

`popfd` was exactly that. It replaces every arithmetic flag with a word off
the stack and sat in _EFLAGS_PRESERVE, while lift_basic_block's neg/sbb carry
scan spelled out `and insns[j].mnemonic != "popfd"` to step around the same
entry -- one call site knowing the list was wrong and working around it
instead of fixing it.

So the classification is asserted here against x86's definition, instruction
by instruction, rather than being left to whoever edits the sets next.
"""

import unittest

from .lifter import (FLAG_SETTERS, _EFLAGS_SETTERS, _FLAGS_UNDEFINED,
                     _EFLAGS_PRESERVE)

# Instructions that WRITE at least one arithmetic flag (CF/ZF/SF/OF/PF/AF).
# Being in _EFLAGS_PRESERVE is a bug for every one of these.
WRITES_FLAGS = {
    "cmp", "test", "add", "sub", "adc", "sbb", "and", "or", "xor",
    "inc", "dec", "neg", "imul", "mul", "div", "idiv",
    "shl", "sal", "shr", "sar", "shld", "shrd",
    "rol", "ror", "rcl", "rcr",
    "bt", "bts", "btr", "btc", "bsf", "bsr",
    "cmpxchg", "xadd",
    "popfd",                      # pops all of EFLAGS off the stack
    "sahf",                       # loads SF/ZF/AF/PF/CF from AH
    "clc", "stc", "cmc",          # write CF directly
    "comiss", "comisd", "ucomiss", "ucomisd",
}

# Instructions that genuinely leave the arithmetic flags alone. Being in any
# of the setter lists for these would make a jcc ask about the wrong thing.
PRESERVES_FLAGS = {
    "mov", "lea", "push", "pop", "nop", "leave", "ret", "call",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",                       # reads flags into AH, writes none
    "pushfd", "pushal", "popal",  # popad restores GPRs, not EFLAGS
    "not",                        # NOT is the one logical op that does not
    "cld", "std", "cli", "sti",   # DF and IF only, not the arithmetic flags
    "enter",
    "loop", "loope", "loopne",    # decrement ecx and branch; write no flags
    "movaps", "movups", "movss", "addss", "mulss", "xorps",
    "prefetchnta", "prefetcht0",
}


class FlagClassificationTest(unittest.TestCase):
    def test_nothing_that_writes_flags_is_marked_as_preserving(self):
        for m in sorted(WRITES_FLAGS):
            with self.subTest(mnemonic=m):
                self.assertNotIn(
                    m, _EFLAGS_PRESERVE,
                    f"{m} writes EFLAGS, so listing it as preserving them "
                    f"carries the previous setter across it and resolves the "
                    f"next jcc against an unrelated instruction")

    def test_popfd_forgets_the_setter(self):
        """The specific regression: popfd must clear tracking, not carry it."""
        self.assertIn("popfd", _FLAGS_UNDEFINED)
        self.assertNotIn("popfd", _EFLAGS_PRESERVE)
        self.assertNotIn("popfd", FLAG_SETTERS)
        self.assertNotIn("popfd", _EFLAGS_SETTERS)

    def test_pushfd_still_preserves(self):
        """Its neighbour reads the flags and must NOT have moved with it."""
        self.assertIn("pushfd", _EFLAGS_PRESERVE)

    def test_nothing_that_preserves_flags_is_marked_as_a_setter(self):
        setters = FLAG_SETTERS | _EFLAGS_SETTERS
        for m in sorted(PRESERVES_FLAGS):
            with self.subTest(mnemonic=m):
                self.assertNotIn(
                    m, setters,
                    f"{m} writes no arithmetic flag, so naming it a setter "
                    f"makes a following jcc ask about it instead of about the "
                    f"comparison that really set the flags")

    def test_the_lists_do_not_overlap(self):
        """A mnemonic in two lists is decided by elif order, not by intent."""
        pairs = (("FLAG_SETTERS", FLAG_SETTERS),
                 ("_EFLAGS_SETTERS", _EFLAGS_SETTERS),
                 ("_FLAGS_UNDEFINED", _FLAGS_UNDEFINED),
                 ("_EFLAGS_PRESERVE", _EFLAGS_PRESERVE))
        for i, (an, a) in enumerate(pairs):
            for bn, b in pairs[i + 1:]:
                # "cmpsd" and "movsd" are each two different instructions --
                # an SSE scalar double op and a string op -- sharing a
                # mnemonic. They are allowed to sit in the string/SSE lists
                # at once; nothing else is.
                overlap = (a & b) - {"cmpsd", "movsd"}
                with self.subTest(a=an, b=bn):
                    self.assertEqual(set(), overlap,
                                     f"{an} and {bn} both claim {overlap}")


class MnemonicAmbiguityTest(unittest.TestCase):
    """"movsd" is two instructions, and the dispatcher must tell them apart.

    The string MOVSD copies a dword from [esi] to es:[edi]. The SSE2 MOVSD
    moves a scalar double in or out of an xmm register. They share a mnemonic
    and nothing else, and the string branch of lift_instruction sits ahead of
    the SSE branch -- so before the operand test an SSE movsd was lifted as a
    string copy, silently and with the wrong operands.

    JSRF has none: all 143 of its movsd are the string form. That is why this
    never bit, and why it needs a test rather than a run.
    """

    def test_string_movsd_still_lifts_as_a_string_copy(self):
        from .lifter import Lifter
        from .disasm import Instruction
        insn = Instruction(0, 2, "movsd", "dword ptr es:[edi], dword ptr [esi]",
                           "", operands=[])
        out = " ".join(Lifter().lift_instruction(insn))
        self.assertIn("edi", out)
        self.assertIn("esi", out)
        self.assertNotIn("xmm", out)

    def test_sse_movsd_does_not_lift_as_a_string_copy(self):
        from .lifter import Lifter
        from .disasm import Instruction, Operand
        ops = [Operand(type="reg", reg="xmm0"),
               Operand(type="mem", mem_base="eax", mem_size=8)]
        insn = Instruction(0, 5, "movsd", "xmm0, qword ptr [eax]", "",
                           operands=ops)
        out = " ".join(Lifter().lift_instruction(insn))
        # The string lifter walks esi/edi and knows nothing about xmm; if this
        # came out of it, the move went to the wrong place entirely.
        self.assertNotIn("edi", out)
        self.assertNotIn("esi", out)

    def test_the_operand_test_is_what_decides(self):
        from .lifter import _has_xmm_operand
        from .disasm import Operand
        self.assertFalse(_has_xmm_operand([]))
        self.assertFalse(_has_xmm_operand(None))
        self.assertFalse(_has_xmm_operand([Operand(type="reg", reg="eax")]))
        self.assertTrue(_has_xmm_operand([Operand(type="reg", reg="xmm7")]))


if __name__ == "__main__":
    unittest.main()
