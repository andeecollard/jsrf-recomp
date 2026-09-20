#!/usr/bin/env python3
"""Run a batch of JSRF audio/frame experiments back to back, unattended.

One game at a time, each arm: boot to the Garage, drive at a set intensity,
capture the mixed waveform and the voice tracer, then measure. Appends one row
per arm to summary.tsv as it goes, so the table is readable while it runs.

No sound is ever played: every arm sets SDL_AUDIODRIVER=no_such_driver and the
arm is discarded if the log does not confirm backend=none.
"""
import itertools
import json
import math
import re
import statistics
import struct
import subprocess
import sys
import time
import wave
from pathlib import Path

ROOT = Path('/Users/andrewcollard/jsrf/xboxrecomp_upstream_integration')
HARNESS = ROOT / 'diagnostics/jsrf_first_fault/stage_harness'
BINARY = Path('/tmp/jsrf-stage-harness-build/jsrf_first_fault')
GAME = Path.home() / 'Library/Application Support/JSRF/game'
HDD = Path.home() / 'jsrf-build/emulated-hdd-warm'
import os
OUT = Path(os.environ.get('SWEEP_OUT', '/tmp/jsrf-fast'))
sys.path.insert(0, str(HARNESS))

# 200 s, not 330: across 14 runs the separation is absolute -- clean arms score
# exactly 0 dropouts, failing arms 69 to 470, and the latest onset ever observed
# is 158.7 s. A longer hold buys margin this outcome does not need.
HOLD = 200
BUTTONS = ('LUP', 'LLEFT', 'LUP', 'A', 'LRIGHT', 'LUP', 'RT', 'LDOWN')


def log(message):
    print('[%s] %s' % (time.strftime('%H:%M:%S'), message), flush=True)


def wait_for_exit():
    while subprocess.run(['pgrep', '-x', 'jsrf_first_fault'],
                         capture_output=True).returncode == 0:
        time.sleep(3)


def launch(run_dir, wav, extra, bare):
    cmd = ['python3', str(HARNESS / 'run.py'),
           '--scenario', str(HARNESS / 'garage.json'),
           '--binary', str(BINARY), '--game', str(GAME), '--hdd', str(HDD),
           '--out', str(run_dir),
           '--env', 'SDL_AUDIODRIVER=no_such_driver',
           '--env', 'RECOMP_APU_WAV=%s' % wav,
           '--env', 'RECOMP_VOICE_LIFECYCLE=1',
           '--env', 'RECOMP_REPORT_MS=10000',
           '--debug-seconds', str(HOLD)]
    for pair in extra:
        cmd += ['--env', pair]
    if bare:
        cmd.append('--bare')
    stdout = open(str(run_dir) + '.log', 'w')
    return subprocess.Popen(cmd, stdout=stdout, stderr=subprocess.STDOUT)


def await_ready(run_dir, deadline=180):
    path = Path(str(run_dir) + '.log')
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        if path.exists():
            text = path.read_text(errors='replace')
            if 'DEBUG READY' in text:
                return True
            if '"outcome"' in text or 'ERROR' in text:
                return False
        time.sleep(3)
    return False


def drive(run_dir, seconds, hold, rest):
    """Drive for `seconds`. hold=0 means stand still, which is a real arm."""
    from debug import request
    start = time.monotonic()
    if hold <= 0:
        while time.monotonic() - start < seconds:
            time.sleep(2)
        return
    for button in itertools.cycle(BUTTONS):
        if time.monotonic() - start > seconds:
            return
        try:
            request(run_dir, {'kind': 'hold', 'button': button, 'seconds': hold})
            if rest:
                time.sleep(rest)
        except Exception as error:
            log('  driver stopped: %s' % error)
            return


def dropouts(wav_path):
    with wave.open(str(wav_path), 'rb') as w:
        rate, step = w.getframerate(), w.getframerate() // 20
        peaks = []
        while True:
            raw = w.readframes(step)
            if not raw:
                break
            n = len(raw) // 2
            v = struct.unpack('<%dh' % n, raw[:n * 2])
            peaks.append(max(abs(x) for x in v) if v else 0)
    runs, start = [], None
    for i, p in enumerate(peaks):
        if p < 40 and start is None:
            start = i
        elif p >= 40 and start is not None:
            runs.append((start * .05, (i - start) * .05))
            start = None
    runs = [r for r in runs if r[0] > 20]       # past the pre-music lead-in
    return runs, len(peaks) * .05


LIFE = re.compile(r'audio_frames=(\d+) event=(\w+) voice=(\d+)')
TRAPS = re.compile(r'\[APU-IDLE\] (\d+) idle-voice traps')
FRAME = re.compile(r'flips=(\d+) mean=([\d.]+) ms \(([\d.]+) fps\) p50=([\d.]+)')


