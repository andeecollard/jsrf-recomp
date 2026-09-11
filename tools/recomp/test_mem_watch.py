"""Generated-store and runtime tests for RECOMP_MEM_WATCH."""

import os
import shutil
import subprocess
import tempfile

from tools.recomp.disasm import BasicBlock, Disassembler, Instruction, Operand
from tools.recomp.lifter import Lifter
from tools.recomp.translator import FunctionTranslator


def _mem(size, pc, base="eax", disp=0, function=0):
    return Operand(type="mem", mem_base=base, mem_disp=disp,
                   mem_size=size, insn_address=pc,
                   function_address=function)


def _reg(name):
    return Operand(type="reg", reg=name)


def test_scalar_store_carries_exact_guest_pc_and_width():
    insn = Instruction(0x00123456, 2, "mov", "dword ptr [eax], ecx", "8908")
    insn.operands = [_mem(4, insn.address, function=0x00123000), _reg("ecx")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert generated == ("RECOMP_MEM_WRITE32(0x00123456u, 0x00123000u, "
                         "eax, ecx);")


def test_disassembly_attaches_pc_before_store_lowering():
    instruction = Disassembler().disassemble_function(
        bytes.fromhex("8908"), 0x00123456, 0x00123458)[0]

    lifter = Lifter()
    lifter.func_start = 0x00123000
    generated = "\n".join(lifter.lift_instruction(instruction))

    assert generated == ("RECOMP_MEM_WRITE32(0x00123456u, 0x00123000u, "
                         "eax, ecx);")


def test_translator_attaches_containing_function_to_store():
    start = 0x00123000
    store = Instruction(start, 2, "mov", "dword ptr [eax], ecx", "8908")
    store.operands = [_mem(4, start), _reg("ecx")]
    ret = Instruction(start + 2, 1, "ret", "", "c3")
    block = BasicBlock(start=start, instructions=[store, ret])
    translator = FunctionTranslator(
        b"\0", {start: {"_addr": start, "end": start + 3}})
    translator._read_func_bytes = lambda _start, _end: b"\x89\x08\xc3"
    translator.disasm.disassemble_function = (
        lambda _raw, _start, _end: [store, ret])
    translator.disasm.build_basic_blocks = (
        lambda _insns, _start, _end, extra_leaders=None: [block])

    generated = translator.translate_function(
        start, {"_addr": start, "end": start + 3})

    assert ("RECOMP_MEM_WRITE32(0x00123000u, 0x00123000u, eax, ecx);"
            in generated)


def test_arithmetic_memory_destination_uses_same_store_seam():
    insn = Instruction(0x00234567, 3, "add", "word ptr [eax+2], cx", "66014802")
    insn.operands = [_mem(2, insn.address, disp=2), _reg("cx")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert "RECOMP_MEM_WRITE16(0x00234567u, 0x00000000u, eax + 2," in generated


def test_register_indexed_bit_memory_destination_uses_store_seam():
    insn = Instruction(0x00245678, 3, "bts", "dword ptr [eax], ecx", "0fab08")
    insn.operands = [_mem(4, insn.address), _reg("ecx")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert "RECOMP_MEM_WRITE32(0x00245678u, 0x00000000u," in generated


def test_scalar_sse_store_is_the_first_non_scalar_class():
    insn = Instruction(0x00345678, 4, "movss", "dword ptr [eax], xmm0", "f30f1100")
    insn.operands = [_mem(4, insn.address), _reg("xmm0")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert generated == ("RECOMP_MEM_WRITEF(0x00345678u, 0x00000000u, "
                         "eax, xmm0.f[0]); /* movss */")


C_RUNTIME_TEST = r"""
#include <stdint.h>
#include <string.h>
#include "recomp_mem_watch.h"

int main(void)
{
    static union { uint64_t align; uint8_t b[0x100]; } ram;
    uint32_t initial = 0x11223344u;

    memcpy(ram.b + 0x20, &initial, sizeof initial);
    /* One mapped mirror at guest 0x100..0x1ff aliases the base bytes. */
    recomp_mem_watch_init(0x100, 1u, 0xF0000000u, 0);
    recomp_mem_watch_add_ram_alias(0x80000020u, 0x20u, 0x20u);

    /* Non-overlap: no record. */
    recomp_mem_watch_guest_store(0x11111111u, 0x11110000u, 0x10u, 4,
                                 ram.b + 0x10, 0xAAAAAAAAu);
    /* Same-value writes are still accesses and must be recorded. */
    recomp_mem_watch_guest_store(0x22222222u, 0x22220000u, 0x20u, 4,
                                 ram.b + 0x20, 0x11223344u);
    /* The mirror VA has the same normalized RAM identity as 0x20. */
    recomp_mem_watch_guest_store(0x33333333u, 0x33330000u, 0x120u, 4,
                                 ram.b + 0x20, 0x55667788u);
    /* The optional physical-heap window is also an alias, rather than the
       otherwise independent contiguous aperture. */
    recomp_mem_watch_guest_store(0x55555555u, 0x55550000u, 0x80000020u, 4,
                                 ram.b + 0x20, 0x01020304u);
    /* A narrower access overlapping the watched dword reports width=2. */
    recomp_mem_watch_guest_store(0x44444444u, 0x44440000u, 0x22u, 2,
                                 ram.b + 0x22, 0x99AAu);
    recomp_mem_watch_shutdown();
    return 0;
}
"""


def test_runtime_filter_same_value_width_pc_and_ram_alias():
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc:
        return

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    source = os.path.join(root, "src", "kernel", "recomp_mem_watch.c")
    include = os.path.join(root, "src", "kernel")
    with tempfile.TemporaryDirectory() as tmp:
        harness = os.path.join(tmp, "mem_watch_test.c")
        executable = os.path.join(tmp, "mem_watch_test")
        with open(harness, "w", encoding="utf-8") as out:
            out.write(C_RUNTIME_TEST)
        built = subprocess.run(
            [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", include,
             harness, source, "-o", executable],
            capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr

        env = dict(os.environ)
        env["RECOMP_MEM_WATCH"] = "0x20:4"
        ran = subprocess.run([executable], env=env, capture_output=True, text=True)
        assert ran.returncode == 0, ran.stdout + ran.stderr

    records = [line for line in ran.stderr.splitlines()
               if "source=guest pc=" in line]
    assert len(records) == 4, ran.stderr
    assert all("pc=0x11111111" not in line for line in records)
    assert "pc=0x22222222" in records[0]
    assert "function=0x22220000" in records[0]
    assert "width=4 old=0x11223344 new=0x11223344" in records[0]
    assert "pc=0x33333333" in records[1]
    assert "va=0x00000120 ram=0x00000020" in records[1]
    assert "pc=0x55555555" in records[2]
    assert "va=0x80000020 ram=0x00000020" in records[2]
    assert "pc=0x44444444" in records[3]
    assert "width=2" in records[3]
