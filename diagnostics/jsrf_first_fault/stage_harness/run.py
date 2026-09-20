#!/usr/bin/env python3
"""State-driven Mac scenarios. Only the supplied disposable HDD is written.

No replay, game-memory patch, savestate, or claim of deterministic simulation.
See README.md for the protocol and the limits of the initial scenario.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

# Keep one Failure/Driver module identity when debug.py imports this CLI.
if __name__ == "__main__":
    sys.modules["run"] = sys.modules[__name__]


class Failure(RuntimeError):
    pass


def atomic_json(path, value):
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(value, indent=2) + '\n')
    temp.replace(path)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def tree_manifest(root):
    result = {}
    for p in sorted(root.rglob('*')):
        if p.is_symlink():
            raise Failure('HDD fixtures must not contain symlinks: ' + str(p))
        if p.is_file():
            result[str(p.relative_to(root))] = {'bytes': p.stat().st_size, 'sha256': digest(p)}
        elif p.is_dir():
            result[str(p.relative_to(root)) + '/'] = {'directory': True}
    return result


def matches(s, condition):
    if not s.get('table_ok') or s.get('sequence') not in condition['sequence']:
        return False
    for player in condition.get('players', []):
        p = s.get('players', {}).get(str(player))
        if not p or len(p.get('position', [])) != 3 or not all(math.isfinite(v) for v in p['position']):
            return False
    for path, expected in condition.get('fields', {}).items():
        value = s
        for part in path.split('.'):
            value = value.get(part) if isinstance(value, dict) else None
        if value is None or expected is None or value != expected:
            return False
    if 'text_contains' in condition:
        if s.get('sample', 0) - s.get('text_sample', -100) > 10:
            return False
        if not any(condition['text_contains'] in n['text'] for n in s.get('text_nodes', [])):
            return False
    return True


BUTTONS = {'NEUTRAL': (0, 0, 0, 0, 0, 0), 'START': (16, 0, 0, 0, 0, 0),
           'A': (0, 255, 0, 0, 0, 0), 'B': (0, 0, 255, 0, 0, 0),
           'UP': (1, 0, 0, 0, 0, 0), 'DOWN': (2, 0, 0, 0, 0, 0),
           'LDOWN': (0, 0, 0, 0, 0, -24000), 'LLEFT': (0, 0, 0, 0, -24000, 0),
           'LRIGHT': (0, 0, 0, 0, 24000, 0), 'LUP': (0, 0, 0, 0, 0, 24000), 'RT': (0, 0, 0, 255, 0, 0)}


# The harness exists to repeat and debug what the player sees, so it has to
# render on the player's path. play_scripted.sh adopted paths.conf on 19 Sep
# 2026 for exactly this reason; this file did not, so every picture it has ever
# produced -- including the ones shown to the player -- was taken on the CPU
# fixed-function path while the player's bundle runs RECOMP_METAL_FF=1 on the
# GPU. The two have disagreed on real geometry before.
PLAYER_CONF = Path.home() / 'Library/Application Support/JSRF/paths.conf'
# An instrument's argument is not behaviour, and an output path would let a
# harness run overwrite the player's own pictures and recordings.
NOT_BEHAVIOUR = ('FB_', 'DUMP', 'WATCH', 'TRACE', 'RECORD', 'LIFECYCLE',
                 'RATES', 'FRESH', 'RING', 'PATH', 'NAMES')


def player_switches(reserved):
    """The player's boolean RECOMP_ switches, by play_scripted.sh's own rule."""
    adopted = {}
    if not PLAYER_CONF.is_file():
        return adopted
    for line in PLAYER_CONF.read_text().splitlines():
        line = line.strip()
        if not line.startswith('export RECOMP_'):
            continue
        key, _, value = line[len('export '):].partition('=')
        key, value = key.strip(), value.split('#')[0].strip().strip('"')
        if key in reserved or value not in ('0', '1'):
            continue
        if any(mark in key for mark in NOT_BEHAVIOUR):
            continue
        adopted[key] = value
    return adopted


class Driver:
    def __init__(self, out, process):
        self.out, self.process = out, process
        self.command_id = 0
        self.last_sample = None
        self.last_fresh = time.monotonic()
        self.latest = None
        self.events = (out / 'events.jsonl').open('w', buffering=1)
        self.last_sequence = None

    def event(self, kind, **fields):
        self.events.write(json.dumps({'time': time.monotonic(), 'event': kind, **fields}) + '\n')

    def read(self):
        if self.process.poll() is not None:
            raise Failure('Guest process exited: ' + str(self.process.returncode))
        path = self.out / 'status.json'
        try:
            s = json.loads(path.read_text())
        except FileNotFoundError:
            s = None
        if s:
            if s.get('protocol') != 1:
                raise Failure('Unsupported bridge protocol')
            if s['sample'] != self.last_sample:
                self.last_sample = s['sample']
                self.last_fresh = time.monotonic()
                self.latest = s
                self.event('sample', state=s)
                if self.last_sequence != s['sequence']:
                    self.last_sequence = s['sequence']
                    print('  sequence:', s['sequence'], flush=True)
        if time.monotonic() - self.last_fresh > 5:
            raise Failure('Observer heartbeat missing for 5s; no usable state reading')
        return s

    def command(self, button='NEUTRAL', ttl=0, snapshot=False, stick=None):
        self.command_id += 1
        seq = (self.latest or {}).get('sequence', 255)
        # Neutral/snapshot commands still need a valid protocol index.
        seq = seq if seq < 64 else 0
        # The named presets are four points on an axis the protocol has always
        # carried in full: the bridge validates left_x/left_y over the whole
        # signed 16-bit range. Steering needs the angles between them.
        pad = BUTTONS[button] if stick is None else tuple(stick)
        # Mirrors the bridge's own bounds: it drops a command that fails them,
        # and a silently dropped command reads exactly like a dead lease.
        if len(pad) != 6 or not all(0 <= int(v) <= 255 for v in pad[:4]) \
                or not all(-32768 <= int(v) <= 32767 for v in pad[4:]):
            raise Failure('Malformed pad vector: ' + repr(pad))
        values = (self.command_id, ttl, seq, *pad, int(snapshot))
        path = self.out / 'command.txt'
        temp = self.out / 'command.tmp'
        temp.write_text(' '.join(map(str, values)) + '\n')
        temp.replace(path)
        self.event('command', id=self.command_id, button='STICK' if stick else button,
                   ttl_ms=ttl, expected_sequence=seq, snapshot=snapshot,
                   **({'stick': list(pad[4:])} if stick else {}))
        return self.command_id

    def settle(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.read()
            time.sleep(.025)

    def pulse(self, button, duration=.18):
        ident = self.command(button, round(duration * 1000))
        deadline = time.monotonic() + 2
        seen = False
        while time.monotonic() < deadline:
            s = self.read()
            if s and s['command'] == ident:
                seen |= s['deliveries'] > 0
                if not s['lease_active']:
                    break
            time.sleep(.025)
        else:
            raise Failure('Command acknowledgement/lease expiry missing')
        self.command()
        self.event('pulse_finished', id=ident, hook_delivered=seen)
        if not seen:
            raise Failure('Input was not delivered before its lease expired or scene changed')
        self.settle(.25)  # a real release edge, never hold START across stages

    def wait_for(self, condition, timeout, stable=.5):
        deadline, since, first_frame = time.monotonic() + timeout, None, None
        while time.monotonic() < deadline:
            s = self.read()
            if s and matches(s, condition):
                if since is None:
                    since, first_frame = time.monotonic(), s['frame']
                if time.monotonic() - since >= stable and s['frame'] > first_frame:
                    return s
            else:
                since = None
            time.sleep(.025)
        raise Failure('Timed out waiting for ' + json.dumps(condition))

    def navigate(self, step):
        deadline = time.monotonic() + step['timeout']
        attempts = 0
        ready_since = None
        while time.monotonic() < deadline:
            s = self.read()
            if s and matches(s, step['expect']):
                return self.wait_for(step['expect'], max(1, deadline - time.monotonic()))
            if s and s['table_ok'] and s['sequence'] in step['act_in'] and matches(s, step.get('act_when', {'sequence': step['act_in']})) and attempts < step['attempts']:
                if ready_since is None:
                    ready_since = time.monotonic()
                if time.monotonic() - ready_since < step.get('settle', 1):
                    time.sleep(.025)
                    continue
                # These are bounded navigation attempts, not proof of menu readiness.
                for action in step['actions']:
                    s = self.read()
                    if matches(s, step['expect']) or s['sequence'] not in step['act_in'] or not matches(s, step.get('act_when', {'sequence': step['act_in']})):
                        break
                    self.pulse(action['button'], action.get('seconds', .18))
                    self.settle(action.get('after', .5))
                attempts += 1
                ready_since = None
                self.event('navigation_attempt', step=step['name'], attempt=attempts)
            else:
                ready_since = None
                time.sleep(.05)
        raise Failure('Navigation deadline at ' + step['name'])

    def run(self, scenario):
        for step in scenario['steps']:
            print('STEP:', step['name'], flush=True)
            self.event('step_start', name=step['name'])
            if step['kind'] == 'wait':
                self.wait_for(step['expect'], step['timeout'], step.get('stable', .5))
            elif step['kind'] == 'navigate':
                self.navigate(step)
            elif step['kind'] in ('talk', 'goto'):
                from debug import execute
                self.wait_for(step['expect'], step['timeout'])
                request = {k: v for k, v in step.items()
                           if k not in ('kind', 'name', 'expect', 'timeout', 'accept')}
                request['kind'] = step['kind']
                result = execute(self, request)
                self.event(step['kind'] + '_result',
                           **{k: v for k, v in result.items() if k != 'state'})
                # 'accept' is the scenario's own list of outcomes that count as
                # progress. A stall reports where it stopped; it does not pass.
                if result['outcome'] not in step.get('accept', ['arrived', 'reached']):
                    raise Failure('%s ended %s: %s' % (step['name'], result['outcome'],
                                  json.dumps({k: v for k, v in result.items()
                                              if k not in ('state', 'track', 'lines')})))
            elif step['kind'] == 'motion':
                from debug import execute, motion_verdict
                self.wait_for(step['expect'], step['timeout'])
                neutral = execute(self, {'kind': 'observe', 'seconds': step['seconds']})
                active = execute(self, {'kind': 'hold', 'button': step['button'], 'seconds': step['seconds']})
                evidence = motion_verdict(neutral, active, step['player'], step['minimum_distance'], step['baseline_factor'])
                self.event('motion_check', **evidence)
                if not evidence['passed']:
                    raise Failure('Controlled movement check failed: ' + json.dumps(evidence))
            else:
                raise Failure('Unknown step kind: ' + step['kind'])
            self.command(snapshot=True)
            self.settle(.15)
            self.event('step_pass', name=step['name'], state=self.latest)
        return {'outcome': scenario['outcome'], 'claim': scenario['claim'], 'state': self.latest}


def validate(scenario):
    if scenario.get('schema') != 1 or not scenario.get('steps'):
        raise Failure('Scenario requires schema=1 and nonempty steps')
    if scenario.get('outcome') not in ('passed', 'review_required'):
        raise Failure('Scenario outcome must be explicit')
    for step in scenario['steps']:
        if step['kind'] not in ('wait', 'navigate', 'motion', 'talk', 'goto') \
                or not 0 < step['timeout'] <= 600:
            raise Failure('Invalid scenario step')
        if step['kind'] in ('talk', 'goto'):
            if not 0 < step.get('seconds', 0) <= 300:
                raise Failure('%s needs a 0..300s budget' % step['kind'])
            if any(o not in ('arrived', 'reached', 'scene_changed', 'stalled', 'deadline',
                             'exhausted', 'unreadable', 'no_input', 'wedged')
                   for o in step.get('accept', [])):
                raise Failure('Unknown accepted outcome in ' + step['name'])
        if step['kind'] == 'goto':
            if 'target_player' not in step and not all(k in step for k in ('x', 'z')):
                raise Failure('goto needs x and z, or target_player')
            if 'target_player' in step and not 0 <= step['target_player'] < 7668:
                raise Failure('goto target_player must be a registry id')
            if 'route' in step and (type(step['route']) is not list or len(step['route']) > 200):
                raise Failure('goto route must be a list of at most 200 waypoints')
        seq = step['expect']['sequence']
        if not seq or any(type(n) is not int or n < 0 or n >= 64 for n in seq):
            raise Failure('Invalid expected sequence')
        if step['kind'] == 'motion':
            if step['button'] not in BUTTONS or not 0 < step['seconds'] <= 10 or step['player'] not in (44, 45):
                raise Failure('Invalid motion probe')
            if not 0 < step['minimum_distance'] < 10000 or not 1 <= step['baseline_factor'] <= 100:
                raise Failure('Invalid movement thresholds')
        if step['kind'] == 'navigate':
            if not 1 <= step['attempts'] <= 10 or not step['act_in']:
                raise Failure('Navigation must have bounded attempts and allowed states')
            for action in step['actions']:
                if action['button'] not in BUTTONS:
                    raise Failure('Unknown input button')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--scenario', type=Path, default=Path(__file__).with_name('startup.json'))
    ap.add_argument('--binary', type=Path)
    ap.add_argument('--game', type=Path)
    ap.add_argument('--hdd', type=Path, help='Source HDD, copied and never modified')
    ap.add_argument('--out', type=Path, help='New output directory; existing paths refused')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VALUE')
    ap.add_argument('--validate', action='store_true')
    ap.add_argument('--bare', action='store_true',
                    help="Do not adopt the player's paths.conf switches")
    ap.add_argument('--observe', action='store_true',
                    help='Watch only: leave port 0 to a real controller or a replay')
    ap.add_argument('--debug-seconds', type=int, default=0, help='Serve bounded debugger requests after scenario (0..1800)')
    args = ap.parse_args()
    if not 0 <= args.debug_seconds <= 1800:
        ap.error('--debug-seconds must be 0..1800')
    scenario = json.loads(args.scenario.read_text())
    validate(scenario)
    # In observe mode the harness is not holding the pad, so a step that presses
    # a button would wait forever on a delivery that cannot happen. Only steps
    # that watch are allowed, and the refusal says so rather than hanging.
    if args.observe and any(step['kind'] != 'wait' for step in scenario['steps']):
        raise Failure('An observed run can only contain wait steps: ' + scenario['name'])
    if args.validate:
        print('Scenario valid:', scenario['name'])
        return 0
    if not all((args.binary, args.game, args.hdd, args.out)):
        ap.error('--binary, --game, --hdd and --out are required')
    args.binary, args.game, args.hdd, args.out = [p.resolve() for p in (args.binary, args.game, args.hdd, args.out)]
    if not args.binary.is_file() or not (args.game / 'default.xbe').is_file() or not args.hdd.is_dir():
        raise Failure('Missing binary, default.xbe or HDD tree')
    if args.hdd == args.out or args.hdd in args.out.parents:
        raise Failure('Output must not be inside the source HDD')
    check = subprocess.run(['pgrep', '-x', 'jsrf_first_fault'], capture_output=True, text=True)
    if check.returncode != 1:
        raise Failure('Another game is running, or process inventory is unavailable: ' + check.stdout + check.stderr)
    args.out.mkdir(parents=True, exist_ok=False)
    result = {'outcome': 'failed', 'claim': 'Run did not complete'}
    process, driver = None, None
    begin = time.monotonic()
    try:
        print('Staging and verifying a private HDD copy...', flush=True)
        before = tree_manifest(args.hdd)
        staged = args.out / 'hdd'
        # APFS clones are independent copies and avoid physically copying 5 GB.
        copy = subprocess.run(['cp', '-cR', str(args.hdd), str(staged)], capture_output=True)
        if copy.returncode:
            if staged.exists():
                shutil.rmtree(staged)
            shutil.copytree(args.hdd, staged)
        after = tree_manifest(staged)
        if before != after:
            raise Failure('HDD copy differs from source manifest')
        atomic_json(args.out / 'hdd-manifest.json', before)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('RECOMP_', 'JSRF_', 'SDL_'))}
        env.update({'RECOMP_PB_EXEC': '1', 'RECOMP_METAL': '1', 'RECOMP_OHCI_ATTACH': '1',
                    'RECOMP_REPORT_MS': '5000', 'RECOMP_ICALL_FEEDBACK_PATH': str(args.out / 'icall.dump')})
        explicit = {}
        for pair in args.env:
            k, v = pair.split('=', 1)
            if not k.startswith(('RECOMP_', 'SDL_')):
                raise Failure('Only RECOMP_/SDL_ runtime overrides are allowed')
            explicit[k] = v
        # The four above are what makes the harness a harness; an A/B arm set on
        # the command line wins over the player's config. Everything else the
        # player runs with is adopted, unless --bare says otherwise.
        adopted = {} if args.bare else player_switches(set(env) | set(explicit))
        env.update(adopted)
        env.update(explicit)
        # No inherited recording scripts or user's save/output paths.
        forbidden = ['RECOMP_FAKE_PAD', 'RECOMP_PAD_SENTINEL']
        if not args.observe:
            # Driving and being driven are the conflict. Watching is not.
            forbidden += ['RECOMP_PAD_SCRIPT', 'RECOMP_PAD_RECORD']
        if any(k in env for k in forbidden):
            raise Failure('Stage driver cannot be combined with ' + ', '.join(forbidden))
        if args.observe:
            env['JSRF_STAGE_OBSERVE'] = '1'
        env.update({'JSRF_STAGE_DIR': str(args.out), 'RECOMP_HDD_ROOT': str(staged),
                    'RECOMP_XBE_PATH': str(args.game / 'default.xbe'), 'RECOMP_GAME_DIR': str(args.game)})
        binary_hash = digest(args.binary)
        atomic_json(args.out / 'manifest.json', {
            'binary': str(args.binary), 'binary_sha256': binary_hash,
            'xbe_sha256': digest(args.game / 'default.xbe'), 'hdd_source': str(args.hdd),
            'scenario': scenario, 'player_switches_adopted': adopted, 'runtime': {k: v for k, v in env.items() if k.startswith(('RECOMP_', 'JSRF_', 'SDL_'))},
            'setup_seconds': time.monotonic() - begin})
        print('Adopted from the player\'s paths.conf:',
              ' '.join('%s=%s' % kv for kv in sorted(adopted.items())) or '(none)', flush=True)
        with (args.out / 'runtime.log').open('w') as log:
            process = subprocess.Popen([str(args.binary)], cwd=args.out, env=env, stdout=log, stderr=subprocess.STDOUT)
            driver = Driver(args.out, process)
            try:
                result = driver.run(scenario)
            except (Failure, OSError, ValueError) as step_error:
                result = {'outcome': 'failed', 'reason': str(step_error),
                          'state': driver.latest}
            # The debugger window matters MOST after a failed step: that is when
            # the game is standing at the boundary worth inspecting, and finding
            # out why costs a whole boot otherwise. The verdict is unchanged by
            # it -- a failed run that was then inspected is still a failed run.
            if args.debug_seconds and process.poll() is None:
                from debug import serve
                serve(driver, args.debug_seconds)
                result['debug_session'] = True
                if result['outcome'] != 'failed':
                    result['state'] = driver.latest
        if digest(args.binary) != binary_hash:
            raise Failure('Binary changed during run; result invalid')
    except (Failure, OSError, ValueError, KeyboardInterrupt) as e:
        result = {'outcome': 'failed', 'reason': str(e) or 'Interrupted', 'state': driver.latest if driver else None}
    finally:
        if driver:
            try:
                driver.command(snapshot=2)
                if process.poll() is None:
                    driver.settle(.2)
            except (Failure, OSError):
                pass
            driver.events.close()
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        result['elapsed_seconds'] = time.monotonic() - begin
        result['process_exit'] = process.returncode if process else None
        atomic_json(args.out / 'result.json', result)
        from debug import report
        report(args.out)
    print(json.dumps(result, indent=2))
    print('Evidence:', args.out)
    return {'passed': 0, 'review_required': 3, 'failed': 1}[result['outcome']]


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (Failure, OSError, ValueError, KeyError) as error:
        print('ERROR:', error, file=sys.stderr)
        sys.exit(2)
