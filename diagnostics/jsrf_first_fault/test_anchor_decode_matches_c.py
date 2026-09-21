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

HERE = os.path.dirname(os.path.abspath(__file__))
DECODER = os.path.join(HERE, "state_trace_diff.py")


def load_anchor_fields():
    """Compile state_trace_diff.py from source, every time.

    NOT `import state_trace_diff`. A cached .pyc is invalidated on source
    mtime AND size, so an edit that changes neither is imported stale -- and
    that happened to THIS test on 21 Sep 2026: reverting a one-character
    perturbation (0x1F back to 0x0F) kept both, Python served the cached
    bytecode, and the drift check went on reporting a mismatch that had
    already been fixed.

    `-B` and PYTHONDONTWRITEBYTECODE were the first attempt and are not
    enough: they stop this run WRITING bytecode, not READING what an earlier
    run left behind. Reading the file and compiling it takes the cache out of
    the path altogether, which is the only version of this that cannot be
    fooled by a same-size same-second edit.

    __name__ is set to something other than "__main__" so the module's own
    command-line entry point does not run on import.
    """
    with open(DECODER) as f:
        src = f.read()
    ns = {"__name__": "state_trace_diff_under_test", "__file__": DECODER}
    exec(compile(src, DECODER, "exec"), ns)
    return ns["anchor_fields"]


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

    anchor_fields = load_anchor_fields()
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
