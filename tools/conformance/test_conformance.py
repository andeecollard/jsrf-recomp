"""Runs the differential conformance suite as part of `pytest tools/`.

Skipped where it cannot run rather than failed: it needs a 32-bit MSVC to
assemble the snippets and build the harness, so it is Windows-only. The rest
of the test suite stays meaningful on Linux.
"""

import unittest

from . import __main__ as conformance


class ConformanceTest(unittest.TestCase):
    def test_lifted_code_matches_the_cpu(self):
        if conformance._find_vcvars() is None:
            self.skipTest("needs a 32-bit MSVC (vcvars32.bat) to assemble and "
                          "build the harness")
        rc = conformance.main_with_args([], allow_container=False)
        self.assertEqual(rc, 0, "the lifted C disagreed with the CPU; run "
                                "`py -3 -m tools.conformance` for the detail")


if __name__ == "__main__":
    unittest.main()
