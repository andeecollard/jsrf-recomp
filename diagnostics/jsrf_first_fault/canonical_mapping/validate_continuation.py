"""Compile and exercise the actual isolated JSRF continuation output."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
GEN=ROOT/'build-macos/jsrf-first-fault/flags-comparison/continuation/gen/recomp_0000.c'
source=GEN.read_text()
body=re.search(r'void sub_00014870\(void\)\n\{.*?\n\}',source,re.S).group()
assert 'if (CMP_BE(_fa, _fb)) goto loc_00014909;' in body
assert 'UNRESOLVED FLAGS' not in body
code=r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
uint32_t mem[16384];
uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_ebp,g_seh_ebp;
#define MEM32(a) mem[(uint32_t)(a)/4]
#define PUSH32(s,v) do { uint32_t saved=(v); (s)-=4; MEM32(s)=saved; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define RECOMP_ABI_CALL(a,f) abort()
'''+body+r'''
int main(void) {
 for(unsigned count=0;count<=1;count++) {
  memset(mem,0,sizeof(mem));
  ecx=0x1000; esp=0xf000; ebx=0x1234; esi=0x5678; edi=0x9abc;
  MEM32(ecx+0xb0)=count; MEM32(ecx+0x54)=4; MEM32(ecx+0x70)=0x2000;
  MEM32(0x2000+4)=2; MEM32(0x2000+0xc)=1; MEM32(0x2000+0x10)=0x3000;
  MEM32(0x3000)=0x11111111; MEM32(0x3004)=0x22222222; MEM32(0x3008)=0x33333333;
  sub_00014870();
  if(esp!=0xf004 || ecx!=count || ebx!=0x1234 || esi!=0x5678 || edi!=0x9abc) return 1;
  if(MEM32(0x2008)!=count) return 2;
  for(unsigned i=0;i<3;i++)
   if(MEM32(0x300c+i*4)!=(count?MEM32(0x3000+i*4):0)) return 3;
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp); (p/'test.c').write_text(code)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Real generated sub_00014870: empty and one-element paths, stack/register restoration, ASan/UBSan PASS')

# Reuse the memory/register macros with a separately written list/ring model.
prefix=code[:code.index(body)]
matrix=Path(__file__).with_name('continuation_matrix.c').read_text()
matrix=prefix+matrix.replace('/* GENERATED_BODY */',body)
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp); (p/'matrix.c').write_text(matrix)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'matrix.c'),'-o',str(p/'matrix')],check=True)
 subprocess.run([str(p/'matrix')],check=True)
# Prove the matrix detects either forced direction of the historical guard.
for forced in ('0', '1'):
 mutant=matrix.replace('if (CMP_BE(_fa, _fb)) goto loc_00014909;',
                       f'if ({forced}) goto loc_00014909;',1)
 with tempfile.TemporaryDirectory() as tmp:
  p=Path(tmp); (p/'mutant.c').write_text(mutant)
  subprocess.run(['cc',str(p/'mutant.c'),'-o',str(p/'mutant')],check=True)
  result=subprocess.run([str(p/'mutant')],capture_output=True)
  assert result.returncode != 0, f'Forced guard {forced} escaped detection'
print('Negative controls: both forced guard directions detected')
