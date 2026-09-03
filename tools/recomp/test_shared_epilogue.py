"""Run with python -m unittest tools.recomp.test_shared_epilogue."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools.recomp.config import Section
from tools.recomp.shared_epilogue import recover_shared_epilogue
from tools.recomp.translator import BatchTranslator, FunctionTranslator


class SharedEpilogueTest(unittest.TestCase):
    def setUp(self):
        self.layout = patch("tools.recomp.config._SECTIONS", [
            Section(".text", 0x12000, 0x1000, 0, 0x1000, True)])
        self.layout.start()
        self.addCleanup(self.layout.stop)

    def recover(self, hex_bytes, name="shared", offset=1):
        raw = b"\x90" + bytes.fromhex(hex_bytes)
        translator = FunctionTranslator(raw, {0x12000: {"end": 0x12000 + len(raw)}})
        return recover_shared_epilogue(translator, 0x12000 + offset, name)

    def test_rejects_branches_calls_missing_returns_and_bad_boundaries(self):
        for sequence in ("74015ec3", "e8000000005ec3", "eb005ec3",
                         "5e90", "c3", "665dc3", "66c9c3", "5ecb",
                         "65890d000000005ec3", "64890d040000005ec3"):
            with self.subTest(sequence=sequence):
                self.assertIsNone(self.recover(sequence))
        # 5e inside a MOV immediate is not a valid entry in the owner's decode.
        self.assertIsNone(self.recover("b85e5ec300c3", offset=2))
        self.assertIsNone(self.recover("5e" * 16 + "c3"))

    def test_compiled_recovery_restores_real_caller_stack(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("C compiler required")
        # First two byte sequences are the actual JSRF shared return blocks.
        bodies = [self.recover("5e33c05dc3", "loader_return"),
                  self.recover("897e545f5b5ec3", "cache_return"),
                  self.recover("5f5e83c410c20800", "locals_return"),
                  self.recover("c9c20400", "frame_return"),
                  self.recover("33c08b8c241c0100005f5e5d5b64890d0000000081c418010000c3", "seh_return")]
        self.assertTrue(all(bodies))
        source = r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <assert.h>
RECOMP_TLS uint32_t g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_ebp, g_seh_ebp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
static uint32_t memory[1024];
''' + "\n".join(bodies) + r'''
int main(void) {
    g_xbox_mem_offset = (ptrdiff_t)memory;
    /* The caller pushes a return address; the split callee saves EBP/ESI. */
    esp = 0x400; esi = 0xabcdef01; g_seh_ebp = 0x12345678;
    PUSH32(esp, 0x11111111); PUSH32(esp, g_seh_ebp); PUSH32(esp, esi);
    esi = 0x800; eax = 99; loader_return();
    assert(esp == 0x400 && esi == 0xabcdef01 && eax == 0 && g_seh_ebp == 0x12345678);
    esp = 0x400; esi = 0x123456; edi = 0xabcdef; ebx = 0x987654;
    PUSH32(esp, 0x22222222); PUSH32(esp, esi); PUSH32(esp, ebx); PUSH32(esp, edi);
    esi = 0x800; edi = 7; cache_return();
    assert(MEM32(0x854) == 7 && esp == 0x400 && esi == 0x123456 && edi == 0xabcdef && ebx == 0x987654);
    esp = 0x400; PUSH32(esp, 2); PUSH32(esp, 1); PUSH32(esp, 0x33333333);
    esp -= 16; PUSH32(esp, 0x55); PUSH32(esp, 0x66); locals_return();
    assert(esp == 0x400 && esi == 0x55 && edi == 0x66);
    esp = 0x400; PUSH32(esp, 1); PUSH32(esp, 0x44444444); PUSH32(esp, 0x1234);
    g_seh_ebp = esp; esp -= 32; frame_return();
    assert(esp == 0x400 && g_seh_ebp == 0x1234);
    /* JSRF's inline exception frame, including the previous FS:[0]. */
    g_fs_base = 0xc00; FS_MEM32(0) = 0x123456;
    esp = 0x800; PUSH32(esp, 0x55555555); PUSH32(esp, 0xffffffff);
    PUSH32(esp, 0x186fae); PUSH32(esp, FS_MEM32(0)); FS_MEM32(0) = esp;
    esp -= 0x10c; PUSH32(esp, 0x11); PUSH32(esp, 0x22);
    PUSH32(esp, 0x33); PUSH32(esp, 0x44); seh_return();
    assert(esp == 0x800 && FS_MEM32(0) == 0x123456 && eax == 0);
    assert(ebx == 0x11 && g_seh_ebp == 0x22 && esi == 0x33 && edi == 0x44);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "check.c"
            src.write_text(source)
            exe = Path(tmp) / "check"
            include = Path(__file__).resolve().parents[2] / "templates/runtime"
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-I", str(include), str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_split_generation_emits_body_header_and_dispatch(self):
        # A conditional tail crosses a detected function boundary and lands
        # inside the next function's shared return block.
        raw = bytes.fromhex("5685c0741c5ec3") + b"\x90" * 25 + bytes.fromhex("905ec3")
        funcs = {0x12000: {"name": "caller", "end": 0x12007},
                 0x12020: {"name": "owner", "end": 0x12023}}
        batch = BatchTranslator.__new__(BatchTranslator)
        batch.translator = FunctionTranslator(raw, funcs)
        with tempfile.TemporaryDirectory() as tmp:
            stats = batch.translate_batch_split(list(funcs.items()), tmp)
            self.assertEqual(stats["recovered_epilogues"], [0x12021])
            self.assertEqual(stats["unresolved_stubs"], 0)
            for filename in ("recomp_funcs.h", "recomp_0000.c", "recomp_dispatch.c"):
                self.assertIn("sub_00012021", (Path(tmp) / filename).read_text())


if __name__ == "__main__":
    unittest.main()
