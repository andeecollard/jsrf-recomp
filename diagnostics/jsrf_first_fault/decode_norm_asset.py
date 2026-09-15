#!/usr/bin/env python3
"""Decode JSRF's Media/**/*.dat asset containers to PNG.

Written while chasing a text rendering defect that turned out not to exist:
the HUD's zero really is a notched filled square, jetfont's 'y' really has a
flat horizontal descender, and jetfont's '$' really has two rectangular
counters. Three screenshots of "wrong glyphs" were the game's own art. The
decoder is kept because the format was not written down anywhere, and the next
question about a texture should not need it rediscovering.

Container:
    +0x00  "NORM"  a container of chunks; "MULT" a container of containers
    +0x04  u32     chunk count
    +0x10  first chunk header
  chunk header, 16 bytes:
    +0x00  u32     payload offset, RELATIVE TO ITS OWN CONTAINER's base
    +0x04  u32     next chunk header offset (0 = end)
    +0x08  u32     payload size
  Nested containers make those offsets relative to themselves; a walker that
  assumes file-absolute offsets works on jetfont.dat and breaks on SprNorm.dat.

Texture payload, 32-byte sub-header then data:
    +0x00  u32     name hash
    +0x14  u32     dim      square, width == height
    +0x18  u32     format   low byte 6 == DXT3
    +0x1C  u32     mip count
  DXT3 is one byte per texel, so payload size == dim*dim for an unmipped page,
  which is how the format byte was confirmed rather than assumed. Leaf payloads
  may instead carry a tag: MDLB a model, NJCA a camera.

  jetfont's colour blocks are almost always c0=0x0000 c1=0xffff
  idx=0x55555555 -- constant white -- so the glyph is carried entirely in the
  4-bit alpha channel.

Usage:  decode_norm_asset.py <file.dat> <outdir>
"""
import sys, struct, zlib, os
def png(path,w,h,rgb):
    raw=b''.join(b'\x00'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def ck(t,dd):
        c=t+dd; return struct.pack('>I',len(dd))+c+struct.pack('>I',zlib.crc32(c))
    open(path,'wb').write(b'\x89PNG\r\n\x1a\n'+ck(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ck(b'IDAT',zlib.compress(raw,6))+ck(b'IEND',b''))
def dxt3_rgb(data,w,h):
    out=bytearray(w*h*3); bw,bh=w//4,h//4; o=0
    for by in range(bh):
        for bx in range(bw):
            blk=data[o:o+16]; o+=16
            al=blk[0:8]; c0,c1=struct.unpack_from('<HH',blk,8); bits=struct.unpack_from('<I',blk,12)[0]
            def unp(c): return ((c>>11&31)*255//31,(c>>5&63)*255//63,(c&31)*255//31)
            C=[unp(c0),unp(c1)]
            C.append(tuple((2*C[0][i]+C[1][i])//3 for i in range(3)))
            C.append(tuple((C[0][i]+2*C[1][i])//3 for i in range(3)))
            for py in range(4):
                arow=al[py*2]|(al[py*2+1]<<8)
                for px in range(4):
                    a=((arow>>(px*4))&0xF)*17
                    idx=(bits>>(2*(py*4+px)))&3
                    x=bx*4+px;y=by*4+py;p=(y*w+x)*3
                    for k in range(3): out[p+k]=(C[idx][k]*a+255*(255-a))//255
    return bytes(out)
OUT=sys.argv[2]; os.makedirs(OUT,exist_ok=True)
MINDIM=int(sys.argv[3]) if len(sys.argv)>3 else 0
count=[0]
def walk(d, base, path, depth):
    if depth>4: return
    magic=d[base:base+4]
    off=base+0x10; i=0
    while off+16<=len(d):
        rp,rn,s,z=struct.unpack_from('<4I',d,off)
        p=base+rp; n=(base+rn) if rn else 0
        if p>=len(d) or s<0 or p+s>len(d)+16: break
        tag=d[p:p+4]
        if tag in (b'NORM',b'MULT'):
            walk(d,p,f"{path}.{i}",depth+1)
        else:
            if s>=0x20:
                hsh,_,_,_,_,dim,fmt,mips=struct.unpack_from('<8I',d,p)
                if dim in (16,32,64,128,256,512,1024) and (fmt&0xff)==6 and s-0x20>=dim*dim and dim>=MINDIM:
                    png(f"{OUT}/{path}.{i}_h{hsh:08x}_{dim}.png",dim,dim,dxt3_rgb(d[p+0x20:p+0x20+dim*dim],dim,dim))
                    count[0]+=1
                    print(f"{path}.{i}: hash=0x{hsh:08x} dim={dim} fmt=0x{fmt:x} mips={mips}")
        if n==0 or n>=len(d) or n<=off: break
        off=n; i+=1
d=open(sys.argv[1],'rb').read()
walk(d,0,"t",0)
print("textures written:",count[0])
