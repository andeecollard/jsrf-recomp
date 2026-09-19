#!/usr/bin/env python3
"""Which unresolved edges did a run actually REACH, and what are they?

control_flow_gate.py counts the edges the lifter could not place: 152 switch
dispatches through 62 tables, 168 stubbed call targets, 122 untranslated
instructions on the 19 Sep gen. It cannot say which of them a run touches.
The runtime can -- [ITAIL], [ICALL] and [UNIMPL] lines name the guest VA --
but nothing joined the two, so a log line reading "Failed to resolve VA
0x000A5B8C" was six hours of disassembly away from "that is arm 2 of the
table at 0x000A60A4, whose owner was carved 0x30 bytes late". This is that
join, per run.

For every VA the runtime reported it says which static class it falls in:

  switch-arm        an arm of a table the lifter left to RECOMP_ITAIL, and
                    which table: G20's shape. The fix is in the detector.
  stub              a call target with a "Recovered entry" stub body and no
                    real translation: the detector never defined it.
  defined-body      a function the gen DOES define. A failure here is not a
                    missing translation: the dispatch table did not know it
                    (a manual override, a body in another unit, a load-order
                    problem). Different bug, different owner.
  untranslated      an [UNIMPL] site: an instruction with no translation was
                    reached. Its text and register state are in the log.
  not-code          the runtime's own verdict: the pointer was not in the
                    image (NULL, a freed object, a device address). Not a
                    translation gap; a guest-state one.
  unknown           inside the image, no body, not an arm of any known
                    table: the detector has no start there at all.

The runtime prints on a powers-of-ten cadence, so the hit counts are the
LAST figure printed for each VA, a lower bound. The [ITAIL] second line says
whether esp sat on a return address -- a genuine tail call that lost one
call -- or inside a frame, which is the stack-stranding case; both are kept.

    python3 diagnostics/jsrf_first_fault/edge_runtime_summary.py \\
        --log build-macos/jsrf-first-fault/render-investigation/<run>/stderr.log \\
        --gen build-macos/jsrf-first-fault/gen \\
        [--xbe "$JSRF_GAME_DIR/default.xbe"]     # or JSRF_GAME_DIR in the env

Without the XBE the switch-arm class cannot be assigned and those VAs read
`unknown`; the run says so. The gen must be the one the run was built on --
the log's [GEN] line and the manifest's translator sha are printed side by
side so a mismatch is visible rather than silently wrong.
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.abspath(os.path.join(HERE, "..", "..")))

import control_flow_gate as gate  # noqa: E402

ITAIL_RE = re.compile(r"\[ITAIL\] Unresolved tail jump to 0x([0-9A-F]{8}) \((\d+) times")
ITAIL_ESP_RE = re.compile(r"\[ITAIL\]   guest esp=0x[0-9A-F]+ holds 0x[0-9A-F]+, which is (code|NOT CODE)")
ICALL_RE = re.compile(r"\[ICALL\] Failed to resolve VA 0x([0-9A-F]{8})")
NOTCODE_RE = re.compile(r"\[ICALL\] skipped not-code target 0x([0-9A-F]{8}) \((\d+) times\)")
RUNAWAY_RE = re.compile(r"\[ICALL\] RUNAWAY: target 0x([0-9A-F]{8}) skipped (\d+) times")
UNIMPL_RE = re.compile(r"\[UNIMPL\] untranslated instruction REACHED: `([^`]*)` at 0x([0-9A-F]{8}) \((\d+) times")
GEN_LINE_RE = re.compile(r"\[GEN\][^\n]*")


def parse_log(path):
    """{va: {"kind": ..., "hits": n, "extra": ...}} and the log's [GEN] line."""
    seen = {}
    gen_line = None
    last_itail = None
    with open(path, errors="ignore") as f:
        for line in f:
            if gen_line is None:
                m = GEN_LINE_RE.search(line)
                if m:
                    gen_line = m.group(0).strip()
            m = ITAIL_RE.search(line)
            if m:
                va, n = int(m.group(1), 16), int(m.group(2))
                e = seen.setdefault(va, {"kind": "itail", "hits": 0, "extra": set()})
                e["hits"] = max(e["hits"], n)
                last_itail = e
                continue
            m = ITAIL_ESP_RE.search(line)
            if m and last_itail is not None:
                last_itail["extra"].add("epilogue ran" if m.group(1) == "code"
                                        else "FRAME STRANDED")
                continue
            m = ICALL_RE.search(line)
            if m:
                va = int(m.group(1), 16)
                e = seen.setdefault(va, {"kind": "icall", "hits": 0, "extra": set()})
                e["hits"] = max(e["hits"], 1)
                continue
            m = NOTCODE_RE.search(line)
            if m:
                va, n = int(m.group(1), 16), int(m.group(2))
                e = seen.setdefault(va, {"kind": "not-code", "hits": 0, "extra": set()})
                e["hits"] = max(e["hits"], n)
                continue
            m = RUNAWAY_RE.search(line)
            if m:
                va, n = int(m.group(1), 16), int(m.group(2))
                e = seen.setdefault(va, {"kind": "icall", "hits": 0, "extra": set()})
                e["hits"] = max(e["hits"], n)
                e["extra"].add("RUNAWAY")
                continue
            m = UNIMPL_RE.search(line)
            if m:
                text, va, n = m.group(1), int(m.group(2), 16), int(m.group(3))
                e = seen.setdefault(va, {"kind": "unimpl", "hits": 0, "extra": set()})
                e["hits"] = max(e["hits"], n)
                e["extra"].add(text)
    return seen, gen_line


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--xbe", default=None)
    ap.add_argument("--window", type=lambda v: int(v, 0), default=gate.WINDOW)
    args = ap.parse_args()

    xbe = args.xbe
    if not xbe and os.environ.get("JSRF_GAME_DIR"):
        cand = os.path.join(os.environ["JSRF_GAME_DIR"], "default.xbe")
        if os.path.exists(cand):
            xbe = cand

    manifest = gate.read_manifest(args.gen)
    _, _, owners_of_table, defined = gate.scan_gen(args.gen)
    stub_names = gate.scan_gen.stub_names
    arm_table = {}
    if xbe:
        _, _, arms_of = gate.walk_arms(xbe, owners_of_table, defined, args.window)
        for table, arms in arms_of.items():
            for a in arms:
                arm_table.setdefault(a, table)

    seen, gen_line = parse_log(args.log)

    def classify(va, kind):
        name = "sub_%08X" % va
        if kind == "unimpl":
            return "untranslated"
        if kind == "not-code":
            return "not-code"
        if va in arm_table:
            return "switch-arm"
        if name in stub_names:
            return "stub"
        if name in defined:
            return "defined-body"
        return "unknown"

    print("unresolved edges reached by %s" % args.log)
    print("  gen manifest: translator %s, head %s" % (
        manifest.get("translator_sha", "?"), manifest.get("git_head", "?")))
    print("  log's own:    %s" % (gen_line or "(no [GEN] line found)"))
    if not xbe:
        print("  no XBE: switch arms cannot be assigned, they will read `unknown`")
    print()
    if not seen:
        print("  no [ITAIL], [ICALL] failure or [UNIMPL] line in this log.")
        print("  (a positive control: the log must contain the runtime's report"
              " lines at all -- check for [ICALL] or [GEN] before reading this"
              " zero as clean)")
        return 0

    by_class = collections.Counter()
    rows = []
    for va, e in sorted(seen.items(), key=lambda kv: -kv[1]["hits"]):
        cls = classify(va, e["kind"])
        by_class[cls] += 1
        detail = ""
        if cls == "switch-arm":
            table = arm_table[va]
            owners = ",".join(sorted(o or "?" for o in owners_of_table.get(table, ())))
            detail = "arm of table 0x%08X, owner %s" % (table, owners[:48])
        if e["extra"]:
            detail = (detail + "; " if detail else "") + ", ".join(sorted(e["extra"]))
        rows.append((va, e["kind"], e["hits"], cls, detail))

    print("  %-10s %-8s %12s  %-14s %s" % ("VA", "via", ">= hits", "class", "detail"))
    for va, kind, hits, cls, detail in rows:
        print("  0x%08X %-8s %12d  %-14s %s" % (va, kind, hits, cls, detail))
    print()
    print("  by class:")
    for cls, n in sorted(by_class.items(), key=lambda kv: -kv[1]):
        print("    %-14s %4d" % (cls, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
