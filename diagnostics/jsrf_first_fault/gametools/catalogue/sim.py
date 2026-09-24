"""Forward simulation of a JSRF mission's command blocks from a flag state.
Model (inferred from GG-Notebook missions/mission_bin.md + opcodes.md, not from the engine code):
 - mission flags (M) cleared at every mission load; chapter flags (C) survive within a chapter,
   cleared by 0xFF next-chapter exits; G/S persist.
 - imm: run once, in order, at mission start (E3 events play back to back).
 - nbl: each runs once when its conditions hold; 0x57 Wait takes args[0] s; 0xBF transmission ~? (assumed 240 f).
 - lis: complete when their trigger fires; only 0x20 (unconditional) is automatic; 0x00 offscreen timer
   treated as automatic after its min seconds (timer reset by the jump).  Everything else = player action.
 - blk: one at a time, lowest index first; each once. E3 event = its duration; E7 talk (auto-advancing text,
   assumed 300 f); E6 exit -> next mission; E4 only fires on death (never, idle player assumed alive).
"""
import sys, os, glob, json
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../progress'))
import missions as MS
EV = json.load(open(os.path.join(HERE, 'evinfo.json')))
LISTENER = {0x00: "offscreen timer", 0x01: "player passes plane region #{2}", 0x02: "player G-stamina reduced {1}% (tagged by rival)",
    0x03: "combo score >= {1}", 0x04: "player talks to talk-character #{1}", 0x06: "combo count >= {1}",
    0x07: "{1} tags finished", 0x08: "all tags cleared", 0x10: "player stamina (death) listener", 0x15: "onscreen timer ran out",
    0x16: "player answers YES", 0x17: "player answers NO", 0x18: "player triggers op-0x70 object #{1} (challenge/door?)",
    0x1c: "lap count {1}", 0x1d: "soul count {1}", 0x22: "player action type {1} x{2}", 0x23: "cans <= {1}",
    0x29: "race result: position == {1} (arg0=1) (0=win)", 0x0a: "enemy group #{1} all defeated (fight)", 0x0d: "enemy group #{1} all defeated/cleared (fight; alt state check)", 0x0c: "enemies defeated >= {2} (fight)", 0x0f: "an enemy of group #{1} reaches state (chase/fight, arg {2})", 0x1b: "NPC/rival player #{0} returns to neutral state (chase/race over)", 0x10: "player #{0} stamina 0 (death/KO)", 0xffffffff: "no-op (never)"}
def ldesc(op, a):
    s = LISTENER.get(op, "listener op 0x%02x args %s" % (op, list(a[:3])))
    try: return s.format(*a)
    except Exception: return s
def pf(s):  # 'C20=1' -> ('C',20,1)
    k, v = s.split('='); return (k[0], int(k[1:]), int(v))

MISSIONS = {}
for p in sorted(glob.glob(os.path.join(MS.MD, 'mssn*.bin'))):
    mid = int(os.path.basename(p)[4:8])
    st, res, cmds, exits = MS.parse(p)
    MISSIONS[mid] = dict(stage="stg%d%d" % st, res=res, exits=exits,
        cmds=[dict(blk='res', i=i, op=-1, a=[r[0], r[1]], cond=[pf(x) for x in r[2]], wr=[pf(x) for x in r[3]]) for i, r in enumerate(res)] + [dict(blk=b, i=i, op=op, a=list(a), cond=[pf(x) for x in c], wr=[pf(x) for x in w]) for b, i, op, a, c, w in cmds])

class State:
    def __init__(s, C=(), G=(), S=()):
        s.f = {('C', c): 1 for c in C}; s.f.update({('G', g): 1 for g in G}); s.f.update({('S', x): 1 for x in S})
    def ok(s, cond): return all(s.f.get((k, i), 0) == v for k, i, v in cond)
    def w(s, wr):
        for k, i, v in wr: s.f[(k, i)] = v
    def clear(s, k): s.f = {kk: v for kk, v in s.f.items() if kk[0] != k}

