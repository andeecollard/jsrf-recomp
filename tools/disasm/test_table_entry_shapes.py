"""Table words naming methods whose bodies close only outside their gap.

Jet Set Radio Future's vtables name 103 methods that v0.14.0's closed-CFG
check (probes_as_callback_body) refused, every one 16-byte aligned behind
padding. Three of them stopped its attract demo on an unresolved indirect
call. They come in three shapes:

  * a guarded virtual-call tail: `test eax, eax ; je done ; mov eax, [ecx] ;
    jmp [eax+N]`. The indirect jmp is the method's exit, as it is for a vcall
    thunk, but the guard in front means probes_as_vcall_thunk never matches;
  * a dispatcher into other methods: `mov eax, [ecx+N] ; jmp [eax*4+table]`,
    whose arms are function starts outside the gap;
  * an error path that leaves by a direct jmp, after a jcc, to a helper nobody
    has found yet. _probes_as_tail_body follows only the first straight block.

The proof stays a closed CFG. Its exits may now be an indirect jmp through
memory, a proven entry (a table arm too), or a direct destination outside the
gap that is itself proven the same way in its own gap.
"""
import struct

import pytest

from tools.disasm import config
from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo

BASE = 0x10000


def _pad(code, to):
    assert len(code) <= to, (len(code), to)
    return code + b"\xcc" * (to - len(code))


