#!/usr/bin/env python3
"""state_trace_diff.py's anchor decoder must agree with jsrf_anchor.c.

Run: python3 diagnostics/jsrf_first_fault/test_anchor_decode_matches_c.py \
         <path to jsrf_anchor_test>

The layout is owned by jsrf_anchor.c. state_trace_diff.py restates it, because
a Python tool cannot link a C function -- so there are two readers of one
definition, which is precisely the shape that produced the jump-table bug
earlier today: CFG recovery and the lifter each carried their own idea of what
a jump table looked like, and they drifted.

There it was fixable by making one import the other. Here it is not. So the C
side is made the ORACLE and this compares against it: `jsrf_anchor_test
--vectors` prints (recorded, live, phrase) triples covering each field alone,
both boundary values of every field, carries between adjacent fields, and the
identical case that must produce no phrase at all.

If this fails, Python has drifted from C -- or C changed and Python was not
told, which is the same defect wearing the other hat.
"""

import os
import subprocess
import sys

# A cached .pyc is invalidated on source mtime AND size, so an edit changing
# neither is imported stale -- which happened to this very test on 21 Sep 2026
# and made it report a mismatch that had already been reverted. The drift
# check must read what is on disk.
sys.dont_write_bytecode = True

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from state_trace_diff import anchor_fields  # noqa: E402


def main():
    if len(sys.argv) < 2:
        print("usage: %s <jsrf_anchor_test binary>" % sys.argv[0],
              file=sys.stderr)
        return 2
    exe = sys.argv[1]
    if not os.path.exists(exe):
        print("no such binary: %s" % exe, file=sys.stderr)
        return 2

    out = subprocess.run([exe, "--vectors"], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    if not out:
        print("the C oracle produced no vectors", file=sys.stderr)
        return 1

    failures = 0
    for line in out:
        parts = line.split(" ", 2)
        if len(parts) < 2:
            continue
        a, b = int(parts[0], 16), int(parts[1], 16)
        want = parts[2] if len(parts) > 2 else ""
        got = anchor_fields(a, b)
        if got == want:
            print("  ok   %08x %08x  %s" % (a, b, got or "(no phrase)"))
        else:
            print("  FAIL %08x %08x  python %r != c %r" % (a, b, got, want))
            failures += 1

    print("%d vector(s) checked, %d mismatch(es)" % (len(out), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
