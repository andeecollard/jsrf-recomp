#!/usr/bin/env python3
"""Put the decompilation project's names onto our addresses.

KeybadeBlox' JSRF decompilation carries a Ghidra symbol table for the same US
XBE we translate -- 1332 named functions with calling conventions and typed
parameters. Its addresses are in our VA space: 1319 of them land exactly on a
function start in our own functions.json, 13 land inside one, and none is
unknown to us. That 99% agreement is what makes this safe to lean on, and
`check` re-measures it rather than trusting this docstring.

The table is NOT vendored here. It belongs to another project, which publishes
no licence and asks that no language-model output be contributed back, so this
reads it in place from a sibling checkout and writes nothing to it.

    ../JSRF-Decompilation/ghidra/symboltable.tsv        (override: --symbols)

Why this exists: a trace that says sub_00011220 called sub_000112D0 is a
puzzle, and the same trace saying CActBase::drawTreeDefault1 called
CActBase::drawManyEvent is an answer. Several of the open questions in
docs/jsrf/ are only hard because the functions were anonymous.

    symbolize.py check                 agreement between the table and our disasm
    symbolize.py map --out names.json  {va: name}, for tools that want the map
    symbolize.py lookup drawTree       find by name substring or by 0xADDRESS
    symbolize.py annotate run.log      rewrite sub_/0x forms into names
    symbolize.py callers 0x00011220    who calls it, and what it calls, by name
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", ".."))
DEFAULT_SYMBOLS = os.path.join(ROOT, "..", "JSRF-Decompilation", "ghidra",
                               "symboltable.tsv")
DEFAULT_FUNCTIONS = os.path.join(ROOT, "build-macos", "jsrf-first-fault",
                                 "disasm", "functions.json")


def load_symbols(path):
    """Parse the Ghidra export into {va: {...}}.

    Columns are positional: address, kind, return type, calling convention,
    inline, fixup, name, comment, then (type, name) pairs for the parameters.
    Rows that are not functions, or are too short to carry a name, are skipped
    rather than guessed at.
    """
    out = {}
    with open(path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 7 or parts[1] != "func":
                continue
            try:
                va = int(parts[0], 16)
            except ValueError:
                continue
            params = []
            rest = parts[8:]
            for i in range(0, len(rest) - 1, 2):
                params.append((rest[i], rest[i + 1]))
            out[va] = {
                "name": parts[6],
                "ret": parts[2],
                "conv": parts[3],
                "comment": parts[7] if len(parts) > 7 else "",
                "params": params,
            }
    return out


def load_functions(path):
    """Our own detected functions, or None when the disasm has not been run."""
    if not os.path.exists(path):
        return None
    return json.load(open(path))


def signature(sym):
    """Render a symbol the way the decompilation declares it."""
    args = ", ".join("%s %s" % (t, n) for t, n in sym["params"])
    conv = "" if sym["conv"] in ("", "unknown") else sym["conv"] + " "
    return "%s %s%s(%s)" % (sym["ret"], conv, sym["name"], args)


def cmd_check(args):
    syms = load_symbols(args.symbols)
    funcs = load_functions(args.functions)
    if funcs is None:
        print("no functions.json at %s -- run the disassembler first"
              % args.functions, file=sys.stderr)
        return 1

    starts = {int(f["start"], 16) for f in funcs}
    spans = sorted((int(f["start"], 16), int(f["end"], 16)) for f in funcs)

    exact, interior, unknown = [], [], []
    for va in sorted(syms):
        if va in starts:
            exact.append(va)
            continue
        hit = next((s for s, e in spans if s < va <= e), None)
        (interior if hit is not None else unknown).append((va, hit))

    total = len(syms)
    print("symbol table : %s" % args.symbols)
    print("our functions: %s  (%d detected)" % (args.functions, len(funcs)))
    print()
    print("  named functions          : %d" % total)
    print("  exact match on our start : %d  (%.1f%%)"
          % (len(exact), 100.0 * len(exact) / total if total else 0.0))
    print("  inside one, not its start: %d" % len(interior))
    print("  unknown to our disasm    : %d" % len(unknown))

    # The interior cases are the interesting ones: each is a place where the
    # decompilation says a function begins and our detector says the bytes are
    # part of the function before it. That is the exact shape of a missed
    # mid-function entry, so name them rather than reporting a count.
    if interior:
        print("\n  boundary disagreements (they say a function starts here;"
              " we say it is interior):")
        for va, owner in interior:
            print("    0x%08X  %-46s inside sub_%08X"
                  % (va, syms[va]["name"], owner))
    if unknown:
        print("\n  named but absent from our disasm:")
        for va, _ in unknown:
            print("    0x%08X  %s" % (va, syms[va]["name"]))
    return 0


def cmd_map(args):
    syms = load_symbols(args.symbols)
    out = {"0x%08X" % va: syms[va]["name"] for va in sorted(syms)}
    if args.out:
        with open(args.out, "w") as f:
            json.dump(out, f, indent=1)
            f.write("\n")
        print("wrote %d names to %s" % (len(out), args.out))
    else:
        json.dump(out, sys.stdout, indent=1)
        sys.stdout.write("\n")
    return 0


def cmd_lookup(args):
    syms = load_symbols(args.symbols)
    hits = []
    for q in args.query:
        try:
            va = int(q, 16)
        except ValueError:
            va = None
        if va is not None and va in syms:
            hits.append((va, syms[va]))
            continue
        low = q.lower()
        hits.extend((va, s) for va, s in sorted(syms.items())
                    if low in s["name"].lower())
    if not hits:
        print("no match", file=sys.stderr)
        return 1
    for va, s in hits:
        print("0x%08X  %s" % (va, signature(s)))
    return 0


# sub_0011ABCD, 0x0011ABCD, and bare 0011ABCD as printed by several of our
# instruments. Ordered longest-first so sub_ wins over the bare hex inside it.
_ADDR = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b|\b0x([0-9A-Fa-f]{8})\b")


def cmd_annotate(args):
    syms = load_symbols(args.symbols)
    names = {va: s["name"] for va, s in syms.items()}

    def repl(m):
        text = m.group(0)
        digits = m.group(1) or m.group(2)
        name = names.get(int(digits, 16))
        if not name:
            return text
        return "%s<%s>" % (text, name) if args.keep else name

    src = open(args.file) if args.file != "-" else sys.stdin
    try:
        for line in src:
            sys.stdout.write(_ADDR.sub(repl, line))
    except BrokenPipeError:
        pass
    finally:
        if src is not sys.stdin:
            src.close()
    return 0


def cmd_callers(args):
    syms = load_symbols(args.symbols)
    funcs = load_functions(args.functions)
    if funcs is None:
        print("no functions.json at %s" % args.functions, file=sys.stderr)
        return 1
    by_start = {int(f["start"], 16): f for f in funcs}

    def label(va):
        n = syms.get(va, {}).get("name")
        return "0x%08X  %s" % (va, n or "sub_%08X" % va)

    for q in args.query:
        try:
            va = int(q, 16)
        except ValueError:
            match = [a for a, s in sorted(syms.items())
                     if q.lower() in s["name"].lower()]
            if not match:
                print("no match for %s" % q, file=sys.stderr)
                continue
            va = match[0]
        f = by_start.get(va)
        print("=== %s" % label(va))
        if va in syms:
            print("    %s" % signature(syms[va]))
        if not f:
            print("    (not a function start in our disasm)")
            continue
        print("  calls_to (%d):" % len(f.get("calls_to", [])))
        for t in f.get("calls_to", []):
            print("    %s" % label(int(t, 16)))
        cb = f.get("called_by", [])
        print("  called_by (%d):" % len(cb))
        for t in cb[:args.limit]:
            print("    %s" % label(int(t, 16)))
        if len(cb) > args.limit:
            print("    ... and %d more" % (len(cb) - args.limit))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="symbolize.py",
        description=__doc__.split("\n\n")[0],
        epilog="The symbol table is read in place from a sibling checkout and "
               "never written to.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbols", default=DEFAULT_SYMBOLS,
                    help="Ghidra symboltable.tsv (default: %(default)s)")
    ap.add_argument("--functions", default=DEFAULT_FUNCTIONS,
                    help="our functions.json (default: %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check", help="agreement between the table and our disasm")
    c.set_defaults(func=cmd_check)

    m = sub.add_parser("map", help="write {va: name} as JSON")
    m.add_argument("--out", default=None, metavar="JSON")
    m.set_defaults(func=cmd_map)

    l = sub.add_parser("lookup", help="find by name substring or 0xADDRESS")
    l.add_argument("query", nargs="+")
    l.set_defaults(func=cmd_lookup)

    a = sub.add_parser("annotate", help="rewrite addresses in a log into names")
    a.add_argument("file", nargs="?", default="-")
    a.add_argument("--keep", action="store_true",
                   help="keep the address and append <name> rather than "
                        "replacing it")
    a.set_defaults(func=cmd_annotate)

    k = sub.add_parser("callers", help="call graph around a function, by name")
    k.add_argument("query", nargs="+")
    k.add_argument("--limit", type=int, default=20, metavar="N",
                   help="cap the called_by list (default %(default)s)")
    k.set_defaults(func=cmd_callers)

    args = ap.parse_args(argv)
    if not os.path.exists(args.symbols):
        print("no symbol table at %s\n"
              "Clone https://codeberg.org/KeybadeBlox/JSRF-Decompilation beside "
              "the repo, or pass --symbols." % args.symbols, file=sys.stderr)
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