def measure(run_dir, wav_path):
    # run.py now serves the debug window even after a FAILED step, which is
    # right for debugging and wrong for a sweep: waiting for DEBUG READY no
    # longer proves the scenario passed. noOTHER-b reached the debug window
    # having never left the title, and scored 0 dropouts off an empty WAV.
    verdict = json.loads((run_dir / 'result.json').read_text())
    if verdict['outcome'] == 'failed':
        return {'error': 'scenario failed: ' + verdict.get('reason', '')[:70]}
    text = (run_dir / 'runtime.log').read_text(errors='replace')
    if 'backend=none' not in text:
        return {'error': 'a backend was opened; arm discarded'}
    # A run that is not presenting is not a comparison either. Disabling
    # NV2A_PMC_UPMIRROR gave a black screen with flips free-running at ~600 fps
    # on 20 Sep 2026; it scored as an ordinary arm until it was seen on screen.
    # Anything above 100 fps here means the flip path is not pacing on vblank.
    probe = [float(c) for _a, _b, c, _d in FRAME.findall(text)]
    if probe and probe[-1] > 100:
        return {'error': 'presentation broken: %.0f fps, not pacing on vblank' % probe[-1]}
    spans, live = 0, {}
    for m in LIFE.finditer(text):
        kind, voice = m.group(2), int(m.group(3))
        if kind == 'on':
            live[voice] = 1
        elif kind == 'retire' and live.pop(voice, None):
            spans += 1
    traps = [int(m.group(1)) for m in TRAPS.finditer(text)]
    frames = [(int(a), float(b), float(c), float(d)) for a, b, c, d in FRAME.findall(text)]
    gaps, length = dropouts(wav_path)
    minutes = max(length - 20, 1) / 60.0
    return {'seconds': round(length, 1), 'dropouts': len(gaps),
            'dropouts_per_min': round(len(gaps) / minutes, 2),
            'silent_s': round(sum(g[1] for g in gaps), 1),
            'first_dropout': round(gaps[0][0], 1) if gaps else None,
            'traps': traps[-1] if traps else 0, 'spans': spans,
            'traps_per_span': round(traps[-1] / spans, 2) if traps and spans else None,
            'p50_ms': frames[-1][3] if frames else None,
            'fps': frames[-1][2] if frames else None}


# One arm per switch named on the command line: each forces that ONE switch to
# 0 and keeps the player's other sixteen. n=1 during the search, because the
# outcome is binary with no overlap; the named answer gets replicated after.
# 'control' is an arm that forces nothing: the player's 17 switches exactly as
# they are. It has to lead a round run on a NEW BINARY, because every earlier
# arm was scored on 92ad42df and a bisect whose baseline was never re-checked
# can eliminate a switch that is no longer the one dying.
ARMS = [(a, 2.0, 0.0, [], False) if a == 'control' else
        ('no-' + a.replace('RECOMP_', '').lower(), 2.0, 0.0, ['%s=0' % a], False)
        for a in sys.argv[1:]]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    table = OUT / 'summary.tsv'
    if not table.exists():
        table.write_text('arm\tdropouts\tper_min\tfirst\tsilent_s\ttraps\tspans\t'
                         'traps_per_span\tp50_ms\tfps\tnote\n')
    for name, hold, rest, extra, bare in ARMS:
        run_dir = OUT / name
        wav = OUT / ('%s.wav' % name)
        if (run_dir / 'runtime.log').exists():
            log('%s already done, skipping' % name)
            continue
        log('=== arm %s (hold=%.1f rest=%.1f bare=%s %s)' % (name, hold, rest, bare, extra))
        wait_for_exit()
        process = launch(run_dir, wav, extra, bare)
        if not await_ready(run_dir):
            log('  %s never reached the Garage; recorded as a failure' % name)
            with table.open('a') as f:
                f.write('%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\tdid not reach the Garage\n' % name)
            wait_for_exit()
            continue
        log('  driving %d s' % (HOLD + 10))
        drive(run_dir, HOLD + 10, hold, rest)
        process.wait()
        wait_for_exit()
        try:
            row = measure(run_dir, wav)
        except Exception as error:
            row = {'error': str(error)}
        log('  %s' % json.dumps(row))
        with table.open('a') as f:
            if 'error' in row:
                f.write('%s\t-\t-\t-\t-\t-\t-\t-\t-\t-\t%s\n' % (name, row['error']))
            else:
                f.write('%s\t%d\t%.2f\t%s\t%.1f\t%d\t%d\t%s\t%s\t%s\t\n' % (
                    name, row['dropouts'], row['dropouts_per_min'], row['first_dropout'],
                    row['silent_s'], row['traps'], row['spans'], row['traps_per_span'],
                    row['p50_ms'], row['fps']))
    log('sweep finished; table at %s' % table)


if __name__ == '__main__':
    main()
