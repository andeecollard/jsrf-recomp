"""A forwarder past nop padding is found whole, not split at its error return.

Panzer Dragoon Orta's 0x0026B970, after a ret and nop padding, reached only
through a vtable:

    test; je L2; ...; ret 0xc
    mov eax, 0x80070057; ret 0xc     <- looks like a constant stub
    L2: mov eax, [eax+0x10]; jmp [eax+0x20]

Found in the same sweep as the ret case, the middle return was still a gap
and was split off; L2 was orphaned into an unresolved stub that popped only
the return address, and the caller lost 12 bytes of stack.
"""
from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


def test_forwarder_past_nop_padding_keeps_its_error_return():
    base = 0x10000
    code = bytearray(b"\x90" * 0x50)
    code[0x00:0x01] = b"\xc3"                          # previous function
    fwd = bytes.fromhex(
        "8b442404"      # 0x10 mov eax, [esp+4]
        "85c0"          # 0x14 test eax, eax
        "7410"          # 0x16 je 0x28
        "b801000000"    # 0x18 mov eax, 1
        "c20c00"        # 0x1d ret 0xc
        "b857000780"    # 0x20 mov eax, 0x80070057
        "c20c00"        # 0x25 ret 0xc
        "8b4010"        # 0x28 mov eax, [eax+0x10]
        "ff6020")       # 0x2b jmp [eax+0x20]
    code[0x10:0x10 + len(fwd)] = fwd
    code[0x40:0x41] = b"\xc3"                          # next function
    text = SectionInfo(".text", base, len(code), 0, len(code), False, True, "")
    image = BinaryImage("synthetic", bytes(code), 0, 0x20000, base, 0, [text])
    engine = DisasmEngine(image)
    for insn in engine._cs.disasm(bytes(code), base):
        engine.instructions[insn.address] = engine._classify_instruction(insn)
    detector = FunctionDetector(engine, image, None, LabelManager())
    for addr in (base, base + 0x40):
        detector._add_candidate(addr, 1.0, "seed")

    detector.detect_all([text])

    assert base + 0x10 in detector.functions
    assert base + 0x20 not in detector.functions, "split at the error return"
    assert detector.functions[base + 0x10].end >= base + 0x2E
