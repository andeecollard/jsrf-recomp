"""A branch out of an alias body into a gap registers the target as an alias.

Alias bodies only exist after the passes that find branch orphans in function
bodies, so a jump from an alias into unclaimed bytes used to be lowered to an
unresolved-target stub that pops only a return address: the block never ran
and esp came back short by the block's own ret N. The shape: a dispatcher
reached through a vtable, branching to two blocks after its own ret, the first
of which a later pass had turned into a function (Jet Set Radio Future,
0x00154D70 -> 0x00154D82 / 0x00154DAA).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector, Function  # noqa: E402

SEC_LO, SEC_HI = 0x1000, 0x1100


class _Insn:
    def __init__(self, addr, ret=False, jcc=None, jmp=None):
        self.address = addr
        self.end_address = addr + 4
        self.is_ret = ret
        self.is_jump = jmp is not None
        self.is_cond_jump = jcc is not None
        self.is_branch = jcc is not None or jmp is not None
        self.jump_target = jcc if jcc is not None else jmp
        self.jump_table = None


class _Section:
    name = "TEXT"
    virtual_addr = SEC_LO
    virtual_size = SEC_HI - SEC_LO
    raw_size = SEC_HI - SEC_LO
    executable = True


class _Image:
    def get_section_at_va(self, addr):
        return _Section() if SEC_LO <= addr < SEC_HI else None


class _Engine:
    def __init__(self, insns, returning):
        self.instructions = {i.address: i for i in insns}
        self.returning = set(returning)

    def get_instructions_in_range(self, lo, hi):
        return [i for a, i in sorted(self.instructions.items()) if lo <= a < hi]

    def probes_as_returning_body(self, addr, max_insns=256):
        return addr in self.returning

    def block_tail_jump(self, addr, max_insns=256):
        return None


def _detector(insns, functions, aliases, returning):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, returning)
    det.image = _Image()
    det.functions = {s: Function(start=s, end=e, name=f"sub_{s:08X}") for s, e in functions}
    det._alias_entries = dict(aliases)
    det._candidates = {}
    return det


class AliasBranchOrphans(unittest.TestCase):
    def test_gap_target_of_an_alias_becomes_an_alias(self):
        # alias 0x1000..0x1010 branches to 0x1010 (a function) and 0x1030 (gap)
        insns = [_Insn(0x1000), _Insn(0x1004, jcc=0x1030), _Insn(0x1008, jcc=0x1010),
                 _Insn(0x100C, ret=True), _Insn(0x1010), _Insn(0x102C, ret=True),
                 _Insn(0x1030), _Insn(0x1034, ret=True)]
        det = _detector(insns, functions=[(0x1010, 0x1030), (0x1040, 0x1050)],
                        aliases={0x1000: 0x1010}, returning=[0x1030])
        self.assertTrue(det._pass_alias_branch_orphans())
        self.assertEqual(det._alias_entries.get(0x1030), 0x1040)
        self.assertNotIn(0x1010, det._alias_entries)   # already a function

    def test_target_inside_a_function_is_left_alone(self):
        insns = [_Insn(0x1000), _Insn(0x1004, jcc=0x1018), _Insn(0x1008, ret=True),
                 _Insn(0x1010), _Insn(0x1018), _Insn(0x101C, ret=True)]
        det = _detector(insns, functions=[(0x1010, 0x1020)], aliases={0x1000: 0x1010},
                        returning=[0x1018])
        self.assertFalse(det._pass_alias_branch_orphans())

    def test_non_returning_target_is_rejected(self):
        insns = [_Insn(0x1000), _Insn(0x1004, jcc=0x1030), _Insn(0x1008, ret=True),
                 _Insn(0x1030)]
        det = _detector(insns, functions=[(0x1040, 0x1050)], aliases={0x1000: 0x1010},
                        returning=[])
        self.assertFalse(det._pass_alias_branch_orphans())
        self.assertNotIn(0x1030, det._alias_entries)

    def test_new_alias_is_scanned_too(self):
        # alias -> gap block A (alias) -> jmp to gap block B
        insns = [_Insn(0x1000), _Insn(0x1004, jcc=0x1030), _Insn(0x1008, ret=True),
                 _Insn(0x1030), _Insn(0x1034, jmp=0x1060), _Insn(0x1060), _Insn(0x1064, ret=True)]
        det = _detector(insns, functions=[(0x1040, 0x1050), (0x1080, 0x1090)],
                        aliases={0x1000: 0x1010}, returning=[0x1030, 0x1060])
        self.assertTrue(det._pass_alias_branch_orphans())
        self.assertIn(0x1030, det._alias_entries)
        self.assertIn(0x1060, det._alias_entries)


if __name__ == "__main__":
    unittest.main()
