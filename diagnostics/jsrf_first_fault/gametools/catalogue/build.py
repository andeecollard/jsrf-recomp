import json, os, sys
from sim import MISSIONS, EV, run_chain, ldesc
from req import need, fl
HERE = os.path.dirname(os.path.abspath(__file__))
PLAYER_G = [1, 10]
PLAYER_S = [384,385,386,387,388,389,390,391,450,451,452,453,455,456,458,460,465,470,499,501]
def wsf(f):   # 'C20=1' -> WriteStateFlag dword
    k, v = f.split('='); arr = "MCGS".index(k[0]); idx = int(k[1:])
    return "0x%08X" % (arr | (idx << 3) | (int(v) << 19))
chap_missions = [m for m in MISSIONS if 100 <= m < 1000]
# 1. forward sweeps from every chapter-jump target
sweeps = {}
for m in chap_missions:
    C = [2] if m == 100 else []   # the jump writes C2=1 (tutorial finished) only for 1:0
    r = run_chain(m, C=C, G=PLAYER_G, S=PLAYER_S)
    r0 = run_chain(m, C=C)        # fresh G/S, to detect save dependence
    sweeps[m] = dict(jump="%d:%d" % (m // 100, m % 100), path=r['path'], end=r['end'], stalls=r['stalls'],
                     events=[dict(e=x['event'], mission="mssn%04d" % x['mission'], block=x['block'], t_frame=x['t'], dur=x['dur'], kind=x['kind']) for x in r['log']],
                     events_fresh_save=[x['event'] for x in r0['log'] if x['kind'] == 'event'])
# 2. per-command analysis
plays = []   # every E3 command
for mid, m in sorted(MISSIONS.items()):
    for c in m['cmds']:
        if c['op'] != 0xE3: continue
        e = "e%03d" % c['a'][0]
        rq = need(mid, c)
        cond = [fl(*x) for x in c['cond']]; wr = [fl(*x) for x in c['wr']]
        # A: any sweep playing this command
        hits = [(s['jump'], k) for s in sweeps.values() for k, x in enumerate(s['events'])
                if x['e'] == e and x['mission'] == "mssn%04d" % mid and x['block'] == "%s[%d]" % (c['blk'], c['i'])]
        own = [h for h in hits if h[0] == "%d:%d" % (mid // 100, mid % 100)]
        cls = None; recipe = None; verified = False
        Cneed = sorted({f for f in rq['flags'] if f[0] == 'C'}, key=lambda f: int(f[1:].split('=')[0]))
        GSneed = sorted(f for f in rq['flags'] if f[0] in 'GS')
        if hits:
            cls = 'A'; verified = True
            j = own[0] if own else sorted(hits, key=lambda h: h[1])[0]
            recipe = "RECOMP_CHAPTER_JUMP=%s" % j[0]
        else:
            Cset = [int(f[1:].split('=')[0]) for f in Cneed if f.endswith('=1')]
            Gs = [int(f[1:].split('=')[0]) for f in GSneed if f[0] == 'G' and f.endswith('=1')]
            Ss = [int(f[1:].split('=')[0]) for f in GSneed if f[0] == 'S' and f.endswith('=1')]
            r = run_chain(mid, C=Cset + ([2] if mid == 100 else []), G=set(PLAYER_G) | set(Gs), S=set(PLAYER_S) | set(Ss), maxm=1)
            played = any(x['event'] == e and x['block'] == "%s[%d]" % (c['blk'], c['i']) for x in r['log'])
            if played:
                cls = 'B'; verified = True
                recipe = "jump %d:%d with chapter flags %s%s" % (mid // 100, mid % 100, Cneed, (" and save flags %s" % GSneed) if GSneed else "")
            elif rq['actions']:
                cls = 'C'
                recipe = "load mssn%04d (jump %d:%d%s), then player: %s" % (mid, mid // 100, mid % 100,
                          (" + chapter flags %s" % Cneed) if Cneed else "", "; ".join(dict.fromkeys(rq['actions'])))
            else:
                cls = 'C?'
                recipe = "not reached by forward sim with flags %s; unresolved %s" % (Cneed + GSneed, rq['unresolved'])
        plays.append(dict(event=e, mission="mssn%04d" % mid, stage=m['stage'], jump="%d:%d" % (mid // 100, mid % 100),
                          block="%s[%d]" % (c['blk'], c['i']), cond=cond, writes=wr, cls=cls, recipe=recipe,
                          sim_verified=verified, chapter_flags_needed=Cneed,
                          chapter_flags_writestateflag=[wsf(f) for f in Cneed],
                          save_flags_needed=GSneed, player_actions=list(dict.fromkeys(rq['actions'])),
                          chain=rq['chain'], unresolved=rq['unresolved'],
                          a_jumps=sorted({h[0] for h in hits})))
json.dump(dict(sweeps={k: v for k, v in sweeps.items()}, plays=plays), open(os.path.join(HERE, 'analysis.json'), 'w'), indent=1)
from collections import Counter
print(Counter(p['cls'] for p in plays))
for p in plays:
    print(p['event'], p['mission'], p['block'], p['cls'], p['recipe'][:150])
