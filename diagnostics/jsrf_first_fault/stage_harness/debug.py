#!/usr/bin/env python3
"""Bounded interactive debugger for a stage-harness session, or offline reports."""
import argparse
import fcntl
import json
import math
from pathlib import Path
import time
import uuid
from run import BUTTONS, Failure, atomic_json


def field(state, path):
    value = state
    for part in path.split('.'):
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return value


def delta(before, after):
    result = {'frames': after['frame'] - before['frame'],
              'sequence': [before['sequence'], after['sequence']], 'players': {}}
    for ident in ('44', '45'):
        a, b = before.get('players', {}).get(ident), after.get('players', {}).get(ident)
        if not a or not b:
            result['players'][ident] = {'comparable': False, 'reason': 'missing player'}
        elif a['object'] != b['object']:
            result['players'][ident] = {'comparable': False, 'reason': 'object replaced'}
        else:
            result['players'][ident] = {'comparable': True,
                'distance': math.dist(a['position'], b['position']),
                'state': [a['state'], b['state']]}
    result['mission_state'] = [field(before, 'manager.mission.state'), field(after, 'manager.mission.state')]
    result['note'] = 'Observed change; cutscenes/animation can move players without input.'
    return result


STICK = 24000          # the magnitude the four presets already use
HEADING_MIN = 3.0      # a leg shorter than this says nothing about facing
# Which way a positive world turn maps onto the stick is a handedness question,
# and it is measured, never assumed. It is remembered between navigations in one
# session because it is a property of the game, not of a particular walk.
SENSE = [1]


def drive(driver, seconds, pad, seq, tick=.15):
    """Hold an input for `seconds`, renewing the short lease before it expires
    and releasing it however this returns. `pad` is called each tick with the
    live reading and returns a button name or an explicit stick tuple, so a
    constant hold and a steering loop share one guard: the scene may not change
    under a live lease, and the hook must acknowledge at least one delivery.

    Returns (delivered, stopped_state). A scene change is reported, not raised:
    for a hold it is a cancelled experiment, for navigation it may be the
    objective firing, and only the caller knows which.
    """
    deadline = time.monotonic() + seconds
    delivered, last_command, stopped = False, None, None
    try:
        while time.monotonic() < deadline:
            s = driver.read()
            if s['sequence'] != seq:
                stopped = s
                break
            if last_command is not None and s['command'] == last_command:
                delivered |= s['deliveries'] > 0
            choice = pad(s)
            last_command = (driver.command(choice, ttl=300) if isinstance(choice, str)
                            else driver.command('NEUTRAL', ttl=300, stick=choice))
            driver.settle(min(tick, max(0, deadline - time.monotonic())))
        s = driver.read()
        if s['command'] == last_command:
            delivered |= s['deliveries'] > 0
    finally:
        driver.command()
    return delivered, stopped


def signed_angle(a, b):
    """Angle from 2-vector a to b, in radians. Both are (x, z) in world space."""
    return math.atan2(a[0] * b[1] - a[1] * b[0], a[0] * b[0] + a[1] * b[1])


def stick_for(turn, sense, magnitude=STICK):
    """Stick vector that asks for a turn of `turn` radians away from straight on.

    JSRF's camera chases the character, so 'up' means 'keep the heading you
    already have': five consecutive forward legs on 20 Sep 2026 converged on one
    world heading (0.00, -1.00) while speed rose 8 -> 55 units a leg. Camera
    forward is therefore the current heading, and a stick angle is a turn
    relative to it. `sense` is +1 or -1 and is MEASURED at run time, because
    which screen direction a positive world rotation corresponds to is a
    handedness question this code does not assume.
    """
    turn = max(-math.pi, min(math.pi, turn)) * sense
    magnitude = max(1, min(STICK, int(magnitude)))
    return (0, 0, 0, 0, round(magnitude * math.sin(turn)), round(magnitude * math.cos(turn)))


def flat(p):
    return (p[0], p[2])


