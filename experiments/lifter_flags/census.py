"""Census of every `(_flags` site in a generated JSRF tree (G56).

`_flags` is the lifter's function-local fallback flag variable. Only the
rep-string, xadd and cmpxchg lifts ever assign it, so a consumer that reads it
is either reading one of those (a real producer, correct) or the lifter could
not resolve the flag producer and emitted a constant: the branch is never
taken, the setcc always writes 0.

For every site this records the site address, the consuming instruction, the
generated function that contains it and the most likely producer, found in
the guest bytes with capstone, and sorts it into a class:

  a  producer in the same function, a mnemonic the lifter does not model
  b  producer across a function-split boundary (the site sits at, or is
     reached by fall-through only from, the entry of a generated function)
  c  parity after an FPU status transfer (fnstsw / sahf / test ah)
  d  loop / loope / loopne
  e  other
  r  RESOLVED -- the site reads `_flags` because a rep cmps/scas, xadd or
     cmpxchg really assigned it. Not a defect; listed so the total adds up.

Usage (repo root; read-only on the gen tree):

    /usr/bin/python3 experiments/lifter_flags/census.py \
        [--gen DIR] [--disasm DIR] [--xbe PATH] [--json OUT] [--md OUT.md]
"""

import argparse
import bisect
import collections
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", ".."))

import capstone  # noqa: E402
from capstone import x86_const as X  # noqa: E402

from tools.recomp import config  # noqa: E402

HOME = os.path.expanduser("~")
DEF_GEN = f"{HOME}/jsrf-build/jsrf-first-fault/gen"
DEF_DISASM = f"{HOME}/jsrf-build/jsrf-first-fault/disasm"
DEF_XBE = (f"{HOME}/Library/Mobile Documents/com~apple~CloudDocs/"
           "Jet Set Radio Future/Jet Set Radio Future (US)/default.xbe")

RE_FUNC = re.compile(r"^(?:static\s+)?void\s+(sub_[0-9A-F]{8})\s*\(void\)")
RE_LABEL = re.compile(r"^loc_([0-9A-F]{8}):\s*;")
RE_SITE = re.compile(r"\(_flags")
# Consumer mnemonic as the lifter writes it into the trailing comment:
# `/* jne: not equal */`, `/* sete */`, `/* loopne: loopne - ...`.
RE_CMT = re.compile(r"/\*\s*(j[a-z]+|set[a-z]+|cmov[a-z]+|loop[a-z]*)\s*(?::|\*/)")

ZF, CF, SF, OF, PF = "ZF", "CF", "SF", "OF", "PF"
COND_READS = {
    "e": {ZF}, "z": {ZF}, "ne": {ZF}, "nz": {ZF},
    "b": {CF}, "c": {CF}, "nae": {CF}, "ae": {CF}, "nb": {CF}, "nc": {CF},
    "be": {CF, ZF}, "na": {CF, ZF}, "a": {CF, ZF}, "nbe": {CF, ZF},
    "l": {SF, OF}, "nge": {SF, OF}, "ge": {SF, OF}, "nl": {SF, OF},
    "le": {ZF, SF, OF}, "ng": {ZF, SF, OF}, "g": {ZF, SF, OF},
    "nle": {ZF, SF, OF},
    "s": {SF}, "ns": {SF}, "o": {OF}, "no": {OF},
    "p": {PF}, "pe": {PF}, "np": {PF}, "po": {PF},
}


def _efl(flag):
    return sum(getattr(X, f"X86_EFLAGS_{k}_{flag}", 0)
               for k in ("MODIFY", "RESET", "SET", "UNDEFINED"))


EFL_MOD = {f: _efl(f) for f in (ZF, CF, SF, OF, PF)}
# Named outright in case capstone leaves their eflags empty.
ALWAYS_WRITES = {"sahf", "popfd", "popf", "fcomi", "fcomip", "fucomi",
                 "fucomip", "comiss", "ucomiss", "comisd", "ucomisd"}
LOOPS = {"loop", "loope", "loopne"}
NO_FALLTHROUGH = {"jmp", "ret", "retn", "retf", "iretd", "int3"}


