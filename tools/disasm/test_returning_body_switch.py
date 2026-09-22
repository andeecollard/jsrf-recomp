"""A body that opens with a switch dispatch still reaches its ret.

probes_as_returning_body ended the probe at any unconditional jmp that was
not an immediate forward branch, which made every `jmp [reg*4 + table]` look
like a tail call. JSRF's 0x000D1440 and 0x000D4470 are reached only as
immediates, have no prologue, and open with exactly that dispatch, so the
imm-ref pass refused them and the 21 switch tables they own were translated
nowhere. A dispatch through a table the engine has measured continues in its
arms; the probe follows it there.
"""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.test_imm_ref_targets import Image as _Image, BASE


class Image(_Image):
    """The imm-ref fixture image plus the word reader resync_jump_tables and
    jump_table_entries use to measure a table."""
    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def _engine(data):
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.resync_jump_tables()
    return engine


def _dispatching_body():
    # BASE+0:  jmp dword ptr [eax*4 + TABLE]      ff 24 85 <TABLE>
    # BASE+7:  arm0: xor eax, eax ; ret            31 c0 c3
    # BASE+10: arm1: mov eax, 1 ; ret              b8 01 00 00 00 c3
    # BASE+16: arm2: mov eax, 2 ; ret              b8 02 00 00 00 c3
    # BASE+22: TABLE: arm0, arm1, arm2
    table = BASE + 22
    data = b"\xff\x24\x85" + struct.pack("<I", table)
    data += b"\x31\xc0\xc3"
    data += b"\xb8\x01\x00\x00\x00\xc3"
    data += b"\xb8\x02\x00\x00\x00\xc3"
    data += struct.pack("<III", BASE + 7, BASE + 10, BASE + 16)
    data += b"\xcc" * 16
    return data, table


def test_a_dispatch_through_a_measured_table_reaches_its_arms():
    data, table = _dispatching_body()
    engine = _engine(data)
    assert table in engine.jump_tables
    assert engine.probes_as_returning_body(BASE)


def test_a_dispatch_through_an_unknown_table_is_still_tail_shaped():
    # Same bytes, but point the jmp at a table the engine never measured:
    # the old answer stands, because nothing says where the arms are.
    data, _ = _dispatching_body()
    stray = struct.pack("<I", BASE + 0x100)
    data = data[:3] + stray + data[7:]
    engine = _engine(data)
    assert not engine.probes_as_returning_body(BASE)