def player_of(state, ident):
    """A player by id, from the discovered list first and the fixed pair second.

    Scenarios written against players.44/45 keep working; anything that has to
    find a character the Garage did not start with reads `cplayers`.
    """
    ident = str(ident)
    for group in ('cplayers', 'players'):
        found = (state.get(group) or {}).get(ident)
        if found:
            return found
    return None


def navigate(driver, req):
    """Walk player 44 to a world (x, z), or to another player's live position.

    The loop steers by the heading it just measured rather than by any stored
    stick-to-world table: the camera turns with the character, so such a table
    is stale as soon as it is written. Reports where it stopped and why; a
    stall with a position is the useful debugging output, not a failure to hide.
    """
    player = str(req.get('player', 44))
    tolerance = req.get('tolerance', 15.0)
    seconds = req.get('seconds', 30)
    if not 1 <= tolerance <= 400:
        raise Failure('Tolerance must be 1..400 units')
    # A route is walked waypoint by waypoint before the real target. Steering
    # can follow a wall it is touching; it cannot know the way through a level,
    # and two sessions ended circling ~280 units short of the next character
    # because of it. The waypoints come from somebody who knew the way.
    route = req.get('route') or []
    if type(route) is not list or len(route) > 200 \
            or any(type(w) is not list or len(w) != 2
                   or not all(type(v) in (int, float) and math.isfinite(v) for v in w)
                   for w in route):
        raise Failure('A route is up to 200 finite [x, z] pairs')
    target_player = req.get('target_player')
    if target_player is not None and (type(target_player) is not int
                                      or not 0 <= target_player < 7668):
        raise Failure('Target player must be a registry id')
    if target_player is None and ('x' not in req or 'z' not in req):
        raise Failure('Navigation needs x and z, or target_player')

    legs_left, skipped = list(route), []

    def final_of(s):
        if target_player is None:
            return (float(req['x']), float(req['z']))
        found = player_of(s, target_player)
        return flat(found['position']) if found else None

    def target_of(s):
        return tuple(legs_left[0]) if legs_left else final_of(s)

    start = driver.read()
    seq = start['sequence']
    sense = req.get('sense', SENSE[0])
    if sense not in (1, -1):
        raise Failure('Sense must be 1 or -1')
    heading, track = None, []
    deadline = time.monotonic() + seconds
    best, best_align, stalls, calibrations, wedged = None, None, 0, 0, 0
    while time.monotonic() < deadline:
        s = driver.read()
        walker = player_of(s, player)
        here = walker['position'] if walker else None
        goal = target_of(s)
        if not here or not goal:
            return {'outcome': 'unreadable', 'reason': 'player or target position missing',
                    'state': s, 'track': track}
        here = flat(here)
        distance = math.dist(here, goal)
        track.append({'at': [round(v, 2) for v in here], 'distance': round(distance, 2),
                      'frame': s['frame'], 'state': walker['state']})
        waypoint_tolerance = max(tolerance, 35)
        if legs_left and distance <= waypoint_tolerance:
            legs_left.pop(0)
            driver.event('waypoint_reached', remaining=len(legs_left),
                         at=[round(v, 1) for v in here])
            best, best_align, stalls, wedged = None, None, 0, 0
            continue
        if not legs_left and distance <= tolerance:
            return {'outcome': 'arrived', 'distance': distance, 'tolerance': tolerance,
                    'sense': sense, 'state': s, 'track': track, 'waypoints_left': 0,
                    'waypoints_skipped': skipped,
                    'claim': 'Position within tolerance; not proof the game registered arrival.'}
        # Near the target the legs are deliberately short, so demanding the
        # same two units of progress from each one reports a stall in the
        # middle of a converging approach.
        near = distance <= max(1.2 * tolerance, 20)
        desired = tuple((goal[i] - here[i]) / distance for i in (0, 1))
        align = None if heading is None else heading[0] * desired[0] + heading[1] * desired[1]
        # Turning round is not stalling. Walking back the way it came needs a
        # half turn, and a skating character takes longer than the old six-leg
        # budget to make one: three return trips in one boot on 20 Sep 2026 were
        # all called stalled while the character was still coming about. A leg
        # counts as progress if it closed distance OR improved the heading.
        closed = best is None or distance < best - (.5 if near else 2)
        coming_about = align is not None and (best_align is None or align > best_align + .05)
        if wedged:
            pass          # an escape attempt is not a stall; `wedged` bounds it
        elif closed or coming_about:
            stalls = 0
            if closed:
                best = distance
            if align is not None and (best_align is None or align > best_align):
                best_align = align
        else:
            stalls += 1
            if stalls >= 8 and legs_left:
                # A waypoint is a hint about the way, not a place that must be
                # stood on: the drive that produced it may have been hugging a
                # wall. Drop it and aim at the next rather than abandon the walk.
                dropped = legs_left.pop(0)
                skipped.append([round(v, 1) for v in dropped])
                driver.event('waypoint_skipped', at=dropped, remaining=len(legs_left))
                best, best_align, stalls, wedged = None, None, 0, 0
                continue
            if stalls >= 8:
                return {'outcome': 'stalled', 'distance': distance, 'closest': best,
                        'sense': sense, 'alignment': align, 'state': s, 'track': track,
                        'waypoints_left': len(legs_left), 'waypoints_skipped': skipped,
                        'claim': 'Eight legs without closing distance or improving heading: '
                                 'blocked, or steering cannot reach this heading from here.'}
        # Skating carries. A 0.35s leg covers ~15 units at full speed and the
        # character drifts ~8 more after release (measured 20 Sep), so holding
        # the same leg length all the way in produces an orbit: run 06 closed to
        # 25 units, overshot to 47 and came back to 25 without ever stopping.
        # Near the target the leg shortens and the stick is released between
        # legs, so the next reading is of a slowed character.
        if heading is None:
            turn, leg = 0.0, .7          # nothing measured yet: go straight and watch
        else:
            turn = signed_angle(heading, desired)
            leg = .25 if near or abs(turn) > 1.0 else .35
        if wedged:
            # Pressed into something. Steering harder at the target only presses
            # harder: on 20 Sep 2026 a character wedged in a corner moved within
            # +/-6 units for fifteen legs while being aimed straight at its
            # target. Follow the obstacle instead -- a quarter turn each way,
            # then away from it -- which is what a player does. This is not
            # pathfinding and does not pretend to be: it escapes a wall it is
            # touching, and nothing more.
            turn += (math.pi / 2, -math.pi / 2, math.pi)[(wedged - 1) % 3]
            leg = .5
        # Full deflection is a skate, a small one a walk, and a skating
        # character has a turning circle: runs 07 and 09 orbited the target at
        # radii of 20 and 40 units, always at full stick. So the stick is cut
        # for BOTH reasons a player would ease off -- the target is close, or
        # the turn being asked for is sharp. Whichever wants less, wins.
        turning = 1.0 if abs(turn) < 1.0 else .6
        closing = max(.55, min(1.0, distance / 45.0))
        magnitude = int(STICK * min(turning, closing))
        moved, stopped = drive(driver, leg,
                               lambda _s: stick_for(turn, sense, magnitude), seq)
        if near:
            driver.settle(.2)            # coast with the lease already released
        after = driver.read()
        if stopped is not None:
            return {'outcome': 'scene_changed', 'sequence': [seq, stopped['sequence']],
                    'state': after, 'track': track,
                    'claim': 'The scene changed while walking; this may be the objective '
                             'firing or an unrelated transition.'}
        moved_to = player_of(after, player)
        if not moved_to:
            continue
        now = flat(moved_to['position'])
        step = (now[0] - here[0], now[1] - here[1])
        length = math.hypot(*step)
        if length < HEADING_MIN:
            if not moved:
                return {'outcome': 'no_input', 'state': after, 'track': track,
                        'claim': 'The input hook acknowledged no delivery during a leg.'}
            wedged += 1
            if wedged > 6 and legs_left:
                dropped = legs_left.pop(0)
                skipped.append([round(v, 1) for v in dropped])
                driver.event('waypoint_skipped', at=dropped, remaining=len(legs_left))
                best, best_align, stalls, wedged = None, None, 0, 0
                continue
            if wedged > 6:
                return {'outcome': 'wedged', 'distance': distance, 'closest': best,
                        'sense': sense, 'state': after, 'track': track,
                        'waypoints_left': len(legs_left), 'waypoints_skipped': skipped,
                        'claim': 'Held against an obstacle with every escape turn tried. '
                                 'A route of waypoints is what this needs, not more steering.'}
            continue
        wedged = 0
        new_heading = (step[0] / length, step[1] / length)
        # Measure the handedness, and only from a leg that can actually show it:
        # a small turn is swamped by the inertia of the heading the character
        # already had. Latching on the first marginal leg sent four of eight
        # trials off in the wrong direction on 20 Sep 2026, so this re-checks.
        if heading is not None and abs(turn) > .6 and calibrations < 2:
            observed = signed_angle(heading, new_heading)
            if abs(observed) > .25:
                calibrations += 1
                if observed * turn < 0:
                    sense = -sense
                    SENSE[0] = sense
                    driver.event('navigation_sense', sense=sense, asked=round(turn, 3),
                                 observed=round(observed, 3))
        heading = new_heading
    return {'outcome': 'deadline', 'closest': best, 'sense': sense,
            'waypoints_left': len(legs_left), 'waypoints_skipped': skipped, 'state': driver.read(), 'track': track}


