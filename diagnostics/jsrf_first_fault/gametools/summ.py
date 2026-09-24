import ast,sys
cur={};segs=[]
for l in open(sys.argv[1]):
    t,w,rest=l.split(' ',2); h=ast.literal_eval(rest.strip()); t=int(t)
    if not h: continue
    f,o=h[0]
    if not f.startswith('BGM/'): continue
    c=cur.get(f)
    if c and 0<o-c[3]<=210000: c[2]=t; c[3]=o; c[4]+=1
    else:
        if c: segs.append(c)
        cur[f]=[f,t,t,o,1,o]
segs+=cur.values(); segs.sort(key=lambda s:s[1])
for f,t0,t1,o,n,o0 in segs:
    if n<3: continue
    rate=(o-o0)/(t1-t0) if t1>t0 else 0
    print(f"{f:20s} t={t0}-{t1}s reads={n} off {o0}->{o} {rate/1000:.1f} KB/s")
