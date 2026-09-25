import sys,re,os,glob
for run in sys.argv[1:]:
    log=open(run+'/runtime.log','rb').read().decode('latin1').split('\n')
    fired=None; ev=[]; cur=None; hits=[]; faults=[]; missions=set(); after=False; laststate=None; lastframe=None
    for l in log:
        if 'fired: mission' in l: fired=int(l.split()[2]); after=True; continue
        m=re.match(r'\[CHAPTER-JUMP\] frame (\d+) mission (\d+) state (0x[0-9A-F]+) \(globals: chapter (\d+) mission (\d+)',l)
        if m and after:
            f,st=int(m[1]),m[3]; laststate=st; lastframe=f
            if st=='0x13' and (cur is None): cur=[f,None]
            elif st!='0x13' and cur is not None: cur[1]=f; ev.append(cur); cur=None
        m=re.match(r'\[GLITCH\] hit (\d+): (\d+) frames?, seq \S+ guest frame (\d+)',l)
        if m: hits.append((int(m[3]),int(m[2])))
        if 'FIRST GUEST FAULT' in l or 'Segmentation' in l or 'HANG' in l.upper() and 'hang' in l.lower() and 'watchdog' in l.lower(): faults.append(l.strip()[:120])
    if cur: ev.append([cur[0],None])
    def where(g):
        for i,(a,b) in enumerate(ev):
            if g>=a and (b is None or g<b): return 'e%d'%(i+1)
        return 'play' if fired and g>fired else 'pre'
    per={}
    for g,L in hits: per.setdefault(where(g),[]).append(g)
    st=open(run+'.status').read().strip().replace('\n',' | ') if os.path.exists(run+'.status') else ''
    print(f'{os.path.basename(run)}: fired@{fired} events={len(ev)} {[(a,b) for a,b in ev]} last={laststate}@{lastframe}')
    print(f'   hits={len(hits)} by event: { {k:len(v) for k,v in per.items()} } faults={faults[:3]}  [{st}]')