def converse(driver, req):
    """Press A until a field reaches a value, reporting every line of dialogue.

    The text node at +0x54 holds the span the box is CURRENTLY typing, so the
    same line is read several times as it grows; the full line is the last
    reading before it changes. Bounded, and an unknown value can never satisfy
    it -- the same rule `wait` follows.
    """
    path, expected = req.get('field', ''), req.get('equals')
    presses = req.get('count', 12)
    button = req.get('button', 'A')
    if button not in BUTTONS or button == 'NEUTRAL':
        raise Failure('Unknown button')
    if not path.startswith(('manager.', 'players.')):
        raise Failure('Conversation must end on a manager.* or players.* field')
    if expected is None:
        raise Failure('Conversation cannot use an unknown/null value as success')
    if type(presses) is not int or not 1 <= presses <= 30:
        raise Failure('Conversation needs 1..30 presses')
    lines, seen, opened = [], set(), None
    # The button that OPENS an interaction is not the button that advances it.
    # Pressing RT repeatedly beside the other player on 20 Sep 2026 flipped that
    # player between states 24 and 25 and showed one dialogue span ('Wh') that
    # vanished again: each press cancelled the conversation the one before it
    # had started. An opener is therefore delivered exactly once.
    opener = req.get('opener')
    if opener is not None:
        if opener not in BUTTONS or opener == 'NEUTRAL':
            raise Failure('Unknown opener button')
        s = driver.read()
        opened, stopped = drive(driver, .3, lambda _s: opener, s['sequence'])
        if stopped is not None:
            return {'outcome': 'scene_changed', 'lines': lines, 'opened': opened,
                    'state': driver.read()}
        if not opened:
            raise Failure('No input-hook delivery acknowledged for the opener')
        driver.settle(1.0)
    for press in range(presses):
        s = driver.read()
        for node in s.get('text_nodes', []):
            if node['text'] not in seen:
                seen.add(node['text'])
                lines.append({'id': node['id'], 'text': node['text'], 'frame': s['frame']})
        if field(s, path) == expected and s['table_ok']:
            return {'outcome': 'reached', 'presses': press, 'lines': lines, 'state': s}
        seq = s['sequence']
        delivered, stopped = drive(driver, .3, lambda _s: button, seq)
        if stopped is not None:
            return {'outcome': 'scene_changed', 'lines': lines, 'state': driver.read()}
        if not delivered:
            raise Failure('No input-hook delivery acknowledged during a dialogue press')
        driver.settle(.55)
    s = driver.read()
    if field(s, path) == expected and s['table_ok']:
        return {'outcome': 'reached', 'presses': presses, 'lines': lines, 'state': s}
    return {'outcome': 'exhausted', 'presses': presses, 'lines': lines, 'state': s,
            'claim': '%s never reached %r within %d presses.' % (path, expected, presses)}


