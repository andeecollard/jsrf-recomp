"""16-bit push and pop move esp by 2.

Both lifted as 32-bit: `pop cx` emitted POP32(esp, cx), which does not
compile (found on Panzer Dragoon Orta), and `push cx` pushed 4 bytes, which
compiles and leaves the stack 2 bytes off. The runtime half lifts a sequence
against the header's own macros and runs it; it skips without a C compiler.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Disassembler
from .lifter import Lifter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNTIME = os.path.join(ROOT, "templates", "runtime")

# push cx; push word [esi]; pop dx; pop word [edi]; push ecx; pop eax
CODE = bytes.fromhex("6651" "66ff36" "665a" "668f07" "51" "58")


def _body(code=CODE):
    insns = Disassembler().disassemble_function(code, 0x1000, 0x1000 + len(code))
    lifter = Lifter()
    return "\n".join(s for i in insns for s in lifter.lift_instruction(i))


class TextTest(unittest.TestCase):
    def test_no_pop32_into_a_word_register(self):
        body = _body()
        self.assertNotIn("POP32(esp, cx)", body)
        self.assertNotIn("POP32(esp, dx)", body)
        self.assertIn("esp -= 2;", body)
        self.assertIn("esp += 2;", body)


HARNESS = r"""
#include "recomp_types.h"
#include <stdio.h>
static uint8_t g_mem[0x10000];
ptrdiff_t g_xbox_mem_offset;
static uint32_t eax, ecx, edx, esi, edi, esp;
int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_mem;
    esp = 0x8000; ecx = 0xAAAA1234u; edx = 0xBBBBFFFFu; eax = 0;
    esi = 0x100; edi = 0x200;
    MEM32(0x100) = 0xCCCC5678u; MEM32(0x200) = 0xDDDDDDDDu;
%s
    printf("%%04X %%08X %%08X %%08X\n", esp, edx, MEM32(0x200), eax);
    return 0;
}
"""


def _cc():
    return shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")


@unittest.skipUnless(_cc(), "no C compiler")
class RuntimeTest(unittest.TestCase):
    def test_stack_and_values(self):
        with tempfile.TemporaryDirectory() as d:
            c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
            open(c, "w").write(HARNESS % _body())
            subprocess.run([_cc(), "-std=c11", "-O1", "-I", RUNTIME,
                            "-w", "-o", exe, c], check=True)
            out = subprocess.run([exe], check=True, capture_output=True,
                                 text=True).stdout.split()
        # esp back at 0x8000; dx took the second word pushed (0x5678) and
        # kept its high half; [edi] took 0x1234 into its low word only; the
        # 32-bit pair is untouched by the word traffic around it.
        self.assertEqual(out, ["8000", "BBBB5678", "DDDD1234", "AAAA1234"])


class EspSourceRuntimeTest(unittest.TestCase):
    @unittest.skipUnless(_cc(), "no C compiler")
    def test_word_push_samples_esp_before_decrementing(self):
        # PUSH SP and PUSH word [ESP] read from the old stack pointer.
        for code, low in (("6654665a", "8000"), ("66ff3424665a", "4321")):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as d:
                src, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
                body = "MEM32(esp) = 0xCAFE4321u;\n" + _body(bytes.fromhex(code))
                Path(src).write_text(HARNESS % body)
                subprocess.run([_cc(), "-std=c11", "-O1", "-I", RUNTIME,
                                "-w", "-o", exe, src], check=True)
                out = subprocess.run([exe], check=True, capture_output=True,
                                     text=True).stdout.split()
                self.assertEqual(out[:2], ["8000", "BBBB" + low])
