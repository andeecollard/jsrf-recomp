"""Comparison snapshots must survive a join of different CMP operands.

Upstream's file, merged. Two things changed and both are recorded here rather
than in the commit message alone, because the difference is the interesting
part of this merge.

1. It was written with bare `def test_*` functions, which pytest collects and
   unittest does not. The suite this tree runs is
   `python3 -m unittest discover -s tools/recomp`, so the file contributed
   zero tests and its assertions never fired in either direction. It is a
   TestCase now, and actually runs.

2. Upstream's `_merge_flag_states` and this tree's
   `_merge_predecessor_flag_states` are independent implementations of the
   same idea; the former is now an alias of the latter. On the cases upstream
   handles they agree exactly, and the two behavioural tests below -- a setne
   and a cmovne reading a join of `cmp eax, edx` with `cmp ebx, esi` -- pass
   unmodified against this tree's implementation.

   Where they differ, ours answers and upstream's refuses:

     * MIXED cmp/test. Upstream returns None because ZF cannot be rebuilt from
       _fa/_fb when one edge compared and the other masked. Ours returns
       MERGED_ZF_PUBLISHED: the lifter writes _zf at each cmp and each test,
       so the join reads a flag that was computed where it was known instead
       of rebuilt where it is read. Admitted only when EVERY predecessor ends
       in a cmp or a test, which is what makes _zf written on every edge.

     * MIXED widths. Upstream returns None because sign and parity handling
       depend on the width. Ours returns MERGED_ZF_CMP/MERGED_ZF_TEST, which
       answers ZF and nothing else -- each predecessor already masked _fa/_fb
       to its own width at its own compare, so equality is correct whichever
       path ran, while js/jns and the signed ordered pair still refuse.

   Upstream's two "is None" assertions therefore asserted the absence of a
   capability upstream does not have. They are rewritten below to assert what
   ours does, and to hold the line that still matters: these joins answer ZF
   ONLY, and an unknown predecessor is still never guessed at.
"""
import unittest

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand
from tools.recomp.lifter import (MERGED_ZF_CMP, MERGED_ZF_PUBLISHED,
                                 _make_condition)

BASE = 0x10000


def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


class FlagJoinTest(unittest.TestCase):
    # ---- upstream's behavioural tests, unmodified ----

    def test_different_cmp_operands_join_for_setne(self):
        code = translate_join()
        self.assertIn('CMP_NE(_fa, _fb)', code, code)
        self.assertNotIn('_flags /* setne */', code)

    def test_different_cmp_operands_join_for_cmovne(self):
        code = translate_join(bytes.fromhex('0f45c7c3'))
        self.assertIn('if (CMP_NE(_fa, _fb)) eax = edi;', code, code)

    def test_unknown_path_is_not_guessed(self):
        self.assertIsNone(_merge_flag_states([None, ('cmp', [])]))

    # ---- where this tree answers and upstream refuses ----

    def test_mixed_operations_merge_to_a_published_zf(self):
        a = Operand(type='reg', reg='eax')
        b = Operand(type='reg', reg='edx')
        merged = _merge_flag_states([('cmp', [a, b]), ('test', [a, b])])
        self.assertEqual(merged, (MERGED_ZF_PUBLISHED, []))

    def test_a_published_zf_join_answers_zf_and_nothing_else(self):
        for jcc, expect in (('je', '_zf'), ('jne', '!_zf')):
            with self.subTest(jcc=jcc):
                got = _make_condition(jcc, MERGED_ZF_PUBLISHED, [])
                self.assertIsNotNone(got)
                self.assertEqual(got[0], expect)
        # CF and OF genuinely differ between a cmp and a test, and the signed
        # ordered pair needs operands neither edge can supply. Still refused.
        for jcc in ('js', 'jns', 'jl', 'jg', 'jle', 'jge', 'jb', 'ja', 'jo'):
            with self.subTest(jcc=jcc):
                self.assertIsNone(_make_condition(jcc, MERGED_ZF_PUBLISHED, []))

    def test_mixed_widths_merge_to_a_zf_only_snapshot(self):
        wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
        narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
        merged = _merge_flag_states([('cmp', wide), ('cmp', narrow)])
        self.assertIsNotNone(merged)
        self.assertEqual(merged[0], MERGED_ZF_CMP)

    def test_a_width_mismatched_join_refuses_the_width_dependent_conditions(self):
        ops = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
        for jcc in ('je', 'jne'):
            with self.subTest(jcc=jcc):
                self.assertIsNotNone(_make_condition(jcc, MERGED_ZF_CMP, ops))
        # These cast back to the operand's own width, which the join lost.
        for jcc in ('js', 'jns', 'jl', 'jg', 'jle', 'jge'):
            with self.subTest(jcc=jcc):
                self.assertIsNone(_make_condition(jcc, MERGED_ZF_CMP, ops))


if __name__ == '__main__':
    unittest.main()