def capture(driver):
    ident = driver.command(snapshot=2)
    deadline = time.monotonic() + 2
    objects = driver.out / ('objects-%s.json' % ident)
    while time.monotonic() < deadline:
        driver.read()
        if objects.exists():
            picture = driver.out / ('picture-%s.bmp' % ident)
            return {'objects': str(objects), 'picture': str(picture) if picture.exists() else None}
        time.sleep(.025)
    raise Failure('Object capture did not acknowledge within 2s')


def execute(driver, req):
    kind = req['kind']
    seconds = req.get('seconds', .18)
    # Navigation and dialogue are budgets for a bounded loop, not one held
    # input, so they get a longer ceiling. Everything else keeps the old one.
    limit = 300 if kind in ('goto', 'talk') else 30
    if type(seconds) not in (int, float) or not 0 < seconds <= limit:
        raise Failure('Duration must be >0 and <=%d seconds' % limit)
    before = driver.read()
    if before is None:
        raise Failure('No state reading')
    if kind == 'status':
        return before
    if kind == 'capture':
        return capture(driver)
    if kind == 'goto':
        return navigate(driver, req)
    if kind == 'talk':
        return converse(driver, req)
    if kind == 'observe':
        driver.settle(seconds)
    elif kind in ('press', 'hold'):
        button = req.get('button')
        if button not in BUTTONS or button == 'NEUTRAL':
            raise Failure('Unknown button')
        if kind == 'press':
            count = req.get('count', 1)
            if type(count) is not int or not 1 <= count <= 20 or seconds > 1:
                raise Failure('Press requires count 1..20 and duration <=1s')
            seq = before['sequence']
            for _ in range(count):
                if driver.read()['sequence'] != seq:
                    raise Failure('Scene changed; remaining presses cancelled')
                driver.pulse(button, seconds)
                driver.settle(.5)
        else:
            delivered, stopped = drive(driver, seconds, lambda _s: button, before['sequence'])
            if stopped is not None:
                raise Failure('Scene changed; hold cancelled')
            driver.settle(.25)
            if not delivered:
                raise Failure('No input-hook delivery acknowledged during hold')
    elif kind == 'wait':
        path = req.get('field', '')
        if not path.startswith(('manager.', 'players.')) and path != 'sequence':
            raise Failure('Wait field must be sequence, manager.* or players.*')
        expected = req.get('equals')
        if expected is None:
            raise Failure('Wait cannot use an unknown/null value as success')
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            s = driver.read()
            if s['table_ok'] and field(s, path) == expected and s['frame'] > before['frame']:
                break
            time.sleep(.025)
        else:
            raise Failure('Watch deadline: %s never reached %r' % (path, expected))
    else:
        raise Failure('Unknown debugger request')
    after = driver.read()
    return {'before': before, 'after': after, 'delta': delta(before, after)}


