import struct, os, sys
E = os.path.expanduser("~/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)/Media/Event/Event")
NAMES = ["sceneIntl","sceneJP","text","models","heads","u6","u7","u8","shadow","textures","effects","sounds"]
def sections(d):
    assert d[:4] == b'MULT'
    n, = struct.unpack_from('<I', d, 4)
    out = []
    o = 16
    for i in range(n):
        off, nxt, size = struct.unpack_from('<III', d, o)
        cnt = struct.unpack_from('<I', d, off + 4)[0] if d[off:off+4] == b'NORM' else -1
        out.append((cnt, size))
        o = nxt
    return out
for n in sys.argv[1:]:
    d = open(f"{E}/e{int(n):03d}.dat", 'rb').read()
    s = sections(d)
    print(f"e{int(n):03d}", " ".join(f"{NAMES[i] if i<12 else i}={c}" for i, (c, sz) in enumerate(s) if c))
