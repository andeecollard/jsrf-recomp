"""Census of the boundary between JSRF's game code and its statically linked D3D8.

Answers, from the recompiler's own disassembly: which D3D functions game code
calls, from where, and which D3D globals it touches directly. Used by
docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_D3D8_BOUNDARY.md.

    /usr/bin/python3 experiments/d3d8_boundary/census.py \
        --disasm ~/jsrf-build/jsrf-first-fault/disasm \
        --symbols ~/jsrf-build/jsrf-xbsymbols.txt \
        [--decomp-symbols ../JSRF-Decompilation/ghidra/symboltable.tsv]

--symbols is an XbSymbolDatabase CLI dump for the same XBE.
"""
import argparse, bisect, collections, json, os, re

ap = argparse.ArgumentParser()
ap.add_argument('--disasm', required=True)
ap.add_argument('--symbols', required=True)
ap.add_argument('--decomp-symbols')
ap.add_argument('--icall-sites', help='tools/recomp/output/icall_sites.json (per-site feedback DB)')
ap.add_argument('--gl-lo', type=lambda s: int(s, 16), default=0x14C000,
                help='start of the engine graphics layer (CMGameGL)')
ap.add_argument('--gl-hi', type=lambda s: int(s, 16), default=0x156200)
a = ap.parse_args()

h = lambda s: int(s, 16)
F = json.load(open(os.path.join(a.disasm, 'functions.json')))
X = json.load(open(os.path.join(a.disasm, 'xrefs.json')))
fs = sorted((h(f['start']), h(f['end']), f['section'], f['name']) for f in F)
st = [f[0] for f in fs]

def owner(addr):
    i = bisect.bisect_right(st, addr) - 1
    while i >= 0:
        if fs[i][0] <= addr < fs[i][1]:
            return fs[i]
        if addr - fs[i][0] > 0x20000:
            return None
        i -= 1

sym = {}
for line in open(a.symbols):
    m = re.match(r'(\S+) = 0x([0-9a-f]+)', line)
    if m:
        sym[int(m.group(2), 16)] = m.group(1).replace('D3D8__', '')
dec = {}
if a.decomp_symbols:
    for line in open(a.decomp_symbols):
        p = line.split('\t')
        if len(p) > 6 and p[1] == 'func':
            dec[int(p[0], 16)] = p[6]

d3d = [f for f in fs if f[2] == 'D3D']
lo, hi = min(f[0] for f in d3d), max(f[1] for f in d3d)
data_lo = min((k for k, v in sym.items() if v.startswith('D3D_g_')), default=hi)
data_hi = data_lo + 0x3000
print('D3D code %08X-%08X, %d functions; D3D globals from %08X' % (lo, hi, len(d3d), data_lo))

calls = collections.Counter(); callers = collections.defaultdict(set)
outside = collections.Counter(); data_out = collections.Counter()
data_refs = 0; data_fns = set(); in_gl = 0
for x in X:
    fa, t = h(x['from']), h(x['to'])
    o = owner(fa)
    if o is None or o[2] == 'D3D':
        continue
    gl = a.gl_lo <= o[0] < a.gl_hi
    if x['type'] in ('call', 'jump') and lo <= t < hi:
        calls[t] += 1; callers[t].add(o[3])
        if gl: in_gl += 1
        else: outside[(o[3], dec.get(o[0], ''), sym.get(t, '%08X' % t))] += 1
    elif x['type'] in ('data_read', 'data_imm') and data_lo <= t < data_hi:
        data_refs += 1; data_fns.add(o[3])
        if not gl: data_out[o[3]] += 1

all_callers = set().union(*callers.values()) if callers else set()
print('call sites into D3D: %d, distinct targets %d, caller functions %d'
      % (sum(calls.values()), len(calls), len(all_callers)))
print('  from the engine graphics layer: %d; elsewhere: %d' % (in_gl, sum(outside.values())))
for t, c in calls.most_common():
    print('  %4d sites %3d fns  %08X %s' % (c, len(callers[t]), t, sym.get(t, '(unnamed)')))
print('outside the graphics layer:')
for k, v in sorted(outside.items()):
    print('  %3d %s' % (v, k))
print('direct references to D3D globals: %d from %d functions; outside the layer: %s'
      % (data_refs, len(data_fns), dict(data_out)))
for name, addr in (('g_pDevice', 'D3D_g_pDevice'), ('MakeSpace', 'D3DDevice_MakeSpace'),
                   ('MakeRequestedSpace', 'D3D_MakeRequestedSpace_8')):
    tgt = next((k for k, v in sym.items() if v == addr), None)
    n = sum(1 for x in X if tgt is not None and h(x['to']) == tgt
            and (owner(h(x['from'])) or ('', '', 'D3D'))[2] != 'D3D')
    print('game-code references to %s: %d' % (name, n))

# Raw listing search: literal mentions in game code of the device object and
# pushbuffer pointers, whatever the addressing form. Needs <disasm>/asm/text.asm.
asm = os.path.join(a.disasm, 'asm', 'text.asm')
if os.path.exists(asm):
    dev_lo, dev_hi = 0x19A000, data_lo + 0x1F4   # up to, not including, the flags word
    pat = re.compile(r'0x([0-9a-f]{5,8})\b', re.I)
    lit = collections.Counter(); hdr = collections.Counter()
    for line in open(asm, errors='replace'):
        for m in pat.finditer(line.split(';')[0]):
            v = int(m.group(1), 16)
            if dev_lo <= v < dev_hi:
                lit[v] += 1
        if re.search(r',\s*0x417fc\b', line, re.I):
            hdr['SET_BEGIN_END header 0x000417FC'] += 1
    print('raw .text mentions of 0x%06X-0x%06X (device object, ring pointers): %d %s'
          % (dev_lo, dev_hi, sum(lit.values()), dict(lit.most_common(5))))
    print('raw .text begin/end command headers: %d' % hdr['SET_BEGIN_END header 0x000417FC'])
else:
    print('raw listing search skipped: %s not found' % asm)

if a.icall_sites:
    sites = json.load(open(a.icall_sites))
    n = 0
    for site, v in sites.items():
        tl = v.get('targets', []) if isinstance(v, dict) else v
        s_ = int(site, 16)
        if any(lo <= int(t, 16) < hi for t in tl) and not (lo <= s_ < hi):
            n += 1
    print('game-side indirect call sites landing in D3D: %d of %d recorded sites' % (n, len(sites)))
