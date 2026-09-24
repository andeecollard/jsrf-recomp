#!/usr/bin/env python3
"""Read RECOMP_GLITCH_WATCH hits (glitch-<k>/) and say which draws changed.

Each hit directory holds frame-<seq>.bmp for the run and one frame each side,
and draws-<seq>.txt reaching two frames further back: the picture of frame-S
is the scene of draws-(S-2). For every glitch frame this prints the draw
states present in both neighbours but missing from it, the states only it has,
how many of its draws carry non-finite transform values, and how many carry a
transform hash neither neighbour has; and it writes sheet.png, the frames side
by side. A frame whose draw list is unchanged but whose picture is not points
at the values the draws were given (a transform or a skeleton), not at a draw
that went missing.

    glitch_triage.py <glitch-dir> [...]
"""
import sys, glob, os, re, collections
from PIL import Image
def draws(p):
    out=[]
    for l in open(p):
        if l[0]=='#': continue
        f=l.split()
        out.append(dict(key=tuple(f[1:12]), xh=f[12], bad=int(f[13])))
    return out
for d in sys.argv[1:]:
    bm=sorted(int(re.findall(r'(\d+)',os.path.basename(p))[0]) for p in glob.glob(d+'/frame-*.bmp'))
    if len(bm)<3: print(d,'incomplete'); continue
    before,after=bm[0],bm[-1]; run=bm[1:-1]
    D=lambda s: draws(f'{d}/draws-{s-2}.txt')
    nb=D(before); na=D(after)
    nbk=collections.Counter(x['key'] for x in nb); nak=collections.Counter(x['key'] for x in na)
    print(f'== {d}: run {run[0]}..{run[-1]}  draws before/after {len(nb)}/{len(na)}')
    for s in run:
        g=D(s); gk=collections.Counter(x['key'] for x in g)
        missing=[k for k in nbk if k in nak and k not in gk]
        extra=[k for k in gk if k not in nbk and k not in nak]
        bad=[x for x in g if x['bad']]
        badn=sum(x['bad'] for x in nb+na)
        print(f'  frame {s} (draws-{s-2}): {len(g)} draws; states missing vs both neighbours {len(missing)}, new {len(extra)}; draws with non-finite xf {len(bad)} (neighbours {badn})')
        for k in missing[:5]: print('     missing:', ' '.join(k))
        for k in extra[:5]: print('     new    :', ' '.join(k))
        # same key, xf hash unique to the glitch frame
        hb=set(x['xh'] for x in nb)|set(x['xh'] for x in na)
        uh=[x for x in g if x['xh'] not in hb]
        print(f'     draws whose transform hash appears in neither neighbour: {len(uh)} of {len(g)}')
    ims=[Image.open(f'{d}/frame-{s}.bmp').convert('RGB').resize((320,240)) for s in bm]
    S=Image.new('RGB',(320*len(ims),240))
    for i,im in enumerate(ims): S.paste(im,(320*i,0))
    S.save(d+'/sheet.png')
