"""
Self-check: --seed-functions must realign the sweep, not drop the seed.

Run: py -3 tools/disasm/test_seed_realign.py

A seeded address is added straight to the detector's candidate set. But
_build_functions drops any candidate with no decoded instruction at its address
(num_insns == 0), so a seed that lands where linear_sweep came out of phase was
silently discarded -- the same failure Engine.decode_at exists to fix, which was
wired into _pass_call_targets and the tail-jump pass but never into seeding.

Measured on JSRF: 0x0007BE30 is 16-aligned, preceded by four nop padding bytes,
opens with the MSVC SEH prologue, was found by func_id's vtable scanner AND
observed as an unresolved indirect call 100 times at runtime -- and never became
a function, because the data run in front of it put the sweep out of step.

These checks cover the engine-level contract the seeding path depends on.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm import config  # noqa: E402
from tools.disasm.engine import DisasmEngine  # noqa: E402


class _Section:
    def __init__(self, va, data, executable=True):
        self.virtual_addr = va
        self.virtual_size = len(data)
        self.executable = executable
        self.name = ".text"
        self.data = data


class _Image:
    def __init__(self, *sections):
        self._secs = sections
        self.base_address = min(s.virtual_addr for s in sections)
        self.image_size = max(s.virtual_addr + s.virtual_size
                              for s in sections) - self.base_address

    def get_section_at_va(self, va):
        for s in self._secs:
            if s.virtual_addr <= va < s.virtual_addr + s.virtual_size:
                return s
        return None

    def get_section_data(self, section):
        return section.data


BASE = 0x00010000

# Bytes chosen so the sweep is left OUT OF PHASE across the 16-byte boundary:
# the 3-byte "add byte ptr [edx - 1], ch" starting at 0x0F swallows the function
# start at 0x10, exactly as the data run before JSRF's 0x0007BE30 does.
#   0x00: 90                       nop                       -> 0x01
#   0x01: ea fb ff c3 90 fc bd     ljmp     (7 bytes)         -> 0x08
#   0x08: 00 x6                    three 2-byte insns         -> 0x0e
#   0x0e: 90                       nop                       -> 0x0f
#   0x0f: 00 6a ff                 add byte ptr [edx - 1], ch -> 0x12  (!)
#   0x10: 6a ff                    push -1   <- the real, 16-aligned start
_DATA = (
    b"\x90"                                       # 0x00  nop
    b"\xea\xfb\xff\xc3\x90\xfc\xbd"             # 0x01  ljmp -> 0x08
    b"\x00\x00\x00\x00\x00\x00"                  # 0x08..0x0d
    b"\x90"                                       # 0x0e  nop -> 0x0f
    b"\x00"                                       # 0x0f  eats 0x10 with 6a ff
    b"\x6a\xff"                                   # 0x10  push -1   <- SEED
    b"\x68\xb1\x7c\x18\x00"                       # 0x12  push 0x187cb1
    b"\x64\xa1\x00\x00\x00\x00"                   # 0x17  mov eax, fs:[0]
    b"\x50"                                       # 0x1d  push eax
    b"\xc3"                                       # 0x1e  ret
)

SEED = BASE + 0x10


def _swept():
    sec = _Section(BASE, _DATA)
    img = _Image(sec)
    eng = DisasmEngine(img)
    eng.linear_sweep(sec)
    return eng


def test_sweep_really_does_step_over_the_seed():
    """Guard the premise: without realignment the seed address does not exist."""
    eng = _swept()
    assert SEED not in eng.instructions, (
        "premise broken: the sweep decoded the seed on its own, so this "
        "fixture no longer reproduces the dropped-seed bug")


def test_decode_at_recovers_the_seeded_function_start():
    eng = _swept()
    assert eng.decode_at(SEED) > 0, "decode_at refused a valid function start"
    assert SEED in eng.instructions
    insn = eng.instructions[SEED]
    assert insn.mnemonic == "push", insn.mnemonic
    assert insn.op_str == "-1", insn.op_str


def test_realigned_seed_starts_a_walkable_chain():
    """
    _build_functions walks from the candidate address, so what matters is that
    an instruction exists AT the seed -- not merely somewhere after it. The
    out-of-phase sweep leaves the interior decoded but the entry point missing,
    which is precisely why the candidate was dropped.
    """
    eng = _swept()
    assert SEED not in eng.instructions
    interior_before = len(eng.get_instructions_in_range(SEED, SEED + 0x10))

    eng.decode_at(SEED)

    assert SEED in eng.instructions
    assert len(eng.get_instructions_in_range(SEED, SEED + 0x10)) > interior_before

    # The chain from the seed is walkable end to end, terminating at the ret.
    addr, seen = SEED, []
    while addr in eng.instructions:
        insn = eng.instructions[addr]
        seen.append(insn.mnemonic)
        if insn.is_terminator:
            break
        addr = insn.end_address
    assert seen == ["push", "push", "mov", "push", "ret"], seen


def test_alignment_rule_matches_the_call_target_pass():
    """Seeding uses the same corroboration constant, so it must exist and be 16."""
    assert config.CALL_TARGET_REALIGN_ALIGNMENT == 16
    assert SEED % config.CALL_TARGET_REALIGN_ALIGNMENT == 0


def test_seeding_path_calls_decode_at_for_undecoded_addresses():
    """The wiring itself: Disassembler must realign seeds, not just add them."""
    import inspect
    from tools.disasm import disasm as disasm_mod
    src = inspect.getsource(disasm_mod.Disassembler.run)
    seed_block = src[src.index("if self.seed_functions:"):]
    seed_block = seed_block[:seed_block.index("num_funcs")]
    assert "decode_at" in seed_block, (
        "seeding no longer realigns the sweep; seeds that land in an "
        "out-of-phase run will be silently dropped again")
    assert "CALL_TARGET_REALIGN_ALIGNMENT" in seed_block, (
        "seeding must apply the same alignment corroboration as the "
        "call-target pass")


def _run():
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    for fn in fns:
        fn()
        print("  ok  %s" % fn.__name__)
    print("%d checks passed" % len(fns))


if __name__ == "__main__":
    _run()
