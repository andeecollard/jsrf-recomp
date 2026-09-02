"""
Self-check for function-end detection around out-of-line tails.

Run: py -3 tools/disasm/test_function_end.py

Regression guard for the bug where a forward branch target was stored in
max_addr and then used as an *exclusive* end. The coverage test
`addr + size >= max_addr` reported "covered" while sitting exactly on the
target, so the function ended at the first instruction it was required to
contain.

MSVC emits this shape constantly: a conditional branch forward, a body, an
unconditional `jmp` backwards, then the branch target parked out of line
after it. Halo's get_edge_vertex (0x00107EC0) ended at 0x00107FB8 -- its own
tail -- so the tail was lifted as a separate function and the `jne` to it
became a tail call that returned without running the epilogue, leaking the
whole 28-byte frame on every call. Three calls in and the caller's saved
esi/edi/ebx came back as garbage.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size, mnemonic="mov", target=None,
                 is_ret=False, is_jump=False, is_cond_jump=False):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.mnemonic = mnemonic
        self.jump_target = target
        self.is_ret = is_ret
        self.is_jump = is_jump
        self.is_cond_jump = is_cond_jump
        self.is_branch = is_jump or is_cond_jump
        self.is_terminator = is_ret or is_jump
        self.is_nop = mnemonic == "nop"


class _Engine:
    def __init__(self, insns):
        self.by_addr = {i.address: i for i in insns}

    def get_instruction(self, addr):
        return self.by_addr.get(addr)


def _detector(insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    return det


# The real shape of Halo get_edge_vertex's tail, addresses preserved.
#   0x107F9F  test dx, dx
#   0x107FA2  jne  0x107FB8      <- forward branch over the epilogue
#   0x107FA4  ...epilogue...
#   0x107FAD  ret
#   0x107FAE  mov ebx, esi
#   0x107FB3  jmp  0x107EE5      <- unconditional, backwards
#   0x107FB8  cmp dx, [edi]      <- the branch target, out of line
#   0x107FC1  ret
TAIL = [
    _Insn(0x107F9F, 3),
    _Insn(0x107FA2, 2, "jne", target=0x107FB8, is_cond_jump=True),
    _Insn(0x107FA4, 9),
    _Insn(0x107FAD, 1, "ret", is_ret=True),
    _Insn(0x107FAE, 5),
    _Insn(0x107FB3, 5, "jmp", target=0x107EE5, is_jump=True),
    _Insn(0x107FB8, 9),
    _Insn(0x107FC1, 1, "ret", is_ret=True),
]


def test_out_of_line_tail_is_included():
    det = _detector(TAIL)
    end = det._find_function_end(0x107F9F, next_func=None, sec_end=0x108100)
    assert end > 0x107FB8, f"function cut off at its own branch target: {end:#x}"
    assert end == 0x107FC2, f"expected end past the tail's ret, got {end:#x}"


def test_plain_function_still_ends_at_ret():
    # No forward branches: the first ret ends it. Guards against the fix
    # running functions together.
    insns = [_Insn(0x1000, 4), _Insn(0x1004, 1, "ret", is_ret=True),
             _Insn(0x1005, 4)]
    det = _detector(insns)
    end = det._find_function_end(0x1000, next_func=None, sec_end=0x2000)
    assert end == 0x1005, f"expected {0x1005:#x}, got {end:#x}"


def test_next_function_still_bounds_the_walk():
    det = _detector(TAIL)
    end = det._find_function_end(0x107F9F, next_func=0x107FB0, sec_end=0x108100)
    assert end <= 0x107FB0, f"walked past the next function: {end:#x}"


def test_int3_ends_noreturn_function_before_padding():
    # MSVC's noreturn-call shape: call, trap, alignment, next body. The trap
    # belongs to the first function; the padding and body do not.
    insns = [
        _Insn(0x1000, 5, "call"),
        _Insn(0x1005, 1, "int3"),
        _Insn(0x1006, 1, "nop"),
        _Insn(0x1007, 1, "nop"),
        _Insn(0x1008, 1, "push"),
        _Insn(0x1009, 1, "ret", is_ret=True),
    ]
    det = _detector(insns)
    end = det._find_function_end(0x1000, next_func=None, sec_end=0x2000)
    assert end == 0x1006, f"expected end after INT3, got {end:#x}"


def test_forward_target_past_int3_is_still_included():
    # A reachable out-of-line block remains authoritative. INT3 only ends the
    # walk once every internal forward target has actually been decoded.
    insns = [
        _Insn(0x1000, 2, "jne", target=0x1008, is_cond_jump=True),
        _Insn(0x1002, 1, "int3"),
        _Insn(0x1003, 5, "nop"),
        _Insn(0x1008, 1, "mov"),
        _Insn(0x1009, 1, "ret", is_ret=True),
    ]
    det = _detector(insns)
    end = det._find_function_end(0x1000, next_func=None, sec_end=0x2000)
    assert end == 0x100A, f"forward target was cut off at {end:#x}"


def test_inline_int3_before_epilogue_is_not_a_boundary():
    # INT3 can be used as a resumable debug breakpoint. Without following NOP
    # padding it remains part of the current function and its epilogue wins.
    insns = [
        _Insn(0x1000, 2, "int"),
        _Insn(0x1002, 1, "int3"),
        _Insn(0x1003, 1, "leave"),
        _Insn(0x1004, 3, "ret", is_ret=True),
    ]
    det = _detector(insns)
    end = det._find_function_end(0x1000, next_func=None, sec_end=0x2000)
    assert end == 0x1007, f"inline INT3 cut off epilogue at {end:#x}"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("function end: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
