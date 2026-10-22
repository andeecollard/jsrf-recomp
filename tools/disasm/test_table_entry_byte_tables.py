"""A byte index table is never a table entry.

MSVC's two-level switch parks a byte index table, `movzx eax, byte ptr
[eax + table]`, right after the dword table it feeds. Its bytes are small arm
numbers and they decode as a run of `add`, which can reach a proven exit:
Jet Set Radio Future's 0xEC090 (`00 04 04 04 ...`) wandered into an indirect
tail and became an entry. The word naming it is the movzx's own displacement,
scanned as a table word while the function holding it is still a gap.
"""
import struct

from tools.disasm.test_table_entry_shapes import BASE, _HEAD, _detect, _pad


def _movzx(table):
    # movzx eax, byte ptr [eax + table] ; ret -- the switch that reads it
    return b"\x0f\xb6\x80" + struct.pack("<I", table) + b"\xc3"


def test_a_byte_table_named_by_a_movzx_is_refused():
    # BASE+16: the reader. BASE+32: the byte table, 0 2 2 0, which decodes
    # as adds and then a `jmp dword ptr [eax+8]`: a closed body with a
    # memory tail, if the bytes were code.
    table = BASE + 32
    text = (_HEAD + _pad(_movzx(table), 16)
            + _pad(b"\x00\x02\x02\x00" b"\xff\x60\x08", 16))
    det, engine = _detect(text, [table])
    assert table in engine.byte_tables
    assert table not in det._alias_entries


def test_an_opcode_below_0f_is_refused_without_a_movzx():
    # The same bytes, nothing naming them: `add` never starts a function.
    text = _HEAD + _pad(b"\x00\x02\x02\x00" b"\xff\x60\x08", 16)
    det, engine = _detect(text, [BASE + 16])
    assert BASE + 16 not in engine.byte_tables
    assert BASE + 16 not in det._alias_entries


def test_a_proven_escape_into_a_byte_table_refuses_the_method():
    # A method whose error path jumps to a byte table is not closed.
    from tools.disasm.test_table_entry_shapes import _escaping
    text = _escaping(b"\x00\x02\x02\x00" b"\xff\x60\x08")
    det, _ = _detect(text, [BASE + 16], functions=(BASE + 32,))
    assert BASE + 16 not in det._alias_entries
