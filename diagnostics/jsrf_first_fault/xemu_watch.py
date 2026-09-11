#!/usr/bin/env python3
"""Catch the guest instruction that writes a CActMan field, using xemu.

The recomp never advances `CActMan +0x7EC4`, which xemu's archived snapshots
show going 0 -> 1 as the Corn tutorial progresses. No instruction in our
disassembly references the offset literally, so the write is formed from a
split base and cannot be found statically. This sets a hardware write
watchpoint on the live field in xemu, lets the guest run, and reports the PC
that fires -- which `symbolize.py` can then name.

Usage: xemu_watch.py [offset] [length]
       offset defaults to 0x7EC4, resolved against the live CActMan root.

Run it, then play xemu forward until the tutorial advances.
"""
import socket
import sys
import time

def recv(sock, timeout=None):
    if timeout is not None:
        sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            part = sock.recv(65536)
        except socket.timeout:
            return None
        if not part:
            raise RuntimeError("xemu closed the connection")
        buf += part
        if b"#" in buf and len(buf) >= buf.index(b"#") + 3:
            start = buf.index(b"$") + 1 if b"$" in buf else 0
            body = buf[start:buf.index(b"#")]
            sock.sendall(b"+")
            return body.decode(errors="replace")


def rsp(sock, cmd, timeout=10.0):
    ck = sum(cmd.encode()) & 0xff
    sock.sendall(f"${cmd}#{ck:02x}".encode())
    return recv(sock, timeout)


def u32(data, off=0):
    return int.from_bytes(data[off:off + 4], "little")


def read(sock, addr, size):
    v = rsp(sock, f"m{addr:x},{size:x}")
    if not v or v.startswith("E"):
        raise RuntimeError(f"read failed at {addr:#010x}: {v!r}")
    return bytes.fromhex(v)


def main():
    offset = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x7EC4
    length = int(sys.argv[2], 0) if len(sys.argv) > 2 else 4

    s = socket.socket()
    s.settimeout(10.0)
    s.connect(("localhost", 1234))
    recv(s, 1.0)
    s.sendall(b"\x03")
    time.sleep(0.2)
    recv(s, 5.0)

    root = u32(read(s, 0x0022FCE0, 4))
    target = root + offset
    before = u32(read(s, target, 4))
    print(f"CActMan root {root:#010x}, watching {target:#010x} "
          f"(+{offset:#x}), current value {before:#010x}")

    reply = rsp(s, f"Z2,{target:x},{length:x}")
    if reply != "OK":
        print(f"write watchpoint rejected ({reply!r}); trying access watchpoint")
        reply = rsp(s, f"Z4,{target:x},{length:x}")
        if reply != "OK":
            print(f"no watchpoint support: {reply!r}")
            s.sendall(b"$c#63")
            return 1
    print("watchpoint armed. Resuming -- play forward until the tutorial "
          "advances. Ctrl-C to give up.\n")
    s.sendall(b"$c#63")

    try:
        while True:
            stop = recv(s, 3600.0)
            if stop is None:
                continue
            if not stop or stop[0] not in "TS":
                print(f"stop reply: {stop!r}")
                continue
            # T05 with a watch field, or plain T05; read PC from the register file.
            regs = rsp(s, "g")
            pc = None
            if regs and not regs.startswith("E"):
                raw = bytes.fromhex(regs)
                if len(raw) >= 36:
                    pc = u32(raw, 32)          # x86 GDB order: eip is reg 8
            after = u32(read(s, target, 4))
            print(f"WATCH HIT  value {before:#010x} -> {after:#010x}"
                  + (f"   writing EIP = {pc:#010x}" if pc else ""))
            print(f"  stop reply: {stop}")
            if pc:
                print(f"\n  symbolise with:\n"
                      f"    python3 diagnostics/jsrf_first_fault/symbolize.py "
                      f"lookup {pc:#010x}")
            before = after
            s.sendall(b"$c#63")
    except KeyboardInterrupt:
        print("\ndisarming and resuming")
        rsp(s, f"z2,{target:x},{length:x}")
        s.sendall(b"$c#63")
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
