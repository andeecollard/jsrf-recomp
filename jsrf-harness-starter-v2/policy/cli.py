#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine import PolicyEngine

def emit(d):
    print(json.dumps({"allowed": d.allowed, "message": d.message, "counters": d.counters}, sort_keys=True))
    raise SystemExit(0 if d.allowed else 3)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    sub = ap.add_subparsers(dest="op", required=True)

    s = sub.add_parser("search")
    s.add_argument("--query", required=True)
    s.add_argument("--path", default=".")

    r = sub.add_parser("read")
    r.add_argument("--path", required=True)
    r.add_argument("--lines", type=int, required=True)

    w = sub.add_parser("write")
    w.add_argument("--path", required=True)

    c = sub.add_parser("command")
    c.add_argument("--kind", choices=["build", "runtime", "test"], required=True)
    c.add_argument("--command", required=True)

    b = sub.add_parser("block")
    b.add_argument("--reason", required=True)

    args = ap.parse_args()
    e = PolicyEngine(args.run)
    if args.op == "search": emit(e.authorize_search(args.query, args.path))
    if args.op == "read": emit(e.authorize_read(args.path, args.lines))
    if args.op == "write": emit(e.authorize_write(args.path))
    if args.op == "command": emit(e.authorize_command(args.command, args.kind))
    if args.op == "block": emit(e.block(args.reason))

if __name__ == "__main__":
    main()
