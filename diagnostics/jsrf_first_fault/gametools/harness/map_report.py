#!/usr/bin/python3
"""Summarise map_run.sh runs: one JSON row per target and a contact sheet.

usage: map_report.py <runs-dir> <targets.txt> <out.json> [contact.png]

Free play = everything after the LAST '[CHAPTER-JUMP] frame ... -> 0x0F' line. Frame
statistics come from the periodic report blocks ([FRAME-WIN], [FLIP-PACE], [STAGE]) that
start after that line; the first such block straddles the transition and is skipped
when there are at least three. FRAME-WIN includes the RECOMP_FLIP_PACE wait (flips held
to one vblank); 'uncapped mean' subtracts that window's pace wait per flip.
"""
import json, re, statistics, sys
from pathlib import Path

RUNS, TARGETS, OUT = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
SHEET = Path(sys.argv[4]) if len(sys.argv) > 4 else None

WIN = re.compile(r'\[FRAME-WIN\] flips=(\d+) mean=([\d.]+) ms .*?p50=([\d.]+) p90=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+) ms\s+over-33ms=(\d+)')
PACE = re.compile(r'\[FLIP-PACE\] flips held to one vblank period: (\d+), waiting ([\d.]+) ms total')
STAGE = re.compile(r'\[STAGE\] per frame:(.*)\| rest=(-?[\d.]+) ms of ([\d.]+)')
STG_ITEM = re.compile(r'(\w+)=([\d.]+) ms \(([\d.]+) calls\)')
STATE = re.compile(r'\[CHAPTER-JUMP\] frame (\d+): (mssn\d+) state (0x[0-9A-F]+) -> (0x[0-9A-F]+)')


def status(path):
    d = {}
    if path.exists():
        for line in path.read_text().splitlines():
            for kv in line.split():
                if '=' in kv:
                    k, v = kv.split('=', 1)
                    d[k] = v
    return d


def picture_of(capfile):
    try:
        return json.loads(capfile.read_text())['result']['picture']
    except Exception:
        return None


def pic_stats(p):
    from PIL import Image, ImageStat
    im = Image.open(p).convert('RGB')
    g = im.convert('L')
    st = ImageStat.Stat(g)
    hist = g.histogram()
    dark = sum(hist[:12]) / (im.size[0] * im.size[1])
    small = im.resize((80, 60))
    colours = len(set(small.getdata()))
    return {'mean': round(st.mean[0], 1), 'stddev': round(st.stddev[0], 1),
            'dark_frac': round(dark, 3), 'colours80x60': colours}


