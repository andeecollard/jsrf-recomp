import struct
XBE = __import__('os').path.expanduser("~/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)/default.xbe")
class Xbe:
    def __init__(self, path=XBE):
        self.d = open(path, 'rb').read()
        d = self.d
        self.base = struct.unpack_from('<I', d, 0x104)[0]
        nsec, secaddr = struct.unpack_from('<II', d, 0x11C)
        self.secs = []
        for i in range(nsec):
            o = secaddr - self.base + i*0x38
            fl, va, vs, ra, rs, nameaddr = struct.unpack_from('<IIIIII', d, o)
            n = d[nameaddr-self.base:].split(b'\0')[0].decode()
            self.secs.append((n, va, vs, ra, rs))
    def read(self, va, n):
        for (nm, sva, vs, ra, rs) in self.secs:
            if sva <= va < sva + vs:
                off = va - sva
                b = self.d[ra+off: ra+min(off+n, rs)]
                return b + b'\0'*(n-len(b))
        raise KeyError(hex(va))
