"""Compare isolated continuation and mixed-ZF translations without mutations."""
from pathlib import Path
import json
from compare_fresh_flags import scan
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]/'build-macos/jsrf-first-fault/flags-comparison'
before=scan(ROOT/'continuation/gen'); after=scan(ROOT/'mixed-zf/gen')
assert before and after
changed=[dict(site=k,before=before[k],after=after[k]) for k in sorted(before.keys()&after.keys()) if before[k]!=after[k]]
unknown=lambda s:'UNRESOLVED FLAGS' in s
result=dict(before_unknown=sum(map(unknown,before.values())),after_unknown=sum(map(unknown,after.values())),
 resolved=[v for v in changed if unknown(v['before']) and not unknown(v['after'])],
 newly_unknown=[v for v in changed if not unknown(v['before']) and unknown(v['after'])],
 other_changes=[v for v in changed if unknown(v['before'])==unknown(v['after'])],
 before_only=sorted(before.keys()-after.keys()),after_only=sorted(after.keys()-before.keys()))
(HERE/'mixed_zf_comparison.json').write_text(json.dumps(result,indent=2)+'\n')
for k,v in result.items():print(k,len(v) if isinstance(v,list) else v)
for row in result['resolved']:print(row['site'],row['after'])

# Include non-branch source changes so unused snapshots cannot hide in totals.
import re
def bodies(directory):
 result={}
 for p in directory.glob('recomp_*.c'):
  for match in re.finditer(r'void (sub_[0-9A-F]+)\(void\)\n\{.*?\n\}',p.read_text(),re.S):
   result[match[1]]=match[0]
 return result
b=bodies(ROOT/'continuation/gen');a=bodies(ROOT/'mixed-zf/gen')
result['changed_bodies']=[k for k in sorted(b.keys()&a.keys()) if b[k]!=a[k]]
result['continuation_body_unchanged']=a['sub_00014870']==b['sub_00014870']
(HERE/'mixed_zf_comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print('changed_bodies',result['changed_bodies'])
print('continuation_body_unchanged',result['continuation_body_unchanged'])
