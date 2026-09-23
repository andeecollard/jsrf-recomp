"""G32: map the D3D lazy-state flags word (0x19DED8): bits set per function (or/and) and bits tested. Run with /usr/bin/python3; paths are this machine's."""
import json,struct,capstone,collections,re,bisect
from capstone import x86 as X86
D='/Users/andrewcollard/jsrf-build/jsrf-first-fault/disasm/'
data=open(json.load(open(D+'summary.json'))['binary'],'rb').read()
base=struct.unpack_from('<I',data,0x104)[0]; n,sa=struct.unpack_from('<II',data,0x11C)
secs=[struct.unpack_from('<IIIII',data,sa-base+i*56)[1:5] for i in range(n)]
def rd(va,k):
    for v,vs,ra,rs in secs:
        if v<=va<v+rs: return data[ra+va-v:ra+va-v+k]
F=json.load(open(D+'functions.json'))
sym={}
for l in open('/Users/andrewcollard/jsrf-build/jsrf-xbsymbols.txt'):
    m=re.match(r'(\S+) = 0x([0-9a-f]+)',l)
    if m: sym[int(m.group(2),16)]=m.group(1).replace('D3D8__','')
dec={}
for l in open('/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv'):
    p=l.split('\t')
    if len(p)>6 and p[1]=='func': dec[int(p[0],16)]=p[6]
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); md.detail=True
FL=0x19DED8
sets=collections.defaultdict(int); clears=collections.defaultdict(int); tests=collections.defaultdict(int); readers=collections.defaultdict(list)
for f in F:
    s=int(f['start'],16); e=int(f['end'],16); name=sym.get(s) or dec.get(s) or f['name']
    ins=list(md.disasm(rd(s,e-s),s))
    loaded=set()  # regs holding flags value
    for i in ins:
        mem=[o for o in i.operands if o.type==X86.X86_OP_MEM and o.mem.base==0 and o.mem.index==0 and (o.mem.disp&0xffffffff)==FL]
        imm=[o.imm&0xffffffff for o in i.operands if o.type==X86.X86_OP_IMM]
        if mem:
            if i.mnemonic=='or' and imm: sets[(f['section'],name)]|=imm[0]
            elif i.mnemonic=='and' and imm: clears[(f['section'],name)]|=(~imm[0])&0xffffffff
            elif i.mnemonic=='test' and imm: tests[(f['section'],name)]|=imm[0]
            elif i.mnemonic=='mov' and i.operands[0].type==X86.X86_OP_REG:
                loaded.add(i.operands[0].reg); readers[(f['section'],name)].append(hex(i.address))
            continue
        # tests/ands on a register loaded from the flags word
        if loaded and i.mnemonic in('test','and','or') and len(i.operands)==2 and i.operands[0].type==X86.X86_OP_REG and i.operands[0].reg in loaded and imm:
            if i.mnemonic=='test': tests[(f['section'],name)]|=imm[0]
            elif i.mnemonic=='or': sets[(f['section'],name)]|=imm[0]
            else: clears[(f['section'],name)]|=(~imm[0])&0xffffffff
        if i.mnemonic in('call','ret','jmp'): loaded=set()
def bits(m): return ' '.join('%X'%(1<<b) for b in range(32) if m>>b&1)
print('SETS (or):');   [print('  %-6s %-45s %s'%(k[0],k[1][:45],bits(v))) for k,v in sorted(sets.items())]
print('CLEARS (and):');[print('  %-6s %-45s %s'%(k[0],k[1][:45],bits(v))) for k,v in sorted(clears.items())]
print('TESTS:');       [print('  %-6s %-45s %s'%(k[0],k[1][:45],bits(v))) for k,v in sorted(tests.items())]
allset=0
for k,v in sets.items(): allset|=v
alltest=0
for k,v in tests.items():
    if k[0]=='D3D': alltest|=v
print('bits set anywhere:',bits(allset)); print('bits tested in D3D:',bits(alltest)); print('set but never tested in D3D:',bits(allset&~alltest))
