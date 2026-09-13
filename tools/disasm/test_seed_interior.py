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
    name = ".text"


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
              refs=None, provenance=None, tables=None, sites=None):
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
    det.engine = _WalkEngine(tables, sites)
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

    def __init__(self, tables=None, sites=None):
        self.jump_tables = dict(tables or {})
        self._sites = dict(sites or {})

    def get_instruction(self, addr):
        return _WalkInsn(addr)

    def jump_table_entries(self, t):
        return list(self.jump_tables.get(t, ()))

    def jump_table_sites(self, t):
        return list(self._sites.get(t, ()))


class _DispatchInsn(_WalkInsn):
    """A one-byte stand-in for `jmp [reg*4 + table]`: an unconditional jump
    that terminates the body and names the table it reads."""
    def __init__(self, addr, table):
        super().__init__(addr)
        self.jump_table = table
        self.is_jump = True
        self.is_terminator = True
        self.mnemonic = "jmp"


class _MidInsn(_WalkInsn):
    """A dispatch that does NOT end the body: `jmp [reg*4+table]` reached by a
    branch, with ordinary code decoded after it."""
    def __init__(self, addr, table):
        super().__init__(addr)
        self.jump_table = table
        self.mnemonic = "jmp"


class _AliasEngine(_WalkEngine):
    def __init__(self, tables, dispatch_at):
        super().__init__(tables, None)
        self._dispatch_at = dispatch_at

    def get_instruction(self, addr):
        if addr in self._dispatch_at:
            return _DispatchInsn(addr, self._dispatch_at[addr])
        if addr in getattr(self, "_mid_at", {}):
            return _MidInsn(addr, self._mid_at[addr])
        return _WalkInsn(addr)

    def get_instructions_in_range(self, lo, hi):
        return [self.get_instruction(a) for a in range(lo, hi)]