def analyse(o, st):
    log = o / 'runtime.log'
    row = {'dir': str(o)}
    if not log.exists():
        row['error'] = 'no runtime.log'
        return row
    lines = log.read_text(errors='replace').splitlines()
    fired = next((i for i, l in enumerate(lines) if '[CHAPTER-JUMP] fired' in l), None)
    row['fired'] = fired is not None
    last0f = None
    trail = []
    for i, l in enumerate(lines):
        m = STATE.search(l)
        if m and fired is not None and i > fired:
            trail.append(m.group(4))
            if m.group(4) == '0x0F':
                last0f = i
            row['mission'] = m.group(2)
    row['reached_0F'] = last0f is not None
    row['last_state'] = trail[-1] if trail else None
    row['states'] = len(trail)
    # faults
    ff = [i for i, l in enumerate(lines) if 'FIRST GUEST FAULT' in l]
    row['fault'] = '\n'.join(lines[ff[0]:ff[0] + 8]) if ff else None
    row['fault_line'] = ff[0] + 1 if ff else None
    # stall: the periodic report keeps printing through a hang, with [FRAME] flips frozen
    fr = [(i, int(m.group(1))) for i, l in enumerate(lines) for m in [re.match(r'  \[FRAME\] flips=(\d+)', l)] if m]
    stuck = 0
    while stuck + 1 < len(fr) and fr[-1 - stuck - 1][1] == fr[-1][1]:
        stuck += 1
    row['stalled_reports'] = stuck
    row['hung'] = stuck >= 2
    if row['hung']:
        row['hang_from_line'] = fr[-1 - stuck][0] + 1
        row['hang_at_flips'] = fr[-1][1]
        row['hang_in_freeplay'] = last0f is not None and fr[-1 - stuck][0] > last0f
    # windows
    start = last0f if last0f is not None else (fired or 0)
    wins, pace_prev, cur = [], None, None
    for i, l in enumerate(lines):
        m = WIN.search(l)
        if m:
            cur = {'i': i, 'flips': int(m.group(1)), 'mean': float(m.group(2)), 'p50': float(m.group(3)),
                   'p90': float(m.group(4)), 'p99': float(m.group(6)), 'max': float(m.group(7)),
                   'over33': int(m.group(8))}
            continue
        m = PACE.search(l)
        if m:
            wait = float(m.group(2))
            if cur is not None:
                cur['pace_wait'] = wait - pace_prev if pace_prev is not None else None
            pace_prev = wait
            continue
        m = STAGE.search(l)
        if m and cur is not None:
            cur['stage'] = {k: float(v) for k, v, _ in STG_ITEM.findall(m.group(1))}
            cur['stage']['rest'] = float(m.group(2))
            if i > start:
                wins.append(cur)
            cur = None
    if len(wins) >= 3:
        wins = wins[1:]
    row['windows'] = len(wins)
    if wins:
        row['p50_med'] = statistics.median(w['p50'] for w in wins)
        row['p90_med'] = statistics.median(w['p90'] for w in wins)
        row['p90_max'] = max(w['p90'] for w in wins)
        row['max'] = max(w['max'] for w in wins)
        row['over33'] = sum(w['over33'] for w in wins)
        fl = sum(w['flips'] for w in wins)
        row['mean'] = sum(w['mean'] * w['flips'] for w in wins) / fl
        pw = [w for w in wins if w.get('pace_wait') is not None]
        if pw:
            row['uncapped_mean'] = sum(w['mean'] * w['flips'] - w['pace_wait'] for w in pw) / sum(w['flips'] for w in pw)
            row['uncapped_win'] = sorted(round((w['mean'] * w['flips'] - w['pace_wait']) / w['flips'], 2) for w in pw)
        keys = set().union(*(w.get('stage', {}).keys() for w in wins))
        row['stagecost'] = {k: round(statistics.mean(w.get('stage', {}).get(k, 0.0) for w in wins), 2) for k in sorted(keys)}
    # hardware-texture rebuilds in free play ([METAL] hw textures is cumulative)
    hw = [(int(m.group(1)), float(m.group(2))) for l in lines[start:]
          for m in [re.search(r'\[METAL\] hw textures.*?: (\d+) built.*?([\d.]+) MiB decoded', l)] if m]
    frl = [int(m.group(1)) for l in lines[start:] for m in [re.match(r'  \[FRAME\] flips=(\d+)', l)] if m]
    if len(hw) >= 2 and len(frl) >= 2 and frl[-1] > frl[0]:
        nfl = frl[-1] - frl[0]
        row['tex_built_per_frame'] = round((hw[-1][0] - hw[0][0]) / nfl, 2)
        row['tex_mib_per_frame'] = round((hw[-1][1] - hw[0][1]) / nfl, 2)
    # drops, texfmt, glitches after start
    tail = lines[start:]
    drops = [l for l in tail if l.startswith('[DROP]') and 'nothing dropped' not in l]
    row['drop_lines'] = len(drops)
    row['drop_warn'] = sorted(set(re.sub(r'\d+ of \d+ batches \([\d.]+%\)', 'N', l) for l in drops if 'WARNING' in l))[:6]
    row['drop_states'] = sorted(set(re.sub(r'draw \d+: ', '', l) for l in drops if l.startswith('[DROP]   draw')))[:6]
    row['drop_windows'] = sorted(set(re.sub(r'window: \d+ flips, \d+ batches', 'window', re.sub(r'= \d+ \([^)]*\)', '', l))
                                  for l in drops if 'window:' in l))[:4]
    tex = [l for l in lines if l.startswith('[TEXFMT]') and 'REFUSED=' in l and 'never reaches' in l]
    row['texfmt_refused'] = sorted(set(re.sub(r'seen=\d+  REFUSED=\d+', '', l).strip() for l in tex))
    gl = [l for l in lines[(fired or 0):] if l.startswith('[GLITCH] hit')]
    row['glitch_hits_after_jump'] = len(gl)
    # by guest frame: a hit is in free play only if it starts after the frame of the last
    # transition to 0x0F (hits are logged a few frames late, so the log position is not enough)
    f0f = int(STATE.search(lines[last0f]).group(1)) if last0f is not None else None
    ffire = int(re.search(r'fired at frame (\d+)', lines[fired]).group(1)) if fired is not None else 0
    gf = [(int(m.group(1)), l) for l in gl for m in [re.search(r'guest frame (\d+)\.\.', l)] if m]
    gl = [l for g, l in gf if g >= ffire]
    row['glitch_hits_after_jump'] = len(gl)
    row['glitch_hits_freeplay'] = len([1 for g, l in gf if f0f is not None and g > f0f])
    row['glitch_examples'] = gl[:3]
    # wild pointer / adx guard spin markers
    row['adx_guard_lines'] = sum(1 for l in tail if 'ADX-GUARD' in l and ('spin' in l.lower() or 'steal' in l.lower()))
    # pictures
    pics = []
    for c in ('pre90', 'fp1', 'fp2', 'end'):
        p = picture_of(Path(str(o) + '.cap-' + c))
        if p and Path(p).exists():
            s = pic_stats(p)
            s['cap'] = c
            s['path'] = p
            pics.append(s)
    row['pictures'] = pics
    return row


