#!/usr/bin/env python3
"""Check JSRF's XDK 4134 DrawVertices packet bytes against a small oracle.

Requires Unicorn and the user's SegaJSRF.xbe. No XBE bytes are stored here.
The two external XDK calls are stubbed: a validation call and pushbuffer
reservation. This checks the packet-writing body, not whole-call equivalence.
"""

import argparse
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDI, UC_X86_REG_EIP, UC_X86_REG_ESI,
                               UC_X86_REG_ESP)

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.func_id.d3d8_identifier import _file_offset, _parse_sections

ENTRY = 0x001992C0
END = 0x0019935F
DEVICE = 0x00300000
PUSHBUF = 0x00400000
STACK = 0x00500000
SENTINEL = 0x00600000


def u32(data):
    return struct.unpack("<I", data)[0]


def packet(primitive, start, count):
    if count < 1 or count > 65536 or start < 0 or start + count > 0x1000000:
        raise ValueError("outside bounded packet oracle")
    batches = (count + 255) // 256
    words = [0x000417FC, primitive, 0x40001810 | (batches << 18)]
    remaining = count
    while remaining > 256:
        words.append(0xFF000000 | start)
        start += 256
        remaining -= 256
    words.extend([((remaining - 1) << 24) | start, 0x000417FC, 0])
    return words


def execute(xbe, primitive, start, count):
    code_offset = _file_offset(_parse_sections(xbe), ENTRY, END - ENTRY, len(xbe))
    if code_offset is None:
        raise ValueError("DrawVertices code is absent from the XBE")
    code = xbe[code_offset:code_offset + END - ENTRY]
    # Pin the one routine version whose instructions and helper boundaries
    # were inspected. Otherwise a different retail build could be tested
    # against invalid hook addresses.
    if code[:8] != bytes.fromhex("53 8B 1D A0 DC 19 00 56") or code[-3:] != bytes.fromhex("C2 0C 00"):
        raise ValueError("unexpected DrawVertices routine bytes")
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    for base, size in ((0x00190000, 0x10000), (DEVICE, 0x1000),
                       (PUSHBUF, 0x2000), (STACK, 0x2000), (SENTINEL, 0x1000)):
        mu.mem_map(base, size)
    mu.mem_write(ENTRY, code)
    mu.mem_write(0x0019DCA0, struct.pack("<I", DEVICE))
    sp = STACK + 0x1000
    mu.mem_write(sp, struct.pack("<IIII", SENTINEL, primitive, start, count))
    mu.reg_write(UC_X86_REG_ESP, sp)
    mu.reg_write(UC_X86_REG_EIP, ENTRY)
    mu.reg_write(UC_X86_REG_EAX, 0)
    mu.reg_write(UC_X86_REG_EBX, 0x12345678)
    mu.reg_write(UC_X86_REG_ECX, 0)
    mu.reg_write(UC_X86_REG_ESI, 0x23456789)
    mu.reg_write(UC_X86_REG_EDI, 0x3456789A)
    helper_calls = []

    def hook(uc, address, _size, _data):
        if address == 0x001992CD:
            helper_calls.append(address)
            uc.reg_write(UC_X86_REG_ESP, uc.reg_read(UC_X86_REG_ESP) + 4)
            uc.reg_write(UC_X86_REG_EIP, 0x001992D2)
        elif address == 0x001992E2:
            helper_calls.append(address)
            uc.reg_write(UC_X86_REG_ESP, uc.reg_read(UC_X86_REG_ESP) + 8)
            uc.reg_write(UC_X86_REG_EAX, PUSHBUF)
            uc.reg_write(UC_X86_REG_EIP, 0x001992E7)

    mu.hook_add(UC_HOOK_CODE, hook)
    mu.emu_start(ENTRY, SENTINEL, count=10000)
    if helper_calls != [0x001992CD, 0x001992E2]:
        raise AssertionError("unexpected helper call sequence")
    if mu.reg_read(UC_X86_REG_EIP) != SENTINEL:
        raise AssertionError("routine did not return to sentinel")
    if mu.reg_read(UC_X86_REG_ESP) != sp + 16:
        raise AssertionError("stdcall stack cleanup mismatch")
    for reg, expected in ((UC_X86_REG_EBX, 0x12345678),
                          (UC_X86_REG_ESI, 0x23456789),
                          (UC_X86_REG_EDI, 0x3456789A)):
        if mu.reg_read(reg) != expected:
            raise AssertionError("callee-saved register changed")
    put = u32(mu.mem_read(DEVICE, 4))
    if not PUSHBUF <= put <= PUSHBUF + 0x2000 or (put - PUSHBUF) % 4:
        raise AssertionError("invalid pushbuffer put pointer")
    if mu.reg_read(UC_X86_REG_EAX) != put:
        raise AssertionError("return register differs from pushbuffer put")
    if bytes(mu.mem_read(DEVICE + 4, 0x100 - 4)) != bytes(0x100 - 4):
        raise AssertionError("unexpected device state write")
    if bytes(mu.mem_read(put, PUSHBUF + 0x2000 - put)) != bytes(PUSHBUF + 0x2000 - put):
        raise AssertionError("unexpected pushbuffer write beyond put")
    return list(struct.unpack("<" + "I" * ((put - PUSHBUF) // 4),
                              mu.mem_read(PUSHBUF, put - PUSHBUF)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("xbe", type=Path, help="local SegaJSRF.xbe")
    args = parser.parse_args()
    xbe = args.xbe.read_bytes()
    counts = (1, 2, 255, 256, 257, 258, 511, 512, 513, 1024)
    for primitive, start in ((3, 7), (5, 0x100)):
        for count in counts:
            actual = execute(xbe, primitive, start, count)
            expected = packet(primitive, start, count)
            if actual != expected:
                raise AssertionError((primitive, start, count, actual, expected))
    print(f"PASS: {len(counts) * 2} DrawVertices packet cases; exact words and put pointer")


if __name__ == "__main__":
    main()
