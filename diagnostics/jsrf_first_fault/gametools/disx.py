import sys, xbe, capstone, csv
x = xbe.Xbe()
syms = {}
for row in open('/Users/andrewcollard/jsrf/JSRF-Decompilation/ghidra/symboltable.tsv'):
    f = row.split('\t')
    try: syms[int(f[0],16)] = f[6] if f[1]=='func' else f[3]
    except: pass
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
va = int(sys.argv[1],16); n = int(sys.argv[2],16)
for i in md.disasm(x.read(va, n), va):
    c = ''
    for tok in i.op_str.replace(',',' ').replace('[',' ').replace(']',' ').split():
        try:
            v = int(tok,16)
            if v in syms: c += ' ; ' + syms[v]
        except: pass
    print(f'{i.address:08x}: {i.mnemonic} {i.op_str}{c}')
