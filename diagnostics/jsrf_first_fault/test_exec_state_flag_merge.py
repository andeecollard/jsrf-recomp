"""Exercise all incoming CMP and TEST edges at the shared JNE."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from backport_exec_state_merge import patch, NEW, OLD, ARMS


def fixture():
    text = ''
    for address in ARMS:
        setter = ('test HI8(eax), 1 (8-bit)' if address < 0x15300
                  else 'cmp MEM32(ebx + 0x10), 7 (32-bit)')
        text += f'loc_{address:08X}: ;\n    /* {setter} */\n'
        if address != 0x153A5:
            text += '    goto loc_000153A9;\n'
    return text + 'loc_000153A9: ;\n' + OLD + '\nloc_000153AB: ;\n'


class ExecStateMergeTest(unittest.TestCase):
    def test_all_incoming_edges(self):
        fixed = patch(fixture())
        functions = []
        for address in ARMS:
            body = re.search(rf'loc_{address:08X}: ;\n(.*?)(?=\nloc_)',fixed,re.S)[1]
            assignment = next(l for l in body.splitlines() if '_flags =' in l)
            functions.append(f'int arm_{address:X}(unsigned _fa,unsigned _fb) {{ int _flags=0;\n'
                             +assignment+'\n'+NEW+'\nreturn 0;\nloc_000153BC:return 1;\n}')
        code = '\n'.join(functions) + '\nint main(void) {\n'
        # Independently specified x86 outcomes: TEST AH,1 sets NZ exactly
        # when bit zero is set, including AH=0x40 and 0x45 from FPU status.
        for address in (0x1520C,0x1527B,0x152BB):
            code += f'for(unsigned ah=0;ah<256;++ah) if(arm_{address:X}(ah,1)!=(ah%2)) return 1;\n'
        code += 'unsigned v[]={0,1,2,3,4,5,6,7,8,0x80000000,0xffffffff};\n'
        for address,imm in zip((0x15311,0x15333,0x15356,0x15375,0x153A5),range(3,8)):
            code += f'for(unsigned i=0;i<11;++i) if(arm_{address:X}(v[i],{imm})!=(v[i]!={imm})) return 2;\n'
        code += 'return 0;}'
        with tempfile.TemporaryDirectory() as t:
            c=Path(t)/'test.c';exe=Path(t)/'test';c.write_text(code)
            subprocess.run([shutil.which('cc'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_idempotence_and_scope(self):
        source=fixture()+'\n/* unrelated */\n'+OLD
        fixed=patch(source)
        self.assertEqual(fixed.count(NEW),1)
        self.assertEqual(fixed.count(OLD),1)
        self.assertEqual(patch(fixed),fixed)
        self.assertEqual(fixed.count('0x153A9 incoming NZ'),8)

    def test_unknown_predecessor_rejected(self):
        with self.assertRaises(ValueError):
            patch(fixture().replace('test HI8(eax), 1 (8-bit)','unknown',1))


if __name__=='__main__':
    unittest.main()
