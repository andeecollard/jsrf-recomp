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
    entry_point = 0x00011000

    def get_section_at_va(self, addr):
        return _Section()


class _Ref:
    def __init__(self, kind):
        self.xref_type = type("K", (), {"value": kind})()


class _Xrefs:
    def __init__(self, refs):
        self._refs = refs or {}

    def get_refs_to(self, addr):
        return [_Ref(k) for k in self._refs.get(addr, [])]


def _detector(bodies, candidates, forced=(), natural=None,
              refs=None, provenance=None):
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
    det.xrefs = _Xrefs(refs)
    det._seed_provenance = dict(provenance or {})
    natural = natural or {}
    det._find_function_end = lambda start, nxt, sec: natural.get(
        start, dict(bodies).get(start, start))
    return det


SEED = (0.95, "seed_vtable_thunk")


class _WalkInsn:
    """One byte, never a terminator: the walk runs to `upper` and stops there,
    so the returned end IS the bound under test."""
    def __init__(self, addr):
        self.address = addr
        self.size = 1
        self.end_address = addr + 1
        self.is_ret = False
        self.is_jump = False
        self.is_cond_jump = False
        self.is_branch = False
        self.is_terminator = False
        self.jump_target = None
        self.jump_table = None
        self.mnemonic = "nop"
        self.op_str = ""


class _WalkEngine:
    instructions = {}
    jump_tables = {}

    def get_instruction(self, addr):
        return _WalkInsn(addr)

    def jump_table_entries(self, t):
        return []


def _end_detector(forced):
    det = FunctionDetector.__new__(FunctionDetector)
    det._forced_bounds = list(forced)
    det.engine = _WalkEngine()
    det._table_after = lambda a, upper: None
    return det


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
        # Dropped, not aliased: no start and no second body. Aliasing was the
        # previous repair and it SIGBUSed, because an alias runs the owner's
        # epilogue without having run its prologue.
        self.assertNotIn(0x00037587, det._candidates)
        self.assertEqual(det._alias_entries, {})
        self.assertEqual(det.dropped_seeds[0]["reference_class"],
                         "unreferenced")

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
        self.assertEqual(det._alias_entries, {})
        self.assertEqual(len(det.dropped_seeds), 4)

    def test_a_declared_extent_protects_a_body_the_sweep_missed(self):
        # The XenonRecomp escape hatch: the analyser cannot resolve a function
        # containing a jump table, so the project states the extent and no seed
        # may split it -- even though the seedless build found nothing there.
        det = _detector([], {0x00154D82: SEED},
                        forced=[(0x00154D70, 0x00154E00)])
        det._pass_demote_interior_seeds([])
        self.assertNotIn(0x00154D82, det._candidates)
        self.assertEqual(det._alias_entries, {})

    def test_a_direct_call_target_is_kept_and_reported(self):
        # Something really calls it, so removing the start would leave that
        # call unresolved. This is the subset that needs a secondary entry.
        det = _detector([(0x00037550, 0x00037960)], {0x00037587: SEED},
                        natural={0x00037550: 0x00037960},
                        refs={0x00037587: ["call"]})
        det._pass_demote_interior_seeds([])
        self.assertIn(0x00037587, det._candidates)
        self.assertEqual(det.dropped_seeds, [])
        self.assertEqual(det.kept_interior_seeds[0]["reference_class"],
                         "direct_call")

    def test_a_measured_indirect_target_is_kept(self):
        # icall_targets.json is a record of the title actually branching there.
        det = _detector([(0x00037550, 0x00037960)], {0x00037587: SEED},
                        natural={0x00037550: 0x00037960},
                        provenance={0x00037587: ["icall_targets.json"]})
        det._pass_demote_interior_seeds([])
        self.assertIn(0x00037587, det._candidates)
        self.assertEqual(det.kept_interior_seeds[0]["reference_class"],
                         "indirect_target")

    def test_an_address_taken_as_data_is_kept(self):
        # A vtable slot names a function by address; the branch is invisible.
        det = _detector([(0x00037550, 0x00037960)], {0x00037587: SEED},
                        natural={0x00037550: 0x00037960},
                        refs={0x00037587: ["data_imm"]})
        det._pass_demote_interior_seeds([])
        self.assertIn(0x00037587, det._candidates)

    def test_the_entry_point_is_never_dropped(self):
        det = _detector([(0x00010F00, 0x00012000)], {0x00011000: SEED},
                        natural={0x00010F00: 0x00012000})
        det._pass_demote_interior_seeds([])
        self.assertIn(0x00011000, det._candidates)
        self.assertEqual(det.kept_interior_seeds[0]["reference_class"],
                         "entry_point")

    def test_a_branch_only_target_is_an_interior_label(self):
        # Reached only from inside the owner: that is a label, not a function.
        det = _detector([(0x00037550, 0x00037960)], {0x00037587: SEED},
                        natural={0x00037550: 0x00037960},
                        refs={0x00037587: ["cond_jump"]})
        det._pass_demote_interior_seeds([])
        self.assertNotIn(0x00037587, det._candidates)
        self.assertEqual(det.dropped_seeds[0]["reference_class"],
                         "speculative")

    def test_a_declared_extent_caps_the_function_end(self):
        # The other half of --function-bounds. Informing the seed test is not
        # enough: a function may not be given an end past the range it was
        # declared to occupy. 0x0003B926 was found running nine bytes beyond
        # its translation unit by auditing every function against an external
        # delinking map -- one straddle in 10,292.
        det = _end_detector([(0x00039B50, 0x0003B938)])
        end = FunctionDetector._find_function_end(
            det, 0x0003B926, None, 0x00200000)
        self.assertEqual(end, 0x0003B938)

    def test_a_declared_extent_does_not_shorten_an_unrelated_function(self):
        det = _end_detector([(0x00039B50, 0x0003B938)])
        end = FunctionDetector._find_function_end(
            det, 0x00100000, None, 0x00100010)
        self.assertEqual(end, 0x00100010)

    def test_nothing_to_do_is_cheap(self):
        # No seeds and no declared extents: the pass must not force a rebuild.
        det = _detector([(0x1000, 0x1010)], {0x2000: (0.9, "call_target")})
        self.assertFalse(det._pass_demote_interior_seeds([]))
        self.assertEqual(det._candidates, {0x2000: (0.9, "call_target")})


if __name__ == "__main__":
    unittest.main()
