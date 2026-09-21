#!/usr/bin/env python3
"""Put xemu's NV2A trace and our runtime log into ONE comparable form, and diff.

WHAT EACH SIDE IS FOR. xemu runs the same guest XBE against a different GPU
model, so its pgraph trace is ground truth for WHAT THE GUEST SUBMITTED and is
worthless for anything about our host code. Our log is the only thing that
knows what we then did with it. The diff therefore answers exactly one class of
question -- "is the method stream the guest handed us the same on both sides?"
-- and a clean diff MOVES THE FAULT INTO OUR BACKEND rather than clearing it.

THE TWO SIDES.

  xemu, from xemu_capture.sh:
      nv2a_pgraph_method       %d: 0x%x -> 0x%04x %s[%d] 0x%x
      nv2a_pgraph_method_abbrev %d: 0x%x -> 0x%04x %s * %d
      nv2a_pgraph_method_unhandled %d: 0x%x -> 0x%04x 0x%x
      nv2a_pgraph_surface_target / _create_* / _hit_* / _match_* / ...
      nv2a_pgraph_flip_stall / _flip_increment_write

  ours, from a run with RECOMP_PB_SCAN=1 RECOMP_COMBINER_TRACE=1:
      [PB] N segments, M words, J jumps, U unrecognised -- D distinct pairs
      [PB]   subch U  method 0xMMMM  xCOUNT  NAME
      [COMBINER] new config=N draw=D result=...
      [COMBINER] config=N MMMM=WWWWWWWW                (the 8 scalar words)
      [COMBINER] config=N MMMM..MMMM: w w w w w w w w  (the 6 8-word arrays)

WHAT COMPARES, AND WHAT DOES NOT.

  method inventory + counts   both sides, any preset.  Counts will NOT match:
                              the two runs cover different numbers of frames.
                              PRESENCE is the signal -- a (subch, method) pair
                              on one side and not the other. The tool prints
                              per-frame rates where a flip count exists, so a
                              rate can be compared where a total cannot.
  combiner configurations     needs xemu --preset full (the abbrev event drops
                              parameters). Compared as SETS of 56-word tuples,
                              order-independent, because draw counts differ.
  surfaces / flips            xemu only. Ours reports surfaces through
                              RECOMP_SURFACE_CENSUS, which is a different
                              instrument with a different trigger; do not
                              silently merge the two.

USAGE
  gpu_stream_diff.py xemu  <trace.txt>            [--json out.json]
  gpu_stream_diff.py ours  <runtime.log>          [--json out.json]
  gpu_stream_diff.py diff  <trace.txt> <runtime.log>
  gpu_stream_diff.py selftest

`selftest` feeds one synthetic line of every format through both parsers and
asserts the result, so a parser that has silently stopped matching fails here
rather than reporting an empty stream as agreement. That failure mode has cost
this project a day before.
"""

import argparse
import json
import re
import sys
from collections import OrderedDict

# The combiner state the NV2A carries, as our trace_combiner() enumerates it:
# eight scalar registers and six eight-entry arrays. The output control words
# are the ones the CD/AB nibble bug lived in, so they are the point of this.
COMBINER_SCALARS = [0x1E60, 0x1E70, 0x1E74, 0x1E78, 0x0288, 0x028C, 0x1E20, 0x1E24]
COMBINER_ARRAYS = [0x0260, 0x0A60, 0x0A80, 0x0AA0, 0x0AC0, 0x1E40]
COMBINER_METHODS = set(COMBINER_SCALARS)
for _b in COMBINER_ARRAYS:
    for _i in range(8):
        COMBINER_METHODS.add(_b + 4 * _i)

NV097_SET_BEGIN_END = 0x17FC

# ---------------------------------------------------------------- xemu side

RE_METHOD = re.compile(
    r"^nv2a_pgraph_method\s+(\d+):\s+0x([0-9a-fA-F]+)\s+->\s+"
    r"0x([0-9a-fA-F]{4})\s+(\S*)\[(\d+)\]\s+0x([0-9a-fA-F]+)\s*$")
