#!/usr/bin/env python3
import json
import socket
import time


def recv(sock):
    buf = b""
    while True:
        try:
            part = sock.recv(65536)
        except socket.timeout:
            return ""
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


def read(sock, address, size):
    value = rsp(sock, f"m{address:x},{size:x}")
    if not value or value.startswith("E"):
        raise RuntimeError(f"read failed at 0x{address:08X}: {value!r}")
    return bytes.fromhex(value)


def u32(data, offset=0):
    return int.from_bytes(data[offset:offset + 4], "little")


sock = socket.socket()
sock.settimeout(5.0)
sock.connect(("localhost", 1234))
try:
    recv(sock)
except socket.timeout:
    pass
sock.sendall(b"\x03")
time.sleep(0.2)
recv(sock)
try:
    root = u32(read(sock, 0x0022FCE0, 4))
    manager = read(sock, root, 0x8840)
    objects = []
    for object_id in range(7668):
        address = u32(manager, 0x98 + object_id * 4)
        if not 0x10000 <= address < 0x08000000:
            continue
        data = read(sock, address, 0x50)
        objects.append({
            "id": object_id,
            "address": address,
            "vtable": u32(data, 0x00),
            "flags": u32(data, 0x04),
            "stored_id": u32(data, 0x08),
            "draw_child_mask": u32(data, 0x0c),
            "fz": u32(data, 0x10),
            "zsort_key": u32(data, 0x14),
            "tx": u32(data, 0x18),
            "ty": u32(data, 0x1c),
            "tz": u32(data, 0x20),
            "parent": u32(data, 0x24),
            "child": u32(data, 0x28),
            "sibling_before": u32(data, 0x2c),
            "sibling_next": u32(data, 0x30),
            "draw_next": u32(data, 0x34),
            "draw_before_ptr": u32(data, 0x38),
            "draw_last_ptr": u32(data, 0x3c),
            "zsort": u32(data, 0x40),
            "extra_44": u32(data, 0x44),
            "extra_48": u32(data, 0x48),
        })
    bins = [
        {"bin": i, "head": u32(manager, 0x7fb4 + i * 4)}
        for i in range(256)
        if u32(manager, 0x7fb4 + i * 4)
    ]
    result = {
        "source": "xemu",
        "root": root,
        "live": u32(manager, 0x87e8),
        "exec_root": u32(manager, 0x87dc),
        "draw_root": u32(manager, 0x7fa4),
        "draw_root_tail": u32(manager, 0x7fa8),
        "draw_sort_root": u32(manager, 0x7fac),
        "draw_sort_tail": u32(manager, 0x7fb0),
        "skip_draw": u32(manager, 0x74),
        "draw_mode": u32(manager, 0x94),
        "state_7930": u32(manager, 0x7930),
        "state_7934": u32(manager, 0x7934),
        "state_7EC4": u32(manager, 0x7ec4),
        "draw_sort_bins": bins,
        "objects": objects,
    }
    out = json.dumps(result, indent=2)
    import sys
    if len(sys.argv) > 1:
        open(sys.argv[1], "w").write(out + "\n")
        print(f"wrote {len(objects)} objects to {sys.argv[1]}", file=sys.stderr)
    else:
        print(out)
finally:
    sock.sendall(b"$c#63")
    sock.close()
