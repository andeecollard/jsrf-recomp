#!/usr/bin/env python3
"""Sample JSRF's five game-mode booleans from xemu via the GDB stub.

The mode booleans live at root+0x40..+0x4C on the game ("god") object.
Resolve the root through MEM32(0x0022FCE0) -- the literal address shifts
between runs, the indirection does not.

Precedence, highest first (per the decompilation author's architecture video):
  +0x40..+0x4C are four booleans; all zero means the fifth mode, default.
"""
import socket, time, sys

HOST, PORT = 'localhost', 1234

def rsp(sock, data):
    ck = sum(data.encode()) & 0xFF
    sock.sendall(f'${data}#{ck:02x}'.encode())
    return recv(sock)

def recv(sock):
    buf = b''
    while True:
        try:
            c = sock.recv(4096)
        except socket.timeout:
            return buf.decode(errors='replace')
        if not c:
            return buf.decode(errors='replace')
        buf += c
        if b'#' in buf and len(buf) >= buf.index(b'#') + 3:
            body = buf[buf.index(b'$')+1:buf.index(b'#')] if b'$' in buf else buf
            sock.sendall(b'+')
            return body.decode(errors='replace')

def rd(sock, addr, n):
    r = rsp(sock, f'm{addr:x},{n:x}')
    if not r or r.startswith('E'):
        return None
    return bytes.fromhex(r)

def u32(b, off=0):
    return int.from_bytes(b[off:off+4], 'little')

def main():
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    s = socket.socket(); s.settimeout(4.0); s.connect((HOST, PORT))
    recv(s)
    print("connected to xemu gdb stub :1234")
    print(" t     root      +40      +44      +48      +4C    mode")
    NAMES = ['covered_pause', 'event', 'freeze_cam', 'uncovered_pause']
    t0 = time.time()
    last = None; n = 0
    while time.time() - t0 < dur:
        s.sendall(b'\x03')                 # halt
        time.sleep(0.15)
        recv(s)
        try:
            p = rd(s, 0x0022FCE0, 4)
            root = u32(p) if p else 0
            flags = rd(s, root + 0x40, 16) if 0 < root < 0x8000000 else None
        finally:
            # Fire-and-forget: the stub sends nothing until the next stop, so
            # waiting for a reply here costs a full socket timeout per sample.
            s.sendall(b'$c#63')            # ALWAYS resume
        if flags:
            v = [u32(flags, 4*k) for k in range(4)]
            hot = [NAMES[k] for k in range(4) if v[k]]
            mode = '+'.join(hot) if hot else 'default'
            line = f"{time.time()-t0:5.1f}  {root:08X} " + " ".join(f"{x:08X}" for x in v) + f"   {mode}"
            if mode != last:
                print(line + "   <-- CHANGED", flush=True)
                last = mode
            elif n % 30 == 0:
                print(line, flush=True)
        n += 1
        time.sleep(0.4)

if __name__ == '__main__':
    main()
