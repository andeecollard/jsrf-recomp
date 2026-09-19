"""Link-order alignment must place names AND refuse to invent them.

The sweep here is paired with negative controls throughout, because the
failure mode that matters is not "no name" -- it is a confident wrong name in
a crash report, which is believed. A test that only checked that alignment
produces output would pass just as well against an aligner that guessed.
"""

import unittest

from .align import monotone_backbone, needleman_wunsch, align_gaps, same_function


class BackboneTest(unittest.TestCase):
    def test_keeps_the_increasing_run(self):
        pairs = [(0, 0), (1, 1), (2, 2), (3, 3)]
        keep, off = monotone_backbone(pairs)
        self.assertEqual(keep, pairs)
        self.assertEqual(off, [])

    def test_drops_an_anchor_that_goes_backwards(self):
        # The out-of-order one cannot bound a gap: either the function moved
        # or the name on it is wrong, and both make it useless as a scaffold.
        pairs = [(0, 0), (1, 9), (2, 1), (3, 2)]
        keep, off = monotone_backbone(pairs)
        self.assertIn((1, 9), off)
        self.assertEqual([d for _t, d in keep], [0, 1, 2])

    def test_empty(self):
        self.assertEqual(monotone_backbone([]), ([], []))


class NeedlemanWunschTest(unittest.TestCase):
    def test_equal_runs_pair_straight_through(self):
        self.assertEqual(needleman_wunsch([10, 20, 30], [10, 20, 30], 0),
                         [(0, 0), (1, 1), (2, 2)])

    def test_a_missing_donor_symbol_costs_only_itself(self):
        # The donor does not name the middle function. The two either side
        # must still pair -- that is the whole reason for the gap model.
        got = needleman_wunsch([10, 20, 30], [10, 30], 0)
        self.assertEqual(got, [(0, 0), (2, 1)])

    def test_an_extra_donor_symbol_costs_only_itself(self):
        got = needleman_wunsch([10, 30], [10, 20, 30], 0)
        self.assertEqual(got, [(0, 0), (1, 2)])

    def test_order_is_preserved(self):
        # Sizes that would pair better out of order must NOT be reordered:
        # link order is the entire evidence, so a crossing pair is not a hit.
        got = needleman_wunsch([10, 20], [20, 10], 0)
        for a, b in zip(got, got[1:]):
            self.assertLess(a[0], b[0])
            self.assertLess(a[1], b[1])

    def test_negative_control_disagreeing_sizes_pair_nothing(self):
        self.assertEqual(needleman_wunsch([11, 21, 31], [10, 20, 30], 0), [])

    def test_tolerance_is_honoured_and_is_not_unbounded(self):
        self.assertEqual(len(needleman_wunsch([11, 21], [10, 20], 1)), 2)
        self.assertEqual(needleman_wunsch([11, 21], [10, 20], 0), [])


class SameFunctionTest(unittest.TestCase):
    def test_identical(self):
        self.assertTrue(same_function("?A@C@@QAEXXZ", "?A@C@@QAEXXZ"))

    def test_namespace_and_return_type_differences_are_the_same_function(self):
        self.assertTrue(same_function(
            "?CodecReady@CAc97Device@DirectSound@@IAEHXZ",
            "?CodecReady@CAc97Device@@IAEHXZ"))
        self.assertTrue(same_function(
            "?SetupVoiceProcessor@CMcpxCore@DirectSound@@IAEXXZ",
            "?SetupVoiceProcessor@CMcpxCore@DirectSound@@IAEJXZ"))

    def test_negative_control_a_different_method_is_not_the_same(self):
        self.assertFalse(same_function(
            "?GetStatus@CDirectSoundStream@DirectSound@@UAGJPAK@Z",
            "?Pause@CDirectSoundBuffer@DirectSound@@QAGJK@Z"))

    def test_negative_control_same_method_different_class(self):
        self.assertFalse(same_function("?Release@CFoo@@UAGKXZ",
                                       "?Release@CBar@@UAGKXZ"))

    def test_unmangled_names_compare_by_equality_only(self):
        self.assertTrue(same_function("_XAudioCalculatePitch@4",
                                      "_XAudioCalculatePitch@4"))
        self.assertFalse(same_function("_XAudioCalculatePitch@4",
                                       "_XAudioDownloadEffectsImage@20"))


class AlignGapsTest(unittest.TestCase):
    """End to end over a miniature section, anchors and all."""

    # (addr, size) in address order; 0x20 and 0x50 are the anchors.
    FUNCS = [(0x20, 8), (0x30, 16), (0x40, 24), (0x50, 8)]
    DONOR = [("anchorA", 8), ("mid1", 16), ("mid2", 24), ("anchorB", 8)]
    NAMED = {0x20: "anchorA", 0x50: "anchorB"}

    def test_fills_the_gap_between_two_anchors(self):
        got, backbone, off, _rej = align_gaps(self.FUNCS, self.NAMED,
                                              self.DONOR, size_tol=0)
        self.assertEqual(len(backbone), 2)
        self.assertEqual(got.get(0x30), {"mid1"})
        self.assertEqual(got.get(0x40), {"mid2"})

    def test_never_overwrites_an_existing_name(self):
        named = dict(self.NAMED)
        named[0x30] = "already-here"
        got, _b, _o, _r = align_gaps(self.FUNCS, named, self.DONOR, size_tol=0)
        self.assertNotIn(0x30, got)

    def test_negative_control_wrong_sizes_fill_nothing(self):
        donor = [("anchorA", 8), ("mid1", 99), ("mid2", 98), ("anchorB", 8)]
        got, _b, _o, _r = align_gaps(self.FUNCS, self.NAMED, donor, size_tol=0)
        self.assertEqual(got, {},
                         "sizes disagree throughout, so nothing may be placed")

    def test_nothing_is_placed_outside_the_anchored_span(self):
        # A run open at one end has no second bound, so its correspondence is
        # unknown however well the sizes happen to line up.
        funcs = [(0x10, 16)] + self.FUNCS + [(0x60, 24)]
        donor = [("before", 16)] + self.DONOR + [("after", 24)]
        got, _b, _o, _r = align_gaps(funcs, self.NAMED, donor, size_tol=0)
        self.assertNotIn(0x10, got)
        self.assertNotIn(0x60, got)

    def test_negative_control_no_anchors_means_no_output(self):
        got, backbone, _o, _r = align_gaps(self.FUNCS, {}, self.DONOR, size_tol=0)
        self.assertEqual(backbone, [])
        self.assertEqual(got, {})


if __name__ == "__main__":
    unittest.main()
