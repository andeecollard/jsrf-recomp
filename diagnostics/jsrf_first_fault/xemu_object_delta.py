#!/usr/bin/env python3
"""Measure how much each registered object mutates in xemu over an interval.

The recomp shows CPlayer id 44 changing 8 dwords where id 45 changes 136 over
the same 12 s, which is the shape of "one character animates and one does not".
That is only a divergence if xemu animates both. This reads each object twice
over the GDB stub and reports the per-object dword delta, so the two sides can
be compared with the same yardstick.

Usage: xemu_object_delta.py [seconds] [out.json]

The guest runs between the two reads; each read halts it only briefly.
"""
import json
import socket
import sys
import time

SPAN = 0x2000          # bytes per object, matching the recomp-side attribution
IDS = 7668


def recv(sock):
    """Read one RSP packet, or None if nothing arrives.

    Connecting to the stub is itself an attach, and an attach halts the VM and
    emits a stop reply. Consuming that leaves a subsequent \x03 with nothing to
    answer it, so a missing reply is normal here and must not be fatal -- an
    exception on this path would abandon the guest in the stopped state.
    """
    buf = b""
    while True:
        try:
            part = sock.recv(65536)
        except socket.timeout:
            return None
        if not part:
            raise RuntimeError("xemu closed the GDB connection")
        buf += part
        if b"#" in buf and len(buf) >= buf.index(b"#") + 3:
            start = buf.index(b"$") + 1 if b"$" in buf else 0
            body = buf[start:buf.index(b"#")]
            sock.sendall(b"+")
            return body.decode(errors="replace")


def rsp(sock, command):
    checksum = sum(command.encode()) & 0xff
    sock.sendall(f"${command}#{checksum:02x}".encode())
    return recv(sock)


def read(sock, address, size, partial=False):
    """Read guest memory, optionally stopping at the first unreadable page.

    Objects sit at arbitrary offsets and a fixed span can run past the end of a
    mapped page, which the stub reports as an error rather than a short read.
    For the per-object window that is expected, not fatal: take what is there
    and let the comparison use the length both samples share.
    """
    out = b""
    while size:
        chunk = min(size, 0x800)
        value = rsp(sock, f"m{address + len(out):x},{chunk:x}")
        if not value or value.startswith("E"):
            if partial:
                return out
            raise RuntimeError(f"read failed at {address + len(out):#010x}")
        out += bytes.fromhex(value)
        size -= chunk
    return out


def u32(data, offset=0):
    return int.from_bytes(data[offset:offset + 4], "little")


def sample(sock):
    """Halt, read every registered object, resume."""
    sock.sendall(b"\x03")
    time.sleep(0.2)
    recv(sock)
    try:
        root = u32(read(sock, 0x0022FCE0, 4))
        manager = read(sock, root, 0x8840)
        out = {}
        for oid in range(IDS):
            addr = u32(manager, 0x98 + oid * 4)
            if not 0x10000 <= addr < 0x08000000:
                continue
            out[oid] = (addr, read(sock, addr, SPAN, partial=True))
        return root, u32(manager, 0x87e8), out
    finally:
        sock.sendall(b"$c#63")


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0
    sock = socket.socket()
    sock.settimeout(15.0)
    sock.connect(("localhost", 1234))
    try:
        recv(sock)
    except socket.timeout:
        pass

    try:
        root, live, first = sample(sock)
    except BaseException:
        sock.sendall(b"$c#63")
        raise
    print(f"first sample: root={root:#010x} live={live} objects={len(first)}",
          file=sys.stderr)
    time.sleep(seconds)
    try:
        _, _, second = sample(sock)
    except BaseException:
        sock.sendall(b"$c#63")
        raise
    sock.close()

    rows = []
    for oid, (addr, a) in sorted(first.items()):
        if oid not in second:
            continue
        b = second[oid][1]
        offs = [i for i in range(0, min(len(a), len(b)), 4)
                if a[i:i + 4] != b[i:i + 4]]
        rows.append({"id": oid, "address": addr, "vtable": u32(a),
                     "changed_dwords": len(offs), "offsets": offs[:16]})

    rows.sort(key=lambda r: -r["changed_dwords"])
    print(f"\n{'id':>6} {'vtable':>10} {'changed':>8}  first offsets")
    for r in rows:
        if not r["changed_dwords"]:
            continue
        print(f"{r['id']:>6} {r['vtable']:#010x} {r['changed_dwords']:>8}  "
              + " ".join(hex(o) for o in r["offsets"][:6]))
    quiet = [r["id"] for r in rows if not r["changed_dwords"]]
    print(f"\nunchanged over {seconds:g}s: {len(quiet)} of {len(rows)}")

    if len(sys.argv) > 2:
        json.dump({"seconds": seconds, "root": root, "objects": rows},
                  open(sys.argv[2], "w"), indent=2)
        print(f"wrote {sys.argv[2]}", file=sys.stderr)


if __name__ == "__main__":
    main()
