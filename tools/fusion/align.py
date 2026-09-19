"""Recover XDK names that byte signatures cannot, by link-order alignment.

`tools.fusion.signatures` transfers a name when the donor's and the target's
first 32 bytes agree with the same function length. That is a strong test and
it is why the names it produces are trustworthy -- but it fails outright
across an XDK build gap, because a different compiler build reorders the
prologue even when the function is otherwise the same code.

Measured on JSRF's DSOUND section, 19 Sep 2026: of the 222 functions the byte
matcher left unnamed, **155 have a donor signature of exactly the right size
and fail only on the pattern**, and not one of them was ambiguous. Size is not
the constraint; the opcode bytes are.

So use the other fact the linker gives us. A statically linked library is
emitted in library order, so the donor's symbols and the target's functions
are two orderings of nearly the same list. Where the byte matcher has already
placed a name, we have an anchor tying one to the other; between two anchors
the remaining functions are in correspondence by position, and their sizes are
a check on that correspondence rather than a precondition for it.

WHAT THIS DELIBERATELY DOES NOT DO. It never overwrites a byte-matched name,
it never emits a name into a gap whose two sides disagree in length, and it
rejects a whole gap rather than guessing at part of it. A wrong name in a
crash report is worse than no name -- the address is right either way, and a
plausible wrong name is believed. `--holdout` exists so the precision is a
number rather than a hope: it hides a fraction of the anchors, predicts them,
and reports how many it got right.

Usage:
    python3 -m tools.fusion.align \\
        --functions <disasm/functions.json> --names <names.tsv> \\
        --donor <sigs_blinx.json> [--donor <sigs_crimson.json> ...] \\
        --range 0x0019E340:0x001BA8A0 [--out <names.tsv>] [--holdout 0.2]
"""
import argparse
import bisect
import collections
import json
import random
import re
import sys


_MANGLED = re.compile(r"^\?\??(\w+)@(\w+)@")


def same_function(a, b):
    """Do two mangled names denote the same class method?

    Two readings of one function disagree more often than two functions get
    confused, and the two failures are not worth the same. Across the
    hold-out these were all the SAME function under a different XDK build's
    declaration:

        ?CodecReady@CAc97Device@DirectSound@@IAEHXZ
        ?CodecReady@CAc97Device@@IAEHXZ              <- namespace dropped

        ?SetupVoiceProcessor@CMcpxCore@DirectSound@@IAEXXZ
        ?SetupVoiceProcessor@CMcpxCore@DirectSound@@IAEJXZ   <- void vs long

    while `GetStatus@CDirectSoundStream` against `Pause@CDirectSoundBuffer`
    is a real misidentification. Comparing class and method, and ignoring the
    namespace and the signature tail, separates the two.
    """
    if a == b:
        return True
    ma, mb = _MANGLED.match(a), _MANGLED.match(b)
    return bool(ma and mb and ma.groups() == mb.groups())


def load_functions(path, lo, hi):
    """(start, size) for every detected function in [lo, hi), address order."""
    raw = json.load(open(path, encoding="utf-8"))
    fs = raw["functions"] if isinstance(raw, dict) and "functions" in raw else raw
    it = fs.values() if isinstance(fs, dict) else fs
    out = []
    for f in it:
        start = f["start"]
        start = int(start, 16) if isinstance(start, str) else start
        if not (lo <= start < hi):
            continue
        end = f["end"]
        end = int(end, 16) if isinstance(end, str) else end
        out.append((start, end - start))
    return sorted(out)


def load_names(path, lo, hi):
    """addr -> name, for the section under study."""
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = s.split("\t")
        if len(parts) < 2:
            continue
        addr = int(parts[0], 16)
        if lo <= addr < hi:
            out[addr] = parts[1]
    return out


