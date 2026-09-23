"""G32: describe every D3D entry point JSRF's game code calls, from its code.

For each call target in the D3D section reached from game code:
  - ret_bytes: stack bytes popped by every reachable `ret N` (must agree)
  - reg_args:  of eax/ecx/edx, those read on some path before being written
  - site_pushes: argument pushes counted back from each call site (cross-check)
Tail jumps into another function are followed. Writes entry_points.json.

    /usr/bin/python3 experiments/d3d8_boundary/entry_points.py \
        --disasm ~/jsrf-build/jsrf-first-fault/disasm \
        --symbols ~/jsrf-build/jsrf-xbsymbols.txt -o experiments/d3d8_boundary/entry_points.json
"""
import argparse, bisect, collections, json, os, re, struct
import capstone
from capstone import x86 as X86

ap = argparse.ArgumentParser()
ap.add_argument('--disasm', required=True)
ap.add_argument('--symbols', required=True)
ap.add_argument('--names', help='JSON {hexaddr: name} for entries the symbol dump lacks')
ap.add_argument('-o', '--output', required=True)
a = ap.parse_args()

h = lambda s: int(s, 16)
summ = json.load(open(os.path.join(a.disasm, 'summary.json')))
data = open(summ['binary'], 'rb').read()
base = struct.unpack_from('<I', data, 0x104)[0]
nsec, sa = struct.unpack_from('<II', data, 0x11C)
secs = [struct.unpack_from('<IIIII', data, sa - base + i * 56)[1:5] for i in range(nsec)]
def rd(va, n):
    for v, vs, ra, rs in secs:
        if v <= va < v + rs:
            return data[ra + va - v: ra + va - v + n]
    return b''

F = json.load(open(os.path.join(a.disasm, 'functions.json')))
X = json.load(open(os.path.join(a.disasm, 'xrefs.json')))
fs = sorted((h(f['start']), h(f['end']), f['section'], f['name']) for f in F)
st = [f[0] for f in fs]
fstart = {f[0]: f for f in fs}
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
if a.names:
    sym.update({int(k, 16): v for k, v in json.load(open(a.names)).items()})

d3d = [f for f in fs if f[2] == 'D3D']
lo, hi = min(f[0] for f in d3d), max(f[1] for f in d3d)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
# Register parts: a read is live only if that part was not written first on the
# path. e = full 32-bit, x = low 16, l / h = low / high byte.
PART = {}
for full, x, l, hh in (('eax', 'AX', 'AL', 'AH'), ('ecx', 'CX', 'CL', 'CH'), ('edx', 'DX', 'DL', 'DH')):
    PART[getattr(X86, 'X86_REG_' + full.upper())] = (full, 'e')
    PART[getattr(X86, 'X86_REG_' + x)] = (full, 'x')
    PART[getattr(X86, 'X86_REG_' + l)] = (full, 'l')
    PART[getattr(X86, 'X86_REG_' + hh)] = (full, 'h')
KILLS = {'e': {'e', 'x', 'l', 'h'}, 'x': {'x', 'l', 'h'}, 'l': {'l'}, 'h': {'h'}}
NEEDS = {'e': {'e'}, 'x': {'e', 'x'}, 'l': {'e', 'x', 'l'}, 'h': {'e', 'x', 'h'}}

def insn_at(va):
    code = rd(va, 16)
    for i in md.disasm(code, va):
        return i
    return None

def rw(ins):
    """(reads, writes) as sets of (reg, part). A read is satisfied by a prior
    write of any part in NEEDS[part]; pushes are slot reserves or saves, not
    argument reads, and calls clobber everything caller-saved."""
    r, w = set(), set()
    ops = ins.operands
    if ins.mnemonic in ('xor', 'sub') and len(ops) == 2 and all(o.type == X86.X86_OP_REG for o in ops) \
       and ops[0].reg == ops[1].reg and ops[0].reg in PART:
        g, pt = PART[ops[0].reg]
        return r, {(g, k) for k in KILLS[pt]}
    if ins.mnemonic == 'push' and ops and ops[0].type == X86.X86_OP_REG:
        return r, w
    if ins.mnemonic == 'call':
        return r, {(g, k) for g in ('eax', 'ecx', 'edx') for k in 'exlh'}
    try:
        regs_read, regs_write = ins.regs_access()
    except capstone.CsError:
        regs_read, regs_write = [], []
    for g in regs_read:
        if g in PART: r.add(PART[g])
    for g in regs_write:
        if g in PART:
            reg, pt = PART[g]
            w |= {(reg, k) for k in KILLS[pt]}
    return r, w

def live_reads(r, written):
    return {g for g, pt in r if not any((g, k) in written for k in NEEDS[pt])}

