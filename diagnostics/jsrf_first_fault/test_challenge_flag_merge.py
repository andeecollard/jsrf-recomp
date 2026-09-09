"""Execute the repaired branch with both incoming CMP snapshots."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from backport_challenge_flag_merge import patch, NEW, OLD


class ChallengeMergeTest(unittest.TestCase):
    def test_both_predecessors_and_boundary_values(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        # Compile the exact replacement, including its target. Both sides of
        # each comparison must work; hardcoding either 1 or 2 is incorrect.
        code = '''#include <stdint.h>
#define CMP_NE(a,b) ((a)!=(b))
static int branch(uint32_t _fa, uint32_t _fb) {
''' + NEW + '''
return 0;
loc_000153BC: return 1;
}
int main(void) {
uint32_t values[] = {0,1,2,3,0x7fffffff,0x80000000,0xffffffff};
for (unsigned arm=1; arm<=2; ++arm)
 for (unsigned i=0; i<sizeof(values)/sizeof(values[0]); ++i)
  if (branch(values[i],arm) != (values[i]!=arm)) return 1;
return 0;
}
'''
        with tempfile.TemporaryDirectory() as t:
            c=Path(t)/'test.c'; exe=Path(t)/'test'; c.write_text(code)
            subprocess.run([compiler,str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_backport_is_scoped_and_idempotent(self):
        text = ('loc_00015275: ;\n' + OLD + '\nloc_0001527B: ;\n'
                'loc_000153A9: ;\n' + OLD)
        changed = patch(text)
        self.assertEqual(changed.count(NEW),1)
        self.assertEqual(changed.count(OLD),1)
        self.assertEqual(patch(changed),changed)

    def test_changed_input_is_rejected(self):
        with self.assertRaises(ValueError):
            patch('loc_00015275: ;\nreturn;\nloc_0001527B: ;')


if __name__ == '__main__':
    unittest.main()
