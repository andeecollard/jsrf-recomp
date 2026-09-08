"""Verify bounded combiner grouping/capture through the public method sink."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class CombinerTraceTest(unittest.TestCase):
    def test_grouping_and_capture(self):
        root = Path(__file__).resolve().parents[2]
        build_dir = Path(os.environ.get(
            "JSRF_TEST_BUILD_DIR",
            root / "build-macos/jsrf-first-fault/build",
        ))
        renderer = build_dir / "jsrf_vsh_render_test"
        with tempfile.TemporaryDirectory(prefix="jsrf-combiner-trace-") as directory:
            prefix = str(Path(directory) / "draw-")
            env = dict(os.environ, RECOMP_COMBINER_TRACE="1", RECOMP_DRAW_CAPTURE=prefix)
            result = subprocess.run([str(renderer)], env=env, check=True,
                                    capture_output=True, text=True)
            log = result.stderr
            self.assertEqual(log.count("[COMBINER] new config="), 3)
            self.assertIn("distinct=3 overflow-draws=0", log)
            self.assertIn("config=0 first-draw=1 draws=2 rejected=0 inline=2 invalid-position=0 collapsed-xy=0", log)
            self.assertIn("config=1 first-draw=4 draws=2 rejected=1 inline=2 invalid-position=0 collapsed-xy=0", log)
            # Two draws in config 2: the modulate draw and the alternate-winding
            # triangle strip that vsh_render_test added after this expectation
            # was first written. The strip changes cull/front-face only, so it
            # belongs to the same combiner config -- grouping it separately
            # would be the bug. Capture still fires once, on first-draw=6.
            self.assertIn("config=2 first-draw=6 draws=2 rejected=0 inline=2 invalid-position=0 collapsed-xy=0", log)
            self.assertIn("input x -0.5..2559.5  y -0.5..1919.5", log)
            captures = {int(p.stem.split("-")[-1]): json.loads(p.read_text())
                        for p in Path(directory).glob("*.json")}
            self.assertEqual(set(captures), {1, 4, 5, 6})
            self.assertEqual(captures[5]["reject_reason"], "blending")
            self.assertFalse(captures[5]["copy_supported"])
            self.assertEqual(captures[5]["texture_address"], 0)
            self.assertEqual(captures[5]["target_address"], 0)
            self.assertTrue(captures[6]["copy_supported"])
            self.assertEqual(captures[6]["registers"]["1e60"], 4)
            self.assertEqual(captures[6]["inline_words"], 12)


if __name__ == "__main__":
    unittest.main()
