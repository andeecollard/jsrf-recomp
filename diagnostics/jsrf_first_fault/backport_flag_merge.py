"""Backport the cross-block flag-merge fix into the preserved JSRF tree.

The loader at 0x0013D3D2 reaches one ``jle`` from two predecessor blocks.
Each predecessor performs a 32-bit ``cmp`` and writes the lifter's shared
``_fa/_fb/_fas/_fbs`` snapshot, but the older translator discarded the flag
state because the compares named different registers.  Its fallback ``_flags``
local is initialized to zero and never written, so the ``jle`` was never taken
and the title recorded loader status -1, eventually opening JSRF_FATAL.ERR.

The source translator now merges compatible snapshots and its regression lives
in ``tools/recomp/test_lifter_flag_merge.py``.  A full regeneration is avoided
here because this generated checkout contains recovered mid-function entries.
This idempotent script applies exactly the statement the fixed lifter emits.
Use ``--check`` for the build-tree regression gate.
"""

import argparse
import sys
from pathlib import Path


OLD = ("if (_flags /* jle: less or equal (signed <=) */) "
       "goto loc_0013D3FB;")
NEW = ("if (CMP_LE(_fas, _fbs)) goto loc_0013D3FB; "
       "/* jle: less or equal (signed <=) */")


p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument("--check", action="store_true",
               help="report without writing; exit 1 if the site is unfixed")
p.add_argument("--gen", type=Path,
               default=Path(__file__).resolve().parents[2]
               / "build-macos/jsrf-first-fault/gen")
a = p.parse_args()

f = a.gen / "recomp_0006.c"
s = f.read_text()
if s.count(NEW) == 1:
    print("0x0013D3F3: already fixed")
    sys.exit(0)
if s.count(OLD) != 1:
    sys.exit(f"0x0013D3F3: expected exactly one unfixed statement in {f}, "
             f"found {s.count(OLD)}")
if a.check:
    print("0x0013D3F3: unfixed", file=sys.stderr)
    sys.exit(1)

f.write_text(s.replace(OLD, NEW))
print(f"0x0013D3F3: fixed in {f}")
