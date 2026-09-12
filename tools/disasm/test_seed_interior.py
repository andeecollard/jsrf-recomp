"""A seed inside a function must not split it.

disasm.py guards a seed against landing inside a decoded *instruction*. It has
never guarded against landing inside a decoded *function*, and that is the one
that carves: JSRF's vtable-thunk feedback seeded four clean instruction
boundaries inside FileManager::readStageObj and the sweep cut one function into
five prologue-less fragments. A fragment starting mid-body ends before the
epilogue that would restore ebx, so it returns having clobbered it -- 126 of
the 131 callee-saved-register violations RECOMP_ABI_CHECK reports at gameplay
are seeded fragments, against a 23% base rate in the database.

The Xbox Dashboard project states the rule from the other direction: never seed
an address inside an existing body, it truncates the container.

_pass_seed_aliases already knows the right answer -- an alias sharing the
enclosing tail, callable without splitting anything -- but it runs last and
bails on `addr in self.functions`, by which point the seed is already a start.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Section:
    virtual_addr = 0
    virtual_size = 0x01000000


class _Image:
    def get_section_at_va(self, addr):
        return _Section()


def _detector(bodies, candidates, forced=(), natural=None):
    """`bodies` are the functions as built; `natural` their true extents.

    natural maps a start to what _find_function_end returns when it is not
    capped by the next function -- which is the whole point of the test.
    """
    det = FunctionDetector.__new__(FunctionDetector)
    det.functions = {s: _Func(s, e) for s, e in bodies}
    det._candidates = dict(candidates)
    det._alias_entries = {}
    det._forced_bounds = list(forced)
    det.image = _Image()
    natural = natural or {}
    det._find_function_end = lambda start, nxt, sec: natural.get(
        start, dict(bodies).get(start, start))
    return det


SEED = (0.95, "seed_vtable_thunk")


class SeedInteriorTest(unittest.TestCase):
    # The pass is opt-in (it heals the carve and the resulting build SIGBUSes;
    # see functions.py). These tests are about whether the rule is right, so
    # they opt in rather than inherit whatever the ambient default is.
    def setUp(self):
        self._prev = os.environ.get("RECOMP_SEED_INTERIOR")
        os.environ["RECOMP_SEED_INTERIOR"] = "1"

    def tearDown(self):
        if self._prev is None:
            os.environ.pop("RECOMP_SEED_INTERIOR", None)
        else:
            os.environ["RECOMP_SEED_INTERIOR"] = self._prev

    def test_a_seed_inside_a_body_becomes_an_alias(self):
        # readStageObj is one function 0x37550..0x37960; 0x37587 is 55 bytes in.
        det = _detector([(0x00037550, 0x00037960)],
                        {0x00037587: SEED},
                        natural={0x00037550: 0x00037960})
        det._pass_demote_interior_seeds([])
        self.assertNotIn(0x00037587, det._candidates)
        self.assertEqual(det._alias_entries[0x00037587], 0x00037960)

    def test_a_seed_in_a_gap_still_starts_a_function(self):
        # The legitimate use: a thunk the detector never reached.
        det = _detector([(0x00037550, 0x00037960)],
                        {0x00038000: SEED},
                        natural={0x00037550: 0x00037960})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates[0x00038000], SEED)
        self.assertEqual(det._alias_entries, {})

    def test_a_seed_on_a_body_start_is_kept(self):
        # Not interior: strictly-inside is the test, so a seed that agrees with
        # the sweep about where a function begins is left alone.
        det = _detector([(0x00037550, 0x00037960)],
                        {0x00037550: SEED},
                        natural={0x00037550: 0x00037960})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates[0x00037550], SEED)
        self.assertEqual(det._alias_entries, {})

    def test_a_carved_run_all_demotes_to_the_owner(self):
        # The real shape: readStageObj already cut into fragments, so the built
        # bodies are the fragments. The owner's NATURAL extent is what decides,
        # which is why the test cannot use the truncated body it was given.
        seeds = {a: SEED for a in (0x00037587, 0x00037604,
                                   0x00037620, 0x0003767E)}
        det = _detector([(0x00037550, 0x00037587), (0x00037587, 0x00037604),
                         (0x00037604, 0x00037620), (0x00037620, 0x0003767E)],
                        seeds,
                        natural={0x00037550: 0x00037960})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates, {})
        self.assertEqual(sorted(det._alias_entries), sorted(seeds))

    def test_a_declared_extent_protects_a_body_the_sweep_missed(self):
        # The XenonRecomp escape hatch: the analyser cannot resolve a function
        # containing a jump table, so the project states the extent and no seed
        # may split it -- even though the seedless build found nothing there.
        det = _detector([], {0x00154D82: SEED},
                        forced=[(0x00154D70, 0x00154E00)])
        det._pass_demote_interior_seeds([])
        self.assertNotIn(0x00154D82, det._candidates)
        self.assertEqual(det._alias_entries[0x00154D82], 0x00154E00)

    def test_nothing_to_do_is_cheap(self):
        # No seeds and no declared extents: the pass must not force a rebuild.
        det = _detector([(0x1000, 0x1010)], {0x2000: (0.9, "call_target")})
        self.assertFalse(det._pass_demote_interior_seeds([]))
        self.assertEqual(det._candidates, {0x2000: (0.9, "call_target")})


if __name__ == "__main__":
    unittest.main()
