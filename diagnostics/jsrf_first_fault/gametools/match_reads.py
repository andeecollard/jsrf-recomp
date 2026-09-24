import os,sys,re
root=os.path.expanduser('~/Library/Application Support/JSRF/game/Media/Z_ADX')
idx={}
for sub in ('BGM','Env'):
    for fn in os.listdir(os.path.join(root,sub)):
        d=open(os.path.join(root,sub,fn),'rb').read()
        for o in range(0,len(d)-4,2048):
            idx.setdefault(d[o:o+4],[]).append((sub+'/'+fn,o))
win=0
for line in open(sys.argv[1]):
    if 'periodic' in line: win+=1; continue
    m=re.search(r'want=(\d+) got=\d+ st=\S+ ((?:[0-9A-F]{2} ){3}[0-9A-F]{2})',line)
    if not m: continue
    pat=bytes.fromhex(m.group(2).replace(' ',''))
    print(win*10, m.group(1), idx.get(pat,[])[:2])