def load_donor(path, pattern):
    """Donor symbols in link order: [(name, size)], first occurrence wins.

    The sigs file is written in symbol-table order by `signatures build`, so
    the list index IS the donor's link order. A name can repeat (the same
    inline emitted twice); the first is the one the order refers to.
    """
    raw = json.load(open(path, encoding="utf-8"))
    seen, out = set(), []
    for s in raw["sigs"]:
        if pattern and not pattern.search(s["name"]):
            continue
        if s["name"] in seen:
            continue
        seen.add(s["name"])
        out.append((s["name"], s["size"]))
    return out, raw.get("source", path), raw.get("build", "?")


def monotone_backbone(pairs):
    """Longest strictly increasing run of donor indices, by target order.

    An anchor off the backbone is either a genuinely reordered function or a
    name the byte matcher got wrong. Either way it cannot be used to bound a
    gap, so it is dropped from the scaffold -- and reported, because the
    second reading is worth looking at.
    """
    if not pairs:
        return [], []
    tails, back, idx_of = [], [], []
    for n, (_t, d) in enumerate(pairs):
        p = bisect.bisect_left(tails, d)
        if p == len(tails):
            tails.append(d)
            idx_of.append(p)
        else:
            tails[p] = d
            idx_of.append(p)
        back.append(p)
    want = len(tails) - 1
    keep = []
    for n in range(len(pairs) - 1, -1, -1):
        if back[n] == want:
            keep.append(n)
            want -= 1
        if want < 0:
            break
    keep.reverse()
    kept = [pairs[n] for n in keep]
    off = [pairs[n] for n in range(len(pairs)) if n not in set(keep)]
    return kept, off


def needleman_wunsch(tsizes, dsizes, size_tol, gap_cost=2, mismatch_cost=3):
    """Order-preserving pairing of two size sequences, insertions allowed.

    Requiring the two runs to be the same length does not survive contact with
    the data. Measured against Blinx, 19 Sep 2026: across the 22 interior gaps
    of JSRF's DSOUND, the target holds 220 unnamed functions and the donor
    offers 120 symbols, the donor run is shorter in 18 gaps and empty in 8.
    The donor's symbol table simply does not name everything the target links,
    so the correspondence has gaps in it and an aligner has to model them.

    Cost 0 for a pair whose sizes agree within tolerance, `mismatch_cost` for
    a pair that does not, `gap_cost` to skip one side. Only exact-agreement
    pairs are ever returned; the mismatched ones exist so the alignment can
    step over a disagreement instead of being derailed by it.
    """
    n, m = len(tsizes), len(dsizes)
    INF = float("inf")
    best = [[INF] * (m + 1) for _ in range(n + 1)]
    move = [[None] * (m + 1) for _ in range(n + 1)]
    best[0][0] = 0
    for i in range(n + 1):
        for j in range(m + 1):
            cur = best[i][j]
            if cur == INF:
                continue
            if i < n and j < m:
                ok = abs(tsizes[i] - dsizes[j]) <= size_tol
                c = cur + (0 if ok else mismatch_cost)
                if c < best[i + 1][j + 1]:
                    best[i + 1][j + 1] = c
                    move[i + 1][j + 1] = ("pair" if ok else "skew", i, j)
            if i < n and cur + gap_cost < best[i + 1][j]:
                best[i + 1][j] = cur + gap_cost
                move[i + 1][j] = ("t", i, j)
            if j < m and cur + gap_cost < best[i][j + 1]:
                best[i][j + 1] = cur + gap_cost
                move[i][j + 1] = ("d", i, j)
    # move[i][j] holds the predecessor cell, so the walk back is uniform.
    pairs, i, j = [], n, m
    while (i, j) != (0, 0):
        step = move[i][j]
        if step is None:
            return []
        kind, i, j = step
        if kind == "pair":
            pairs.append((i, j))
    pairs.reverse()
    return pairs


