#!/usr/bin/env python3
"""Where do two runs of the same recording first disagree?

RECOMP_STATE_TRACE=<path> makes the harness write one line per guest frame:

    f<frame> a=<scene anchor> pt=<playtime> ic=<indirect calls> dr=<hw draws>

The pad recorder can say the input was identical and the scene was the same.
It cannot say where two BUILDS parted company. This diffs two traces and
names the first frame each field differs on, which is the earliest execution
difference this runtime can see for free -- and it is usually many seconds
before the symptom, which is the whole point.

READ THE FLOOR FIRST. Two runs of the SAME binary on the same recording do
not produce identical traces: audio threads, kernel timers and the host's
scheduler all move things the title does per frame. Diff those two first;
that is the noise floor, and a cross-build difference is only a finding
where it is EARLIER or LARGER than the floor. The tool prints the running
gap in indirect calls so a slow drift can be told from a step.

    python3 diagnostics/jsrf_first_fault/state_trace_diff.py a.trace b.trace
    python3 diagnostics/jsrf_first_fault/state_trace_diff.py a.trace b.trace \\
        --marks <recording.padrec>      # say which mark is nearest the first step
"""
import argparse
import re
import sys

LINE = re.compile(r"^f(\d+) a=([0-9a-fA-F]+) pt=(\d+) ic=(\d+) dr=(\d+)")
FIELDS = ("a", "pt", "ic", "dr")
NAMES = {"a": "scene anchor", "pt": "playtime", "ic": "indirect calls",
         "dr": "hw draws"}


def load(path):
    rows = {}
    header = {}
    with open(path, errors="ignore") as f:
        for line in f:
            if line.startswith("#!"):
                k, _, v = line[2:].strip().partition(" ")
                header[k] = v
                continue
            m = LINE.match(line)
            if not m:
                continue
            fr = int(m.group(1))
            rows[fr] = {"a": int(m.group(2), 16), "pt": int(m.group(3)),
                        "ic": int(m.group(4)), "dr": int(m.group(5))}
    return rows, header


def marks_of(path):
    out = []
    with open(path, errors="ignore") as f:
        for line in f:
            if line.startswith("#!mark "):
                parts = line[7:].strip().split(" ", 1)
                out.append((int(parts[0]), parts[1] if len(parts) > 1 else ""))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--marks", help="a .padrec whose #!mark lines locate the symptom")
    ap.add_argument("--step", type=int, default=1000,
                    help="an ic gap that GROWS by this much between consecutive"
                         " frames is a step, not drift (default 1000)")
    args = ap.parse_args()

    A, ha = load(args.a)
    B, hb = load(args.b)
    if not A or not B:
        print("empty trace: %s=%d frames, %s=%d frames" % (args.a, len(A), args.b, len(B)))
        return 2
    print("state trace diff")
    print("  a: %s  (%d frames, build %s, gen %s)" % (args.a, len(A), ha.get("build", "?"), ha.get("gen", "?")))
    print("  b: %s  (%d frames, build %s, gen %s)" % (args.b, len(B), hb.get("build", "?"), hb.get("gen", "?")))
    if ha.get("gen") != hb.get("gen"):
        print("  the two runs are on DIFFERENT gens: differences below are translation OR timing")
    else:
        print("  same gen: differences below are timing (this is the floor)")
    common = sorted(set(A) & set(B))
    if not common:
        print("  no common frames")
        return 2
    print("  frames compared: %d (f%d..f%d)" % (len(common), common[0], common[-1]))
    print()

    first = {}
    for fr in common:
        for k in FIELDS:
            if k not in first and A[fr][k] != B[fr][k]:
                first[k] = fr
    for k in FIELDS:
        if k in first:
            fr = first[k]
            print("  %-15s first differs at f%-7d a=%-12d b=%-12d" % (
                NAMES[k], fr, A[fr][k], B[fr][k]))
        else:
            print("  %-15s identical over all %d frames" % (NAMES[k], len(common)))
    print()

    # The indirect-call gap over time: drift or step?
    gaps = [(fr, A[fr]["ic"] - B[fr]["ic"]) for fr in common]
    steps = []
    prev = gaps[0][1]
    for fr, g in gaps[1:]:
        if abs(g - prev) >= args.step:
            steps.append((fr, g - prev))
        prev = g
    last = gaps[-1][1]
    print("  indirect-call gap (a minus b): f%d %+d ... f%d %+d; %d step(s) of >= %d in one frame" % (
        gaps[0][0], gaps[0][1], gaps[-1][0], last, len(steps), args.step))
    for fr, d in steps[:10]:
        print("    step at f%d: %+d calls in one frame" % (fr, d))

    if args.marks:
        ms = marks_of(args.marks)
        if ms:
            pivot = steps[0][0] if steps else (min(first.values()) if first else None)
            if pivot is not None:
                near = min(ms, key=lambda m: abs(m[0] - pivot))
                print()
                print("  nearest mark to the first step (f%d): f%d \"%s\", %+d frames" % (
                    pivot, near[0], near[1], near[0] - pivot))
        else:
            print("  (no #!mark lines in %s)" % args.marks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
