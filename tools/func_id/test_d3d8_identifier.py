"""
Self-check for D3D8 identification via NV2A push-buffer method constants.

Run: python3 -m pytest tools/func_id/test_d3d8_identifier.py -q
     (or: python3 tools/func_id/test_d3d8_identifier.py)

Deliberately capstone-free, like d3d8_identifier itself: the rest of
tools/func_id and tools/disasm need capstone and are therefore unrunnable in
the environment this was written in, which is exactly how a scanner rots into
something that passes for the wrong reason.

The three things worth guarding:
  * the 0xC7 /0 store decode, since that is the encoding D3D8 uses for a
    push-buffer command word and the one imm_scanner.py skips;
  * the decisive/weak rule split, which stops a lone incidental register from
    naming a large function;
  * the count<<18 encoding, since a bare offset like 0x304 collides with
    ordinary arithmetic constants.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.func_id.d3d8_identifier import (  # noqa: E402
    BARE_METHOD_MIN,
    STATE_BLOCK_NAME,
    WEAK_RULE_MAX_METHODS,
    classify_methods,
    decode_pushbuffer_methods,
    extract_immediates,
    load_nv097_methods,
    scan_body_pushes,
)

# A few real values, so the test does not depend on the whole header parsing.
M = {
    0x17FC: "NV097_SET_BEGIN_END",
    0x1808: "NV097_ARRAY_ELEMENT32",
    0x0130: "NV097_FLIP_STALL",
    0x012C: "NV097_FLIP_INCREMENT_WRITE",
    0x0304: "NV097_SET_BLEND_ENABLE",
}


def _encoded(method, count=1):
    return (count << 18) | method


def test_pushbuffer_store_is_decoded():
    # mov dword ptr [edi], 0x000417FC  ->  C7 07 FC 17 04 00
    # This is the encoding imm_scanner.py explicitly does not handle, and the
    # only one that finds a command word written straight into the ring.
    code = bytes([0xC7, 0x07, 0xFC, 0x17, 0x04, 0x00])
    assert _encoded(0x17FC) in extract_immediates(code)


def test_pushbuffer_store_with_displacement():
    # mov dword ptr [edi+0x10], 0x000417FC  ->  C7 47 10 FC 17 04 00
    code = bytes([0xC7, 0x47, 0x10, 0xFC, 0x17, 0x04, 0x00])
    assert _encoded(0x17FC) in extract_immediates(code)
    # mov dword ptr [edi+0x1000], ...      ->  C7 87 00 10 00 00 ...
    code = bytes([0xC7, 0x87, 0x00, 0x10, 0x00, 0x00, 0xFC, 0x17, 0x04, 0x00])
    assert _encoded(0x17FC) in extract_immediates(code)


def test_push_and_mov_reg_still_scanned():
    push = bytes([0x68, 0xFC, 0x17, 0x04, 0x00])
    movr = bytes([0xB8, 0xFC, 0x17, 0x04, 0x00])
    assert _encoded(0x17FC) in extract_immediates(push)
    assert _encoded(0x17FC) in extract_immediates(movr)


def test_c7_with_nonzero_reg_field_is_not_a_mov():
    # C7 /1 is not "mov r/m32, imm32". Decoding it as one would invent an
    # immediate at the wrong offset and could fabricate a method match.
    code = bytes([0xC7, 0x0F, 0xFC, 0x17, 0x04, 0x00])
    assert _encoded(0x17FC) not in extract_immediates(code)


def test_encoded_command_word_matches_method():
    got = decode_pushbuffer_methods({_encoded(0x17FC)}, M)
    assert got == {"NV097_SET_BEGIN_END"}


def test_bare_small_constant_is_not_matched():
    # 0x304 is SET_BLEND_ENABLE but also an utterly ordinary constant. Only
    # values at or above BARE_METHOD_MIN are trusted unencoded, and 0x304 is
    # reached solely through the encoded form.
    assert BARE_METHOD_MIN > 0
    assert decode_pushbuffer_methods({0x304 - 1}, M) == set()
    assert decode_pushbuffer_methods({_encoded(0x304)}, M) == {"NV097_SET_BLEND_ENABLE"}


def test_absurd_count_is_rejected():
    # (count << 18) with a huge count is not a command word; matching on the
    # low bits alone would tag any large constant that happens to end in a
    # method offset.
    assert decode_pushbuffer_methods({(4096 << 18) | 0x17FC}, M) == set()


def test_indexed_draw_beats_plain_draw():
    indexed = {"NV097_SET_BEGIN_END", "NV097_ARRAY_ELEMENT32"}
    plain = {"NV097_SET_BEGIN_END"}
    assert classify_methods(indexed)[0] == "D3DDevice_DrawIndexedVertices"
    assert classify_methods(plain)[0] == "D3DDevice_DrawVertices"


def test_swap_needs_both_flip_markers():
    # FLIP_STALL alone appears in device setup too; only the pair is the flip.
    assert classify_methods({"NV097_FLIP_STALL"})[0] != "D3DDevice_Swap"
    assert classify_methods(
        {"NV097_FLIP_STALL", "NV097_FLIP_INCREMENT_WRITE"})[0] == "D3DDevice_Swap"


def test_broad_function_is_not_named_by_a_weak_rule():
    # JSRF's sub_0018D0F0: 11 methods across surface setup, depth/stencil, the
    # flip and one lighting register. The weak SET_LIGHT rule called it
    # SetLight; a function this broad is a state block, not an entry point.
    broad = {
        "NV097_FLIP_STALL", "NV097_NO_OPERATION", "NV097_SET_DEPTH_TEST_ENABLE",
        "NV097_SET_LIGHT_AMBIENT_COLOR", "NV097_SET_STENCIL_TEST_ENABLE",
        "NV097_SET_SURFACE_CLIP_HORIZONTAL", "NV097_SET_SURFACE_COLOR_OFFSET",
        "NV097_SET_SURFACE_FORMAT", "NV097_SET_SURFACE_PITCH",
        "NV097_SET_SURFACE_ZETA_OFFSET", "NV097_WAIT_FOR_IDLE",
    }
    assert len(broad) > WEAK_RULE_MAX_METHODS
    name, conf = classify_methods(broad)
    assert name == STATE_BLOCK_NAME, name
    assert conf < 0.5
    # ...but the same lone register in a small function still names it.
    assert classify_methods({"NV097_SET_LIGHT_AMBIENT_COLOR"})[0] == "D3DDevice_SetLight"


def test_decisive_rule_survives_a_broad_method_set():
    # A draw that also programs a lot of state is still a draw: SET_BEGIN_END
    # brackets one operation and nothing else emits it.
    broad = {
        "NV097_SET_BEGIN_END", "NV097_ARRAY_ELEMENT32",
        "NV097_SET_SURFACE_CLIP_HORIZONTAL", "NV097_SET_SURFACE_PITCH",
        "NV097_SET_LIGHT_AMBIENT_COLOR", "NV097_SET_FOG_COLOR",
        "NV097_SET_TEXTURE_CONTROL0", "NV097_WAIT_FOR_IDLE",
    }
    assert len(broad) > WEAK_RULE_MAX_METHODS
    assert classify_methods(broad)[0] == "D3DDevice_DrawIndexedVertices"


def test_unknown_method_set_is_not_named():
    assert classify_methods(set()) == (None, 0.0)
    assert classify_methods({"NV097_NO_OPERATION"}) == (None, 0.0)


def test_method_table_parses_and_excludes_enum_values():
    # Guards the indent discriminator: NV097_SET_BEGIN_END is a method at
    # 0x17FC; NV097_SET_BEGIN_END_OP_TRIANGLES is the VALUE 0x05 nested under
    # it, and matching that would tag any function using the constant 5.
    #
    # Note the assertion is on the nested BEGIN_END_OP names specifically, not
    # on "_OP_" anywhere -- NV097_SET_STENCIL_OP_FAIL and
    # NV097_SET_LOGIC_OP_ENABLE are genuine methods whose names contain it.
    methods = load_nv097_methods()
    if not methods:
        return  # header not present in this checkout; nothing to assert
    assert methods.get(0x17FC) == "NV097_SET_BEGIN_END"
    assert not any(n.startswith("NV097_SET_BEGIN_END_OP") for n in methods.values())
    # NV097_SET_OBJECT is genuinely method 0, and the next real method is
    # NV097_NO_OPERATION at 0x100. Nothing legitimate lands in between, so that
    # band is where leaked enum values (1, 2, 5 ...) would show up.
    strays = sorted(k for k in methods if 0 < k < 0x100)
    assert not strays, [hex(k) for k in strays]


def _pushing_body(command_word, call_rel):
    """mov ecx, imm32 ; mov edx, 0 ; call rel32"""
    return (bytes([0xB9]) + command_word.to_bytes(4, "little")
            + bytes([0xBA, 0, 0, 0, 0])
            + bytes([0xE8]) + call_rel.to_bytes(4, "little", signed=True))


def test_method_pushed_through_a_helper_is_attributed_to_the_caller():
    # The shape that matters: JSRF's push primitive is fastcall, so the command
    # word sits in the CALLER and the callee holds no constant at all. A body
    # scan alone therefore misses every state setter in the game.
    base = 0x00150000
    # call is at offset 10, next insn at 15; target = base + 15 + rel
    rel = 0x1000
    target = base + 15 + rel
    body = _pushing_body(_encoded(0x17FC), rel)
    names, targets = scan_body_pushes(body, base, M, {target})
    assert names == {"NV097_SET_BEGIN_END"}, names
    assert targets == {target: 1}, targets


def test_call_target_must_be_a_known_function():
    # 0xE8 turns up inside other instructions' encodings constantly. Without
    # the function-start check a constant pairs with a displacement byte and
    # produces an address that is not code -- 0x24656186 was the real one.
    base = 0x00150000
    body = _pushing_body(_encoded(0x17FC), 0x1000)
    names, targets = scan_body_pushes(body, base, M, set())
    assert names == set()
    assert targets == {}


def test_non_method_constant_in_ecx_is_ignored():
    base = 0x00150000
    rel = 0x1000
    target = base + 15 + rel
    body = _pushing_body(0xDEADBEEF, rel)
    names, _ = scan_body_pushes(body, base, M, {target})
    assert names == set()


def test_call_beyond_the_window_is_not_paired():
    # A constant separated from the call by a lot of unrelated code is not an
    # argument to it.
    base = 0x00150000
    rel = 0x1000
    filler = bytes([0x90]) * 64
    body = (bytes([0xB9]) + _encoded(0x17FC).to_bytes(4, "little") + filler
            + bytes([0xE8]) + rel.to_bytes(4, "little", signed=True))
    target = base + 5 + len(filler) + 5 + rel
    names, _ = scan_body_pushes(body, base, M, {target})
    assert names == set()


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
