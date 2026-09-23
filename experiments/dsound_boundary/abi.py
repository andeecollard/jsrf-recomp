"""Add each DSOUND entry point's calling convention to entry_points.json.

For every target the census found: the `ret N` byte counts in its body (a
stdcall/thiscall callee pops N bytes of arguments), whether it reads ecx
before writing it (thiscall `this`), and the pushes at each game call site.
A replacement body must honour exactly this; the runtime census then checks it
on every call, as G34 did for D3D.

    /usr/bin/python3 experiments/dsound_boundary/abi.py --xbe <default.xbe> \
        --disasm ~/jsrf-build/jsrf-first-fault/disasm
"""
import argparse, collections, json, os, capstone
from capstone import x86

ap = argparse.ArgumentParser()
ap.add_argument('--xbe', required=True)
ap.add_argument('--disasm', required=True)
ap.add_argument('--entries', default=os.path.join(os.path.dirname(__file__), 'entry_points.json'))
a = ap.parse_args()
b = open(a.xbe, 'rb').read()
SECS = [(0x11000, 0x1000, 0x17BB30), (0x19E340, 0x18C000, 0x1C32C)]
def rd(va, n):
    for v, r, s in SECS:
        if v <= va < v + s:
            return b[r + va - v:r + va - v + n]
    raise ValueError(hex(va))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
h = lambda s: int(s, 16)
F = {h(f['start']): h(f['end']) for f in json.load(open(os.path.join(a.disasm, 'functions.json')))}
X = json.load(open(os.path.join(a.disasm, 'xrefs.json')))
E = json.load(open(a.entries))
sites = collections.defaultdict(list)
for x in X:
    if x['type'] == 'call':
        sites[h(x['to'])].append(h(x['from']))

def body(start):
    end = F.get(start, start + 0x400)
    return list(md.disasm(rd(start, end - start), start))

def pushes_before(site):
    """Pushes in the straight line before a call, back to the previous call/jump/label-ish break."""
    ins = list(md.disasm(rd(site - 0x40, 0x45), site - 0x40))
    ins = [i for i in ins if i.address <= site]
    if not ins or ins[-1].address != site:
        return None
    n = 0
    for i in reversed(ins[:-1]):
        if i.mnemonic == 'push': n += 1
        elif i.mnemonic in ('call', 'ret', 'jmp') or i.mnemonic.startswith('j'): break
    return n

for e in E:
    t = h(e['address'])
    ins = body(t)
    rets = sorted({(i.operands[0].imm if i.operands else 0) for i in ins if i.mnemonic == 'ret'})
    ecx_in = False
    for i in ins:
        r, w = i.regs_access()
        if x86.X86_REG_ECX in r or x86.X86_REG_CX in r or x86.X86_REG_CL in r:
            ecx_in = True; break
        if x86.X86_REG_ECX in w: break
        if i.mnemonic in ('call', 'ret', 'jmp'): break
    tail = None
    if len(ins) <= 3 and ins and ins[-1].mnemonic == 'jmp':
        tail = ins[-1].op_str
    e['ret_bytes'] = rets
    e['reads_ecx'] = ecx_in
    e['tail_jump'] = tail
    e['site_pushes'] = dict(collections.Counter(str(pushes_before(s)) for s in sites[t]))
json.dump(E, open(a.entries, 'w'), indent=1)
for e in E:
    print('%s %-42s ret=%-8s ecx=%d pushes=%s%s' % (e['address'], e['name'], e['ret_bytes'], e['reads_ecx'],
          e['site_pushes'], '  tail->' + e['tail_jump'] if e['tail_jump'] else ''))