def analyse(entry, depth=0):
    """Walk the CFG from entry. Returns (ret_bytes set, live-in regs, notes)."""
    seen, rets, live, notes = set(), set(), set(), []
    work = [(entry, frozenset())]            # (pc, regs written on this path)
    states = {}
    steps = 0
    while work and steps < 20000:
        pc, written = work.pop(); steps += 1
        key = pc
        prev = states.get(key)
        if prev is not None and prev <= written:
            continue                         # already visited with fewer kills
        states[key] = written if prev is None else (prev & written)
        written = states[key]
        while True:
            ins = insn_at(pc)
            if ins is None:
                notes.append('undecodable at %08X' % pc); break
            r, w = rw(ins)
            live |= live_reads(r, written)
            written = written | w
            m = ins.mnemonic
            if m == 'ret':
                rets.add(ins.operands[0].imm if ins.operands else 0); break
            if m == 'jmp':
                op = ins.operands[0]
                if op.type == X86.X86_OP_IMM:
                    t = op.imm
                    if t in fstart and t != entry and not (entry <= t < fstart.get(entry, (0, 0))[1]):
                        # tail jump into another function
                        if depth < 3:
                            r2, l2, n2 = analyse(t, depth + 1)
                            rets |= r2; live |= {g for g in l2 if not any((g, k) in written for k in NEEDS['e'])}; notes += n2
                            notes.append('tail-jumps to %08X' % t)
                        break
                    work.append((t, written)); break
                notes.append('indirect jmp at %08X' % pc)
                if op.type == X86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                    notes.append('import thunk [%08X]' % (op.mem.disp & 0xffffffff))
                    rets.add('import')
                break
            if m.startswith('j') or m in ('loop', 'loope', 'loopne', 'jecxz'):
                op = ins.operands[0]
                if op.type == X86.X86_OP_IMM:
                    work.append((op.imm, written))
            if m in ('int3', 'hlt', 'ud2'):
                break
            pc += ins.size
    return rets, live, notes

# call sites from game code
calls = collections.defaultdict(list)
for x in X:
    if x['type'] not in ('call', 'jump'):
        continue
    t, fa = h(x['to']), h(x['from'])
    o = owner(fa)
    if o is None or o[2] == 'D3D' or not (lo <= t < hi):
        continue
    calls[t].append((fa, o[3], x['type']))

def pushes_before(site, fn_start):
    """Count pushes immediately preceding the call, walking back linearly."""
    code = rd(fn_start, site - fn_start + 8)
    ins = list(md.disasm(code, fn_start))
    idx = next((i for i, k in enumerate(ins) if k.address == site), None)
    if idx is None:
        return None
    n = 0
    for k in reversed(ins[:idx]):
        if k.mnemonic == 'push':
            n += 1
        elif k.mnemonic in ('call', 'ret', 'jmp') or k.mnemonic.startswith('j'):
            break
        elif k.mnemonic in ('add', 'sub') and k.op_str.startswith('esp'):
            return None                     # stack adjusted by hand: unknown
    return n

out = []
for t in sorted(calls):
    rets, live, notes = analyse(t)
    sites = calls[t]
    fn_of = {s: owner(s) for s, _, _ in sites}
    push_counts = collections.Counter()
    for s, _, typ in sites:
        if typ == 'call':
            push_counts[pushes_before(s, fn_of[s][0])] += 1
    numeric = sorted(r for r in rets if r != 'import')
    out.append({
        'address': '0x%08X' % t,
        'name': sym.get(t, None),
        'ret_bytes': numeric,
        'ret_consistent': len(numeric) <= 1,
        'reg_args': sorted(live & {'ecx', 'edx', 'eax'}),
        'site_pushes': {str(k): v for k, v in sorted(push_counts.items(), key=lambda kv: str(kv[0]))},
        'call_sites': len(sites),
        'callers': sorted({n for _, n, _ in sites}),
        'notes': sorted(set(notes)),
    })

json.dump(out, open(a.output, 'w'), indent=1)
bad = [e for e in out if not e['ret_consistent'] or not e['ret_bytes'] and 'import' not in str(e['notes'])]
mism = []
for e in out:
    rb = e['ret_bytes'][0] if e['ret_bytes'] else None
    for k, v in e['site_pushes'].items():
        if k != 'None' and rb is not None and int(k) * 4 < rb:
            mism.append((e['address'], e['name'], rb, k, v))
print('entries %d, unnamed %d, register-arg entries %d, ret-inconsistent/unknown %d, push<ret mismatches %d'
      % (len(out), sum(1 for e in out if not e['name']),
         sum(1 for e in out if e['reg_args']), len(bad), len(mism)))
for e in bad: print('  RET?', e['address'], e['name'], e['ret_bytes'], e['notes'][:3])
for m in mism: print('  PUSH<RET', m)
