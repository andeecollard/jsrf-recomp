"""Count conditional branches the translator could not resolve.

When no usable flag state reaches a jcc, the lifter emits the function-local
`_flags` fallback. The translator initialises it to zero, and only the
rep-string and xadd paths ever write it -- so for a jcc it is a constant zero
and the branch is never taken. Arbitrary, not conservative, and silent.

That is not hypothetical. The `jle` at 0x0013D3F3 is reached from two
predecessors that both `cmp` but name different registers; the older translator
discarded the state, the branch could never be taken, the loader recorded status
-1 and JSRF opened JSRF_FATAL.ERR after the anti-graffiti screen.
`_merge_predecessor_flag_states` resolves that shape now, and
`backport_flag_merge.py` applies it to this preserved tree, but 110 sites the
merge cannot reach survive.

Most are in functions with no static caller and no dispatch-table entry, so they
are probably cold. "Probably" is the point: each is a branch that can only go
one way, and the last one that mattered cost a boot. This counts them and
distinguishes a real `_flags` write -- `rep cmpsb`, `scas`, `xadd`, which do set
it -- from the dead fallback.

  --max N   exit 1 if more than N dead branches are found. The build wires this
            as a ratchet at the current count so the number cannot grow
            unnoticed; lower it whenever a fix brings it down.
"""

import argparse
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

FUNC = re.compile(r'^void (sub_([0-9A-Fa-f]+))\(void\)')
COND = re.compile(r'/\* (\w+):')

p = argparse.ArgumentParser(
    description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument("--gen", type=Path,
               default=Path(__file__).resolve().parents[2]
               / "build-macos/jsrf-first-fault/gen")
p.add_argument("--max", type=int, default=None,
               help="exit 1 if more dead branches than this are found")
p.add_argument("--list", action="store_true",
               help="print every site, not just the summary")
a = p.parse_args()

dead = defaultdict(list)
live = 0
for f in sorted(a.gen.glob("recomp_*.c")):
    name = va = None
    written = False          # a real (non-initialiser) write to _flags
    for lineno, line in enumerate(f.read_text().splitlines(), 1):
        m = FUNC.match(line)
        if m:
            name, va, written = m.group(1), int(m.group(2), 16), False
            continue
        if "_flags = " in line and "_flags = 0;" not in line:
            written = True
        if "if (_flags" in line:
            if written:
                live += 1
            else:
                c = COND.search(line)
                dead[(va, name)].append(
                    (f.name, lineno, c.group(1) if c else "?"))

total = sum(len(v) for v in dead.values())
print(f"{a.gen}")
print(f"  _flags branches with a real preceding write (legitimate): {live}")
print(f"  dead fallbacks -- condition is constant zero:              {total}"
      f"  in {len(dead)} function(s)")
if total:
    counts = Counter(c for sites in dead.values() for _, _, c in sites)
    print("  by condition:", ", ".join(f"{k}={v}" for k, v in counts.most_common()))
if a.list:
    for (va, name), sites in sorted(dead.items()):
        for fname, lineno, cond in sites:
            print(f"    0x{va:08X} {name:20s} {fname}:{lineno}  {cond}")

if a.max is not None and total > a.max:
    print(f"FAIL: {total} dead fallbacks, ratchet is {a.max}", file=sys.stderr)
    sys.exit(1)
