#!/usr/bin/env python3
"""Find JSRF's game-mode booleans empirically instead of trusting an offset.

Snapshot the game object twice in the SAME game state to learn which dwords
are merely noisy, then snapshot again in a DIFFERENT state (e.g. paused).
A mode boolean is a dword that held still across the first pair and moved
across the second. Everything that moves every frame is filtered out.
"""
import socket, time, sys, json, os

def rsp(s, d):
    ck = sum(d.encode()) & 0xFF
    s.sendall(f'${d}#{ck:02x}'.encode())
    return recv(s)

def recv(s):
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: return buf.decode(errors='replace')
        if not c: return buf.decode(errors='replace')
        buf += c
        if b'#' in buf and len(buf) >= buf.index(b'#') + 3:
            body = buf[buf.index(b'$')+1:buf.index(b'#')] if b'$' in buf else buf
            s.sendall(b'+'); return body.decode(errors='replace')

def snap(label, size=0x8800):
    s = socket.socket(); s.settimeout(5.0); s.connect(('localhost', 1234)); recv(s)
    s.sendall(b'\x03'); time.sleep(0.2); recv(s)
    try:
        r = rsp(s, 'm22fce0,4')
        root = int.from_bytes(bytes.fromhex(r), 'little')
        out = b''
        CH = 0x400
        for off in range(0, size, CH):
            hx = rsp(s, f'm{root+off:x},{CH:x}')
            if not hx or hx.startswith('E'): break
            out += bytes.fromhex(hx)
    finally:
        s.sendall(b'$c#63')
    s.close()
    print(f"  {label}: root=0x{root:08X}, {len(out)} bytes")
    return root, out

if __name__ == '__main__':
    label = sys.argv[1]
    root, data = snap(label)
    open(f'snap_{label}.bin','wb').write(data)
    json.dump({'root':root}, open(f'snap_{label}.json','w'))
