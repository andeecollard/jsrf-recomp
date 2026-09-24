#!/usr/bin/env python3
"""Drawn against dropped, per report window, from a session log (G54).

Every report block prints CUMULATIVE counters: [FRAME] flips, [GPU] draws,
[TEXTURE] prepared/rejected and its per-reason table, [VSH] rejected and its
table, [METAL] native/fallback batches, [RASTER] CPU batches, the texture-mode
approximations, and -- from G54 on -- a [DROP] window line that is already
per-window. This differences each block against the one before it and prints
one row per window, so a reason that only fires in one scene (Rokkaku's fog:
0 in the tutorial, 94-96% of batches in the level) stands out instead of
hiding in a total.

A block ends at its "[METAL] N batches native" line (or "[RASTER]" when the
run had no Metal path); every counter seen since the previous block end
belongs to it.

    python3 diagnostics/jsrf_first_fault/drop_census.py <log> [<log> ...]
        [--all]      print every window, not only those with a drop
        [--min PCT]  a window's reason is listed from PCT% of its draws (0.1)

Read the "unaccounted" column: draws the executor counted that were neither
prepared, refused by the gate, nor refused by the vertex stage -- short
batches, the untransformed-FF guard, host-skipped draws in a host draw mode.
Before G54 nothing named them.
"""
import re
import sys

RE_FLIPS = re.compile(r"\[FRAME\] flips=(\d+)")
RE_DRAWS = re.compile(r"^\[GPU\] draws (\d+)")
RE_TEX = re.compile(r"^\[TEXTURE\] prepared=(\d+) rejected=(\d+)")
RE_TEXR = re.compile(r"^\[TEXTURE\]\s+(\d+)\s\s(.+)$")
RE_VSH = re.compile(r"^\[VSH\] executed batches=(\d+) rejected=(\d+)")
RE_VSHR = re.compile(r"^\[VSH\]\s+(\d+)\s\s(.+?) \(first detail")
RE_METAL = re.compile(r"^\[METAL\] (\d+) batches native, (\d+) software fallbacks")
RE_RASTER = re.compile(r"^\[RASTER\] (\d+) batches \+ (\d+) triangles on the CPU")
RE_APPROX = re.compile(r"^\[COMBINER\]\s+unit (\d+) mode\s+(\d+)\s+x(\d+)\s+<-- drawn without the displacement")
RE_FOGDROP = re.compile(r"^\[FOG\]\s+DROPPED (\d+) draws: final combiner (CW0 \S+ CW1 \S+)")
RE_DROPWIN = re.compile(r"^\[DROP\] \S+ window: (\d+) flips, (\d+) batches \|(.*)$")
RE_DROPITEM = re.compile(r" (DROPPED|SIMPLIFIED|PARTIAL) ([^:]+): (.+?) = (\d+) \(")


def blocks(path):
    """Yield (cumulative dict, per-window [DROP] items) per report block."""
    cur = {"reasons": {}}
    drop_items = []
    have_metal = False
    with open(path, errors="replace") as f:
        lines = f.readlines()
    for ln in lines:
        if RE_METAL.match(ln):
            have_metal = True
            break
    for ln in lines:
        ln = ln.rstrip("\n")
        m = RE_FLIPS.search(ln)
        if m:
            cur["flips"] = int(m.group(1))
        m = RE_DRAWS.match(ln)
        if m:
            cur["draws"] = int(m.group(1))
        m = RE_TEX.match(ln)
        if m:
            cur["prepared"], cur["rejected"] = int(m.group(1)), int(m.group(2))
            continue
        m = RE_TEXR.match(ln)
        if m:
            cur["reasons"]["gate: " + m.group(2).strip()] = int(m.group(1))
            continue
        m = RE_VSH.match(ln)
        if m:
            cur["vsh_rej"] = int(m.group(2))
        m = RE_VSHR.match(ln)
        if m:
            cur["reasons"]["vertex: " + m.group(2).strip()] = int(m.group(1))
        m = RE_APPROX.match(ln)
        if m:
            cur["reasons"]["simplified: unit %s mode %s without displacement" % (m.group(1), m.group(2))] = int(m.group(3))
        m = RE_FOGDROP.match(ln)
        if m:
            cur["reasons"]["fog gate: " + m.group(2)] = int(m.group(1))
        m = RE_DROPWIN.match(ln)
        if m:
            for it in RE_DROPITEM.finditer(m.group(3)):
                drop_items.append((it.group(1), it.group(2).strip(), it.group(3).strip(), int(it.group(4))))
        m = RE_RASTER.match(ln)
        if m:
            cur["cpu_batches"] = int(m.group(1))
            if not have_metal:
                yield dict(cur, reasons=dict(cur["reasons"])), drop_items
                drop_items = []
        m = RE_METAL.match(ln)
        if m:
            cur["native"], cur["fallbacks"] = int(m.group(1)), int(m.group(2))
            yield dict(cur, reasons=dict(cur["reasons"])), drop_items
            drop_items = []


