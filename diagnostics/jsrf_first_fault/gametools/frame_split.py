"""G76: a harness run's [FRAME-SPLIT] windows in free play, averaged (flip-weighted).

    frame_split.py <run dir> ...      (a stage_harness --out directory; <dir>.retry is taken when present)

Free play starts at the first `[CHAPTER-JUMP] frame ... -> 0x0F` line (map_run.sh's
arms); the first window after it is dropped. "walk+rest" is the pusher's frame less
its idle, [STAGE]'s timed stages, the host tokens and the software-method wait: the
push-buffer walk and method dispatch. "d3d work" is the title's D3D time less the
fence wait. Needs RECOMP_FRAME_SPLIT=1 in the run."""
import re, sys, os
num = r'(-?[\d.]+)'
P1 = re.compile(r'\[FRAME-SPLIT\] per frame over (\d+) flips \(([\d.]+) ms\): title: game ' + num + r' ms, d3d ' + num +
                r' ms \(' + num + r' calls; of it waiting on the fence ' + num + r' \(' + num + r'\), ring space ' + num +
                r' \(' + num + r'\), state flush ' + num + r' \(' + num + r'\), kickoff ' + num + r' \(' + num +
                r'\), mirror hooks ' + num + r', kernel inside ' + num + r'\), kernel outside d3d ' + num)
P2 = re.compile(r"other threads' d3d " + num + r' ms')
P3 = re.compile(r'pusher: host tokens check ' + num + r' ms \(' + num + r'\), replace ' + num + r' ms \(' + num +
                r': FF registers ' + num + r', build ' + num + r' \(' + num + r'; vertex fetch ' + num + r'\), host encode ' +
                num + r' \(' + num + r'\)\), pre ' + num + r' ms \(' + num + r'\); waiting for a software method.s acknowledgement ' +
                num + r' ms \(' + num + r'\) \| gpu: command buffers covered ' + num + r' ms \(' + num)
PS = re.compile(r'\[STAGE\] per frame:(.*)\| rest=' + num + r' ms of ' + num)
for name in sys.argv[1:]:
    d = name.rstrip('/')
    name = os.path.basename(d)
    if os.path.exists(d + '.retry/runtime.log'): d += '.retry'
    L = open(d + '/runtime.log', errors='replace').read().splitlines()
    i0 = next((i for i, l in enumerate(L) if 'CHAPTER-JUMP] frame' in l and '-> 0x0F' in l), None)
    if i0 is None: print(name, 'never 0x0F'); continue
    wins = []; cur = None
    for l in L[i0:]:
        m = PS.search(l)
        if m:
            st = dict((k, float(v)) for k, v in re.findall(r'(\w+)=(-?[\d.]+) ms', m.group(1)))
            cur = {'stage': sum(st.values()) - st.get('idle', 0.0), 'idle': st.get('idle', 0.0)}
            continue
        m = P1.search(l)
        if m and cur is not None:
            g = [float(x) for x in m.groups()]
            cur.update(flips=g[0], frame=g[1], game=g[2], d3d=g[3], wait=g[5], space=g[7], state=g[9], kick=g[11],
                       hooks=g[13], kin=g[14], kout=g[15]); continue
        m = P2.search(l)
        if m and cur is not None: cur['other'] = float(m.group(1)); continue
        m = P3.search(l)
        if m and cur is not None and 'flips' in cur:
            g = [float(x) for x in m.groups()]
            cur.update(check=g[0], replace=g[2], regs=g[4], build=g[5], fetch=g[7], encode=g[8], swm=g[12], gpu=g[14])
            wins.append(cur); cur = None
    wins = wins[1:]
    if not wins: print(name, 'no windows'); continue
    n = sum(w['flips'] for w in wins)
    a = lambda k: sum(w.get(k, 0.0) * w['flips'] for w in wins) / n
    # other threads' d3d went into `d3d` in the first binary (no 'other' key): take BlockUntilVerticalBlank out there
    title_d3d = a('d3d')
    walk = a('frame') - a('idle') - a('stage') - a('check') - a('replace') - a('swm')
    print(f"{name:7s} flips={int(n):5d} frame={a('frame'):5.2f} | title: game={a('game'):5.2f} d3d={title_d3d:5.2f}"
          f" (fence wait={a('wait'):5.2f} ring={a('space'):5.2f} state={a('state'):4.2f} hooks={a('hooks'):4.2f}"
          f" d3d work={title_d3d - a('wait'):5.2f}) kernel out={a('kout'):4.2f} other-thread d3d={a('other'):5.2f}"
          f" | pusher: check={a('check'):4.2f} replace={a('replace'):5.2f} (regs={a('regs'):4.2f} build={a('build'):4.2f}"
          f" fetch={a('fetch'):4.2f} encode={a('encode'):4.2f}) swm={a('swm'):4.2f} stages={a('stage'):4.2f}"
          f" idle={a('idle'):4.2f} walk+rest={walk:5.2f} | gpu={a('gpu'):4.2f}")
