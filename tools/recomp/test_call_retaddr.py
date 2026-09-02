"""
Self-check for guest return addresses on the simulated stack.

Run: py -3 tools/recomp/test_call_retaddr.py

Regression guard for two coupled bugs:

1. `call` pushed a literal 0 instead of the address of the following
   instruction. `ret` never reads the slot, so this was invisible until guest
   code did -- __SEH_prolog finds its scope table through [esp], _alloca probes
   walk back to it, and "mov eax, [esp]" is the standard CRT idiom for "who
   called me". All of them saw 0.

2. _fixup_icall_esp_save walked backwards from an ICALL looking for the run of
   argument pushes, and tested `startswith('PUSH32(esp,')` before it tested for
   a direct call. A direct call is emitted as "PUSH32(esp, <retva>); name();"
   on one line, so it matched as an argument push and the scan ran straight
   through it -- putting the _icall_esp save before a call that had already
   returned, so a failing ICALL rewound g_esp too far.
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402
from tools.recomp.translator import _fixup_icall_esp_save  # noqa: E402


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None, imm=0):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm


class _Insn:
    def __init__(self, mnemonic, operands, address, size, call_target=None):
        self.mnemonic = mnemonic
        self.operands = operands
        self.address = address
        self.size = size
        self.call_target = call_target

    @property
    def end_address(self):
        return self.address + self.size


def test_direct_call_pushes_following_address():
    # call rel32 at 0x00120000, 5 bytes long -> return address is 0x00120005.
    insn = _Insn("call", [_Op("0x1a2b3c", type="imm", imm=0x1A2B3C)],
                 address=0x00120000, size=5, call_target=0x001A2B3C)
    out = "\n".join(Lifter().lift_instruction(insn))
    assert "PUSH32(esp, 0x00120005u);" in out, out
    assert "PUSH32(esp, 0);" not in out, out
    # still a real call to the target
    assert "0x001A2B3C" in out, out


def test_indirect_call_pushes_following_address():
    # call eax at 0x00130000, 2 bytes long -> return address is 0x00130002.
    insn = _Insn("call", [_Op("eax", type="reg", reg="eax")],
                 address=0x00130000, size=2, call_target=None)
    out = "\n".join(Lifter().lift_instruction(insn))
    assert "PUSH32(esp, 0x00130002u);" in out, out
    assert "RECOMP_ICALL_SAFE(" in out, out


def test_retaddr_is_the_instruction_after_the_call():
    # A 6-byte call must not report a 5-byte return address.
    for size in (2, 5, 6, 7):
        insn = _Insn("call", [_Op("0x200000", type="imm", imm=0x200000)],
                     address=0x00110000, size=size, call_target=0x00200000)
        out = "\n".join(Lifter().lift_instruction(insn))
        assert f"PUSH32(esp, 0x{0x00110000 + size:08X}u);" in out, (size, out)


def test_ret_still_only_discards_the_slot():
    # The return address is data, never control flow: ret must not try to use it.
    lifter = Lifter()
    lifter.func_start = 0x00140000  # not an SEH helper, so no g_seh_ebp bridge
    out = "\n".join(lifter._lift_ret(_Insn("ret", [], 0x00140010, 1), []))
    assert "esp += 4;" in out, out
    assert "return;" in out, out
    # ret N adds the stdcall cleanup on top of the 4-byte return slot
    imm = _Op("8", type="imm", imm=8)
    out = "\n".join(lifter._lift_ret(_Insn("ret", [imm], 0x00140010, 3), [imm]))
    assert "esp += 12;" in out, out


def test_icall_save_stops_at_a_completed_direct_call():
    # Two arg pushes, a direct call, then an ICALL with one arg of its own.
    # The save must land on the ICALL's own arg push, not before the direct call.
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00120005u); sub_001A2B3C(); /* call 0x001A2B3C */",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(edx, _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    save_idx = next(i for i, l in enumerate(out) if "_icall_esp = g_esp" in l)
    call_idx = next(i for i, l in enumerate(out) if "/* call 0x" in l)
    assert save_idx > call_idx, (
        "save must be after the completed direct call:\n" + "\n".join(out))
    # and it must still cover the ICALL's own argument
    arg_idx = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, eax);")
    assert save_idx < arg_idx, "\n".join(out)


def test_icall_save_still_covers_a_plain_arg_run():
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(edx, _icall_esp); /* indirect call */",
    ]
    out = _fixup_icall_esp_save(lines)
    save_idx = next(i for i, l in enumerate(out) if "_icall_esp = g_esp" in l)
    first_arg = next(i for i, l in enumerate(out) if l.strip() == "PUSH32(esp, ecx);")
    assert save_idx < first_arg, "\n".join(out)


def test_callee_clean_icall_keeps_the_full_rewind():
    # No caller-side cleanup follows the call, so the site is stdcall/thiscall
    # and a failed lookup must rewind past the arguments the callee would have
    # popped. This is the overwhelmingly common shape -- 5216 of JSRF's 5293
    # indirect call sites -- so a regression here is worse than the bug below.
    lines = [
        "    PUSH32(esp, eax);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(edx, _icall_esp); /* indirect call */",
        "loc_00120010: ;",
        "    eax = MEM32(esi + 4);",
    ]
    out = _fixup_icall_esp_save(lines)
    call = next(l for l in out if "RECOMP_ICALL_SAFE" in l)
    assert "_icall_esp)" in call and "_icall_esp -" not in call, call


def test_caller_clean_icall_leaves_the_arguments_for_the_guest():
    # "call eax; mov dl,[esp+0x17]; add esp,4" -- __cdecl, so the guest cleans
    # its own argument after the call. Rewinding to the pre-argument esp on a
    # failed lookup double-counts that cleanup and leaves esp 4 bytes high,
    # which shifts every later pop in the function by one slot.
    #
    # This is the shape that broke JSRF's vblank handshake: sub_00193D90's
    # unresolved callback left it returning ebx = 0, so the DPC above it
    # reloaded NV_PMC_INTR_EN_0 from address 0xB0 and the GPU interrupt was
    # never re-enabled.
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00193EB7u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
        "loc_00193EB7: ;",
        "    SET_LO8(edx, MEM8(esp + 0x17));",
        "    _cf = (int)((((uint64_t)(esp) + (uint64_t)(4)) >> 32) & 1);",
        "    esp = esp + 4;",
    ]
    out = _fixup_icall_esp_save(lines)
    call = next(l for l in out if "RECOMP_ICALL_SAFE" in l)
    assert "_icall_esp - 4)" in call, call


def test_cleanup_behind_a_branch_is_not_attributed_to_the_call():
    # A second label means another basic block also reaches the "add esp, N",
    # so it is not certainly this call's cleanup. Guess callee-clean rather
    # than trade one silent drift for another.
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
        "loc_00120010: ;",
        "loc_00120014: ;",
        "    esp = esp + 4;",
    ]
    out = _fixup_icall_esp_save(lines)
    call = next(l for l in out if "RECOMP_ICALL_SAFE" in l)
    assert "_icall_esp -" not in call, call


def test_local_frame_teardown_is_not_argument_cleanup():
    # "add esp, 0x400" is a local frame being released, not an argument list.
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
        "loc_00120010: ;",
        "    esp = esp + 0x400;",
    ]
    out = _fixup_icall_esp_save(lines)
    call = next(l for l in out if "RECOMP_ICALL_SAFE" in l)
    assert "_icall_esp -" not in call, call


def test_hex_argument_cleanup_is_recognised():
    # The lifter emits "add esp, 16" as hex; a decimal-only match would miss it
    # and the test above would pass for the wrong reason.
    lines = [
        "    PUSH32(esp, ecx);",
        "    PUSH32(esp, 0x00120010u); RECOMP_ICALL_SAFE(eax, _icall_esp); /* indirect call */",
        "loc_00120010: ;",
        "    esp = esp + 0x10;",
    ]
    out = _fixup_icall_esp_save(lines)
    call = next(l for l in out if "RECOMP_ICALL_SAFE" in l)
    assert "_icall_esp - 16)" in call, call


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
