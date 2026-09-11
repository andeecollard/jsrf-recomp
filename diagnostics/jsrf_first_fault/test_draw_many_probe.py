"""Exercise the diagnostic's checked reads, predicate labels and counters."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class DrawManyProbeTest(unittest.TestCase):
    def test_predicates_and_dispatch_are_distinct(self):
        source = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static uint32_t ram[1024];
void *xbox_GpuMemoryRange(uint32_t a, size_t n) {
    return a <= sizeof ram && n <= sizeof ram-a ? (char *)ram+a : 0;
}
double xbox_TraceSeconds(void) { static double t; return t+=6; }
void jsrf_draw_many_probe(uint32_t,unsigned,uint32_t,uint32_t);
int main(void) {
    uint32_t *o=ram+64, *s=ram+128;
    o[0]=0x300; o[2]=44; ram[0x300/4+3]=0x84200;
    s[5]=1; s[8]=2; s[9]=4; s[10]=8;
    for(unsigned reason=1;reason<=4;reason++) {
        o[1]=reason==1 ? 0 : reason==3 ? 1 : reason==4 ? 11 : 3;
        o[3]=reason==2 ? 0 : 4;
        jsrf_draw_many_probe(reason==4 ? 0x111d0 : 0x111b1,
                             reason==4 ? 2 : 0,0x100,0x200);
    }
    for(unsigned i=0;i<1021;i++) jsrf_draw_many_probe(0x111f9,1,0x100,0x200);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.c').write_text(source)
            subprocess.run(['cc', str(p/'test.c'), str(Path(__file__).with_name('draw_one_probe.c')),
                            '-o', str(p/'test')], check=True)
            disabled=subprocess.run([str(p/'test')],capture_output=True,text=True,
                                    env={k:v for k,v in os.environ.items() if k!='RECOMP_DRAW_MANY_TRACE'})
            self.assertEqual(disabled.stderr,'')
            result=subprocess.run([str(p/'test')],capture_output=True,text=True,
                                  env=dict(os.environ,RECOMP_DRAW_MANY_TRACE='1'),check=True)
            for reason in range(1,5): self.assertIn(f'reason={reason} ',result.stderr)
            self.assertIn('visits=3 dispatch=1021 exclusion_tests=1',result.stderr)
            self.assertIn('invalid=0 overflow=0',result.stderr)
            filtered=subprocess.run([str(p/'test')],capture_output=True,text=True,
                env=dict(os.environ,RECOMP_DRAW_MANY_TRACE='1',RECOMP_DRAW_MANY_ID='45'),check=True)
            self.assertIn('visits=3 dispatch=1021 exclusion_tests=1 rows=0',filtered.stderr)


if __name__=='__main__': unittest.main()