def census(path, show_all=False, min_pct=0.1):
    print("=" * 100)
    print(path)
    prev = None
    totals = {}
    windows = 0
    hdr = "%4s %7s %9s %9s %9s %7s %7s %7s  %s" % ("win", "flips", "draws", "prepared", "rejected", "vsh", "fallbk",
                                                   "unacct", "dropped by reason (draws, % of the window's draws)")
    print(hdr)
    for cur, items in blocks(path):
        windows += 1
        if prev is None:
            prev = {"reasons": {}}
        d = lambda k: cur.get(k, 0) - prev.get(k, 0)
        draws = d("draws")
        flips = d("flips")
        rej = d("rejected")
        vsh = d("vsh_rej")
        unacct = draws - d("prepared") - rej - vsh
        parts = []
        for r, v in sorted(cur["reasons"].items(), key=lambda kv: -(kv[1] - prev["reasons"].get(kv[0], 0))):
            dv = v - prev["reasons"].get(r, 0)
            if dv <= 0:
                continue
            totals[r] = totals.get(r, 0) + dv
            pct = 100.0 * dv / draws if draws else 0.0
            if pct >= min_pct or show_all:
                parts.append("%s=%d (%.1f%%)" % (r, dv, pct))
        if unacct > 0 and draws and (100.0 * unacct / draws >= min_pct or show_all):
            parts.append("unaccounted=%d (%.1f%%)" % (unacct, 100.0 * unacct / draws))
        for kind, stage, reason, n in items:
            key = "[DROP] %s %s: %s" % (kind, stage, reason)
            totals[key] = totals.get(key, 0) + n
            pct = 100.0 * n / draws if draws and kind != "PARTIAL" else 0.0
            parts.append("%s=%d%s" % (key, n, "" if kind == "PARTIAL" else " (%.1f%%)" % pct))
        if parts or show_all:
            print("%4d %7d %9d %9d %9d %7d %7d %7d  %s" % (windows, flips, draws, d("prepared"), rej, vsh, d("fallbacks"),
                                                          unacct, "; ".join(parts) if parts else "-"))
        prev = cur
    if prev is None:
        print("  no report blocks found")
        return
    draws = prev.get("draws", 0)
    print("-" * 100)
    print("session: %d windows, %d flips, %d draws, %d prepared, %d gate-refused, %d vertex-refused, %d unaccounted, %d Metal fallbacks"
          % (windows, prev.get("flips", 0), draws, prev.get("prepared", 0), prev.get("rejected", 0), prev.get("vsh_rej", 0),
             draws - prev.get("prepared", 0) - prev.get("rejected", 0) - prev.get("vsh_rej", 0), prev.get("fallbacks", 0)))
    for r, v in sorted(totals.items(), key=lambda kv: -kv[1]):
        print("  %10d  %5.2f%%  %s" % (v, 100.0 * v / draws if draws else 0.0, r))


def main(argv):
    show_all, min_pct, paths = False, 0.1, []
    it = iter(argv[1:])
    for a in it:
        if a == "--all":
            show_all = True
        elif a == "--min":
            min_pct = float(next(it))
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    for p in paths:
        census(p, show_all, min_pct)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
