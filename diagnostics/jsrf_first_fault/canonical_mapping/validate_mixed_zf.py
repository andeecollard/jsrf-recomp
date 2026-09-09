"""Execute actual snapshot/branch snippets for the ten audited incoming edges."""
from pathlib import Path
import re, subprocess, tempfile
HERE=Path(__file__).resolve().parent
GEN=HERE.parents[2]/'build-macos/jsrf-first-fault/flags-comparison/mixed-zf/gen'
text='\n'.join(p.read_text() for p in GEN.glob('recomp_*.c'))
def block(label):
 return re.search(r'loc_'+label+r': ;.*?(?=\nloc_|\n})',text,re.S)[0]
edges=[('0001520C','000153A9','((v>>8)&1)!=0'),
 ('0001527B','000153A9','((v>>8)&1)!=0'),('000152BB','000153A9','((v>>8)&1)!=0')]
edges += [(a,'000153A9',f'v!={n}') for a,n in zip(
 ['00015311','00015333','00015356','00015375','000153A5'],range(3,8))]
edges += [('000A1385','000A13A1','v==w'),('000A139C','000A13A1','(v&0x80000)==0')]
functions=[];checks=[];evidence=[]
for i,(arm,join,expected) in enumerate(edges):
 source=block(arm);lines=source.splitlines()
 start=max(j for j,line in enumerate(lines) if line.strip().startswith('_fa ='))
 end=next(j for j in range(start,len(lines)) if lines[j].strip().startswith('_zf ='))
 snapshot='\n'.join(lines[start:end+1])
 branch=next(line for line in block(join).splitlines() if 'if (' in line)
 target=re.search(r'goto (loc_\w+);',branch)[1]
 functions.append(f'''static int edge{i}(uint32_t v,uint32_t w) {{
 uint32_t eax=v,edi=w,ebx=0,esi=0,_fa=0,_fb=0; int32_t _fas=0,_fbs=0; int _zf=0;
 {snapshot}
 {branch}
 return 0; {target}: return 1;
 }}''')
 checks.append(f'if(edge{i}(v,w)!=({expected})) return {i+1};')
 evidence.append(f'{arm} -> {join}\n{snapshot}\n{branch}')
code='''#include <stdint.h>
#define HI8(x) (((x)>>8)&255)
#define MEM32(a) (v)
'''+ '\n'.join(functions)+'''
int main(void) {
 uint32_t edge[]={0,1,2,3,4,5,6,7,8,0x40,0x45,0x80000,0x7fffffff,0x80000000,0xffffffff};
 for(unsigned x=0;x<65551;x++) {
 uint32_t v=x<65536?x:edge[x-65536];
 for(unsigned y=0;y<15;y++) {uint32_t w=edge[y];
'''+ '\n'.join(checks)+'\n}}return 0;}\n'
(HERE/'mixed_zf_edge_evidence.txt').write_text('\n\n'.join(evidence)+'\n')
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['cc','-Wno-parentheses-equality','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 # CMP-only handling of TEST is the known bad historical alternative.
 bad=code.replace('_zf = ((_fa & _fb) == 0);','_zf = (_fa == _fb);')
 assert bad!=code
 (p/'bad.c').write_text(bad)
 subprocess.run(['cc','-Wno-parentheses-equality',str(p/'bad.c'),'-o',str(p/'bad')],check=True)
 assert subprocess.run([str(p/'bad')],capture_output=True).returncode!=0
print('PASS: 10 actual emitted edges, 9,832,650 predicate checks; CMP-only negative control detected')