RE_ABBREV = re.compile(
    r"^nv2a_pgraph_method_abbrev\s+(\d+):\s+0x([0-9a-fA-F]+)\s+->\s+"
    r"0x([0-9a-fA-F]{4})\s+(\S*)\s+\*\s+(\d+)\s*$")
RE_UNHANDLED = re.compile(
    r"^nv2a_pgraph_method_unhandled\s+(\d+):\s+0x([0-9a-fA-F]+)\s+->\s+"
    r"0x([0-9a-fA-F]{4})\s+0x([0-9a-fA-F]+)\s*$")
RE_SURFACE = re.compile(r"^(nv2a_pgraph_surface_\w+)\s+(.*)$")
RE_FLIP = re.compile(r"^(nv2a_pgraph_flip_\w+)")


def parse_xemu(path):
    methods = OrderedDict()          # (subch, method) -> {"name", "count"}
    unhandled = OrderedDict()
    surfaces = OrderedDict()
    flips = 0
    values = {}                      # method -> latest parameter, for combiner
    combiner_configs = []            # list of tuples, first-seen order
    seen_configs = set()
    have_values = False
    lines = 0

    def bump(table, key, name, n):
        row = table.get(key)
        if row is None:
            table[key] = {"name": name, "count": n}
        else:
            row["count"] += n
            if not row["name"] and name:
                row["name"] = name

    def snapshot():
        words = [values.get(m, 0) for m in COMBINER_SCALARS]
        for base in COMBINER_ARRAYS:
            words += [values.get(base + 4 * i, 0) for i in range(8)]
        t = tuple(words)
        if t not in seen_configs:
            seen_configs.add(t)
            combiner_configs.append(t)

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            lines += 1
            line = line.rstrip("\n")
            m = RE_METHOD.match(line)
            if m:
                have_values = True
                subch, method = int(m.group(1)), int(m.group(3), 16)
                param = int(m.group(6), 16)
                bump(methods, (subch, method), m.group(4), 1)
                if method in COMBINER_METHODS:
                    values[method] = param
                elif method == NV097_SET_BEGIN_END and param == 0:
                    snapshot()          # END: the state this draw was made with
                continue
            m = RE_ABBREV.match(line)
            if m:
                bump(methods, (int(m.group(1)), int(m.group(3), 16)),
                     m.group(4), int(m.group(5)))
                continue
            m = RE_UNHANDLED.match(line)
            if m:
                bump(unhandled, (int(m.group(1)), int(m.group(3), 16)), "", 1)
                continue
            m = RE_SURFACE.match(line)
            if m:
                surfaces[m.group(1)] = surfaces.get(m.group(1), 0) + 1
                continue
            if RE_FLIP.match(line):
                flips += 1

    return {
        "side": "xemu",
        "source": path,
        "lines": lines,
        "flips": flips,
        "have_parameter_values": have_values,
        "methods": {"%d:%04X" % k: v for k, v in methods.items()},
        "unhandled": {"%d:%04X" % k: v for k, v in unhandled.items()},
        "surface_events": surfaces,
        "combiner_configs": [list(t) for t in combiner_configs],
    }


# ----------------------------------------------------------------- our side

RE_PB_ROW = re.compile(
    r"^\s*\[PB\]\s+subch\s+(\d+)\s+method\s+0x([0-9A-Fa-f]{4})\s+x(\d+)\s*(\S*)")
RE_PB_HEAD = re.compile(
    r"^\s*\[PB\]\s+(\d+) segments, (\d+) words, (\d+) jumps, (\d+) unrecognised")
RE_COMB_NEW = re.compile(r"^\s*\[COMBINER\]\s+new config=(\d+)\s+draw=(\d+)\s+result=(\S+)")
RE_COMB_SCALAR = re.compile(
    r"^\s*\[COMBINER\]\s+config=(\d+)\s+([0-9A-Fa-f]{4})=([0-9A-Fa-f]{8})\s*$")
RE_COMB_ARRAY = re.compile(
    r"^\s*\[COMBINER\]\s+config=(\d+)\s+([0-9A-Fa-f]{4})\.\.[0-9A-Fa-f]{4}:\s*(.*)$")
