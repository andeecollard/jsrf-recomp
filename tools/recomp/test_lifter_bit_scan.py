"""Tests for BSF/BSR lifting and their zero flag."""

from tools.recomp.disasm import BasicBlock, Instruction, Operand
from tools.recomp.lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def test_lifts_32_bit_bsf():
    insn = Instruction(0, 3, "bsf", "eax, ecx", "0fbcc1")
    insn.operands = [_reg("eax"), _reg("ecx")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert "uint32_t _bs_index = 0;" in generated
    assert "eax = _bs_index;" in generated
    assert "_flags = (_bs_value == 0);" in generated


def test_lifts_16_bit_bsr_without_clobbering_upper_bits():
    insn = Instruction(0, 4, "bsr", "ax, word ptr [ecx]", "660fbd01")
    insn.operands = [
        _reg("ax"),
        Operand(type="mem", mem_base="ecx", mem_size=2),
    ]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert "uint32_t _bs_index = 15;" in generated
    assert "SET_LO16(eax, _bs_index);" in generated


def test_preserves_destination_when_source_is_zero():
    insn = Instruction(0, 3, "bsf", "eax, ecx", "0fbcc1")
    insn.operands = [_reg("eax"), _reg("ecx")]

    generated = "\n".join(Lifter().lift_instruction(insn))

    assert "if (_bs_value != 0) {" in generated
    assert "eax = _bs_index;" in generated


def test_omits_zero_flag_snapshot_when_function_has_no_consumer():
    insn = Instruction(0, 3, "bsf", "eax, ecx", "0fbcc1")
    insn.operands = [_reg("eax"), _reg("ecx")]
    lifter = Lifter()
    lifter.needs_flags = False

    generated = "\n".join(lifter.lift_instruction(insn))

    assert "_flags" not in generated


def test_jz_reads_snapshotted_zero_flag_after_source_is_clobbered():
    scan = Instruction(0, 3, "bsf", "eax, ecx", "0fbcc1")
    scan.operands = [_reg("eax"), _reg("ecx")]
    move = Instruction(3, 5, "mov", "ecx, 1", "b901000000")
    move.operands = [_reg("ecx"), Operand(type="imm", imm=1)]
    jump = Instruction(8, 2, "je", "0x10", "7406")
    jump.jump_target = 0x10

    lifter = Lifter()
    lifter.func_start = 0
    lifter.func_end = 0x20
    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[scan, move, jump]))

    assert "if (_flags) goto loc_00000010;" in generated[-1]
    assert all("ecx == 0" not in statement for statement in generated)


def test_jnz_reads_snapshotted_zero_flag():
    scan = Instruction(0, 3, "bsr", "eax, ecx", "0fbdc1")
    scan.operands = [_reg("eax"), _reg("ecx")]
    jump = Instruction(3, 2, "jne", "0x10", "750b")
    jump.jump_target = 0x10

    lifter = Lifter()
    lifter.func_start = 0
    lifter.func_end = 0x20
    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=[scan, jump]))

    assert "if (!_flags) goto loc_00000010;" in generated[-1]
