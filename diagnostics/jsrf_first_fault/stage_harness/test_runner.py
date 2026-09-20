import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import run


class Clock:
    def __init__(self): self.now = 0
    def monotonic(self): return self.now
    def sleep(self, n): self.now += n


class Gates(unittest.TestCase):
    def test_unknown_scene_and_missing_player_cannot_pass(self):
        cond = {'sequence': [30], 'players': [44]}
        s = {'sequence': 30, 'table_ok': False, 'players': {'44': {'position': [1, 2, 3]}}}
        self.assertFalse(run.matches(s, cond))
        s['table_ok'] = True
        self.assertTrue(run.matches(s, cond))
        s['players']['44'] = None
        self.assertFalse(run.matches(s, cond))
        s['players']['44'] = {'position': [float('nan'), 0, 0]}
        self.assertFalse(run.matches(s, cond))

    def test_frozen_scene_cannot_pass_even_with_live_observer(self):
        clock = Clock()
        driver = object.__new__(run.Driver)
        driver.read = lambda: {'table_ok': True, 'sequence': 12, 'frame': 9}
        with patch.object(run, 'time', clock), self.assertRaises(run.Failure):
            driver.wait_for({'sequence': [12]}, timeout=2)

    def test_variable_loading_time_and_release_of_stability(self):
        clock = Clock()
        driver = object.__new__(run.Driver)
        driver.read = lambda: {'table_ok': True, 'sequence': 8 if clock.now < 1 else 12,
                               'frame': int(clock.now * 60)}
        with patch.object(run, 'time', clock):
            result = driver.wait_for({'sequence': [12]}, timeout=3)
        self.assertEqual(result['sequence'], 12)
        self.assertGreaterEqual(clock.now, 1.5)

    def test_missing_observer_fails(self):
        clock = Clock()
        class Process:
            def poll(self): return None
        with tempfile.TemporaryDirectory() as tmp, patch.object(run, 'time', clock):
            driver = run.Driver(Path(tmp), Process())
            clock.now = 6
            with self.assertRaisesRegex(run.Failure, 'heartbeat'):
                driver.read()
            driver.events.close()

    def test_navigation_can_advance_while_waiting_to_press(self):
        clock = Clock()
        driver = object.__new__(run.Driver)
        driver.read = lambda: {'table_ok': True, 'sequence': 12 if clock.now < .2 else 30,
                               'frame': int(clock.now * 60)}
        driver.event = lambda *args, **kwargs: None
        driver.pulse = lambda *args: self.fail('Must not press after reaching target')
        step = {'name': 'advance', 'timeout': 3, 'expect': {'sequence': [30]},
                'act_in': [12], 'attempts': 2, 'settle': 1, 'actions': [{'button': 'START'}]}
        with patch.object(run, 'time', clock):
            result = driver.navigate(step)
        self.assertEqual(result['sequence'], 30)

    def test_source_tree_manifest_detects_content_and_empty_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'save').write_bytes(b'abc')
            a = run.tree_manifest(root)
            (root / 'save').write_bytes(b'abd')
            self.assertNotEqual(a, run.tree_manifest(root))
            b = run.tree_manifest(root)
            (root / 'empty').mkdir()
            self.assertNotEqual(b, run.tree_manifest(root))

    def test_initial_scenario_does_not_claim_tutorial_completion(self):
        scenario = json.loads(Path(__file__).with_name('startup.json').read_text())
        run.validate(scenario)
        self.assertEqual(scenario['outcome'], 'review_required')


if __name__ == '__main__': unittest.main()