RE_GPU_UNHANDLED = re.compile(r"^\s*\[GPU\]\s+0x([0-9A-Fa-f]{4})\s+x(\d+)\s*$")
# [FRAME] flips= is CUMULATIVE; [FRAME-WIN] flips= is a per-window count, and
# they read 31719 and 242 in the same report block. A cumulative total and a
# windowed one are different instruments -- match the cumulative one by NAME
# rather than taking a maximum over every flips= in the log.
RE_FLIPS = re.compile(r"\[FRAME\]\s+flips=(\d+)")


def parse_ours(path):
    methods = OrderedDict()
    unhandled = OrderedDict()
    configs = {}                      # id -> {method: word}
    order = []
    flips = 0
    pb_totals = None
    lines = 0

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            lines += 1
            m = RE_PB_ROW.match(line)
            if m:
                key = (int(m.group(1)), int(m.group(2), 16))
                row = methods.setdefault(key, {"name": m.group(4), "count": 0})
                row["count"] += int(m.group(3))
                if not row["name"] and m.group(4):
                    row["name"] = m.group(4)
                continue
            m = RE_PB_HEAD.match(line)
            if m:
                pb_totals = {"segments": int(m.group(1)), "words": int(m.group(2)),
                             "jumps": int(m.group(3)), "unrecognised": int(m.group(4))}
                continue
            m = RE_COMB_NEW.match(line)
            if m:
                cid = int(m.group(1))
                configs.setdefault(cid, {})
                if cid not in order:
                    order.append(cid)
                continue
            m = RE_COMB_SCALAR.match(line)
            if m:
                cid = int(m.group(1))
                configs.setdefault(cid, {})[int(m.group(2), 16)] = int(m.group(3), 16)
                continue
            m = RE_COMB_ARRAY.match(line)
            if m:
                cid, base = int(m.group(1)), int(m.group(2), 16)
                words = [int(w, 16) for w in m.group(3).split()]
                for i, w in enumerate(words):
                    configs.setdefault(cid, {})[base + 4 * i] = w
                continue
            m = RE_GPU_UNHANDLED.match(line)
            if m:
                unhandled[(0, int(m.group(1), 16))] = {
                    "name": "", "count": int(m.group(2))}
                continue
            m = RE_FLIPS.search(line)
            if m:
                flips = max(flips, int(m.group(1)))   # last report wins

    combiner_configs = []
    for cid in order or sorted(configs):
        c = configs[cid]
        words = [c.get(mth, 0) for mth in COMBINER_SCALARS]
        for base in COMBINER_ARRAYS:
            words += [c.get(base + 4 * i, 0) for i in range(8)]
        combiner_configs.append(words)

    return {
        "side": "ours",
        "source": path,
        "lines": lines,
        "flips": flips,
        "pb_totals": pb_totals,
        "have_parameter_values": bool(combiner_configs),
        "methods": {"%d:%04X" % k: v for k, v in methods.items()},
        "unhandled": {"%d:%04X" % k: v for k, v in unhandled.items()},
        "combiner_configs": combiner_configs,
    }


# --------------------------------------------------------------------- diff

def _rate(count, flips):
    return (count / flips) if flips else None


