"""Self-check for conservative global callback function discovery.

    python3 tools/disasm/test_callback_targets.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.disasm.engine import Instruction
from tools.disasm.functions import Function, FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.xrefs import XRef, XRefTracker, XRefType


TEXT_START = 0x00100000
TEXT_END = 0x00110000
SLOT = 0x00200000
STORE = 0x00100100
CALL = 0x00100200
TARGET = 0x00100300


class Section:
    name = ".text"
    virtual_addr = TEXT_START
    virtual_size = TEXT_END - TEXT_START
    executable = True


class Image:
    entry_point = TEXT_START

    def get_section_at_va(self, address):
        return Section() if TEXT_START <= address < TEXT_END else None


class Engine:
    def __init__(self, instructions):
        self.instructions = {insn.address: insn for insn in instructions}

    def decode_at(self, address):
        return None


def make_detector(slot_called=True, target=TARGET):
    store = Instruction(
        address=STORE,
        size=10,
        mnemonic="mov",
        op_str=f"dword ptr [{SLOT:#x}], {target:#x}",
        bytes_hex="c7050000200000031000",
        memory_ref=SLOT,
        imm_ref=target,
    )
    target_insn = Instruction(
        address=target,
        size=1,
        mnemonic="ret",
        op_str="",
        bytes_hex="c3",
        is_ret=True,
    )
    engine = Engine([store, target_insn])
    xrefs = XRefTracker()
    xrefs.add(XRef(STORE, SLOT, XRefType.DATA_READ))
    xrefs.add(XRef(STORE, target, XRefType.DATA_IMM))
    if slot_called:
        xrefs.add(XRef(CALL, SLOT, XRefType.CALL))
    detector = FunctionDetector(engine, Image(), xrefs, LabelManager())
    return detector


def main():
    detector = make_detector()
    assert detector._pass_global_callback_targets([Section()]) == 1
    assert detector._candidates[TARGET][1] == "global_callback"

    # A code-looking immediate stored as ordinary data is insufficient.
    detector = make_detector(slot_called=False)
    assert detector._pass_global_callback_targets([Section()]) == 0

    # A target outside executable memory is insufficient.
    detector = make_detector(target=0x00300000)
    assert detector._pass_global_callback_targets([Section()]) == 0

    # Do not split an existing function body on this evidence alone.
    detector = make_detector()
    detector.functions[TEXT_START] = Function(
        start=TEXT_START,
        end=TARGET + 1,
        name="existing",
    )
    assert detector._pass_global_callback_targets([Section()]) == 0

    print("OK: 4 global callback discovery cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
