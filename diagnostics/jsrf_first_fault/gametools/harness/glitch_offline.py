import sys,glob,os,struct
import numpy as np
def load(p):
    d=open(p,'rb').read(); off,=struct.unpack_from('<I',d,10); w,h=struct.unpack_from('<ii',d,18); bpp,=struct.unpack_from('<H',d,28)
    a=np.frombuffer(d[off:off+abs(w*h)*bpp//8],np.uint8).reshape(abs(h),w,bpp//8).astype(np.float32)
    g=a[...,2]*0.299+a[...,1]*0.587+a[...,0]*0.114
    H,W=g.shape; g=g[:H//(H//120)*(H//120),:W//(W//160)*(W//160)]
    return g.reshape(120,H//120,160,W//160).mean(axis=(1,3))
fs=sorted(glob.glob(os.path.join(sys.argv[1],'frame-*.bmp')))
T=[load(f) for f in fs]
d=lambda i,j: float(np.abs(T[i]-T[j]).mean())
hits=[]
for n in range(2,len(T)):
  for L in range(1,6):
    p=n-L-1
    if p<0: break
    run=range(p+1,n)
    m=min(min(d(p,x),d(n,x)) for x in run)
    if m>3 and d(p,n)<0.5*m: hits.append((p+1,n-1,m,d(p,n))); break
for h in hits: print(h)
print(len(hits),'hits in',len(T))
