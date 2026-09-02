import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import BasicBlock, Disassembler, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base, size=4):
    return Operand(type="mem", mem_base=base, mem_size=size)


def _insn(address, mnemonic, operands, op_str=""):
    insn = Instruction(address, 4, mnemonic, op_str, "")
    insn.operands = operands
    return insn


class XaddLifterTest(unittest.TestCase):
    def test_capstone_representation_matches_jsrf_lock_xadd(self):
        decoder = Disassembler()
        samples = (
            (0x00177FEE, bytes.fromhex("f00fc101"),
             "dword ptr [ecx], eax"),
            (0x001664DA, bytes.fromhex("f00fc110"),
             "dword ptr [eax], edx"),
        )
        for address, raw, op_str in samples:
            with self.subTest(address=f"0x{address:08X}"):
                decoded = list(decoder._cs.disasm(raw, address))
                self.assertEqual(1, len(decoded))
                self.assertEqual("lock xadd", decoded[0].mnemonic)
                self.assertEqual(op_str, decoded[0].op_str)

    def test_locked_memory_xadd_is_atomic_and_returns_old_destination(self):
        insn = _insn(
            0,
            "lock xadd",
            [_mem("ecx"), _reg("eax")],
            "dword ptr [ecx], eax",
        )

        generated = "\n".join(Lifter().lift_instruction(insn))

        self.assertNotIn("TODO", generated)
        self.assertIn("RECOMP_ATOMIC_XADD32", generated)
        self.assertIn("_xadd_addr", generated)
        self.assertIn("_xadd_src", generated)
        self.assertIn("eax = _xadd_old", generated)
        self.assertIn("RECOMP_ADD_FLAGS32", generated)

    def test_unlocked_memory_xadd_updates_memory_and_source(self):
        insn = _insn(
            0,
            "xadd",
            [_mem("edx"), _reg("ecx")],
            "dword ptr [edx], ecx",
        )

        generated = "\n".join(Lifter().lift_instruction(insn))

        self.assertNotIn("RECOMP_ATOMIC", generated)
        self.assertIn("MEM32(_xadd_addr) = _xadd_result", generated)
        self.assertIn("ecx = _xadd_old", generated)

    def test_register_xadd_implements_exchange_and_add(self):
        insn = _insn(0, "xadd", [_reg("eax"), _reg("ecx")], "eax, ecx")

        generated = "\n".join(Lifter().lift_instruction(insn))

        self.assertIn("_xadd_old = (uint32_t)(eax)", generated)
        self.assertIn("eax = _xadd_result", generated)
        self.assertIn("ecx = _xadd_old", generated)

    def test_same_register_xadd_leaves_the_sum(self):
        insn = _insn(0, "xadd", [_reg("eax"), _reg("eax")], "eax, eax")

        generated = "\n".join(Lifter().lift_instruction(insn))

        self.assertLess(
            generated.rfind("eax = _xadd_old;"),
            generated.rfind("eax = _xadd_result;"),
        )

    def test_all_architectural_operand_widths_are_supported(self):
        cases = ((1, "al", "8"), (2, "ax", "16"), (4, "eax", "32"))
        for size, source, suffix in cases:
            with self.subTest(size=size):
                insn = _insn(
                    0,
                    "lock xadd",
                    [_mem("ecx", size), _reg(source)],
                )
                generated = "\n".join(Lifter().lift_instruction(insn))
                self.assertIn(f"RECOMP_ATOMIC_XADD{suffix}", generated)
                self.assertIn(f"RECOMP_ADD_FLAGS{suffix}", generated)

    def test_je_and_jne_consume_xadd_zero_flag(self):
        xadd = _insn(
            0,
            "lock xadd",
            [_mem("eax"), _reg("edx")],
            "dword ptr [eax], edx",
        )
        je = Instruction(4, 2, "je", "0x10", "")
        je.jump_target = 0x10
        jne = Instruction(6, 2, "jne", "0x14", "")
        jne.jump_target = 0x14

        lifted, state = lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[xadd, je, jne]))
        generated = "\n".join(lifted)

        self.assertIn("RECOMP_EFLAGS_ZF(_flags)", generated)
        self.assertIn("!RECOMP_EFLAGS_ZF(_flags)", generated)
        self.assertEqual("lock xadd", state[0])

    def test_runtime_atomic_helpers_and_add_flags(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler available")
        root = Path(__file__).resolve().parents[2]
        source = r'''
#include <stdint.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

int main(void) {
    union {
        uint32_t dword;
        uint16_t word[2];
        uint8_t byte[4];
    } cell;
    uint32_t flags;

    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)&cell;

    cell.dword = 1;
    if (RECOMP_ATOMIC_XADD32(0, 1) != 1 || cell.dword != 2) return __LINE__;
    if (RECOMP_ATOMIC_XADD32(0, UINT32_MAX) != 2 || cell.dword != 1)
        return __LINE__;

    cell.word[0] = 0xFFFFu;
    if (RECOMP_ATOMIC_XADD16(0, 1) != 0xFFFFu || cell.word[0] != 0)
        return __LINE__;
    cell.byte[0] = 0x7Fu;
    if (RECOMP_ATOMIC_XADD8(0, 1) != 0x7Fu || cell.byte[0] != 0x80u)
        return __LINE__;

    flags = RECOMP_ADD_FLAGS32(UINT32_MAX, 1, 0);
    if (!RECOMP_EFLAGS_CF(flags) || !RECOMP_EFLAGS_ZF(flags) ||
        !RECOMP_EFLAGS_PF(flags) || !RECOMP_EFLAGS_AF(flags) ||
        RECOMP_EFLAGS_SF(flags) || RECOMP_EFLAGS_OF(flags)) return __LINE__;

    flags = RECOMP_ADD_FLAGS32(0x7FFFFFFFu, 1, 0x80000000u);
    if (RECOMP_EFLAGS_CF(flags) || RECOMP_EFLAGS_ZF(flags) ||
        !RECOMP_EFLAGS_SF(flags) || !RECOMP_EFLAGS_OF(flags)) return __LINE__;

    flags = RECOMP_ADD_FLAGS8(0x7Fu, 1, 0x80u);
    if (!RECOMP_EFLAGS_SF(flags) || !RECOMP_EFLAGS_OF(flags)) return __LINE__;
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="xadd-runtime-") as tmp:
            tmp = Path(tmp)
            src = tmp / "test.c"
            exe = tmp / "test"
            src.write_text(source, encoding="utf-8")
            built = subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(root / "templates" / "runtime"),
                 str(src), "-o", str(exe)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(0, built.returncode, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(0, ran.returncode, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
