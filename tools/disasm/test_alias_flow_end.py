"""An alias measured against a boundary that no longer exists ends where its
own flow ends.

The tail-jump and seed passes record an alias as running to the end of the
body it lands in. That body was measured while a switch arm was still a seeded
start, so it ended at the arm -- and so did every alias inside it. The
seed-interior pass then drops the arm and the real bodies are re-measured, but
the alias record keeps the stale end. On JSRF gen 46bb115c, 139 of 142
unresolved switch dispatches were `jmp [reg*4 + table]` translated as dead
code inside such a padded alias, while the function that really owned the
dispatch was never found because the padding covered its start.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.test_seed_interior import (  # noqa: E402
    _alias_detector, _Func)


class AliasFlowEnd(unittest.TestCase):
    def _det(self, aliases, natural, candidates=(), functions=None):
        det = _alias_detector(
            functions=functions or [(0x1000, 0x1100)],
            aliases=aliases, tables={}, dispatch_at={}, natural=natural)
        det._candidates = {a: (0.9, "seed_vtable_thunk") for a in candidates}
        return det

    def test_alias_shrinks_to_its_flow_when_its_boundary_was_demoted(self):
        # Alias at 0x1200 was recorded to 0x1300, the arm that has since been
        # dropped: no function, no candidate, no alias starts there. Its flow
        # ends at 0x1240.
        det = self._det({0x1200: 0x1300}, natural={0x1200: 0x1240})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x1200].end, 0x1240)
        self.assertEqual(det._alias_entries[0x1200], 0x1240)

    def test_alias_keeps_a_boundary_that_still_starts_something(self):
        # Same shape, but 0x1300 is a real function start: the alias may
        # genuinely share the body up to it, so the record stands.
        det = self._det({0x1200: 0x1300}, natural={0x1200: 0x1240},
                        functions=[(0x1000, 0x1100), (0x1300, 0x1400)])
        det._build_alias_entries()
        self.assertEqual(det.functions[0x1200].end, 0x1300)

    def test_alias_keeps_a_boundary_that_is_still_a_candidate(self):
        # The seed-interior pass is off: the arm is still a candidate, so
        # nothing has been demoted and nothing is stale.
        det = self._det({0x1200: 0x1300}, natural={0x1200: 0x1240},
                        candidates=[0x1300])
        det._build_alias_entries()
        self.assertEqual(det.functions[0x1200].end, 0x1300)

    def test_alias_grows_when_the_vanished_boundary_was_cutting_it(self):
        # The dropped start sat in the middle of the alias's real flow.
        det = self._det({0x1200: 0x1300}, natural={0x1200: 0x1380})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x1200].end, 0x1380)

    def test_section_end_is_a_boundary(self):
        # _Section runs 0..0x01000000; an alias recorded to the section end
        # is not stale even though nothing starts there.
        det = self._det({0x1200: 0x01000000}, natural={0x1200: 0x1240})
        det._build_alias_entries()
        self.assertEqual(det.functions[0x1200].end, 0x01000000)


if __name__ == "__main__":
    unittest.main()
