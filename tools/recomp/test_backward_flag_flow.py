"""Backward CFG edges must carry flags before address-ordered emission."""
import unittest
from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import _analyze_block_flag_states, FunctionTranslator
from . import config


def insn(address, mnemonic, operands=()):
    i = Instruction(address, 1, mnemonic, '', '')
    i.operands = list(operands)
    return i


def compare(address, immediate, mnemonic='cmp', width=4):
    reg = 'eax' if width == 4 else 'al'
    return insn(address, mnemonic, [Operand(type='reg', reg=reg),
                                   Operand(type='imm', imm=immediate)])


def block(address, *instructions):
    return BasicBlock(start=address, instructions=list(instructions))


class BackwardFlagFlowTest(unittest.TestCase):
    def setUp(self):
        # Translation config is process-global; do not leak this fixture's
        # section map to subsequent tests.
        saved = dict(config.__dict__)
        self.addCleanup(config.__dict__.update, saved)

    def test_real_translation_backward_cmp_predecessor(self):
        # test edx,edx; jne later; cmp eax,1; shared: jne unequal;
        # return 0; unequal: return 1; later: cmp eax,2; jmp shared
        image=bytes.fromhex('85d2751283f8017507b800000000c390b801000000c383f802ebec')
        base=0x10000
        config._install([config.Section('.text',base,len(image),0,len(image),True)],
                        entry_point=base,kernel_thunk_addr=base,origin='backward-flags-test')
        info={'start':f'0x{base:08X}', 'end':base+len(image),
              '_addr':base, 'size':len(image)}
        source=FunctionTranslator(image,{base:info}).translate_function(base,info)
        shared=source.split('loc_00010007: ;',1)[1].split('\nloc_',1)[0]
        self.assertIn('CMP_NE(_fa, _fb)',shared)
        self.assertNotIn('UNRESOLVED FLAGS',shared)

    def test_order_independent_through_preserving_loop(self):
        blocks=[block(0,compare(0,1)),block(10,insn(10,'nop')),
                block(20,insn(20,'jmp'))]
        preds={0:set(),10:{0,20},20:{10}}
        for order in (blocks,list(reversed(blocks))):
            states=_analyze_block_flag_states(order,preds,0)
            self.assertEqual(states[10][0],'cmp')
            self.assertEqual(states[20][1][1].imm,1)

    def test_unknown_entry_path_cannot_gain_loop_flags(self):
        blocks=[block(0,insn(0,'nop')),block(10,insn(10,'nop')),
                block(20,compare(20,2))]
        states=_analyze_block_flag_states(blocks,{0:set(),10:{0,20},20:{10}},0)
        self.assertIsNone(states[10])

    def test_conflicting_backedge_and_width_are_conservative(self):
        for other in (compare(20,1,'add'),compare(20,1,width=1)):
            blocks=[block(0,compare(0,1)),block(10,insn(10,'nop')),block(20,other)]
            states=_analyze_block_flag_states(blocks,{0:set(),10:{0,20},20:{0}},0)
            self.assertIsNone(states[10])

    def test_undefined_flags_on_one_path_poison_join(self):
        blocks=[block(0,compare(0,1)),block(10,insn(10,'nop')),
                block(20,insn(20,'idiv'))]
        self.assertIsNone(_analyze_block_flag_states(
            blocks,{0:set(),10:{0,20},20:{0}},0)[10])

    def test_two_constants_produce_runtime_snapshot_condition(self):
        blocks=[block(0,compare(0,1)),block(10,insn(10,'jne')),block(20,compare(20,2))]
        states=_analyze_block_flag_states(blocks,{0:set(),10:{0,20},20:{0}},0)
        jump=blocks[1].instructions[0];jump.jump_target=30
        lifter=Lifter();lifter.func_end=40
        emitted,_=lift_basic_block(lifter,blocks[1],states[10])
        self.assertIn('CMP_NE(_fa, _fb)','\n'.join(emitted))


if __name__=='__main__':
    unittest.main()
