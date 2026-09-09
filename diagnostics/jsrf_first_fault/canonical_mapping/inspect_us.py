from pathlib import Path
import struct, hashlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
p=Path('../Jet Set Radio Future (US)/default.xbe'); b=p.read_bytes()
u=lambda o:struct.unpack_from('<I',b,o)[0]
base=u(0x104); cert=u(0x118)-base
print('SHA256',hashlib.sha256(b).hexdigest(),'size',len(b))
print('certificate title ID/version/region:',*[hex(u(cert+o)) for o in (8,0xac,0xa0)])
secs=[]
for i in range(u(0x11c)):
 o=u(0x120)-base+i*56; secs.append((u(o+4),u(o+8),u(o+12),u(o+16)))
def read(a,n):
 for va,vs,raw,sz in secs:
  if va<=a and a+n<=va+sz:return b[raw+a-va:raw+a-va+n]
 raise ValueError(hex(a))
md=Cs(CS_ARCH_X86,CS_MODE_32)
for a,z in [(0x11000,0x11b60),(0x11b60,0x11be0),(0x11d00,0x12020),(0x12100,0x12170),(0x123e0,0x12ae0),(0x12c80,0x131a0),(0x13a80,0x13f80)]:
 print('\nRANGE',hex(a),hex(z))
 for i in md.disasm(read(a,z-a),a): print(f'{i.address:08x} {i.bytes.hex():20} {i.mnemonic} {i.op_str}')

print("Base vtable", [hex(v) for v in struct.unpack("<16I",read(0x1c4390,64))])
