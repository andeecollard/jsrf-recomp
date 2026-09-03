"""Immediate discovery must not split an instruction into a false function."""

import struct
from types import SimpleNamespace

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.xrefs import XRefTracker


BASE = 0x10000


class Image:
    base_address = BASE
    entry_point = BASE

    def __init__(self, data):
        self.data = data
        self.image_size = len(data)
        self.section = SimpleNamespace(name=".text", virtual_addr=BASE,
                                       virtual_size=len(data), executable=True)

    def get_section_at_va(self, addr):
        return self.section if BASE <= addr < BASE + len(self.data) else None

    def get_section_data(self, section):
        return self.data

    def read_bytes_at_va(self, addr, size):
        return self.data[addr - BASE:addr - BASE + size]


def detector(target):
    data = b"\xb8" + struct.pack("<I", target) + b"\xc3" + b"\x90" * 10
    data += bytes.fromhex("85c9c3")  # test ecx, ecx; ret
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.instructions[BASE].imm_ref = target
    return FunctionDetector(engine, image, XRefTracker(), LabelManager())


def test_immediate_inside_test_does_not_become_leave_function():
    target = BASE + 0x11
    det = detector(target)
    assert det.engine.probes_as_returning_body(target)  # misleading leave; ret
    assert not det._pass_imm_ref_targets([det.image.section])
    assert target not in det._candidates
    assert target not in det.engine.instructions


def test_boundary_without_prologue_remains_a_valid_immediate_target():
    target = BASE + 0x10
    det = detector(target)
    assert det._pass_imm_ref_targets([det.image.section])
    assert det._candidates[target][1] == "imm_ref_target"