def render_diff(x, o, out=sys.stdout):
    w = out.write
    w("=== SOURCES\n")
    w("  xemu : %s  (%d lines, %d flip events)\n" % (x["source"], x["lines"], x["flips"]))
    w("  ours : %s  (%d lines, flips=%s)\n" % (o["source"], o["lines"], o["flips"] or "unknown"))
    if not x["methods"]:
        w("  !! xemu side has NO method rows. Wrong preset, or the trace is empty.\n")
    if not o["methods"]:
        w("  !! our side has NO [PB] rows. RECOMP_PB_SCAN=1 was not set on that run.\n")

    keys = sorted(set(x["methods"]) | set(o["methods"]),
                  key=lambda k: (int(k.split(":")[0]), k.split(":")[1]))
    only_x, only_o, both = [], [], []
    for k in keys:
        a, b = x["methods"].get(k), o["methods"].get(k)
        if a and not b:
            only_x.append(k)
        elif b and not a:
            only_o.append(k)
        else:
            both.append(k)

    w("\n=== METHODS THE GUEST SUBMITTED ON XEMU AND WE NEVER SAW  (%d)\n" % len(only_x))
    w("    These are the interesting ones: the guest asked and our parser did\n"
      "    not record it. Either we do not decode it, or the scene differed.\n")
    for k in only_x:
        r = x["methods"][k]
        w("    subch %s method 0x%s  x%-8d %s\n"
          % (k.split(":")[0], k.split(":")[1], r["count"], r["name"]))

    w("\n=== METHODS WE RECORDED AND XEMU'S TRACE DID NOT  (%d)\n" % len(only_o))
    w("    Usually a scene difference or an event-set gap, NOT a finding.\n")
    for k in only_o:
        r = o["methods"][k]
        w("    subch %s method 0x%s  x%-8d %s\n"
          % (k.split(":")[0], k.split(":")[1], r["count"], r["name"]))

    w("\n=== IN BOTH -- counts, and per-flip rates where both flip counts exist\n")
    w("    %-16s %-34s %10s %10s %9s %9s\n"
      % ("subch/method", "name", "xemu", "ours", "x/flip", "o/flip"))
    for k in both:
        a, b = x["methods"][k], o["methods"][k]
        ra, rb = _rate(a["count"], x["flips"]), _rate(b["count"], o["flips"])
        w("    %-16s %-34s %10d %10d %9s %9s\n"
          % (k, a["name"] or b["name"],
             a["count"], b["count"],
             "%.2f" % ra if ra else "-", "%.2f" % rb if rb else "-"))

    w("\n=== METHODS XEMU ITSELF DOES NOT IMPLEMENT (nv2a_pgraph_method_unhandled)\n")
    for k, r in sorted(x["unhandled"].items()):
        w("    %s x%d\n" % (k, r["count"]))
    w("=== METHODS WE DO NOT IMPLEMENT ([GPU] 0xMMMM xN -- top N only, raise\n"
      "    RECOMP_PB_EXEC_TOP or this list is the ten most frequent and no more)\n")
    for k, r in sorted(o["unhandled"].items()):
        w("    %s x%d\n" % (k, r["count"]))

    w("\n=== SURFACE EVENTS (xemu only; ours come from RECOMP_SURFACE_CENSUS,\n"
      "    a different instrument -- do not merge the two)\n")
    for k, v in sorted(x.get("surface_events", {}).items()):
        w("    %-44s x%d\n" % (k, v))

    w("\n=== COMBINER CONFIGURATIONS\n")
    if not x["have_parameter_values"]:
        w("    xemu trace has no parameter values: capture with --preset full.\n")
    elif not o["combiner_configs"]:
        w("    our log has no [COMBINER] config= blocks: set RECOMP_COMBINER_TRACE=1.\n")
    else:
        xs = {tuple(c) for c in x["combiner_configs"]}
        os_ = {tuple(c) for c in o["combiner_configs"]}
        w("    xemu %d distinct, ours %d distinct, %d shared\n"
          % (len(xs), len(os_), len(xs & os_)))
        labels = (["%04X" % m for m in COMBINER_SCALARS]
                  + ["%04X+%d" % (b, i) for b in COMBINER_ARRAYS for i in range(8)])
        for tag, only in (("ONLY ON XEMU", xs - os_), ("ONLY IN OURS", os_ - xs)):
            for cfg in sorted(only):
                w("    %s:\n" % tag)
                for lab, word in zip(labels, cfg):
                    if word:
                        w("        %-8s %08X\n" % (lab, word))
    return 0


# ----------------------------------------------------------------- selftest

XEMU_SAMPLE = """\
nv2a_pgraph_method 0: 0x97 -> 0x1e60 NV097_SET_COMBINER_CONTROL[0] 0x00000101
nv2a_pgraph_method 0: 0x97 -> 0x1e40 NV097_SET_COMBINER_COLOR_OCW[0] 0x000820d0
nv2a_pgraph_method 0: 0x97 -> 0x17fc NV097_SET_BEGIN_END[0] 0x00000005
nv2a_pgraph_method 0: 0x97 -> 0x17fc NV097_SET_BEGIN_END[0] 0x00000000
nv2a_pgraph_method_abbrev 0: 0x97 -> 0x1b00 NV097_SET_TEXTURE_OFFSET * 4
nv2a_pgraph_method_unhandled 0: 0x97 -> 0x0a1c 0x00000000
nv2a_pgraph_surface_target       Target: [COLOR @ 0x03c00000] (fmt) aa:0 clip:x=0,w=640,y=0,h=480
nv2a_pgraph_flip_stall
"""