def motion_verdict(neutral, active, player, minimum, factor):
    ident = str(player)
    n, a = neutral['delta']['players'][ident], active['delta']['players'][ident]
    readings = [r[k] for r in (neutral, active) for k in ('before', 'after')]
    objects = [field(s, 'players.' + ident + '.object') for s in readings]
    sequences = [s['sequence'] for s in readings]
    valid = (n['comparable'] and a['comparable'] and None not in objects and len(set(objects)) == 1
             and len(set(sequences)) == 1
             and neutral['delta']['frames'] > 0 and active['delta']['frames'] > 0
             and all(field(s, 'manager.fatal') == 0 and
                     not (field(s, 'manager.pause_3c') and field(s, 'manager.pause_40')) for s in readings))
    threshold = max(minimum, n.get('distance', 0) * factor)
    passed = valid and a.get('distance', 0) > threshold
    return {'passed': bool(passed), 'player': player, 'neutral_distance': n.get('distance'),
            'active_distance': a.get('distance'), 'required_distance': threshold,
            'same_object_and_scene': bool(valid),
            'claim': 'Movement smoke check only; not speed accuracy or objective completion.'}


def serve(driver, seconds):
    deadline = time.monotonic() + seconds
    session = driver.out / 'session.json'
    inbox = driver.out / 'request.json'
    handled = None
    atomic_json(session, {'state': 'ready', 'deadline_monotonic': deadline})
    print('DEBUG READY:', driver.out, 'for', seconds, 'seconds', flush=True)
    driver.event('debug_start', deadline=deadline)
    try:
        while time.monotonic() < deadline:
            driver.read()
            if inbox.exists():
                req = json.loads(inbox.read_text())
                ident = req.get('id')
                if ident and ident != handled:
                    handled = ident
                    driver.event('debug_request', request=req)
                    if req.get('kind') == 'stop':
                        atomic_json(driver.out / 'response.json', {'id': ident, 'ok': True, 'result': 'stopped'})
                        break
                    try:
                        reply = {'id': ident, 'ok': True, 'result': execute(driver, req)}
                    except (Failure, ValueError, KeyError, TypeError) as error:
                        driver.command()
                        reply = {'id': ident, 'ok': False, 'error': str(error), 'state': driver.latest}
                    driver.event('debug_response', response=reply)
                    atomic_json(driver.out / 'response.json', reply)
            time.sleep(.025)
    finally:
        driver.command()
        atomic_json(session, {'state': 'closed'})
        driver.event('debug_end')


