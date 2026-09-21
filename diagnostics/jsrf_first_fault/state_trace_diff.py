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
that is the same-build divergence baseline, and a cross-build difference is
only a finding where it is EARLIER or LARGER than that. "Same build" does not
make a difference timing -- it narrows the candidates and no more; the
transition table is what separates skew from divergent control flow. The tool
prints the running
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
# The anchor packs four fields into one word. Printing it as a decimal --
# "a=218103808 b=201326592" -- is the same unreadability the replay's
# misalignment line used to have, and the answer there was to name the field
# that moved. THE LAYOUT LIVES IN jsrf_anchor.c; this is a second reader of it
# and will not be linked against it, so if that packing changes, change this
# too. Field order matches the C describer deliberately.
ANCHOR_FIELDS = (("sequence", 24, 0xFF), ("chapter", 20, 0x0F),
                 ("mission", 16, 0x0F), ("minutes", 0, 0xFFFF))


def sq_transitions(rows, frames):
    """(frame, state) for each change in the scene index, in order.

    Taken from the ANCHOR's top byte rather than the line's own sq= field:
    the parser already decodes the anchor, it is the same value by
    construction (both are jsrf_seq_index), and using one source keeps this
    tied to ANCHOR_FIELDS above. 0xFF is "the object did not validate", which
    is not a state and must not read as a transition into one.
    """
    shift, mask = ANCHOR_FIELDS[0][1], ANCHOR_FIELDS[0][2]
    out, last = [], None
    for fr in frames:
        s = (rows[fr]["a"] >> shift) & mask
        if s == 0xFF or s == last:
            continue
        out.append((fr, s))
        last = s
    return out


def classify_ic_gap(A, B, frames):
    """Phase noise or a real step? The sign is what tells them apart."""
    gaps = [A[f]["ic"] - B[f]["ic"] for f in frames]
    if not gaps:
        return
    neg, pos = min(gaps), max(gaps)
    zeros = sum(1 for g in gaps if g == 0)
    crosses = neg < 0 < pos
    print("  indirect-call gap shape: range %d..%d, %d frame(s) at exactly 0"
          % (neg, pos, zeros))
    if crosses or zeros:
        print("    crosses zero -- SAMPLING PHASE, not a divergence. A"
              " thread-global counter read at frame boundaries cannot be"
              " compared frame to frame; do not chase the first differing"
              " frame.")
    else:
        print("    never crosses zero -- a persistent offset, which IS worth"
              " investigating: one run made calls the other did not.")


def report_transitions(A, B, frames):
    """Separate a uniform time shift from divergent control flow.

    THIS IS THE CHECK THAT LICENSES THE WORD "TIMING". Two runs of one build
    disagreeing on a per-frame counter says only that they disagree. If the
    scene states occur in the SAME ORDER with the SAME VALUES and the frame
    offsets are uniform, a scheduling skew explains everything and nothing
    took a different path. A missing, extra or reordered state cannot be
    explained that way and is a real finding.
    """
    ta, tb = sq_transitions(A, frames), sq_transitions(B, frames)
    sa, sb = [s for _, s in ta], [s for _, s in tb]
    print("  scene transitions: a=%d b=%d" % (len(ta), len(tb)))
    if sa != sb:
        print("    ORDER OR CONTENT DIFFERS -- not a time shift, investigate")
        print("      a: %s" % sa)
        print("      b: %s" % sb)
        return
    print("    same states in the same order")
    offs = []
    print("    %-7s %-9s %-9s %s" % ("state", "a frame", "b frame", "a-b"))
    for (fa, st), (fb, _) in zip(ta, tb):
        offs.append(fa - fb)
        print("    %-7d %-9d %-9d %+d" % (st, fa, fb, fa - fb))
    nz = [o for o in offs if o]
    if not nz:
        print("    every transition on the same frame")
    elif len(set(nz)) == 1:
        print("    a single uniform shift of %+d frames, from state %d onward"
              " -- consistent with skew, not with a different path taken"
              % (nz[0], ta[offs.index(nz[0])][1]))
    else:
        print("    offsets are NOT uniform (%s) -- skew alone does not explain"
              " this" % sorted(set(nz)))


def anchor_fields(a, b):
    """Name only the anchor fields that differ, as jsrf_anchor.c does."""
    parts = []
    for name, shift, mask in ANCHOR_FIELDS:
        x, y = (a >> shift) & mask, (b >> shift) & mask
        if x != y:
            slow = " (slow counter)" if name == "minutes" else ""
            parts.append("%s %d->%d%s" % (name, x, y, slow))
    if not parts and a != b:
        return "no named field (layout mismatch?)"
    return ", ".join(parts)


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
        # NOT "these are timing". Same build narrows the cause; it does not
        # establish one. Scheduling is the likely explanation, but uninitialised
        # state, an address-dependent branch, or any other nondeterminism looks
        # identical here. The transition table below is what distinguishes them:
        # same states in the same order, uniformly shifted, is consistent with
        # skew; missing, extra or reordered states is not, and is a finding.
        print("  same gen: same-build divergence; cause UNCLASSIFIED"
              " (see the transition table)")
    common = sorted(set(A) & set(B))
    if not common:
        print("  no common frames")
        return 2
    print("  frames compared: %d (f%d..f%d)" % (len(common), common[0], common[-1]))
    report_transitions(A, B, common)
    classify_ic_gap(A, B, common)
    print()

    first = {}
    for fr in common:
        for k in FIELDS:
            if k not in first and A[fr][k] != B[fr][k]:
                first[k] = fr
    for k in FIELDS:
        if k in first:
            fr = first[k]
            extra = ""
            if k == "ic":
                # "first differs at f1" is NOT a divergence, and reporting it
                # like one sent an investigation after it on 21 Sep 2026.
                #
                # g_icall_count is a SINGLE GLOBAL with no thread-local
                # qualifier, incremented by every guest thread (five were live
                # in these runs) and sampled on the presenting thread at each
                # frame boundary. It is never reset, so a frame's value is
                # "every indirect call any thread has made since process
                # start, as of whenever this thread looked". Two runs sampling
                # at slightly different points in the same work produce a
                # different number without anything having taken a different
                # path.
                #
                # The frame it FIRST differs on is therefore meaningless. What
                # carries information is the SHAPE of the gap, below: one that
                # changes sign or returns to zero is phase, and one that steps
                # once and stays is a candidate.
                extra = "  [thread-global, sampled -- see the gap shape]"
            if k == "a":
                what = anchor_fields(A[fr][k], B[fr][k])
                if what:
                    extra = "  [%s]" % what
            print("  %-15s first differs at f%-7d a=%-12d b=%-12d%s" % (
                NAMES[k], fr, A[fr][k], B[fr][k], extra))
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
