"""A measured switch table places every arm, inside the function or not.

The lifter reads a table straight from the XBE and stops at the first entry
outside the function, so a table whose cases alternate between local arms and
calls to other functions collapsed to one arm and was thrown away -- JSRF's
0x001FA008 (21 entries, in .rdata) resolved to nothing once its arms stopped
being seeded as function starts. With the disassembler's measurement of the
table, an arm inside the function is a goto, an arm that is another function
is the tail jump a direct `jmp sub_X` becomes, and the runtime fallback keeps
the rest.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
TABLE = BASE + 0x40
ARM0, ARM1, OTHER = BASE + 7, BASE + 8, BASE + 0x20


def _image():
    # BASE+0:    jmp dword ptr [eax*4 + TABLE]
    # BASE+7:    ret                      (arm 0, inside)
    # BASE+8:    ret                      (arm 1, inside)
    # BASE+9..:  int3 padding to BASE+0x20
    # BASE+0x20: ret                      (a separate function)
    # BASE+0x40: TABLE: ARM0, OTHER, ARM1
    img = b"\xff\x24\x85" + struct.pack("<I", TABLE) + b"\xc3\xc3"
    img += b"\xcc" * (0x20 - len(img)) + b"\xc3"
    img += b"\xcc" * (0x40 - len(img))
    img += struct.pack("<III", ARM0, OTHER, ARM1) + b"\x00" * 16
    config._install(
        [config.Section(".text", BASE, len(img), 0x0000, len(img), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="measured-table-test")
    return img


def _db():
    return {
        BASE: {"start": f"0x{BASE:08X}", "end": BASE + 0x10, "_addr": BASE,
               "size": 0x10, "name": f"sub_{BASE:08X}"},
        OTHER: {"start": f"0x{OTHER:08X}", "end": OTHER + 1, "_addr": OTHER,
                "size": 1, "name": f"sub_{OTHER:08X}"},
    }


def test_measured_table_places_local_arms_and_tail_calls_the_other():
    img, db = _image(), _db()
    tr = FunctionTranslator(img, db, jump_tables={TABLE: [ARM0, OTHER, ARM1]})
    c = tr.translate_function(BASE, db[BASE])
    assert f"goto loc_{ARM0:08X};" in c, c
    assert f"goto loc_{ARM1:08X};" in c, c
    assert f"if (_jt == 0x{OTHER:08X}u) {{ g_seh_ebp = ebp; sub_{OTHER:08X}(); return;" in c, c
    assert "RECOMP_ITAIL(_jt)" in c, c


def test_without_the_measurement_the_read_stops_at_the_other_function():
    img, db = _image(), _db()
    c = FunctionTranslator(img, db).translate_function(BASE, db[BASE])
    # One arm inside before the read leaves the function: not a switch, so
    # the whole dispatch is the runtime fallback, exactly as before.
    assert "_jt" not in c, c
    assert "RECOMP_ITAIL(MEM32(eax * 4 + 0x10040))" in c, c


def test_a_measured_table_with_one_placeable_arm_is_not_a_switch():
    img, db = _image(), _db()
    stray = BASE + 0x300  # neither inside nor a known function
    tr = FunctionTranslator(img, db, jump_tables={TABLE: [ARM0, stray, stray]})
    c = tr.translate_function(BASE, db[BASE])
    assert "_jt" not in c, c
