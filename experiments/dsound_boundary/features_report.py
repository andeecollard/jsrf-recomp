"""G48 §2c: which DSOUND features the title uses that the lift ignores.

Reads the [DSOUND-CALL] argument log a census run writes
(stage_dsound_census.py, RECOMP_DSOUND_CENSUS=1, with RECOMP_DSOUND_CENSUS_LOG_CAP
high enough to cover the run) and answers, with counts:
  - buffer descriptor flags, and how many carry DSBCAPS_CTRL3D (0x10);
  - distinct values passed to SetEG / SetLFO / SetFilter / SetAllParameters /
    SetI3DL2Source / listener setters;
  - distinct mix-bin assignments;
  - the 3D setters' use, and SetEffectData's.
Also the [DSOUND-CENSUS] totals, which are not capped.

    python3 experiments/dsound_boundary/features_report.py <runtime.log>
"""
import collections, re, sys

log = sys.argv[1]
calls = collections.defaultdict(list)
totals = {}
for line in open(log, errors='replace'):
    m = re.search(r'\[DSOUND-CALL\] t=([\d.]+) (\w+)\(([^)]*)\) = (\w+) from (\w+)(.*)', line)
    if m:
        calls[m.group(2)].append((float(m.group(1)), m.group(3).split(), m.group(4), m.group(5), m.group(6).strip()))
        continue
    m = re.search(r'\[DSOUND-CENSUS\] 0x\w+ (\w+)\s+game=(\d+)', line)
    if m:
        totals[m.group(1)] = int(m.group(2))

def show(title, rows, n=12):
    print('\n' + title)
    for k, c in rows.most_common(n):
        print('  %6d  %s' % (c, k))
    if len(rows) > n:
        print('  ... %d more distinct' % (len(rows) - n))

print('game calls (uncapped census totals), for the entry points the lift ignores or approximates:')
for k in sorted(totals):
    if re.search(r'SetEG|SetLFO|SetFilter|SetMode|SetPosition|SetVelocity|Distance|Cone|Doppler|Rolloff|I3DL2|'
                 r'SetAllParameters|EffectData|SetMixBins|SetHeadroom|SetFrequency|SetVolume', k):
        print('  %8d  %s' % (totals[k], k))

flags = collections.Counter(); fmt = collections.Counter(); ctrl3d = 0
for t, a, r, ra, x in calls.get('IDirectSound_CreateSoundBuffer', []):
    m = re.search(r'flags=(\w+)', x); f = int(m.group(1), 16) if m else 0
    flags['%08X' % f] += 1; ctrl3d += bool(f & 0x10)
    m = re.search(r'wfx\{([^}]*)\}', x)
    if m: fmt[m.group(1)] += 1
print('\nCreateSoundBuffer logged: %d, with DSBCAPS_CTRL3D: %d'
      % (len(calls.get('IDirectSound_CreateSoundBuffer', [])), ctrl3d))
show('descriptor flags', flags)
show('formats', fmt)
for name in ('IDirectSoundBuffer_SetEG', 'IDirectSoundBuffer_SetLFO', 'IDirectSoundBuffer_SetFilter',
             'IDirectSoundBuffer_SetAllParameters', 'IDirectSoundBuffer_SetI3DL2Source',
             'IDirectSound_SetI3DL2Listener', 'IDirectSound_SetAllParameters', 'IDirectSoundBuffer_SetMixBins',
             'IDirectSound_SetEffectData', 'IDirectSoundBuffer_SetMode', 'IDirectSoundBuffer_SetPosition',
             'IDirectSoundBuffer_SetMinDistance', 'IDirectSoundBuffer_SetMaxDistance',
             'IDirectSoundBuffer_SetFrequency', 'IDirectSoundBuffer_SetVolume', 'IDirectSoundBuffer_SetHeadroom'):
    rows = calls.get(name, [])
    if not rows:
        continue
    c = collections.Counter()
    for t, a, r, ra, x in rows:
        key = x if x else ' '.join(a[1:])
        c['%s  (from %s)' % (key, ra)] += 1
    show('%s: %d logged' % (name, len(rows)), c, 8)
