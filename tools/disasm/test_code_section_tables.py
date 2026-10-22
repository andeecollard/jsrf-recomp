"""Pointer tables that live inside a code section, between functions.

XDK libraries link their data into their own executable section. Fallout:
Brotherhood of Steel's D3D keeps a 30-entry render-state handler table at
0x000B39F0, between two D3D functions, and three of the handlers are reached
only through it. _pass_data_ptr_targets used to scan data sections only.
"""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo

BASE = 0x10000
HANDLER = b"\x8b\x44\x24\x04\xc2\x04\x00"   # mov eax, [esp+4] ; ret 4


def _detect(text_bytes):
    text = SectionInfo('D3D', BASE, len(text_bytes), 0, len(text_bytes),
                       False, True, '')
    image = BinaryImage('synthetic', text_bytes, 0, 0x20000, BASE, 0, [text])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    detector._pass_data_ptr_targets([text])
    return detector


def test_table_in_a_code_section_gap_names_a_function():
    # +0  ret, padded        (the entry point: a known function)
    # +16 table: one pointer to +32
    # +20 padding
    # +32 a handler nothing calls
    body = b"\xc3" + b"\xcc" * 15
    body += struct.pack("<I", BASE + 32) + b"\xcc" * 12
    body += HANDLER
    detector = _detect(body)
    assert BASE + 32 in detector._alias_entries


def test_word_inside_a_function_body_is_not_a_table():
    # +0  mov eax, <BASE+16> ; ret    -- the immediate looks like a pointer
    # +16 the same handler shape
    body = b"\xb8" + struct.pack("<I", BASE + 16) + b"\xc3" + b"\xcc" * 10
    body += HANDLER
    detector = _detect(body)
    assert BASE + 16 not in detector._alias_entries


def test_switch_table_in_a_gap_is_not_a_table_of_functions():
    # Same layout as the first test, but the engine knows +16 is a switch's
    # jump table: its words are case labels, not callables.
    body = b"\xc3" + b"\xcc" * 15
    body += struct.pack("<I", BASE + 32) + b"\xcc" * 12
    body += HANDLER
    text = SectionInfo('D3D', BASE, len(body), 0, len(body), False, True, '')
    image = BinaryImage('synthetic', body, 0, 0x20000, BASE, 0, [text])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    engine.jump_tables[BASE + 16] = BASE + 20
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    detector._pass_data_ptr_targets([text])
    assert BASE + 32 not in detector._alias_entries
