"""Census of the boundary between JSRF's game code and its statically linked DSOUND.

The DSOUND counterpart of experiments/d3d8_boundary/census.py. Answers, from the
recompiler's disassembly: which DSOUND functions code outside the DSOUND section
calls, how often and from where; which reach it only through data (vtables,
callbacks); and which DSOUND globals are touched from outside. Used by
docs/jsrf/plans/JSRF_PLAN_2026-09-23_LIFT_AT_THE_DSOUND_BOUNDARY.md.

    /usr/bin/python3 experiments/dsound_boundary/census.py \
        --disasm ~/jsrf-build/jsrf-first-fault/disasm \
        --symbols experiments/dsound_boundary/xbsymbol_dsound_4134.tsv \
        [--decomp-symbols ../JSRF-Decompilation/ghidra/symboltable.tsv] [--json out.json]
"""
import argparse, bisect, collections, json, os

ap = argparse.ArgumentParser()
ap.add_argument('--disasm', required=True)
ap.add_argument('--symbols', required=True, help='address<TAB>name, XbSymbolDatabase')
ap.add_argument('--decomp-symbols')
ap.add_argument('--json')
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
    p = line.rstrip('\n').split('\t')
    if len(p) == 2:
        sym[int(p[0], 16)] = p[1].replace('DSOUND__', '')
dec = {}
if a.decomp_symbols and os.path.exists(a.decomp_symbols):
    for line in open(a.decomp_symbols):
        p = line.split('\t')
        if len(p) > 6 and p[1] == 'func':
            dec[int(p[0], 16)] = p[6]

ds = [f for f in fs if f[2] == 'DSOUND']
lo, hi = min(f[0] for f in ds), max(f[1] for f in ds)
starts = {f[0] for f in ds}
print('DSOUND code %08X-%08X, %d functions, %d named by XbSymbolDatabase'
      % (lo, hi, len(ds), sum(1 for f in ds if f[0] in sym)))

calls = collections.Counter(); callers = collections.defaultdict(set)
by_section = collections.Counter(); data_refs = collections.Counter()
data_from = collections.defaultdict(set); interior = collections.Counter()
for x in X:
    fa, t = h(x['from']), h(x['to'])
    if not (lo <= t < hi):
        continue
    o = owner(fa)
    if o is not None and o[2] == 'DSOUND':
        continue
    sec = o[2] if o else 'data?'
    name = o[3] if o else '%08X' % fa
    if x['type'] in ('call', 'jump'):
        calls[t] += 1; callers[t].add(name); by_section[sec] += 1
        if t not in starts: interior[t] += 1
    elif x['type'] in ('data_imm', 'data_read'):
        data_refs[t] += 1; data_from[t].add(name)

nm = lambda t: sym.get(t, 'sub_%08X' % t)
print('call sites into DSOUND: %d, distinct targets %d, caller functions %d'
      % (sum(calls.values()), len(calls), len(set().union(*callers.values()))))
print('  by caller section: %s' % dict(by_section))
if interior:
    print('  calls landing inside a function, not at a start: %d' % sum(interior.values()))
print('\n%-8s %5s %4s  %s' % ('target', 'sites', 'fns', 'name'))
for t, c in sorted(calls.items(), key=lambda kv: (-kv[1], kv[0])):
    cs = sorted(callers[t])
    print('%08X %5d %4d  %-50s %s' % (t, c, len(cs), nm(t),
          ' '.join(dec.get(h(n[4:]), n) if n.startswith('sub_') else n for n in cs[:4])
          + (' ...' if len(cs) > 4 else '')))
print('\nreached from outside only through data (vtables, callbacks): %d targets'
      % len(set(data_refs) - set(calls)))
for t in sorted(set(data_refs) - set(calls)):
    print('%08X %3d  %-50s from %s' % (t, data_refs[t], nm(t), ' '.join(sorted(data_from[t])[:4])))
if a.json:
    json.dump([{'address': '0x%08X' % t, 'name': nm(t), 'call_sites': calls[t],
                'data_refs': data_refs[t], 'callers': sorted(callers[t] | data_from[t])}
               for t in sorted(set(calls) | set(data_refs))], open(a.json, 'w'), indent=1)
