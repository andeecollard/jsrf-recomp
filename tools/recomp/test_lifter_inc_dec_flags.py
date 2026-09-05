"""Execute INC/DEC flag consumers after the destination has been overwritten."""
import shutil
import subprocess
import tempfile
from pathlib import Path
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block, _make_condition
from diagnostics.jsrf_first_fault.backport_adx_loop_flags import DECREMENT, CONDITION


class IncDecFlagsTest(unittest.TestCase):
    def run_c(self, body):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
            src.write_text('#include <stdint.h>\nint main(void) {\n' + body + '\n}\n')
            subprocess.run([compiler, '-std=c11', str(src), '-o', str(exe)],
                           check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_adx_counter_survives_pointer_reload(self):
        ops = [Operand(type='reg', reg='ecx')]
        dec = Instruction(0x14519F, 1, 'dec', 'ecx', '49', operands=ops)
        mov = Instruction(0x1451A6, 5, 'mov', 'ecx, 0x410dfa4', '',
                          operands=[ops[0], Operand(type='imm', imm=0x410DFA4)])
        jump = Instruction(0x1451B6, 6, 'jne', '0x145000', '', jump_target=0x145000)
        lifter = Lifter()
        lifter.func_start, lifter.func_end = 0x144F60, 0x14520C
        statements, _ = lift_basic_block(lifter, BasicBlock(dec.address,
                                            instructions=[dec, mov, jump]))
        self.run_c('''uint32_t ecx=16, remaining=16, iterations=0, _fa=0;
int32_t _fas=0;
loc_00145000:
if (++iterations > 16) return 1;
ecx=remaining;
''' + '\n'.join(statements).replace('ecx = 0x410DFA4;',
                                   'remaining=ecx; ecx = 0x410DFA4;') +
                   '\nreturn iterations != 16;')
        self.assertEqual('\n    '.join(lifter.lift_instruction(dec)), DECREMENT)
        self.assertEqual(_make_condition('jne', 'dec', ops)[0], CONDITION)

    def test_width_and_signed_overflow_with_overwritten_destination(self):
        for mnemonic, start, expected in [('dec', 0x80, 1), ('inc', 0x7F, 0),
                                         ('dec', 1, 0), ('inc', 0xFF, 0)]:
            with self.subTest(mnemonic=mnemonic, start=start):
                operand = Operand(type='reg', reg='al')
                insn = Instruction(0, 2, mnemonic, 'al', '', operands=[operand])
                lifted = '\n'.join(Lifter().lift_instruction(insn))
                cond = _make_condition('jl', mnemonic, [operand])[0]
                self.run_c('''
#define LO8(x) ((uint8_t)(x))
#define SET_LO8(x,v) ((x)=((x)&0xFFFFFF00u)|(uint8_t)(v))
uint32_t eax=''' + str(start) + ''', _fa=0;
int32_t _fas=0;
''' + lifted + '\neax=42;\nreturn (' + cond + ') != ' + str(expected) + ';')

    def test_inc_dec_does_not_modify_carry(self):
        lifter = Lifter()
        lifter.needs_cf = True
        for mnemonic in ('inc', 'dec'):
            insn = Instruction(0, 1, mnemonic, 'ecx', '',
                               operands=[Operand(type='reg', reg='ecx')])
            self.assertNotIn('_cf =', '\n'.join(lifter.lift_instruction(insn)))


if __name__ == '__main__':
    unittest.main()