def reads_of(mn):
    if mn in LOOPS:
        return {ZF} if mn != "loop" else set()
    for pre in ("set", "cmov", "j"):
        if mn.startswith(pre):
            return COND_READS.get(mn[len(pre):], set())
    return set()


def writes(insn, need):
    if insn.mnemonic in ALWAYS_WRITES:
        return True
    if not need:
        return False
    try:
        ef = insn.eflags
    except Exception:
        return False
    return any(ef & EFL_MOD[f] for f in need)


class Image:
    def __init__(self, xbe):
        config.configure_from_xbe(xbe)
        with open(xbe, "rb") as fh:
            self.data = fh.read()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._cache = {}

    def insn_at(self, va):
        if va in self._cache:
            return self._cache[va]
        off = config.va_to_file_offset(va)
        insn = None
        if off is not None:
            for i in self.md.disasm(self.data[off:off + 16], va, count=1):
                insn = i
        self._cache[va] = insn
        return insn

    def sweep(self, start, end):
        out, va = [], start
        while va < end:
            i = self.insn_at(va)
            if i is None:
                break
            out.append(i)
            va += i.size
        return out


RE_UNCOND = re.compile(r"(^|;\s*)(return;|goto loc_[0-9A-F]{8};)")


def _unconditional(stmt):
    """A statement after which control cannot fall through (not inside an if)."""
    return not stmt.startswith("if ") and bool(RE_UNCOND.search(stmt))


def _functions(path):
    """(name, [lines]) for every generated function in one C file."""
    name, body = None, []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            s = line.strip()
            m = RE_FUNC.match(s)
            if m:
                if name:
                    yield name, body
                name, body = m.group(1), []
                continue
            if name:
                body.append(s)
    if name:
        yield name, body


def parse_gen(gen):
    """One dict per `(_flags` line, with its function and block context.

    Also decides whether the line is reachable in the C: a line after an
    unconditional return/goto in its block, or in a block whose label nothing
    in the function jumps to and which nothing falls into, is dead code there
    whatever the guest does.
    """
    for path in sorted(glob.glob(os.path.join(gen, "recomp_*.c"))):
        for func, body in _functions(path):
            if not any(RE_SITE.search(l) for l in body):
                continue
            text = "\n".join(body)
            fn_va = int(func[4:], 16)
            label, block, prev_stmt = None, [], ""
            label_fallthrough = True
            for s in body:
                m = RE_LABEL.match(s)
                if m:
                    label = int(m.group(1), 16)
                    label_fallthrough = not _unconditional(prev_stmt)
                    block = []
                    continue
                if s and not s.startswith("/*") and not s.startswith("*"):
                    block.append(s)
                    prev_stmt = s
                if RE_SITE.search(s):
                    dead_in_block = any(_unconditional(l) for l in block[:-1])
                    entered = (label is None or label == fn_va
                               or f"goto loc_{label:08X};" in text
                               or label_fallthrough)
                    yield {"file": os.path.basename(path), "func": func,
                           "label": label, "text": s, "block": list(block),
                           "c_reachable": entered and not dead_in_block}


def consumer_mnemonic(text):
    if re.match(r"if \(!?_flags\) break;", text):
        return None  # the loop body of a rep cmps/scas
    m = RE_CMT.search(text)
    return m.group(1) if m else None


def locate(img, site, mn):
    """Guest address of the consumer: the k-th `mn` after the block label."""
    start = site["label"] if site["label"] is not None else int(site["func"][4:], 16)
    k = sum(1 for l in site["block"]
            if (RE_CMT.search(l) and RE_CMT.search(l).group(1) == mn))
    va, seen = start, 0
    for _ in range(4000):
        i = img.insn_at(va)
        if i is None:
            return None
        if i.mnemonic == mn:
            seen += 1
            if seen == k:
                return i.address
        va += i.size
    return None