def run_mission(mid, st, log, t0=0, maxt=60*60*10):
    """Returns (t, next_mission or None, why_stopped, stalled_listeners)."""
    m = MISSIONS[mid]; st.clear('M'); t = t0
    done = set()
    def ev(c, kind):
        nonlocal t
        e = "e%03d" % c['a'][0]; dur = EV.get(e, {}).get('duration_frames') or 0
        log.append(dict(t=t, mission=mid, kind=kind, event=e, block="%s[%d]" % (c['blk'], c['i']), dur=dur))
        t += dur
    for c in m['cmds']:
        if c['blk'] == 'res' and st.ok(c['cond']): st.w(c['wr'])
    for c in m['cmds']:
        if c['blk'] != 'imm': continue
        if st.ok(c['cond']):
            if c['op'] == 0xE3: ev(c, 'event')
            st.w(c['wr'])
    pending = []   # (t_done, cmd) for waits
    blkbusy = None
    for _ in range(5000):
        prog = False
        for c in m['cmds']:
            key = (c['blk'], c['i'])
            if key in done or c['blk'] not in ('nbl', 'lis'): continue
            if not st.ok(c['cond']): continue
            if c['blk'] == 'lis':
                if c['op'] == 0x20: done.add(key); st.w(c['wr']); prog = True
                elif c['op'] == 0x00:
                    done.add(key); pending.append((t + 60 * c['a'][1], c))
                continue
            done.add(key); prog = True
            if c['op'] == 0x57: pending.append((t + 60 * c['a'][0], c))
            elif c['op'] == 0xBF: pending.append((t + 240, c))
            else: st.w(c['wr'])
        if prog: continue
        # blocking
        for c in m['cmds']:
            key = (c['blk'], c['i'])
            if c['blk'] != 'blk' or key in done or c['op'] in (0xE4, 0xffffffff): continue
            if not st.ok(c['cond']): continue
            done.add(key); prog = True
            if c['op'] == 0xE3: ev(c, 'event')
            elif c['op'] == 0xE7:
                log.append(dict(t=t, mission=mid, kind='talk', event="TE%03d" % c['a'][0], block="blk[%d]" % c['i'], dur=300)); t += 300
            elif c['op'] == 0xE5: t += 300
            st.w(c['wr'])
            if c['op'] == 0xE6:
                typ, tgt, spawn = m['exits'][c['a'][0]]
                chap = mid // 100
                if typ == 0xFF:
                    st.clear('C'); return t, (chap + 1) * 100 + 96, 'exit next chapter', []
                if typ in (0xFB, 0xFC, 0xFD):
                    return t, chap * 100 + tgt % 100, 'exit 0x%x' % typ, []
                return t, None, 'exit type 0x%x (menu/special) -> stops' % typ, []
            break
        if prog: continue
        if pending:
            pending.sort(key=lambda x: x[0]); td, c = pending.pop(0); t = max(t, td); st.w(c['wr']); continue
        break
    stalls = []
    for c in m['cmds']:
        if c['blk'] == 'lis' and (c['blk'], c['i']) not in done and st.ok(c['cond']) and c['op'] not in (0xffffffff, 0x10):
            stalls.append("lis[%d] %s -> %s" % (c['i'], ldesc(c['op'], c['a']), ["%s%d=%d" % w for w in c['wr']]))
    return t, None, 'idle: waits for player', stalls

def run_chain(mid, C=(), G=(), S=(), maxm=6):
    st = State(C, G, S); log = []; t = 0; path = []; stalls = []; why = ''
    seen = set()
    while mid is not None and mid in MISSIONS and len(path) < maxm:
        if (mid, tuple(sorted(st.f.items()))) in seen: break
        seen.add((mid, tuple(sorted(st.f.items()))))
        path.append(mid)
        t, nxt, why, stalls = run_mission(mid, st, log, t)
        mid = nxt
    return dict(path=path, log=log, end=why, stalls=stalls, t=t, final=st)

if __name__ == '__main__':
    import pprint
    r = run_chain(int(sys.argv[1]), C=[int(x) for x in sys.argv[2:]])
    print(r['path'], r['end']); [print(x) for x in r['log']]; [print(' stall', s) for s in r['stalls']]
