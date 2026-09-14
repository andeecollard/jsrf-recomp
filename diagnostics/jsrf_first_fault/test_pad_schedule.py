"""Lint every .pad schedule against the parser that will actually read it.

A broken pad schedule does not announce itself. It boots, it runs for four
minutes, and it produces a log that looks like a measurement -- of whatever the
title did while the input you thought you were sending was being dropped. Two
such traps are real and have both already cost time here, so they are asserted
rather than described:

  * SAME-AXIS STICK OVERLAP. src/input/xinput_device.c:743 writes a scripted
    deflection only while that axis is still near neutral:

        if (ev->lx && st->Gamepad.sThumbLX > -4096 && st->Gamepad.sThumbLX < 4096)

    so when two events overlap on one axis the second is REFUSED -- the first
    in file order holds the axis until it expires. (The guard is deliberate:
    it is what lets a live controller take over a scripted run.) moving.pad's
    header asserted the opposite for a while, that a later event replaces an
    earlier one. It got the right answer anyway because its segments happen to
    be adjacent rather than overlapping, which is exactly how this kind of
    mistake survives review.

  * A SYNTHETIC START DURING GAMEPLAY. Past the title screen START is PAUSE.
    A schedule that keeps firing it leaves the game sitting in covered pause,
    which reads precisely like a hang; one whole session went on diagnosing
    that. Schedules which must keep pressing START -- new_game.pad exists to
    get through logos of unpredictable length -- say so with the waiver
    comment, so that the exception is a decision and not an oversight.

The grammar below mirrors pad_script_parse()/pad_script_buttons(). If the C
changes, this must change with it: the point is to be the same parser, not a
reasonable approximation of one.
"""
import glob
import os
import unittest

PAD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pad")

PAD_SCRIPT_MAX = 256        # PAD_SCRIPT_MAX
PAD_SCRIPT_HOLD = 0.20      # PAD_SCRIPT_HOLD
GAMEPLAY_T = 48.0           # every boot prefix here has finished by t=47.2
WAIVER = "PAD-LINT: allow-late-start"

DIGITAL = ("START", "BACK", "LTHUMB", "RTHUMB", "UP", "DOWN", "LEFT", "RIGHT")
ANALOG = ("A", "B", "X", "Y", "WHITE", "BLACK", "LT", "RT")
STICK = {"LLEFT": "lx", "LRIGHT": "lx", "LDOWN": "ly", "LUP": "ly",
         "RLEFT": "rx", "RRIGHT": "rx", "RDOWN": "ry", "RUP": "ry"}
KNOWN = set(DIGITAL) | set(ANALOG) | set(STICK)


def parse(text):
    """(events, errors); an event is (t0, t1, names)."""
    events, errors = [], []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0]
        for token in line.replace(",", " ").replace(";", " ").split():
            head, sep, rest = token.partition(":")
            if not sep:
                errors.append("%r: expected <t>:<BUTTONS>" % token)
                continue
            buttons, _, hold = rest.partition(":")
            try:
                t0 = float(head)
            except ValueError:
                errors.append("%r: bad timestamp" % token)
                continue
            try:
                h = float(hold) if hold else PAD_SCRIPT_HOLD
            except ValueError:
                errors.append("%r: bad hold" % token)
                continue
            if h <= 0.0:
                h = PAD_SCRIPT_HOLD          # matches the C
            names = [n.upper() for n in buttons.split("+") if n]
            unknown = [n for n in names if n not in KNOWN]
            if unknown or not names:
                errors.append("%r: unknown button(s) %s" % (token, unknown))
                continue
            events.append((t0, t0 + h, names))
    return events, errors


def pad_files():
    return sorted(glob.glob(os.path.join(PAD_DIR, "*.pad")))


class PadScheduleTest(unittest.TestCase):
    def test_there_are_schedules_to_check(self):
        # Otherwise every test below passes vacuously, which is the one result
        # that would make this file worse than not existing.
        self.assertTrue(pad_files(), "no .pad files found in %s" % PAD_DIR)

    def test_every_event_parses(self):
        for path in pad_files():
            with open(path) as f:
                _, errors = parse(f.read())
            self.assertEqual(errors, [], "%s: %s" % (os.path.basename(path), errors))

    def test_event_count_within_parser_limit(self):
        for path in pad_files():
            with open(path) as f:
                events, _ = parse(f.read())
            self.assertLessEqual(
                len(events), PAD_SCRIPT_MAX,
                "%s has %d events; the parser silently drops past %d"
                % (os.path.basename(path), len(events), PAD_SCRIPT_MAX))

    def test_no_same_axis_stick_overlap(self):
        for path in pad_files():
            with open(path) as f:
                events, _ = parse(f.read())
            held = []           # (axis, t0, t1, names)
            for t0, t1, names in events:
                for axis in {STICK[n] for n in names if n in STICK}:
                    for (a, p0, p1, pn) in held:
                        if a == axis and t0 < p1 and p0 < t1:
                            self.fail(
                                "%s: %s (%.2f-%.2f) overlaps %s (%.2f-%.2f) on "
                                "axis %s -- the later deflection is REFUSED, so "
                                "this schedule does not do what it reads as"
                                % (os.path.basename(path), "+".join(names), t0, t1,
                                   "+".join(pn), p0, p1, axis))
                    held.append((axis, t0, t1, names))

    def test_no_synthetic_start_during_gameplay(self):
        for path in pad_files():
            with open(path) as f:
                text = f.read()
            if WAIVER in text:
                continue
            events, _ = parse(text)
            late = [t0 for t0, _, names in events
                    if "START" in names and t0 >= GAMEPLAY_T]
            self.assertEqual(
                late, [],
                "%s fires START at %s, past gameplay (t=%.0f), where START is "
                "PAUSE -- the game will sit in covered pause looking like a "
                "hang. If that is intended, add the comment '# %s'."
                % (os.path.basename(path), late, GAMEPLAY_T, WAIVER))


if __name__ == "__main__":
    unittest.main()