def load_db(disasm):
    funcs = json.load(open(os.path.join(disasm, "functions.json")))
    starts = sorted(int(f["start"], 16) for f in funcs)
    byaddr = {int(f["start"], 16): f for f in funcs}
    xrefs = collections.defaultdict(list)
    for x in json.load(open(os.path.join(disasm, "xrefs.json"))):
        if x["type"] not in ("data_imm", "data_read", "data_write"):
            xrefs[int(x["to"], 16)].append((int(x["from"], 16), x["type"]))
    tables = []
    jt_path = os.path.join(disasm, "jump_tables.json")
    if os.path.exists(jt_path):
        for k, v in json.load(open(jt_path)).items():
            tables.append((int(k, 16), int(v["end"], 16)))
    return starts, byaddr, xrefs, tables


def back_walk(img, starts, site_va, need, hops=6, limit=96):
    """Nearest linear predecessor of `site_va` writing a flag it needs.

    Sweeps forward from a sync point (a detected function start) and walks the
    result backwards. Detected starts are dense here -- every tail-jump alias
    is one -- so when the walk runs off the front of its window it re-syncs at
    the start before and tries again. Stops at an instruction with no
    fall-through: the flags then come from whoever jumps to the block.
    """
    i = bisect.bisect_right(starts, site_va - 1) - 1
    for _ in range(hops):
        if i < 0:
            break
        seq = img.sweep(starts[i], site_va)
        if seq and seq[-1].address + seq[-1].size == site_va:
            for insn in reversed(seq[-limit:]):
                if insn.mnemonic in NO_FALLTHROUGH:
                    return None, "no fall-through", insn.address + insn.size
                if writes(insn, need):
                    return insn, "", None
            if len(seq) > limit:
                return None, "no setter within window", None
        i -= 1
    return None, "no setter found", None


def prev_insns(img, sync, va, n):
    seq = img.sweep(sync, va)
    return seq[-n:] if seq and seq[-1].address + seq[-1].size == va else []