def _detect(text_bytes, table_words, functions=()):
    """Sweep `text_bytes`, make `functions` starts, then run the data-table
    pass on a .data section holding `table_words`."""
    text = SectionInfo(".text", BASE, len(text_bytes), 0, len(text_bytes),
                       False, True, "")
    table = b"".join(struct.pack("<I", w) for w in table_words)
    data = SectionInfo(".data", BASE + 0x1000, len(table), len(text_bytes),
                       len(table), False, False, "")
    image = BinaryImage("synthetic", text_bytes + table, 0, 0x20000, BASE, 0,
                        [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    engine.resync_jump_tables()
    det = FunctionDetector(engine, image, None, LabelManager())
    det._pass_known_addresses()
    for start in functions:
        det._add_candidate(start, config.CONFIDENCE_CC_BOUNDARY, "test")
    det._build_functions([text])
    det._pass_data_ptr_targets([text])
    return det, engine


# BASE+0: ret, then padding: the entry point is a function and the rest is a gap.
_HEAD = _pad(b"\xc3", 16)


def test_guarded_vcall_tail_is_a_table_entry():
    # BASE+16: test eax, eax ; je +5 ; mov eax, [ecx] ; jmp [eax+8] ; ret
    method = b"\x85\xc0" b"\x74\x05" b"\x8b\x01" b"\xff\x60\x08" b"\xc3"
    text = _HEAD + _pad(method, 16)
    det, engine = _detect(text, [BASE + 16])
    # Not a closed CFG by the plain rule: the indirect jmp is unresolved...
    assert not engine.probes_as_callback_body(BASE + 16, BASE + 32)
    # ...and not a vcall thunk, because of the guard in front.
    assert not engine.probes_as_vcall_thunk(BASE + 16)
    assert BASE + 16 in det._alias_entries


def test_a_bare_register_tail_is_not_enough():
    # Same guard, but `jmp eax` with nothing loading eax: a stray word can
    # decode to `ff e0`, and no load says the register holds a handler.
    method = b"\x85\xc0" b"\x74\x02" b"\xff\xe0" b"\xc3"
    text = _HEAD + _pad(method, 16)
    det, _ = _detect(text, [BASE + 16])
    assert BASE + 16 not in det._alias_entries


def test_a_register_the_body_loaded_is_a_tail():
    # mov eax, [ecx+4] ; test eax, eax ; je +2 ; jmp eax ; ret -- a handler
    # fetched from the object, as JSRF's 0x52010 fetches one from a table.
    method = b"\x8b\x41\x04" b"\x85\xc0" b"\x74\x02" b"\xff\xe0" b"\xc3"
    text = _HEAD + _pad(method, 16)
    det, _ = _detect(text, [BASE + 16])
    assert BASE + 16 in det._alias_entries


def test_dispatcher_whose_arms_are_functions_is_a_table_entry():
    # Three one-line functions, then at BASE+64 the method:
    #   mov eax, [ecx+0x44] ; jmp dword ptr [eax*4 + TABLE]
    # with TABLE at BASE+80 naming the three.
    f1, f2, f3 = BASE + 16, BASE + 32, BASE + 48
    table = BASE + 80
    method = b"\x8b\x41\x44" + b"\xff\x24\x85" + struct.pack("<I", table)
    text = (_HEAD + _pad(b"\x31\xc0\xc3", 16)
            + _pad(b"\xb8\x01\x00\x00\x00\xc3", 16)
            + _pad(b"\xb8\x02\x00\x00\x00\xc3", 16)
            + _pad(method, 16)
            + struct.pack("<III", f1, f2, f3) + b"\xcc" * 16)
    det, engine = _detect(text, [BASE + 64], functions=(f1, f2, f3))
    assert engine.jump_tables.get(table) == table + 12
    assert all(f in det.functions for f in (f1, f2, f3))
    assert BASE + 64 in det._alias_entries


def test_dispatcher_whose_arms_are_unknown_is_refused():
    # The same dispatcher, but the arms are nobody's: a table of three words
    # that happen to point into the gap after the padding.
    table = BASE + 32
    method = b"\x8b\x41\x44" + b"\xff\x24\x85" + struct.pack("<I", table)
    text = (_HEAD + _pad(method, 16)
            + struct.pack("<III", BASE + 48, BASE + 52, BASE + 56)
            + b"\xcc" * 32)
    det, _ = _detect(text, [BASE + 16])
    assert BASE + 16 not in det._alias_entries


def _escaping(helper):
    # BASE+16: test eax, eax ; je +1 ; ret ; jmp HELPER         (the method)
    # BASE+32: ret                                              (a function)
    # BASE+48: HELPER, in the gap after that function
    # The jmp sits on the taken path, behind the ret: the straight-line walk
    # of _probes_as_tail_body ends at the ret and never sees it.
    rel = (BASE + 48) - (BASE + 16 + 5 + 5)
    method = b"\x85\xc0" b"\x74\x01" b"\xc3" b"\xe9" + struct.pack("<i", rel)
    return _HEAD + _pad(method, 16) + _pad(b"\xc3", 16) + _pad(helper, 16)


def test_direct_escape_to_a_proven_helper_is_a_table_entry():
    text = _escaping(b"\x31\xc0\xc3")                   # xor eax, eax ; ret
    det, engine = _detect(text, [BASE + 16], functions=(BASE + 32,))
    # The jmp leaves the gap for code nobody has found: refused as it is.
    assert not engine.probes_as_callback_body(BASE + 16, BASE + 32,
                                              tail_targets=det.functions)
    assert BASE + 16 in det._alias_entries


def test_direct_escape_to_an_open_helper_is_refused():
    text = _escaping(b"\x40\xcc")                       # inc eax ; int3
    det, _ = _detect(text, [BASE + 16], functions=(BASE + 32,))
    assert BASE + 16 not in det._alias_entries


def test_escapes_are_reported_and_proven_arms_are_exits():
    # The engine alone: the destination comes back in `escapes`, and with
    # escapes=None (the default) the same body still refuses.
    text = _escaping(b"\x31\xc0\xc3")
    _, engine = _detect(text, [])
    escapes = set()
    assert engine.probes_as_callback_body(BASE + 16, BASE + 32, escapes=escapes)
    assert escapes == {BASE + 48}
    assert not engine.probes_as_callback_body(BASE + 16, BASE + 32)