OURS_SAMPLE = """\
[PB] 12 segments, 3400 words, 3 jumps, 0 unrecognised -- 2 distinct (subchannel, method) pairs
  [PB]   subch 0  method 0x1B00  x4      SET_TEXTURE_OFFSET
  [PB]   subch 0  method 0x1E60  x1      SET_COMBINER_CONTROL
[COMBINER] new config=0 draw=17 result=prepared
[COMBINER] config=0 1E60=00000101
[COMBINER] config=0 1E40..1E5C: 000820D0 00000000 00000000 00000000 00000000 00000000 00000000 00000000
  [GPU]   0x0A1C x9
[FRAME] flips=31 mean=20.8 ms (48.0 fps)
[FRAME-WIN] flips=7 mean=20.7 ms
"""


def selftest():
    import tempfile
    import os
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + what)
        if not cond:
            ok = False

    d = tempfile.mkdtemp()
    xp, op = os.path.join(d, "t.txt"), os.path.join(d, "r.log")
    open(xp, "w").write(XEMU_SAMPLE)
    open(op, "w").write(OURS_SAMPLE)

    x = parse_xemu(xp)
    print("xemu parser:")
    check(x["methods"].get("0:1E60", {}).get("count") == 1, "scalar method row")
    check(x["methods"].get("0:1B00", {}).get("count") == 4, "abbrev row carries its count")
    check(x["methods"]["0:1E60"]["name"] == "NV097_SET_COMBINER_CONTROL", "method name kept")
    check(x["unhandled"].get("0:0A1C", {}).get("count") == 1, "unhandled row")
    check(x["flips"] == 1, "flip event counted")
    check(x["surface_events"].get("nv2a_pgraph_surface_target") == 1, "surface event")
    check(x["have_parameter_values"], "parameter values present")
    check(len(x["combiner_configs"]) == 1, "one config snapshotted at END")
    check(x["combiner_configs"][0][0] == 0x00000101, "0x1E60 lands in slot 0")
    check(x["combiner_configs"][0][8 + 5 * 8] == 0x000820D0, "0x1E40[0] lands in the 1E40 array")

    o = parse_ours(op)
    print("our parser:")
    check(o["methods"].get("0:1B00", {}).get("count") == 4, "[PB] row")
    check(o["methods"]["0:1B00"]["name"] == "SET_TEXTURE_OFFSET", "[PB] name")
    check(o["pb_totals"]["words"] == 3400, "[PB] header totals")
    check(o["unhandled"].get("0:0A1C", {}).get("count") == 9, "[GPU] unhandled row")
    check(o["flips"] == 31, "flip count")
    check(len(o["combiner_configs"]) == 1, "one config")
    check(o["combiner_configs"][0][0] == 0x00000101, "scalar word placed")
    check(o["combiner_configs"][0][8 + 5 * 8] == 0x000820D0, "array word placed")

    print("cross:")
    check(tuple(x["combiner_configs"][0]) == tuple(o["combiner_configs"][0]),
          "the SAME combiner state parses to the SAME 56-word tuple on both sides")
    render_diff(x, o, out=open(os.devnull, "w"))
    print("  ok   render_diff ran")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("xemu"); p.add_argument("trace"); p.add_argument("--json")
    p = sub.add_parser("ours"); p.add_argument("log"); p.add_argument("--json")
    p = sub.add_parser("diff"); p.add_argument("trace"); p.add_argument("log")
    sub.add_parser("selftest")
    a = ap.parse_args()

    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "xemu":
        d = parse_xemu(a.trace)
    elif a.cmd == "ours":
        d = parse_ours(a.log)
    else:
        return render_diff(parse_xemu(a.trace), parse_ours(a.log))
    text = json.dumps(d, indent=2)
    if getattr(a, "json", None):
        open(a.json, "w").write(text)
        print("wrote", a.json)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