def classify(img, db, site):
    starts, byaddr, xrefs, _tables = db
    t = site["text"]
    mn = consumer_mnemonic(t)
    site["consumer"] = mn or "rep-internal"
    fn_va = int(site["func"][4:], 16)
    site["fn_va"] = fn_va
    site["va"] = locate(img, site, mn) if mn else None

    if "UNRESOLVED" not in t:
        site["cls"] = "r"
        prod = "rep cmps/scas"
        for l in reversed(site["block"][:-1]):
            if "_xadd" in l:
                prod = "lock xadd"
                break
            if "cmpxchg" in l:
                prod = "cmpxchg"
                break
            if "_flags =" in l:
                break
        site["producer"] = prod
        site["why"] = "reads _flags that the producer really assigned"
        # A rep compare that runs zero times leaves EFLAGS alone on x86, but
        # leaves `_flags` at whatever it last held here. Harmless only when
        # the count is a non-zero constant; record what loaded ECX.
        if prod == "rep cmps/scas":
            blk = site["block"]
            loop_at = max((i for i, l in enumerate(blk)
                           if l.startswith("while (ecx != 0)")), default=None)
            count = "not in this block"
            if loop_at is not None:
                count = "not in this block"
                for l in reversed(blk[:loop_at]):
                    m = re.match(r"ecx = (.+?);", l)
                    if m:
                        count = m.group(1)
                        break
            site["rep_count"] = count
        return

    va = site["va"]
    if va is None:
        site["cls"], site["why"], site["producer"] = "e", "consumer not located", "?"
        return
    need = reads_of(mn)
    tables = db[3]
    in_table = [f"{a:08X}-{b:08X}" for a, b in tables if a <= va < b]
    prod, why, blk_start = back_walk(img, starts, va, need)
    site["producer"] = (f"{prod.mnemonic} {prod.op_str} @{prod.address:08X}"
                        if prod else f"none ({why})")
    site["producer_mn"] = prod.mnemonic if prod else None
    ctx_sync = starts[max(0, bisect.bisect_right(starts, va - 1) - 3)]
    site["context"] = [f"{p.address:08X} {p.mnemonic} {p.op_str}"
                       for p in prev_insns(img, ctx_sync, va, 4)]
    blk = site["label"] if site["label"] is not None else fn_va
    site["block_va"] = blk
    site["jumped_from"] = [f"{a:08X}/{k}" for a, k in xrefs.get(blk, [])]
    # When the site's guest block is entered only by jumps, name the flag
    # producer on each incoming edge.
    if blk_start is not None:
        edges = []
        for src, kind in xrefs.get(blk_start, []):
            p2, w2, _ = back_walk(img, starts, src, need)
            edges.append(f"{src:08X}/{kind}: " + (
                f"{p2.mnemonic} {p2.op_str} @{p2.address:08X}" if p2 else w2))
        site["edge_producers"] = edges
        site["producer"] += f"; block {blk_start:08X} entered from " + (
            "; ".join(edges) if edges else "no recorded xref")
    fent = byaddr.get(fn_va)
    site["fn_called_by"] = len(fent.get("called_by", [])) if fent else None
    site["fn_detect"] = fent.get("detection_method") if fent else "recovered entry"
    site["site_is_fn_entry"] = (blk == fn_va)

    parity = PF in need and prod is not None and (
        prod.mnemonic in ("sahf", "fnstsw", "fstsw")
        or (prod.mnemonic in ("test", "and") and "ah" in prod.op_str))
    outside_edges = bool(site.get("edge_producers")) and all(
        int(e[:8], 16) < fn_va for e in site["edge_producers"])

    # Impact first: what a wrong answer here can cost.
    if in_table:
        site["impact"] = f"dead: inside jump table {in_table[0]}"
    elif not site["c_reachable"]:
        site["impact"] = "dead: unreachable in this C function"
    elif site["fn_detect"] == "tail_jump_alias" or site["fn_detect"] == "recovered entry":
        site["impact"] = ("alias copy: owner lifts the same bytes resolved; "
                          "live only if something enters the alias")
    else:
        site["impact"] = "live"

    if mn in LOOPS:
        site["cls"], site["why"] = "d", f"{mn} lifted through the generic jcc path"
        return
    if in_table:
        site["cls"], site["why"] = "e", "data (a switch table) decoded as code"
        return
    if (prod is not None and prod.address < fn_va <= va) or outside_edges:
        site["cls"] = "b"
        site["why"] = (f"producer is before the entry of {site['func']}; the "
                       "lifter does not carry flags into a split entry"
                       + (" (x87 parity idiom)" if parity else ""))
        return
    if parity:
        site["cls"], site["why"] = "c", "parity from the x87 status word"
        return
    if not site["c_reachable"]:
        site["cls"] = "e"
        site["why"] = ("dead in the C: follows an unconditional exit in an "
                       "over-long function and no label reaches it")
        return
    if prod is not None:
        site["cls"] = "a"
        site["why"] = f"in-function setter `{prod.mnemonic}` not resolved"
        return
    site["cls"], site["why"] = "e", why


