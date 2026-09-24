"""Backward requirement analysis: what must hold for command c of mission mid to run."""
from sim import MISSIONS, ldesc
AUTO_LIS = (0x20, 0x00)
def fl(k, i, v): return "%s%d=%d" % (k, i, v)
def writers(mid, k, i, v):
    return [c for c in MISSIONS[mid]['cmds'] if (k, i, v) in c['wr'] and c['op'] != 0xffffffff]
def rank(c):
    if c['blk'] == 'lis' and c['op'] not in AUTO_LIS: return 2
    return 0 if c['blk'] in ('imm', 'nbl', 'res') else 1
def need(mid, c, depth=0, seen=None):
    """-> dict(flags=set of 'C..=1' etc (non-M), actions=[str], chain=[str], unresolved=[str])"""
    seen = set() if seen is None else seen
    out = dict(flags=set(), actions=[], chain=[], unresolved=[])
    key = (c['blk'], c['i'])
    if key in seen or depth > 25: return out
    seen = seen | {key}
    if c['blk'] == 'lis' and c['op'] not in AUTO_LIS:
        out['actions'].append("%s[%d]: %s" % (c['blk'], c['i'], ldesc(c['op'], c['a'])))
    for k, i, v in c['cond']:
        if k != 'M':
            if v == 1 or k in 'GS': out['flags'].add(fl(k, i, v))
            continue
        if v == 0: continue
        ws = sorted(writers(mid, k, i, 1), key=rank)
        if not ws:
            out['unresolved'].append(fl(k, i, v)); continue
        best = None
        for w in ws:
            r = need(mid, w, depth + 1, seen)
            score = (len(r['actions']), len(r['unresolved']), len(r['flags']))
            if best is None or score < best[0]: best = (score, w, r)
        _, w, r = best
        out['chain'].append("%s <- %s[%d] op=0x%02x" % (fl(k, i, v), w['blk'], w['i'], w['op']))
        out['chain'] += r['chain']; out['flags'] |= r['flags']; out['actions'] += r['actions']; out['unresolved'] += r['unresolved']
    return out
