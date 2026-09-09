"""Compile actual backward-join output, including mixed 8/32-bit setters."""
import subprocess
import tempfile
from pathlib import Path
import unittest
from . import config
from .translator import FunctionTranslator, _merge_predecessor_flag_states
from .lifter import MERGED_COMPARE_ZF, _make_condition
from .disasm import Operand

class MixedCompareZFTest(unittest.TestCase):
    def setUp(self):
        saved=dict(config.__dict__)
        self.addCleanup(config.__dict__.update,saved)

    def test_compiled_backward_join_je_and_jne(self):
        for opcode in ('74','75'):
            # edx chooses CMP EAX,1 or backward TEST AH,1; shared JE/JNE.
            image=bytes.fromhex('85d2751283f801'+opcode+'07b800000000c390b801000000c3f6c401ebec')
            base=0x10000
            config._install([config.Section('.text',base,len(image),0,len(image),True)],
                            entry_point=base,kernel_thunk_addr=base,origin='mixed-zf-test')
            info={'end':base+len(image),'size':len(image)}
            source=FunctionTranslator(image,{base:info}).translate_function(base,info)
            self.assertIn('int _zf = 0;',source)
            self.assertIn('_zf = ((_fa & _fb) == 0);',source)
            self.assertIn('_zf = (_fa == _fb);',source)
            self.assertNotIn('UNRESOLVED FLAGS',source)
            code='''#include <stdint.h>
uint32_t eax,edx,esp;
#define HI8(x) (((x)>>8)&255)
#define TEST_NZ(a,b) (((a)&(b))!=0)
'''+source+'''
int main(void) {
for(unsigned path=0;path<2;path++) for(unsigned v=0;v<65536;v++) {
 eax=v;edx=path;esp=100;
 unsigned zf=path?(((v>>8)&1)==0):(v==1);
 sub_00010000();
 if(eax!=EXPECTED || esp!=104) return 1;
}
return 0;
}
'''.replace('EXPECTED','zf' if opcode=='74' else '!zf')
            with tempfile.TemporaryDirectory() as tmp:
                p=Path(tmp);(p/'test.c').write_text(code)
                subprocess.run(['cc','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
                subprocess.run([str(p/'test')],check=True)

    def test_unused_mixed_state_does_not_add_snapshots(self):
        image=bytes.fromhex('85d2751283f8019090b800000000c390b801000000c3f6c401ebec')
        base=0x10000
        config._install([config.Section('.text',base,len(image),0,len(image),True)],
                        entry_point=base,kernel_thunk_addr=base,origin='unused-zf-test')
        info={'end':base+len(image),'size':len(image)}
        source=FunctionTranslator(image,{base:info}).translate_function(base,info)
        self.assertNotIn('_zf',source)

    def test_unknown_predecessor_remains_unknown(self):
        op=[Operand(type='reg',reg='eax'),Operand(type='imm',imm=1)]
        self.assertIsNone(_merge_predecessor_flag_states([('cmp',op),('test',op),None]))
        self.assertIsNone(_merge_predecessor_flag_states([('cmp',op),('test',op),('add',op)]))

    def test_other_flag_consumers_rejected(self):
        for cc in ('ja','jb','jbe','jl','jle','js','jo','jp'):
            self.assertIsNone(_make_condition(cc,MERGED_COMPARE_ZF,[]))

if __name__=='__main__':unittest.main()
