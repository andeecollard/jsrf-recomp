"""The gap-testing passes, asked again once aliases have real extents.

Three things they got wrong on JSRF the first time detect() re-ran them
(22 Sep 2026), each pinned here.
"""
import struct
from types import SimpleNamespace

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.xrefs import XRefTracker
from tools.disasm.test_imm_ref_targets import BASE, Image as _Image


class Image(_Image):
    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def _detector(data):
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.resync_jump_tables()
    return FunctionDetector(engine, image, XRefTracker(), LabelManager()), image


def test_imm_ref_coverage_survives_an_overlapping_alias():
    # BASE+0: mov eax, TARGET ; ret ; nops ; TARGET: test ecx, ecx ; ret
    target = BASE + 0x10
    data = b"\xb8" + struct.pack("<I", target) + b"\xc3" + b"\x90" * 10
    data += bytes.fromhex("85c9c3") + b"\xcc" * 8
    det, image = _detector(data)
    det.engine.instructions[BASE].imm_ref = target
    # One real function covering the target, plus an alias INSIDE it whose
    # own flow ends before the target. Sorted by start the alias is the
    # nearest range below the target and does not reach it; the function
    # above it does.
    f = SimpleNamespace(start=BASE, end=BASE + 0x20)
    a = SimpleNamespace(start=BASE + 0x8, end=BASE + 0xC)
    det.functions = {f.start: f, a.start: a}
    assert not det._pass_imm_ref_targets([image.section])
    assert target not in det._candidates


def test_gap_prologue_does_not_start_on_a_tables_padding():
    # BASE+0: jmp [eax*4 + TABLE]   (registers the table)
    # BASE+7: arms: three `ret`s at +7, +8, +9
    # BASE+10: ret          <- the function's last ret
    # BASE+11: mov edi, edi <- hot-patch padding, probes as a prologue
    # BASE+13: TABLE: three entries
    table = BASE + 13
    data = b"\xff\x24\x85" + struct.pack("<I", table)
    data += b"\xc3\xc3\xc3" + b"\xc3" + b"\x8b\xff"
    data += struct.pack("<III", BASE + 7, BASE + 8, BASE + 9)
    data += b"\xcc" * 16
    det, image = _detector(data)
    assert table in det.engine.jump_tables
    det.functions = {}
    det._pass_gap_prologues([image.section])
    assert BASE + 11 not in det._candidates


class TwoSectionImage:
    """.text at BASE, .rdata right after it."""
    base_address = BASE
    entry_point = BASE

    def __init__(self, text, rdata):
        self.data = text + rdata
        self.image_size = len(self.data)
        self.text = SimpleNamespace(name=".text", virtual_addr=BASE,
                                    virtual_size=len(text), executable=True)
        self.rdata = SimpleNamespace(name=".rdata",
                                     virtual_addr=BASE + len(text),
                                     virtual_size=len(rdata), executable=True)

    def get_section_at_va(self, addr):
        for s in (self.text, self.rdata):
            if s.virtual_addr <= addr < s.virtual_addr + s.virtual_size:
                return s
        return None

    def get_section_data(self, section):
        off = section.virtual_addr - BASE
        return self.data[off:off + section.virtual_size]

    def read_bytes_at_va(self, addr, size):
        return self.data[addr - BASE:addr - BASE + size]

    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def test_a_table_in_rdata_is_measured_against_the_dispatch_section():
    # .text: jmp [eax*4 + TABLE] ; three one-byte arms ; padding
    text = b"\xff\x24\x85" + b"\0\0\0\0" + b"\xc3\xc3\xc3" + b"\xcc" * 6
    table = BASE + len(text)
    text = text[:3] + struct.pack("<I", table) + text[7:]
    rdata = struct.pack("<III", BASE + 7, BASE + 8, BASE + 9) + b"\0" * 8
    image = TwoSectionImage(text, rdata)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.text)
    engine.resync_jump_tables()
    assert engine.jump_tables.get(table) == table + 12
    assert engine.jump_table_entries(table) == [BASE + 7, BASE + 8, BASE + 9]
    # Nothing was decoded over .rdata: no resync, no realignment there.
    assert engine.get_instruction(table) is None
    assert engine.get_instruction(table + 12) is None


def test_a_pointer_array_in_rdata_with_no_dispatch_is_not_a_table():
    text = b"\xc3" + b"\xcc" * 15
    table = BASE + len(text)
    rdata = struct.pack("<III", BASE, BASE, BASE)
    image = TwoSectionImage(text, rdata)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.text)
    engine._jt_candidates.add(table)          # named, but no site dispatches it
    engine.resync_jump_tables()
    assert table not in engine.jump_tables
