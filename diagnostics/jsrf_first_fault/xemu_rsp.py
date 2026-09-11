"""Shared, resume-safe RSP helper for xemu's GDB stub.

Two rules, each learned by wedging the emulator and replaying the tutorial:

1. **Connecting IS attaching, and attaching halts the VM.** Sending a further
   \\x03 then leaves an interrupt with nothing to answer it and desynchronises
   the stream, after which reads return nothing and the stub is left stopped
   with a dead peer -- it then refuses new connections entirely. So probe with
   a read first and only interrupt if that read fails.
2. **Resume on every exit path.** Any exception between the halt and the
   continue strands the guest, which costs a full manual replay.

Use as a context manager:

    with XemuRSP() as x:
        root = x.u32(x.read(0x0022FCE0, 4))
"""
import socket
import time


class XemuRSP:
    def __init__(self, host="localhost", port=1234, timeout=20.0):
        self._addr = (host, port)
        self._timeout = timeout
        self.sock = None
        self._resumed = True

    def __enter__(self):
        self.sock = socket.socket()
        self.sock.settimeout(self._timeout)
        self.sock.connect(self._addr)
        self._recv(1.0)                      # the attach's own stop reply, if any
        self._resumed = False                # attaching stopped the guest
        if self.read(0x0022FCE0, 4) is None:  # only interrupt if truly running
            self.sock.sendall(b"\x03")
            time.sleep(0.25)
            self._recv(5.0)
        return self

    def __exit__(self, *exc):
        self.resume()
        try:
            self.sock.close()
        except Exception:
            pass
        return False

    def resume(self):
        if not self._resumed:
            try:
                self.sock.sendall(b"$c#63")
            except Exception:
                pass
            self._resumed = True

    def _recv(self, t=None):
        self.sock.settimeout(t or self._timeout)
        buf = b""
        while True:
            try:
                part = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not part:
                return None
            buf += part
            if b"#" in buf and len(buf) >= buf.index(b"#") + 3:
                start = buf.index(b"$") + 1 if b"$" in buf else 0
                body = buf[start:buf.index(b"#")]
                self.sock.sendall(b"+")
                return body.decode(errors="replace")

    def cmd(self, command, timeout=None):
        checksum = sum(command.encode()) & 0xff
        self.sock.sendall(f"${command}#{checksum:02x}".encode())
        return self._recv(timeout)

    def read(self, address, size):
        out = b""
        while size:
            chunk = min(size, 0x800)
            value = self.cmd(f"m{address + len(out):x},{chunk:x}")
            if not value or value.startswith("E"):
                return out or None
            out += bytes.fromhex(value)
            size -= chunk
        return out

    @staticmethod
    def u32(data, offset=0):
        return int.from_bytes(data[offset:offset + 4], "little")