def request(out, payload):
    # One client owns the request/response slots; the server alone owns input.
    with (out / 'client.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        session = json.loads((out / 'session.json').read_text())
        if session['state'] != 'ready' or time.monotonic() >= session['deadline_monotonic']:
            raise Failure('Debugger session is not live')
        payload['id'] = uuid.uuid4().hex
        atomic_json(out / 'request.json', payload)
        # A navigation may be given minutes; a client that gave up after one
        # would report a timeout while the walk it asked for was still running.
        deadline = time.monotonic() + max(60, (payload.get('seconds') or 0) + 30)
        while time.monotonic() < deadline:
            reply_path = out / 'response.json'
            if reply_path.exists():
                reply = json.loads(reply_path.read_text())
                if reply['id'] == payload['id']:
                    return reply
            if json.loads((out / 'session.json').read_text())['state'] == 'closed':
                raise Failure('Session closed before request completed')
            time.sleep(.05)
        raise Failure('Debugger request timed out')


def object_diff(before_path, after_path):
    a, b = [json.loads(Path(p).read_text()) for p in (before_path, after_path)]
    if 'error' in a or 'error' in b:
        raise Failure('Cannot diff an unreadable registry')
    aa, bb = [{o['id']: o for o in v['objects']} for v in (a, b)]
    changes = []
    for ident in sorted(aa.keys() | bb.keys()):
        old, new = aa.get(ident), bb.get(ident)
        if old is None or new is None:
            changes.append({'id': ident, 'change': 'created' if old is None else 'removed'})
        elif old['address'] != new['address'] or old['vtable'] != new['vtable']:
            changes.append({'id': ident, 'change': 'replaced'})
        else:
            words = [{'offset': '0x%03X' % (i * 4), 'before': '0x%08X' % x, 'after': '0x%08X' % y}
                     for i, (x, y) in enumerate(zip(old['words'], new['words'])) if x != y]
            if words:
                changes.append({'id': ident, 'address': hex(old['address']),
                                'changed_words': len(words), 'words': words})
    return {'before_frame': a['frame'], 'after_frame': b['frame'], 'changes': changes,
            'coverage': {k: [a.get(k), b.get(k)] for k in ('omitted', 'unreadable')},
            'note': 'Unsynchronised observations; field changes are leads, not causal proof.'}


def report(out):
    events_path = out / 'events.jsonl'
    events = [json.loads(line) for line in events_path.read_text().splitlines()] if events_path.exists() else []
    result = json.loads((out / 'result.json').read_text())
    lines = ['# Stage debugger report', '', '**Outcome:** ' + result['outcome'], '',
             result.get('reason', result.get('claim', '')), '', '## Timeline', '',
             '| Seconds | Event | Detail |', '|---:|---|---|']
    start = events[0]['time'] if events else 0
    last_identity = None
    for e in events:
        kind, detail = e['event'], ''
        if kind == 'sample':
            s = e['state']
            identity = (s['sequence'], field(s, 'manager.mission.state'),
                        field(s, 'players.44.state'), field(s, 'players.45.state'))
            if identity == last_identity:
                continue
            last_identity = identity
            detail = 'sequence=%s mission=%s player44=%s player45=%s frame=%s' % (*identity, s['frame'])
        elif kind in ('step_start', 'step_pass'):
            detail = e['name']
        elif kind == 'debug_request':
            detail = json.dumps(e['request'])
        elif kind == 'debug_response':
            reply = e['response']
            detail = 'ok' if reply['ok'] else reply.get('error', 'failed')
        elif kind == 'motion_check':
            detail = json.dumps({k: v for k, v in e.items() if k not in ('time', 'event')})
        elif kind == 'pulse_finished':
            detail = 'hook delivery=' + str(e['hook_delivered'])
        else:
            continue
        lines.append('| %.2f | %s | %s |' % (e['time'] - start, kind, detail.replace('|', '/')))
    state = result.get('state') or {}
    lines += ['', '## Last reading', '', '```json', json.dumps(state, indent=2), '```', '',
              '## Interpretation', '',
              '- A steady mission state may be waiting for an objective; it is not by itself a hang.',
              '- Hook delivery does not prove game input consumption. Compare objective/state changes.',
              '- Player position changes during cutscenes; compare a neutral interval before claiming an input response.',
              '- Full requests, responses and samples remain in events.jsonl. Object snapshots and screenshots are beside this report.']
    if field(state, 'manager.fatal'):
        lines += ['- The observed manager fatal flag is set: inspect the runtime fault log before input debugging.']
    if field(state, 'manager.pause_3c') and field(state, 'manager.pause_40'):
        lines += ['- Both observed pause fields are set: check covered pause before treating immobility as a stall.']
    path = out / 'report.md'
    path.write_text('\n'.join(lines) + '\n')
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('out', type=Path)
    sub = ap.add_subparsers(dest='kind', required=True)
    for name in ('status', 'capture', 'stop', 'report'):
        sub.add_parser(name)
    p = sub.add_parser('diff')
    p.add_argument('before', type=Path)
    p.add_argument('after', type=Path)
    for name in ('press', 'hold'):
        p = sub.add_parser(name)
        p.add_argument('button', choices=BUTTONS)
        p.add_argument('--seconds', type=float, default=.18 if name == 'press' else 1)
        if name == 'press': p.add_argument('--count', type=int, default=1)
    p = sub.add_parser('observe'); p.add_argument('--seconds', type=float, default=2)
    p = sub.add_parser('wait'); p.add_argument('field'); p.add_argument('equals', type=json.loads)
    p.add_argument('--seconds', type=float, default=10)
    p = sub.add_parser('goto')
    p.add_argument('--x', type=float); p.add_argument('--z', type=float)
    p.add_argument('--target-player', type=int, choices=(44, 45))
    p.add_argument('--tolerance', type=float, default=15)
    p.add_argument('--seconds', type=float, default=45)
    p.add_argument('--route', type=Path, help='A route JSON from route.py')
    p = sub.add_parser('talk')
    p.add_argument('field'); p.add_argument('equals', type=json.loads)
    p.add_argument('--count', type=int, default=12)
    p.add_argument('--seconds', type=float, default=60)
    args = ap.parse_args()
    if args.kind == 'report':
        print(report(args.out)); return
    if args.kind == 'diff':
        print(json.dumps(object_diff(args.before, args.after), indent=2)); return
    if getattr(args, 'route', None) is not None:
        args.route = json.loads(args.route.read_text())['route']
    # An absent option is absent, not a null for the server to interpret.
    reply = request(args.out, {k: v for k, v in vars(args).items()
                               if k != 'out' and v is not None})
    print(json.dumps(reply, indent=2))
    if not reply['ok']: raise SystemExit(1)


if __name__ == '__main__':
    try: main()
    except (Failure, OSError, ValueError, KeyError) as e:
        raise SystemExit(str(e))
