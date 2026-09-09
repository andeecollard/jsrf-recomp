"""Read-only source-shape audit of retained repairs in isolated generation.

Presence is not all-path or runtime equivalence. Keep the excerpts for review.
"""
from pathlib import Path
import json
import re
ROOT=Path(__file__).resolve().parents[3]
GEN=ROOT/'build-macos/jsrf-first-fault/flags-comparison/continuation/gen'
functions={}
for p in GEN.glob('recomp_*.c'):
 for m in re.finditer(r'void (sub_[0-9A-F]+)\(void\)\n\{.*?\n\}',p.read_text(),re.S):
  functions[m[1]]=m[0]
def block(fn,label):
 return re.search(r'loc_'+label+r': ;.*?(?=\nloc_|\n})',functions['sub_'+fn],re.S)[0]
checks=[
 ('empty-list guard via 14870','00014870','00014885','CMP_BE(_fa, _fb)','present on inlined entry path'),
 ('challenge comparison merge','00015130','00015275','CMP_NE(_fa, _fb)','present'),
 ('mixed execution-state NZ','00015130','000153A9','UNRESOLVED FLAGS','MISSING: mixed CMP/TEST'),
 ('shared ZF','000A0F10','000A13A1','UNRESOLVED FLAGS','MISSING: mixed CMP/TEST'),
 ('loader comparison merge',None,'0013D3F3','CMP_LE(_fas, _fbs)','present'),
 ('read-length result merge','001403B0','001404DB','if ((eax != 0))','present'),
 ('seek-position result merge','001403B0','001404F1','if ((edi == 0))','present'),
 ('ADX decrement snapshot',None,'00145181','if ((_fa != 0))','present'),
]
rows=[]
for name,fn,label,needle,status in checks:
 if fn is None:
  candidates=[k[4:] for k,v in functions.items() if 'loc_'+label+': ;' in v]
  assert len(candidates)==1,candidates
  fn=candidates[0]
 excerpt=block(fn,label)
 assert needle in excerpt,(name,excerpt)
 if name=='ADX decrement snapshot':assert 'dec result snapshot' in excerpt
 rows.append(dict(repair=name,function=fn,block=label,status=status,excerpt=excerpt))
for fn,condition in [('0014C850','TEST_NZ(_fa, _fb)'),('0014C870','TEST_Z(_fa, _fb)')]:
 excerpt=functions['sub_'+fn]
 assert f'if ({condition}) {{ fp_top() = fp_st1(); }}' in excerpt
 rows.append(dict(repair='FCMOV',function=fn,status='present',excerpt=excerpt))
assert 'UNRESOLVED FLAGS' in block('00014885','00014885')
rows.append(dict(repair='standalone empty-list fragment',function='00014885',
 status='MISSING for independent entry; path through 14870 repaired',
 excerpt=block('00014885','00014885')))
Path(__file__).with_name('fresh_backport_audit.json').write_text(json.dumps(rows,indent=2)+'\n')
for row in rows:print(row['repair']+': '+row['status'])