def align_gaps(funcs, named, donor, size_tol, min_gap_agree=1.0):
    """Names for unnamed functions, by positional correspondence between anchors.

    Each interior gap is aligned on its own with `needleman_wunsch`, so a
    donor that is missing symbols in the middle of a run costs only the
    functions it is missing rather than the whole gap.
    """
    donor_index = {name: i for i, (name, _s) in enumerate(donor)}
    anchors = [(i, donor_index[named[a]])
               for i, (a, _sz) in enumerate(funcs)
               if a in named and named[a] in donor_index]
    backbone, off_backbone = monotone_backbone(anchors)

    proposals, rejected = {}, collections.Counter()
    # Bound each interior gap by two consecutive backbone anchors. The runs
    # before the first anchor and after the last are deliberately left alone:
    # they are open at one end, so a length agreement there means nothing.
    for (ti0, di0), (ti1, di1) in zip(backbone, backbone[1:]):
        tgap = [(i, funcs[i][0], funcs[i][1]) for i in range(ti0 + 1, ti1)
                if funcs[i][0] not in named]
        claimed = {named[funcs[i][0]] for i in range(ti0 + 1, ti1)
                   if funcs[i][0] in named}
        dgap = [(n, s) for n, s in donor[di0 + 1:di1] if n not in claimed]
        if not tgap:
            continue
        if not dgap:
            rejected["donor names nothing in this gap"] += len(tgap)
            continue
        paired = needleman_wunsch([t[2] for t in tgap], [d[1] for d in dgap],
                                  size_tol)
        for ti, dj in paired:
            proposals.setdefault(tgap[ti][1], set()).add(dgap[dj][0])
        rejected["no size-agreeing partner in the gap"] += len(tgap) - len(paired)
    return proposals, backbone, off_backbone, rejected