rows = []
for line in TARGETS.read_text().splitlines():
    if not line.strip() or line.startswith('#'):
        continue
    jump, stage, label = line.split(None, 2)
    c, m = jump.split(':')
    n = 'm%02d%02d' % (int(c), int(m))
    for suffix in ('', '.retry', '.rerun', '.rerun.retry', '.pressA', '.pressA.retry'):
        o = RUNS / (n + suffix)
        st = status(Path(str(o) + '.status'))
        if not st:
            continue
        r = {'target': n, 'jump': jump, 'stage': stage, 'label': label, 'run': n + suffix, 'status': st}
        r.update(analyse(o, st))
        rows.append(r)
OUT.write_text(json.dumps(rows, indent=1))
print('rows', len(rows))

if SHEET:
    from PIL import Image, ImageDraw
    best = {}
    for r in rows:
        if r['run'].endswith('.retry') and r['target'] in best and best[r['target']].get('pictures'):
            continue
        if r.get('pictures') or r['target'] not in best:
            best.setdefault(r['target'], r)
            if r.get('pictures'):
                best[r['target']] = r
    items = list(best.values())
    tw, th, cols = 320, 240, 6
    rws = (len(items) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rws * (th + 18)), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for k, r in enumerate(items):
        x, y = (k % cols) * tw, (k // cols) * (th + 18)
        pics = r.get('pictures') or []
        pick = next((p for p in pics if p['cap'] == 'fp2'), None) or next((p for p in pics if p['cap'] == 'fp1'), None) or (pics[-1] if pics else None)
        if pick:
            sheet.paste(Image.open(pick['path']).convert('RGB').resize((tw, th)), (x, y + 18))
        d.text((x + 4, y + 3), '%s %s %s %s' % (r['jump'], r['stage'], r['label'][:18], r.get('status', {}).get('reason', '?')), fill=(255, 255, 255))
    sheet.save(SHEET)
    print('sheet', SHEET, sheet.size)
