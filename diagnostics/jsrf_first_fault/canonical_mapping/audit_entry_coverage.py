"""Compare definitions and dispatch coverage without changing either output."""
from pathlib import Path
import re,json
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
BUILD=ROOT/'build-macos/jsrf-first-fault'
def inventory(directory):
 bodies={}
 for p in directory.glob('recomp_*.c'):
  for m in re.finditer(r'void (sub_[0-9A-F]+)\(void\)[ \t\n]*(?:\{[^\n]*\}|\{\n.*?^\})',p.read_text(),re.M|re.S):
   bodies[m[1][4:]]=dict(file=p.name,stub='recomp_stub_ran(' in m[0],body=m[0])
 dispatch=set(re.findall(r'\{ 0x([0-9A-F]+)u,', (directory/'recomp_dispatch.c').read_text()))
 return bodies,dispatch
old,od=inventory(BUILD/'gen');new,nd=inventory(BUILD/'flags-comparison/mixed-zf/gen')
def state(address):
 b=new.get(address)
 return 'absent' if not b else 'stub' if b['stub'] else 'body'
groups={}
for name,path in [('shared_epilogues',BUILD/'shared_epilogues.json'),('midfunction_entries',BUILD/'midfunction_entries.json'),('startup',ROOT/'diagnostics/jsrf_first_fault/startup_entries.json'),('icall',ROOT/'diagnostics/jsrf_first_fault/icall_entries.json')]:
 entries=json.loads(path.read_text());rows=[]
 for e in entries:
  a=e['start'] if isinstance(e,dict) else e
  a=f'{int(a,16):08X}'
  rows.append(dict(address=a,preserved='stub' if old.get(a,{}).get('stub') else 'body' if a in old else 'absent',fresh=state(a),dispatch=a in nd))
 groups[name]=rows
result=dict(preserved_bodies=len(old),fresh_bodies=len(new),
 missing_bodies=sorted(old.keys()-new.keys()),
 regressed_to_stub=sorted(a for a in old.keys()&new.keys() if not old[a]['stub'] and new[a]['stub']),
 missing_dispatch=sorted(od-nd),groups=groups)
refs={}
for target in ('00014881','00014885'):
 refs[target]=[dict(caller=a,stub=b['stub']) for a,b in new.items() if a!=target and re.search(r'\bsub_'+target+r'\b',b['body'])]
result['fragment_source_references']=refs
(HERE/'entry_coverage.json').write_text(json.dumps(result,indent=2)+'\n')
for k in ('preserved_bodies','fresh_bodies','missing_bodies','regressed_to_stub','missing_dispatch'):
 v=result[k];print(k,len(v) if isinstance(v,list) else v)
for k,rows in groups.items():
 print(k,'total',len(rows),'no fresh body',sum(r['fresh']!='body' for r in rows),'missing dispatch',sum(not r['dispatch'] for r in rows))
print('fragment references',refs)
