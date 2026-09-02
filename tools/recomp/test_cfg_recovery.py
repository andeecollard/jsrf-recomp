"""Regression checks for local computed-jump CFG recovery."""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402


BASE = 0x00010000


def _setup_image(size=0x80):
    image = bytearray(b"\xCC" * size)
    config._install(
        [config.Section(".text", BASE, size, 0, size, True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="cfg-recovery-test")
    return image


def test_biased_table_origin():
    image = _setup_image()
    origin = BASE + 0x20
    targets = [BASE + 0x40, BASE + 0x48, BASE + 0x50]
    # Slot zero is deliberately not a code pointer.  The table is addressed
    # with an index already proven to be in 1..3.
    for index, target in enumerate(targets, 1):
        struct.pack_into("<I", image, origin - BASE + index * 4, target)

    translator = FunctionTranslator(bytes(image), {})
    assert translator._read_local_jump_table(
        origin, BASE, BASE + len(image)) == targets
    print("ok  biased_table_origin")


def test_table_only_edge_extends_truncated_function():
    image = _setup_image()
    origin = BASE + 0x20
    target_a = BASE + 0x30
    target_b = BASE + 0x38

    # push ebp; mov ebp,esp; xor ecx,ecx;
    # jmp dword ptr [ecx*4 + origin]
    prefix = bytes.fromhex("55 89E5 31C9 FF248D") + struct.pack("<I", origin)
    image[:len(prefix)] = prefix
    original_end = BASE + len(prefix)
    struct.pack_into("<II", image, origin - BASE, target_a, target_b)
    image[target_a - BASE] = 0xC3
    image[target_b - BASE] = 0xC3

    next_function = BASE + 0x60
    image[next_function - BASE] = 0xC3
    db = {
        BASE: {"start": f"0x{BASE:08X}", "end": original_end,
               "_addr": BASE, "section": ".text", "has_prologue": True,
               "called_by": []},
        next_function: {"start": f"0x{next_function:08X}",
                        "end": next_function + 1,
                        "_addr": next_function, "section": ".text",
                        "has_prologue": False, "called_by": [BASE]},
    }

    translator = FunctionTranslator(bytes(image), db)
    translator.discover_cfg_ownership()
    recovered = translator._recovered_cfg[BASE]
    addresses = {insn.address for insn in recovered["instructions"]}
    assert recovered["end"] == target_b + 1
    assert target_a in addresses and target_b in addresses
    assert recovered["jump_tables"][origin] == [target_a, target_b]
    # The table bytes between the prefix and the first target are data: no
    # instruction may be decoded inside that region.
    assert not any(origin <= addr < target_a for addr in addresses)
    # The targets are internal blocks, not functions of their own.
    assert target_a not in translator.func_db
    assert target_b not in translator.func_db
    assert target_a not in translator.owned_function_starts
    assert target_b not in translator.owned_function_starts
    print("ok  table_only_edge_extends_truncated_function")


if __name__ == "__main__":
    test_biased_table_origin()
    test_table_only_edge_extends_truncated_function()
    print("\nall passed")
