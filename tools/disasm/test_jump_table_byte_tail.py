"""A measured jump table stops in front of the byte index table after it.

MSVC's two-level switch packs the byte index table straight after the dword
table. Its first four bytes are small arm numbers, and read as a dword they
are an address low in the section: `00 02 02 00` is 0x20200, inside a .text
that starts at 0x11000. resync_jump_tables took it as one more arm, so a
dispatcher `mov eax, [ecx+N] ; jmp [eax*4+table]` whose real arms are other
functions had a fourth arm nobody could prove, and the method was refused.
"""
import struct

from tools.disasm.test_table_entry_shapes import BASE, _HEAD, _detect, _pad


def _two_level(byte_table_named):
    # BASE+16: the dispatcher
    #   movzx eax, byte ptr [eax + BYTES]   (only when byte_table_named)
    #   jmp dword ptr [eax*4 + TABLE]
    # BASE+32: TABLE, three arms, all function starts
    # BASE+44: BYTES, `00 00 01 00`: as a dword, BASE itself
    f1, f2, f3 = BASE + 48, BASE + 64, BASE + 80
    table, bytes_ = BASE + 32, BASE + 44
    code = b""
    if byte_table_named:
        code += b"\x0f\xb6\x80" + struct.pack("<I", bytes_)
    code += b"\xff\x24\x85" + struct.pack("<I", table)
    text = (_HEAD + _pad(code, 16)
            + struct.pack("<III", f1, f2, f3) + b"\x00\x00\x01\x00"
            + _pad(b"\x31\xc0\xc3", 16)
            + _pad(b"\xb8\x01\x00\x00\x00\xc3", 16)
            + _pad(b"\xb8\x02\x00\x00\x00\xc3", 16))
    return text, table, (f1, f2, f3)


def test_the_table_ends_where_the_byte_table_begins():
    text, table, functions = _two_level(byte_table_named=True)
    det, engine = _detect(text, [BASE + 16], functions=functions)
    assert engine.jump_tables.get(table) == table + 12
    assert engine.jump_table_entries(table) == list(functions)
    assert BASE + 16 in det._alias_entries


def test_an_unnamed_fourth_word_still_counts():
    # Nothing says the fourth word is a byte table, and it points into the
    # section: the old measurement stands, and the entry it adds is BASE,
    # a function start, so the dispatcher is still proven.
    text, table, functions = _two_level(byte_table_named=False)
    det, engine = _detect(text, [BASE + 16], functions=functions)
    assert engine.jump_tables.get(table) == table + 16
    assert engine.jump_table_entries(table) == [*functions, BASE]
