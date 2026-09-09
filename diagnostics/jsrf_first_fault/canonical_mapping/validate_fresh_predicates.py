"""Execute the nine newly resolved conditions against x86 CF/ZF outcomes."""
from pathlib import Path
import json,re,subprocess,tempfile,shutil
here=Path(__file__).resolve().parent
root=here.parents[2]/'build-macos/jsrf-first-fault/flags-comparison/current/gen'
data=json.loads((here/'fresh_flags_comparison.json').read_text())
header=(root/'recomp_types.h').read_text()
macros='\n'.join(l for l in header.splitlines() if re.match(r'#define (CMP_EQ|CMP_NE|CMP_B|CMP_AE|CMP_BE|TEST_Z)\(',l))
functions=[];checks=[]
for index,entry in enumerate(data['resolved']):
    # Balanced parentheses extract the actual condition emitted by the translator.
    line=entry['after'];start=line.index('(');depth=0
    for end in range(start,len(line)):
        depth += (line[end]=='(')-(line[end]==')')
        if depth==0:break
    expr=line[start+1:end]
    functions.append(f'static int branch{index}(uint32_t _fa,uint32_t _fb) {{return !!({expr});}}')
    mnemonic=re.search(r'/\* (\w+):',line)[1]
    is_test=entry['site'].startswith(('sub_00056990:00056B7D:','sub_001609E0:'))
    # Reference flags model for 32-bit CMP or TEST, independently selected
    # from the inspected incoming instructions, not from the emitted macro.
    flags='unsigned cf=0,zf=((a & b)==0);' if is_test else 'unsigned cf=(a<b),zf=((uint32_t)(a-b)==0);'
    expected={'jne':'!zf','je':'zf','jb':'cf','jae':'!cf','jbe':'cf || zf'}[mnemonic]
    checks.append(f'{{{flags} if(branch{index}(a,b)!=!!({expected})) return {index+1};}}')
code='#include <stdint.h>\n'+macros+'\n'+'\n'.join(functions)+'''
int main(void) {
uint32_t edge[]={0,1,2,3,7,9,10,11,0x42b,0x45f,0x494,0x4be,0x7fffffff,0x80000000,0xffffffff};
for(unsigned x=0;x<271;++x) for(unsigned y=0;y<271;++y) {
uint32_t a=x<256?x:edge[x-256],b=y<256?y:edge[y-256];
'''+ '\n'.join(checks)+'\n} return 0;}\n'
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'predicates.c';exe=Path(tmp)/'predicates';c.write_text(code)
    subprocess.run([shutil.which('cc'),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print(f'PASS: {len(checks)} emitted predicates, 271 x 271 input pairs each')
