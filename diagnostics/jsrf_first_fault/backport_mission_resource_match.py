"""Restore the second key comparison in CMission::InitResources' lookup.

JSRF's mission resource lookup walks a candidate list matching a two-word key
against a table entry:

    loc_000523E9  ecx = (index << 5) + table_base
                  cmp  MEM32(eax), MEM32(ecx)      ; key0
                  jne  -> 0x00052404               ; mismatch, next candidate
    loc_000523FC  cmp  ebp, MEM32(ecx + 4)         ; key1
                  je   -> 0x0005240D               ; BOTH match: found

Function discovery promoted the mid-function entry 0x00052402 to a function of
its own -- five bytes, two instructions -- which lands exactly between that
second `cmp` and its `je`. The generated flag temporaries `_fa`/`_fb` are
function-local, so the comparison is computed in the caller and the branch that
consumes it is in a different C function. Nothing carries the flags across, and
the translator emitted the dead `_flags` fallback, which is a constant zero.

The consequence is not a missed optimisation: the `je` is the only edge to
0x0005240D, so the "found" path is unreachable and the lookup can never
succeed. It always walks to the end of the candidate list and reports failure.
The probe at site 32 confirms this executes -- it fired during the tutorial, in
a session where mission-owned objects were not present and the tutorial did not
progress.

Repaired in the caller, where the flags actually exist, rather than by trying
to carry state across the boundary. sub_00052404 is the recovered body of the
not-taken path (`eax = MEM32(eax + 8)`, then 0x00052407), so branching to it
reproduces 0x00052402's fallthrough exactly.

The underlying defect is the split itself: promoting a mid-function entry that
sits between a flag producer and its consumer severs the dataflow. Other sites
of the same shape will exist; this repairs the one shown to execute.
"""
import argparse
from pathlib import Path
import sys

OLD = ('    g_seh_ebp = ebp; sub_00052402(); return;'
       ' /* fallthrough 0x00052402 */')
NEW = ('    /* je 0x0005240D: both key words matched. The flags come from the\n'
       '     * cmp directly above; 0x00052402 is a separate function and\n'
       '     * cannot see them. */\n'
       '    if (CMP_EQ(_fa, _fb)) { g_seh_ebp = ebp; sub_0005240D(); return; }\n'
       '    g_seh_ebp = ebp; sub_00052404(); return;'
       ' /* fallthrough 0x00052404 */')


def patch_all(text):
    return text.replace(OLD, NEW)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--check', action='store_true')
    p.add_argument('--gen', type=Path,
                   default=Path(__file__).resolve().parents[2]
                   / 'build-macos/jsrf-first-fault/gen')
    a = p.parse_args()
    # Every copy, not the first one found. recover_midfunction_entries.py
    # replicates an owner's body from each recovered entry point, so this loop
    # exists once in its own chunk and again in recomp_stubs_unresolved.c for
    # each recovered entry. Patching one leaves the others failing, and a
    # lookup that succeeds down one path and cannot down another is worse than
    # one that consistently fails.
    files = sorted(a.gen.glob('recomp_*.c'))
    total = 0
    missing = 0
    for q in files:
        text = q.read_text()
        n = text.count(OLD)
        if not n and NEW not in text:
            continue
        patched = patch_all(text)
        if a.check:
            if n:
                missing += n
        elif patched != text:
            q.write_text(patched)
        total += n
    if a.check:
        print('mission resource key match: '
              + ('PASS' if not missing else f'MISSING at {missing} site(s)'))
        return 1 if missing else 0
    print(f'mission resource key match restored at {total} site(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
