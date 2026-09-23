"""G49: census of JSRF's input boundary (XAPI's XInput/XDevice functions, in
the XPP section). Call sites, callers, pop counts, data references. Writes
experiments/xinput_boundary/entry_points.json.

    /usr/bin/python3 experiments/xinput_boundary/census.py <default.xbe> [disasm dir]
"""
import sys, json, os, bisect, collections, re, capstone
b = open(sys.argv[1], 'rb').read()
dis_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/jsrf-build/jsrf-first-fault/disasm')
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
T = {0x1BCBCC: 'XInitDevices', 0x1BD5FF: 'XGetDevices', 0x1BD621: 'XGetDeviceChanges',
     0x1C3BA1: 'XInputOpen', 0x1C3C16: 'XInputClose', 0x1C3C22: 'XInputGetCapabilities',
     0x1C3E14: 'XInputGetState', 0x1C3E85: 'XInputSetState', 0x1C3EBD: 'XInputPoll',
     0x1BC98C: 'MU_Init'}
h = lambda s: int(s, 16)
F = {h(f['start']): (h(f['end']), f['name'], f['section']) for f in json.load(open(os.path.join(dis_dir, 'functions.json')))}
X = json.load(open(os.path.join(dis_dir, 'xrefs.json')))
st = sorted(F)
def owner(a):
    i = bisect.bisect_right(st, a) - 1
    return (st[i],) + F[st[i]] if i >= 0 and a < F[st[i]][0] else None
sites = collections.defaultdict(list); data = collections.defaultdict(int)
for x in X:
    t = h(x['to'])
    if t in T:
        o = owner(h(x['from']))
        if x['type'] in ('call', 'jump'): sites[t].append((x['from'], o[2] if o else '?', o[3] if o else '?'))
        else: data[t] += 1
SECS = [(0x11000, 0x1000, 0x17BB30), (0x1BC7C0, 0x1AC000, 0x7798)]
def rd(va, n):
    for v0, r0, s in SECS:
        if v0 <= va < v0 + s: return b[r0 + va - v0:r0 + va - v0 + n]
    return b''
out = []
for t, n in sorted(T.items()):
    end = F.get(t, (t + 0x200,))[0]
    rets = sorted({int(i.op_str, 16) if i.op_str else 0 for i in md.disasm(rd(t, end - t), t) if i.mnemonic == 'ret'})
    s = sites[t]
    print('%08X %-22s ret=%-10s sites=%-3d sections=%s callers=%s data_refs=%d'
          % (t, n, rets, len(s), sorted({c[2] for c in s}), sorted({c[1] for c in s})[:5], data[t]))
    out.append({'address': '0x%08X' % t, 'name': n, 'ret_bytes': rets, 'call_sites': len(s),
                'callers': sorted({c[1] for c in s}), 'caller_sections': sorted({c[2] for c in s}), 'data_refs': data[t]})
json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'entry_points.json'), 'w'), indent=1)