def render_md(sites):
    cls_name = {
        "a": "(a) producer in function, not understood",
        "b": "(b) producer across a function split",
        "c": "(c) parity from the FPU status word",
        "d": "(d) loop / loope / loopne",
        "e": "(e) other",
        "r": "(r) resolved: _flags really assigned (rep cmps/scas, xadd, cmpxchg)",
    }
    out = []
    cnt = collections.Counter(s["cls"] for s in sites)
    out.append(f"Total `(_flags` sites: **{len(sites)}**, of which "
               f"**{sum(1 for s in sites if s['cls'] != 'r')}** are UNRESOLVED.\n")
    out.append("| class | sites |\n|---|---:|")
    for c in "abcder":
        out.append(f"| {cls_name[c]} | {cnt.get(c, 0)} |")
    out.append("")
    imp = collections.Counter((s["cls"], s.get("impact", "resolved"))
                              for s in sites if s["cls"] != "r")
    out.append("Unresolved sites by class and impact:\n")
    out.append("| class | impact | sites |\n|---|---|---:|")
    for (c, i), n in sorted(imp.items()):
        out.append(f"| {c} | {i} | {n} |")
    out.append("")
    out.append("Per consumer mnemonic and class:\n")
    per = collections.Counter((s["consumer"], s["cls"]) for s in sites)
    mns = sorted({s["consumer"] for s in sites})
    out.append("| consumer | " + " | ".join(c for c in "abcder") + " | total |")
    out.append("|---|" + "---:|" * 7)
    for m in mns:
        row = [per.get((m, c), 0) for c in "abcder"]
        out.append(f"| {m} | " + " | ".join(str(x) for x in row) +
                   f" | {sum(row)} |")
    out.append("")
    out.append("Per producer mnemonic (unresolved sites only):\n")
    pm = collections.Counter((s.get("producer_mn") or "none", s["cls"])
                             for s in sites if s["cls"] != "r")
    out.append("| producer | class | sites |\n|---|---|---:|")
    for (p, c), n in sorted(pm.items(), key=lambda kv: -kv[1]):
        out.append(f"| {p} | {c} | {n} |")
    out.append("")
    rp = collections.Counter((s["producer"], s["consumer"])
                             for s in sites if s["cls"] == "r")
    out.append("Resolved sites by producer and consumer:\n")
    out.append("| producer | consumer | sites |\n|---|---|---:|")
    for (p, c), n in sorted(rp.items(), key=lambda kv: -kv[1]):
        out.append(f"| {p} | {c} | {n} |")
    out.append("")
    rc = collections.Counter(s.get("rep_count") for s in sites
                             if s["cls"] == "r" and s["producer"] == "rep cmps/scas")
    out.append("What loaded ECX before each resolved rep compare (a zero "
               "count leaves EFLAGS alone):\n")
    out.append("| ECX | sites |\n|---|---:|")
    for k, n in sorted(rc.items(), key=lambda kv: -kv[1]):
        out.append(f"| `{str(k).replace('|', chr(92) + '|')}` | {n} |")
    out.append("")
    out.append("## Every unresolved site\n")
    out.append("| class | site | consumer | generated function | producer | "
               "block reached from | function entry kind | impact |")
    out.append("|---|---|---|---|---|---|---|---|")
    for s in sorted((s for s in sites if s["cls"] != "r"),
                    key=lambda s: (s["cls"], s.get("va") or 0)):
        va = f"`{s['va']:08X}`" if s.get("va") else "?"
        jf = ", ".join(s.get("jumped_from", [])[:4]) or "fall-through only"
        if len(s.get("jumped_from", [])) > 4:
            jf += ", ..."
        kind = s.get("fn_detect")
        if s.get("fn_called_by"):
            kind = f"{kind}, {s['fn_called_by']} static callers"
        out.append(f"| {s['cls']} | {va} | {s['consumer']} | {s['func']} | "
                   f"`{s['producer']}` | {jf} | {kind} | {s['impact']} |")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=DEF_GEN)
    ap.add_argument("--disasm", default=DEF_DISASM)
    ap.add_argument("--xbe", default=DEF_XBE)
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    img = Image(a.xbe)
    db = load_db(a.disasm)
    sites = list(parse_gen(a.gen))
    for s in sites:
        classify(img, db, s)
        s.pop("block", None)
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(sites, fh, indent=1, default=str)
    if a.md:
        marker = "<!-- everything below is generated by census.py -->\n"
        head = ""
        if os.path.exists(a.md):
            text = open(a.md).read()
            if marker in text:
                head = text[:text.index(marker)]
        with open(a.md, "w") as fh:
            fh.write(head + marker + "\n" + render_md(sites))
    cnt = collections.Counter(s["cls"] for s in sites)
    print(f"{len(sites)} sites", dict(sorted(cnt.items())))
    if a.verbose:
        for s in sites:
            if s["cls"] == "r":
                continue
            va = f"{s['va']:08X}" if s.get("va") else "????????"
            print(f"{s['cls']} {va} {s['consumer']:7s} {s['func']} "
                  f"prod={s.get('producer')} from={s.get('jumped_from')[:3]} "
                  f"det={s.get('fn_detect')}/{s.get('fn_called_by')} "
                  f"ctx={s.get('context')} | {s['why']}")


if __name__ == "__main__":
    main()