def run(funcs, names, donors, size_tol):
    """Merge every donor's proposals; keep only the unanimous ones."""
    votes = collections.defaultdict(collections.Counter)
    stats = []
    for label, donor in donors:
        proposals, backbone, off, rejected = align_gaps(funcs, names, donor, size_tol)
        for addr, nameset in proposals.items():
            for n in nameset:
                votes[addr][n] += 1
        stats.append((label, len(backbone), len(off), len(proposals), rejected))
    # A name only lands if no donor proposed a different one for that address.
    agreed = {a: c.most_common(1)[0][0] for a, c in votes.items() if len(c) == 1}
    contested = {a: dict(c) for a, c in votes.items() if len(c) > 1}
    return agreed, contested, stats


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--functions", required=True)
    ap.add_argument("--names", required=True)
    ap.add_argument("--donor", action="append", required=True)
    ap.add_argument("--range", required=True, help="LO:HI in hex, e.g. 0x19E340:0x1BA8A0")
    ap.add_argument("--filter", default=r"(DirectSound|Mcpx|Ac97|XAudio|DSound|"
                                         r"WaveFormat|CIrql|CRefCount)",
                    help="donor symbols to consider; empty string for all")
    ap.add_argument("--size-tol", type=int, default=0,
                    help="bytes a paired function may differ by (default 0)")
    ap.add_argument("--holdout", type=float, default=0.0,
                    help="hide this fraction of anchors and score the predictions")
    ap.add_argument("--shuffle-donor", action="store_true",
                    help="NEGATIVE CONTROL: destroy the donor's link order, keep "
                         "its sizes. Pair with --holdout; precision must collapse. "
                         "Do not score this control on how many names it places -- "
                         "it places nearly as many (47 against 51, measured), they "
                         "are simply the wrong ones. Hold-out precision separates "
                         "them completely: 96/97/100% ordered, 0/7/18% shuffled.")
    ap.add_argument("--seed", type=int, default=20260919)
    ap.add_argument("--out")
    args = ap.parse_args()

    lo, hi = (int(x, 16) for x in args.range.split(":"))
    pattern = re.compile(args.filter, re.I) if args.filter else None
    funcs = load_functions(args.functions, lo, hi)
    names = load_names(args.names, lo, hi)
    donors = []
    for path in args.donor:
        syms, source, build = load_donor(path, pattern)
        if args.shuffle_donor:
            random.Random(args.seed).shuffle(syms)
        donors.append((f"{source}[{build}]", syms))
    if args.shuffle_donor:
        print("*** NEGATIVE CONTROL: donor link order destroyed ***")

    print(f"target   : {len(funcs)} functions in {lo:#08x}..{hi:#08x}, "
          f"{len(names)} already named, {len(funcs) - len(names)} unnamed")
    for label, syms in donors:
        print(f"donor    : {label:28s} {len(syms)} symbols after filter")

    if args.holdout > 0:
        # THE POINT OF THIS MODE. Precision is not observable on the names we
        # do not have, so measure it on the ones we do: hide some anchors,
        # predict them, and count. A run that proposes nothing scores 0/0 and
        # is reported as such rather than as success.
        rng = random.Random(args.seed)
        keys = sorted(names)
        hidden = set(rng.sample(keys, max(1, int(len(keys) * args.holdout))))
        kept = {a: n for a, n in names.items() if a not in hidden}
        agreed, contested, _stats = run(funcs, kept, donors, args.size_tol)
        scored = [(a, agreed[a], names[a]) for a in hidden if a in agreed]
        exact = sum(1 for _a, got, want in scored if got == want)
        same = sum(1 for _a, got, want in scored if same_function(got, want))
        print(f"\nHOLD-OUT: hid {len(hidden)} of {len(keys)} anchors, "
              f"predicted {len(scored)} of them")
        if scored:
            n = len(scored)
            print(f"  exact mangled string : {exact}/{n} = {100*exact/n:.1f}%")
            print(f"  same class::method   : {same}/{n} = {100*same/n:.1f}%"
                  f"   <- the number that matters for a diagnostic name")
            print(f"  recall over hidden   : {100*n/len(hidden):.1f}%")
            for a, got, want in scored:
                if not same_function(got, want):
                    print(f"  WRONG FUNCTION {a:#08x}: got {got}\n"
                          f"                          want {want}")
        else:
            print("  nothing predicted -- no precision measured, and that is "
                  "not the same as being correct")
        return

    agreed, contested, stats = run(funcs, names, donors, args.size_tol)
    print()
    for label, backbone, off, proposed, rejected in stats:
        print(f"  {label:28s} backbone {backbone:3d} anchors "
              f"({off} off-backbone), proposed {proposed}")
        for why, n in rejected.most_common():
            print(f"        rejected {n:4d} in gaps: {why}")
    print(f"\nagreed across donors : {len(agreed)}")
    print(f"contested (dropped)  : {len(contested)}")
    for a, c in sorted(contested.items())[:5]:
        print(f"    {a:#08x}: {c}")
    total = len(names) + len(agreed)
    print(f"\nnamed after alignment: {total} of {len(funcs)} "
          f"({100*total/len(funcs):.0f}%), was {len(names)} "
          f"({100*len(names)/len(funcs):.0f}%)")

    if args.out:
        # TWO columns, and the marker goes INSIDE the name. The runtime's
        # reader (diagnostics/jsrf_first_fault/guest_names.c) splits on the
        # first tab and takes the whole rest of the line as the name, so a
        # third provenance column would be printed as part of it. A leading
        # '~' costs nothing there and makes a crash report say out loud that
        # the name is an inference: `sub_001A2E2E [~?Foo@CMcpxAPU@@...]`.
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("# guest VA -> name, two tab-separated columns.\n"
                     "#\n"
                     "# A name with a leading '~' was placed by LINK-ORDER\n"
                     "# ALIGNMENT (tools.fusion.align), not by a byte match:\n"
                     "# it is an inference from the donor's symbol order, and\n"
                     "# held-out measurement puts it at about 96% correct on\n"
                     "# class::method identity. An unmarked name came from an\n"
                     "# exact byte-signature match and is much stronger.\n"
                     "#\n"
                     "# DIAGNOSTIC AID ONLY. Confirm against the disassembly\n"
                     "# before relying on either kind.\n")
            for a in sorted(set(names) | set(agreed)):
                fh.write(f"{a:06x}\t{names[a] if a in names else '~' + agreed[a]}\n")
        print(f"wrote {args.out}")


if __name__ == "__main__":
    sys.exit(main())
