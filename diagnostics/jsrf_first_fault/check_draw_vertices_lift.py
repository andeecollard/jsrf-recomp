#!/usr/bin/env python3
"""Differentially execute generated DrawVertices and its native replacement.

Helper doubles check the guest ABI and model state emission, buffer relocation
and volatile-register clobbering. This does not replace a live scene comparison.
No generated game code is copied into the repository.
"""
import argparse
import ctypes
import hashlib
from pathlib import Path
import random
import subprocess
import struct
import sys
import tempfile

from stage_draw_vertices_lift import BODY_SHA, extract

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t ram[0x800000 / 4];
static uint32_t eax, ebx, ecx, edx, esi, edi, esp;
static unsigned calls, variant;
#define MEM32(a) ram[(uint32_t)(a)/4]
#define PUSH32(s,v) do { (s)-=4; MEM32(s)=(v); } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define RECOMP_MEM_WRITE32(pc,fn,a,v) (MEM32(a)=(v))
#define RECOMP_ABI_CALL(va,fn) fn()
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
static void require(int good) { if (!good) abort(); }
static void sub_00196520(void) {
    require(calls++ == 0 && ecx == 0x300000);
    require(esp == 0x500000-20 && MEM32(esp)==0x199312 && MEM32(esp+4)==0);
    require(ebx==0x300000 && esi==0x23456789 && edi==0x3456789a);
    MEM32(0x300008)=0x12340000 | variant; /* dirty-state side effect */
    MEM32(0x3ff000)=0x40304; /* state emitted before geometry */
    MEM32(0x3ff004)=variant;
    eax=0x77777777; ecx=0x88888888; edx=0x99999999;
    esp+=8;
}
static void sub_001916C0(void) {
    require(calls++ == 1 && esp==0x500000-24);
    require(MEM32(esp)==0x199327 && MEM32(esp+4)==0x300000);
    require(MEM32(esp+8)==(((MEM32(0x50000c)-1)>>8)+1)+5);
    require(edi==MEM32(0x50000c) && esi==((edi-1)>>8)+1);
    MEM32(0x300010)=MEM32(esp+8);
    eax=0x400000 + variant*0x1000; /* reservation may relocate */
    ecx=0xaaaaaaaa; edx=0xbbbbbbbb;
    esp+=12;
}
__ORIGINAL__
static unsigned fallback;
static void jsrf_draw_vertices_original(void) { fallback++; }
__NATIVE__
void run(unsigned native, uint32_t primitive, uint32_t first, uint32_t count,
         unsigned mode) {
    memset(ram, 0xa5, sizeof(ram));
    variant=mode; calls=0; fallback=0;
    eax=0x11111111; ebx=0x12345678; ecx=0x22222222; edx=0x33333333;
    esi=0x23456789; edi=0x3456789a; esp=0x500000;
    MEM32(0x19dce0)=0x300000;
    MEM32(esp)=0x600000; MEM32(esp+4)=primitive;
    MEM32(esp+8)=first; MEM32(esp+12)=count;
    if (native==3) return;
    if (native==2) sub_00199300();
    else if (native) jsrf_draw_vertices_lift();
    else generated_draw();
}
uint32_t *memory(void) { return ram; }
void registers(uint32_t *out) {
    uint32_t values[]={eax,ebx,ecx,edx,esi,edi,esp,calls,fallback};
    memcpy(out,values,sizeof(values));
}
'''


def xbox_oracle(xbe_path, cases, snapshot):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                                  UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                                  UC_X86_REG_ESP, UC_X86_REG_EIP)
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.func_id.d3d8_identifier import _file_offset, _parse_sections
    xbe = xbe_path.read_bytes()
    offset = _file_offset(_parse_sections(xbe), 0x199300, 159, len(xbe))
    if offset is None:
        raise ValueError("US DrawVertices absent")
    code = xbe[offset:offset+159]
    if hashlib.sha256(code).hexdigest() != "d4a65d6f2302f5fcb2c7ad4dd4b29bd064e6798229b4051d232444fcfa2d155d":
        raise ValueError("unverified XBE routine")
    registers = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                 UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP)
    for case in cases:
        mu = Uc(UC_ARCH_X86, UC_MODE_32)
        mu.mem_map(0, 0x800000)
        initial_regs, initial_mem = snapshot(3, case)
        mu.mem_write(0, initial_mem)
        mu.mem_write(0x199300, code)
        for reg, value in zip(registers, struct.unpack('<9I', initial_regs)):
            mu.reg_write(reg, value)
        calls = []
        def read(address):
            return struct.unpack('<I', mu.mem_read(address, 4))[0]
        def write(address, value):
            mu.mem_write(address, struct.pack('<I', value))
        def hook(uc, address, size, data):
            if address not in (0x196520, 0x1916c0):
                return
            sp = uc.reg_read(UC_X86_REG_ESP)
            target = read(sp)
            calls.append(address)
            if address == 0x196520:
                assert target == 0x199312 and read(sp+4) == 0
                assert uc.reg_read(UC_X86_REG_ECX) == 0x300000
                write(0x300008, 0x12340000 | case[3])
                write(0x3ff000, 0x40304)
                write(0x3ff004, case[3])
                uc.reg_write(UC_X86_REG_EAX, 0x77777777)
                uc.reg_write(UC_X86_REG_ECX, 0x88888888)
                uc.reg_write(UC_X86_REG_EDX, 0x99999999)
                uc.reg_write(UC_X86_REG_ESP, sp+8)
            else:
                assert target == 0x199327 and read(sp+4) == 0x300000
                assert read(sp+8) == ((case[2]-1) >> 8)+6
                write(0x300010, read(sp+8))
                uc.reg_write(UC_X86_REG_EAX, 0x400000+case[3]*0x1000)
                uc.reg_write(UC_X86_REG_ECX, 0xaaaaaaaa)
                uc.reg_write(UC_X86_REG_EDX, 0xbbbbbbbb)
                uc.reg_write(UC_X86_REG_ESP, sp+12)
            uc.reg_write(UC_X86_REG_EIP, target)
        mu.hook_add(UC_HOOK_CODE, hook)
        mu.emu_start(0x199300, 0x600000, count=10000)
        assert mu.reg_read(UC_X86_REG_EIP) == 0x600000
        assert calls == [0x196520, 0x1916c0]
        mu.mem_write(0x199300, initial_mem[0x199300:0x199300+159])
        actual_regs = struct.pack('<9I', *(mu.reg_read(r) for r in registers), 2, 0)
        expected_regs, expected_mem = snapshot(1, case)
        assert actual_regs == expected_regs, case
        assert bytes(mu.mem_read(0, 0x800000)) == expected_mem, case
    print(f"PASS: {len(cases)} original-XBE vs native comparisons in Unicorn "
          "(helper doubles; full memory and general registers)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated_chunk", type=Path)
    parser.add_argument("--xbe", type=Path)
    parser.add_argument("--no-sanitize", action="store_true",
                        help="for Apple system Python, which cannot load the UBSan runtime")
    args = parser.parse_args()
    _, _, body = extract(args.generated_chunk.read_text())
    if hashlib.sha256(body.encode()).hexdigest() != BODY_SHA:
        raise ValueError("unexpected generated body")
    native = Path(__file__).with_name("draw_vertices_lift.h").read_text()
    body = body.replace("void sub_00199300(void)", "static void generated_draw(void)", 1)
    source = HARNESS.replace("__ORIGINAL__", body).replace("__NATIVE__", native)
    with tempfile.TemporaryDirectory(prefix="jsrf-draw-lift-") as directory:
        root = Path(directory)
        (root / "test.c").write_text(source)
        sanitizer = [] if args.no_sanitize else ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
        subprocess.run(["cc", "-std=c11", "-O2", "-shared", "-fPIC", *sanitizer,
                        str(root / "test.c"), "-o", str(root / "test.dylib")], check=True)
        lib = ctypes.CDLL(str(root / "test.dylib"))
        lib.memory.restype = ctypes.c_void_p
        lib.run.argtypes = [ctypes.c_uint32] * 5
        def snapshot(mode, case):
            lib.run(mode, *case)
            regs = (ctypes.c_uint32 * 9)()
            lib.registers(regs)
            return bytes(regs), ctypes.string_at(lib.memory(), 0x800000)
        rng = random.Random(4134)
        cases = [(p, s, n, v) for p in (1, 3, 5, 10)
                 for s in (0, 7, 0x100, 0xff0000)
                 for n in (1, 2, 255, 256, 257, 511, 512, 513, 1024, 65536)
                 for v in (0, 1)]
        cases += [(rng.randrange(1, 11), rng.randrange(0xff0000),
                   rng.randrange(1, 65537), rng.randrange(2)) for _ in range(100)]
        for case in cases:
            assert snapshot(0, case) == snapshot(1, case), case
        if args.xbe:
            xbox_oracle(args.xbe, cases[:20] + cases[-10:], snapshot)
        import os
        enabled = os.environ.get("RECOMP_JSRF_DRAW_LIFT") == "1"
        for case in ((5, 7, 257, 1), (5, 0, 0, 0), (5, 0, 65537, 0),
                     (5, 0xffffff, 2, 0), (5, 0x1000000, 1, 0)):
            regs, _ = snapshot(2, case)
            values = (ctypes.c_uint32 * 9).from_buffer_copy(regs)
            accepted = enabled and case == (5, 7, 257, 1)
            assert values[7] == (2 if accepted else 0)
            assert values[8] == (0 if accepted else 1)
        print(f"PASS: {len(cases)} full-memory/register/helper comparisons; "
              f"5 dispatch/fallback checks (switch={int(enabled)}); UBSan={not args.no_sanitize}")


if __name__ == "__main__":
    main()
