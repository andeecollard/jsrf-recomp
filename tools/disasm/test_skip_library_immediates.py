"""--skip-library-immediates: a library section's immediate never starts a .text function."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo

BASE = 0x10000
TEXT = 0x11000
LIB = 0x12000
STUB = bytes.fromhex('558becc3')     # push ebp; mov ebp,esp; ret

# Two .text bodies no function claims: one taken by .text code, one taken only
# by library code (an NV2A register constant that equals a .text address).
TEXT_BODY = TEXT + 0x20
LIB_BODY = TEXT + 0x30
LIB_OWN_BODY = LIB + 0x10            # taken by library code, inside the library


def _mov_eax(addr):
    return b'\xb8' + struct.pack('<I', addr) + b'\xc3'


def _detector(skip):
    text_code = (_mov_eax(TEXT_BODY).ljust(0x20, b'\x90') + STUB.ljust(0x10, b'\x90')
                 + STUB).ljust(0x40, b'\xcc')
    lib_code = (_mov_eax(LIB_BODY) + _mov_eax(LIB_OWN_BODY)).ljust(0x10, b'\x90') + STUB
    text = SectionInfo('.text', TEXT, len(text_code), 0, len(text_code),
                       False, True, '')
    lib = SectionInfo('D3D', LIB, len(lib_code), 0x100, len(lib_code),
                      True, True, '')
    raw = text_code.ljust(0x100, b'\xcc') + lib_code
    image = BinaryImage('synthetic', raw, BASE, 0x20000, TEXT, 0, [text, lib])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    engine.linear_sweep(lib)
    det = FunctionDetector(engine, image, None, LabelManager(),
                           skip_library_immediates=skip)
    det._pass_known_addresses()
    det._build_functions([text, lib])
    det._pass_imm_ref_targets([text, lib])
    return det


def test_default_takes_a_library_immediate_as_a_function():
    # Unchanged behaviour: this is the cut the option exists to avoid.
    det = _detector(skip=False)
    assert LIB_BODY in det._candidates
    assert TEXT_BODY in det._candidates
    assert LIB_OWN_BODY in det._candidates


def test_option_ignores_a_library_immediate_that_names_text():
    det = _detector(skip=True)
    assert LIB_BODY not in det._candidates


def test_option_keeps_text_to_text_references():
    det = _detector(skip=True)
    assert TEXT_BODY in det._candidates


def test_option_keeps_library_to_library_references():
    det = _detector(skip=True)
    assert LIB_OWN_BODY in det._candidates