def _alias_detector(functions, aliases, tables, dispatch_at, natural=None,
                    mid_at=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.functions = {s: _Func(s, e) for s, e in functions}
    det._alias_entries = dict(aliases)
    det._forced_bounds = []
    det.image = _Image()
    det.engine = _AliasEngine(tables, dispatch_at)
    det.engine._mid_at = dict(mid_at or {})
    det.labels = _Labels()
    natural = natural or {}
    # Mirrors the real _find_function_end's clamp: `upper` is lowered to the
    # next function start, so a fake that ignored nxt could not show the bug
    # the clamp exists for.
    det._find_function_end = lambda start, nxt, sec: min(
        natural.get(start, start), nxt if nxt is not None else 1 << 32)
    return det


class _Labels:
    def get(self, addr):
        return None

    def auto_name_function(self, addr, sec, conf):
        pass


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

    def test_a_switch_arm_is_dropped_even_though_it_was_measured(self):
        # The self-sustaining case. sub_00025040 is `push esi; mov esi,ecx;
        # mov eax,[esi+0x54]; cmp eax,7; ja default; push ebx; push edi;
        # jmp [eax*4+0x252BC]`. Every arm ends by popping edi, ebx and esi,
        # so an arm carved into its own function returns with all three
        # changed -- which is what RECOMP_ABI_CHECK reports. The arms are only
        # ever reached through the table, so the runtime feed records them and
        # they come back as seeds on the next run, forever.
        det = _detector([(0x00025040, 0x00025058)],
                        {0x00025058: SEED, 0x00025078: SEED},
                        natural={0x00025040: 0x000252DC},
                        provenance={0x00025058: ["icall_targets.json"],
                                    0x00025078: ["icall_targets.json"]},
                        tables={0x000252BC: [0x00025058, 0x00025078]},
                        sites={0x000252BC: [0x00025051]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates, {})
        self.assertEqual([r["reference_class"] for r in det.dropped_seeds],
                         ["switch_arm", "switch_arm"])
        self.assertEqual(det.dropped_seeds[0]["dispatch"], "0x00025051")

    def test_a_table_entry_dispatched_from_elsewhere_is_kept(self):
        # Both halves of the test have to hold. An address that appears in
        # some table is an arm of the function whose own code dispatches
        # through it -- not of whatever function it happens to sit inside.
        det = _detector([(0x00025040, 0x00025058)], {0x00025058: SEED},
                        natural={0x00025040: 0x000252DC},
                        provenance={0x00025058: ["icall_targets.json"]},
                        tables={0x000252BC: [0x00025058]},
                        sites={0x000252BC: [0x00099000]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det.dropped_seeds, [])
        self.assertEqual(det.kept_interior_seeds[0]["reference_class"],
                         "indirect_target")

    def test_the_first_arm_is_dropped_though_it_is_interior_to_nothing(self):
        # MSVC makes case 0 the dispatching jump's own fall-through, so the
        # first arm sits exactly ON the boundary its own seed created: the
        # owner was clamped to end there, so nothing contains it and the
        # interior test never looks at it. Every later arm is then judged
        # against an owner starting AFTER the dispatch, so the owner-bounded
        # test fails for those too -- which left 133 of JSRF's 422 resynced
        # tables still carved after a459a6d recognised the arms.
        det = _detector([(0x0002C360, 0x0002C397)],
                        {0x0002C397: SEED, 0x0002CA05: SEED},
                        natural={0x0002C360: 0x0002CFBA},
                        provenance={0x0002C397: ["vtable_seeds_accum.json"]},
                        tables={0x0002CBB4: [0x0002C397, 0x0002CA05]},
                        sites={0x0002CBB4: [0x0002C390]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates, {})
        self.assertEqual([r["reference_class"] for r in det.dropped_seeds],
                         ["switch_arm", "switch_arm"])
        self.assertEqual(det.dropped_seeds[0]["dispatch"], "0x0002C390")

    def test_a_table_entry_far_from_its_dispatch_is_not_a_first_arm(self):
        # The backward look is deliberately short. An arm that is neither
        # interior to its owner nor immediately after the dispatch is not
        # something this rule can speak for, and must be left alone.
        det = _detector([(0x0002C360, 0x0002C397)], {0x0002C397: SEED},
                        natural={0x0002C360: 0x0002C397},
                        provenance={0x0002C397: ["icall_targets.json"]},
                        tables={0x0002CBB4: [0x0002C397]},
                        sites={0x0002CBB4: [0x0002C100]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det.dropped_seeds, [])
        self.assertEqual(det._candidates, {0x0002C397: SEED})

    def test_every_later_arm_of_a_carved_run_is_dropped(self):
        # JSRF 0x0007E550, measured 13 Sep, and the case that survived both
        # a459a6d and 289f21f. `cmp edx,4; ja default; jmp [edx*4+0x7E68C]`
        # with five arms. Dropping the first one is not enough: the dispatcher
        # still ends at the jump, so arm 1 is owned by arm 0, arm 2 by arm 1,
        # and so on down the run. The dispatch is then outside every one of
        # those owners and more than sixteen bytes behind, so both earlier
        # tests decline and four arms stay carved.
        #
        # They matter because the dispatcher pushes ecx, ebx, esi and edi
        # before the jump and the arms pop them: a carved arm returns with all
        # four changed. RECOMP_ABI_CHECK reported exactly that, and the same
        # defect one table over is what left a code address in
        # CActMan::Idle's `this` and put the title on a black screen.
        arms = [0x0007E575, 0x0007E58D, 0x0007E594, 0x0007E5A3, 0x0007E5AA]
        det = _detector([(0x0007E550, 0x0007E575)],
                        {a: SEED for a in arms},
                        natural={0x0007E550: 0x0007E575,
                                 **{a: 0x0007E68A for a in arms}},
                        provenance={a: ["icall_targets.json"] for a in arms},
                        tables={0x0007E68C: arms},
                        sites={0x0007E68C: [0x0007E56E]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det._candidates, {})
        self.assertEqual([r["reference_class"] for r in det.dropped_seeds],
                         ["switch_arm"] * 5)
        self.assertEqual({r["dispatch"] for r in det.dropped_seeds},
                         {"0x0007E56E"})

    def test_a_run_broken_by_unrelated_code_is_left_alone(self):
        # The discriminator, stated as a test rather than as a distance. The
        # dispatch is inside a real function and the arm really is in its
        # table, but a function that is NOT an entry of that table sits
        # between them -- so this is not one original body carved into
        # fragments, and the rule must not speak for it.
        det = _detector([(0x00030000, 0x00030040), (0x00030100, 0x00030180)],
                        {0x00030200: SEED},
                        natural={0x00030000: 0x00030040,
                                 0x00030100: 0x00030180,
                                 0x00030200: 0x00030400},
                        provenance={0x00030200: ["icall_targets.json"]},
                        tables={0x00030500: [0x00030200]},
                        sites={0x00030500: [0x00030030]})
        det._pass_demote_interior_seeds([])
        self.assertEqual(det.dropped_seeds, [])
        self.assertEqual(det._candidates, {0x00030200: SEED})

    def test_an_alias_ending_at_its_own_switch_is_re_measured(self):
        # JSRF 0x0007E550. Every pass that records an alias extent runs BEFORE
        # _pass_demote_interior_seeds rebuilds the function set, so dropping a
        # switch arm leaves behind an alias whose body ends where the arm used
        # to begin -- which is the dispatching jump itself. A function cannot
        # end at its own switch and exclude its own cases, and while it does,
        # _analyze_switch_table refuses the table (it needs
        # func_start <= arm < func_end), the tail jump stays indirect, and the
        # arms are never translated at all. The arms are what pop the four
        # registers the dispatcher pushed.
        #
        # Measured 13 Sep: 270 of JSRF's 2,557 aliases sit in this state.
        det = _alias_detector(
            functions=[(0x0007E360, 0x0007E524), (0x0007E5C3, 0x0007E68A)],
            aliases={0x0007E550: 0x0007E575},
            tables={0x0007E68C: [0x0007E575, 0x0007E58D, 0x0007E594,
                                 0x0007E5A3, 0x0007E5AA]},
            dispatch_at={0x0007E574: 0x0007E68C},
            natural={0x0007E550: 0x0007E5C3})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x0007E550].end, 0x0007E5C3)

    def test_an_alias_that_covers_its_own_arms_is_left_alone(self):
        # The trigger is a provably wrong end, not merely an old one. An alias
        # whose body already contains every arm of the switch it dispatches is
        # correct, and re-measuring 2,557 aliases that are fine is not a fix.
        det = _alias_detector(
            functions=[(0x00030000, 0x00030040)],
            aliases={0x00030100: 0x00030200},
            tables={0x000301F0: [0x00030120, 0x00030130]},
            dispatch_at={0x000301FF: 0x000301F0},
            natural={0x00030100: 0x00030800})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x00030100].end, 0x00030200)

    def test_a_re_measured_alias_stops_at_the_next_real_function(self):
        # The clamp exists because _find_function_end run with no upper bound
        # runs away -- 289f21f measured p99.9 of 270,906 bytes against 4,639
        # for the clamped walk. A re-measured body must still stop somewhere.
        #
        # It stops at the next REAL function start. 0x30150 is an arm and does
        # not stop it, because absorbing the arms is the entire purpose;
        # 0x30300 is an ordinary function and does.
        det = _alias_detector(
            functions=[(0x00030000, 0x00030040), (0x00030300, 0x00030400)],
            aliases={0x00030100: 0x00030140},
            tables={0x00030500: [0x00030150, 0x00030160]},
            dispatch_at={0x0003013F: 0x00030500},
            natural={0x00030100: 0x00030900})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x00030100].end, 0x00030300)

    def test_another_alias_does_not_stop_a_re_measure(self):
        # The correction the runtime forced, and the reason the clamp is not
        # simply "the next start".
        #
        # An alias is an alternate ENTRY into a body, not a boundary of one --
        # _build_alias_entries overlaps them deliberately -- so treating an
        # alias start as a clamp stops a dispatcher absorbing its own arms.
        # Measured on JSRF: 198 of 200 dispatches whose arms fell outside
        # their body were stopped by a tail_jump_alias sitting between the
        # jump and its table. sub_000F8B10 and sub_000FDA20 are two of them,
        # and RECOMP_ABI_CHECK caught them losing 32 and 16 bytes of stack
        # with edi clobbered, seconds before the screen went black.
        #
        # The snapshot is also taken before the loop starts, so an extent does
        # not depend on how many aliases happen to be materialised already.
        det = _alias_detector(
            functions=[(0x00030000, 0x00030040)],
            aliases={0x00030100: 0x00030140, 0x00030200: 0x00030280},
            tables={0x00030500: [0x00030150, 0x00030160]},
            dispatch_at={0x0003013F: 0x00030500},
            natural={0x00030100: 0x00030400})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x00030100].end, 0x00030400)

    def test_a_dispatch_in_the_MIDDLE_of_a_body_still_triggers(self):
        # A truncated body often ends at its switch -- sub_0007E550 and
        # sub_0006D770 both did -- but it need not. Cut after the dispatch and
        # still short of the arms, the last instruction is ordinary code and a
        # test that looks only there skips the case entirely. That left 156 of
        # 161 remaining cases untouched.
        #
        # sub_00114A80 is one: 692 bytes, its `jmp [eax*4+0x114F90]` well
        # inside the body, and its arms past the end. It leaks the 0x98-byte
        # frame on every call and returns esi zeroed -- measured as the object
        # registry root going 040D3A70 -> 0, the scene reading back as pixel
        # data, and the sequence resetting to Init mid-tutorial.
        det = _alias_detector(
            functions=[(0x00030000, 0x00030040), (0x00030800, 0x00030900)],
            aliases={0x00030100: 0x00030180},
            tables={0x00030500: [0x00030200, 0x00030210]},
            dispatch_at={},
            mid_at={0x00030120: 0x00030500},
            natural={0x00030100: 0x00030600})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x00030100].end, 0x00030600)

    def test_nothing_to_do_is_cheap(self):
        # No seeds and no declared extents: the pass must not force a rebuild.
        det = _detector([(0x1000, 0x1010)], {0x2000: (0.9, "call_target")})
        self.assertFalse(det._pass_demote_interior_seeds([]))
        self.assertEqual(det._candidates, {0x2000: (0.9, "call_target")})


if __name__ == "__main__":
    unittest.main()
