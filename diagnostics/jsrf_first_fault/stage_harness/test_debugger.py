import json
import math
from pathlib import Path
import tempfile
import threading
import time
import unittest
import debug
from run import Driver, Failure


def state():
    return {'table_ok': True, 'sequence': 30, 'frame': 1, 'players': {
        '44': {'object': 100, 'position': [0, 0, 0], 'state': 23}, '45': None},
        'manager': {'mission': {'state': 7}}}


class DebuggerTests(unittest.TestCase):
    def test_replaced_player_is_not_movement(self):
        a, b = state(), state()
        b['players']['44']['object'] = 101
        b['players']['44']['position'] = [100, 0, 0]
        result = debug.delta(a, b)['players']['44']
        self.assertFalse(result['comparable'])
        self.assertNotIn('distance', result)

    def test_object_diff_separates_replacement_and_real_field_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = [Path(tmp) / name for name in ('a.json', 'b.json')]
            a.write_text(json.dumps({'frame': 1, 'objects': [
                {'id': 8, 'address': 100, 'vtable': 10, 'words': [1, 2]},
                {'id': 44, 'address': 200, 'vtable': 11, 'words': [1, 2]}]}))
            b.write_text(json.dumps({'frame': 2, 'objects': [
                {'id': 8, 'address': 100, 'vtable': 10, 'words': [1, 3]},
                {'id': 44, 'address': 300, 'vtable': 11, 'words': [1, 99]}]}))
            result = debug.object_diff(a, b)['changes']
            self.assertEqual(result[0]['words'][0]['offset'], '0x004')
            self.assertEqual(result[1], {'id': 44, 'change': 'replaced'})

    def test_no_unknown_value_can_satisfy_watch(self):
        class D:
            def read(self): return state()
        with self.assertRaises(Failure):
            debug.execute(D(), {'kind': 'wait', 'field': 'manager.absent', 'equals': None})

    def test_real_mailbox_status_and_stop(self):
        class D:
            def __init__(self, out): self.out, self.latest = out, state()
            def read(self): return self.latest
            def event(self, *args, **kwargs): pass
            def command(self, *args, **kwargs): pass
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            thread = threading.Thread(target=debug.serve, args=(D(out), 3))
            thread.start()
            try:
                until = time.monotonic() + 1
                while not (out / 'session.json').exists() and time.monotonic() < until:
                    time.sleep(.01)
                reply = debug.request(out, {'kind': 'status'})
                self.assertTrue(reply['ok'])
                self.assertEqual(reply['result']['sequence'], 30)
                reply = debug.request(out, {'kind': 'stop'})
                self.assertTrue(reply['ok'])
            finally:
                thread.join(timeout=4)
            self.assertFalse(thread.is_alive())
            with self.assertRaises(Failure): debug.request(out, {'kind': 'status'})

    def test_steering_turns_toward_the_target_and_measures_its_own_handedness(self):
        north, east = (0, -1), (1, 0)
        self.assertAlmostEqual(abs(debug.signed_angle(north, north)), 0)
        quarter = debug.signed_angle(north, east)
        self.assertAlmostEqual(abs(quarter), math.pi / 2)
        # Straight on is the presets' own LUP; a turn leaves it, and the two
        # senses are mirror images, which is what the run-time calibration picks
        # between. Nothing here assumes which one this game uses.
        self.assertEqual(debug.stick_for(0, 1), (0, 0, 0, 0, 0, debug.STICK))
        left, right = debug.stick_for(quarter, 1), debug.stick_for(quarter, -1)
        self.assertEqual(left[4], -right[4])
        self.assertTrue(abs(left[4]) == debug.STICK)

    def test_stick_vectors_stay_inside_the_bridge_bounds(self):
        for degrees in range(-360, 361, 7):
            for sense in (1, -1):
                pad = debug.stick_for(math.radians(degrees), sense)
                self.assertEqual(len(pad), 6)
                self.assertTrue(all(0 <= v <= 255 for v in pad[:4]), pad)
                self.assertTrue(all(-32768 <= v <= 32767 for v in pad[4:]), pad)

    def test_navigation_reports_being_stuck_instead_of_claiming_arrival(self):
        class D:
            latest = state()
            def __init__(self): self.commands = 0
            def read(self):
                s = state()
                s['players']['44']['position'] = [0, 0, 0]   # never moves
                s['players']['45'] = {'object': 200, 'position': [500, 0, 0], 'state': 1}
                s['deliveries'], s['command'] = 1, self.commands
                return s
            def command(self, *a, **k):
                self.commands += 1
                return self.commands
            def settle(self, seconds): pass
            def event(self, *a, **k): pass
        # A player that never moves at all is held against something, which is
        # a different report from one that moves without closing.
        result = debug.navigate(D(), {'target_player': 45, 'seconds': 8, 'tolerance': 10})
        self.assertEqual(result['outcome'], 'wedged')
        self.assertGreater(result['distance'], 10)
        self.assertTrue(result['track'])

    def test_a_discovered_player_can_be_walked_to(self):
        s = state()
        s['cplayers'] = {'44': s['players']['44'],
                         '61': {'object': 300, 'position': [9, 0, 9], 'state': 1}}
        self.assertEqual(debug.player_of(s, 61)['object'], 300)
        self.assertEqual(debug.player_of(s, '44')['object'], 100)
        self.assertIsNone(debug.player_of(s, 45))      # present but null

        class D:
            def __init__(self): self.commands = 0
            def read(self):
                r = state()
                r['cplayers'] = {'44': r['players']['44'],
                                 '61': {'object': 300, 'position': [9, 0, 9], 'state': 1}}
                r['deliveries'], r['command'] = 1, self.commands
                return r
            def command(self, *a, **k):
                self.commands += 1
                return self.commands
            def settle(self, seconds): pass
            def event(self, *a, **k): pass
        # Id 61 is in no fixed pair, and the walk must still aim at it.
        result = debug.navigate(D(), {'target_player': 61, 'seconds': 6, 'tolerance': 2})
        self.assertEqual(result['outcome'], 'wedged')
        self.assertAlmostEqual(result['track'][0]['distance'], 12.73, places=1)

    def test_a_route_is_walked_before_the_target_and_is_not_a_place_to_stand(self):
        # The walker crawls along +x whatever it is told, and the route lies
        # that way, so each waypoint is retired in turn and only the final
        # target can report arrival.
        class D:
            def __init__(self): self.commands, self.at = 0, [0.0, 0.0, 0.0]
            def read(self):
                r = state()
                r['players']['44'] = {'object': 100, 'position': list(self.at), 'state': 1}
                r['deliveries'], r['command'] = 1, self.commands
                return r
            def command(self, *a, **k):
                self.commands += 1
                return self.commands
            def settle(self, seconds): pass
            def event(self, kind, **fields): self.events.append((kind, fields))
            events = []
        driver = D()
        original = debug.drive

        def crawl(d, seconds, pad, seq, tick=.15):
            pad(d.read())               # the pad is still asked for, as in a real leg
            d.at[0] += 10.0
            return True, None
        try:
            debug.drive = crawl
            result = debug.navigate(driver, {'x': 300, 'z': 0, 'route': [[100, 0], [200, 0]],
                                             'tolerance': 12, 'seconds': 30})
        finally:
            debug.drive = original
        self.assertEqual(result['outcome'], 'arrived')
        self.assertEqual(result['waypoints_left'], 0)
        self.assertEqual([f['remaining'] for k, f in driver.events if k == 'waypoint_reached'],
                         [1, 0])

    def test_a_route_must_be_finite_pairs(self):
        class D:
            def read(self): return state()
        for bad in ([[1]], [[1, float('nan')]], 'route', [[1, 2]] * 201):
            with self.subTest(route=bad), self.assertRaises(Failure):
                debug.navigate(D(), {'x': 1, 'z': 1, 'route': bad})

    def test_conversation_cannot_end_on_an_unknown_field(self):
        class D:
            def read(self): return state()
        for bad in ({'kind': 'talk', 'field': 'manager.absent', 'equals': None},
                    {'kind': 'talk', 'field': 'sequence', 'equals': 30},
                    {'kind': 'talk', 'field': 'manager.mission.state', 'equals': 7, 'count': 99}):
            with self.subTest(req=bad), self.assertRaises(Failure):
                debug.execute(D(), bad)

    def test_command_range_is_bounded(self):
        class D:
            def read(self): return state()
        for seconds in (float('nan'), float('inf'), -1, 31):
            with self.subTest(seconds=seconds), self.assertRaises(Failure):
                debug.execute(D(), {'kind': 'hold', 'button': 'LUP', 'seconds': seconds})


if __name__ == '__main__': unittest.main()
