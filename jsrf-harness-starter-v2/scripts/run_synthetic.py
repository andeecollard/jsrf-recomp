#!/usr/bin/env python3
from __future__ import annotations
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "policy"))
from engine import PolicyEngine

def show(label, d):
    print(f"{label:28} {'ALLOWED' if d.allowed else 'DENIED'}  {d.message}")

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_synthetic.py runs/<run-id>")
    e = PolicyEngine(Path(sys.argv[1]).resolve())
    show("search #1", e.authorize_search("alpha", "jsrf-harness-starter-v2/tests/fixtures/src"))
    show("search #2", e.authorize_search("beta", "jsrf-harness-starter-v2/tests/fixtures/src"))
    show("search #3", e.authorize_search("gamma", "jsrf-harness-starter-v2/tests/fixtures/src"))
    show("allowed read", e.authorize_read("jsrf-harness-starter-v2/tests/fixtures/src/allowed.txt", 3))
    show("out-of-scope read", e.authorize_read("src/kernel/nv2a_pb_exec.c", 1))
    show("allowed evidence write", e.authorize_write("jsrf-harness-starter-v2/tests/fixtures/evidence/result.txt"))
    show("unapproved source write", e.authorize_write("jsrf-harness-starter-v2/tests/fixtures/src/allowed.txt"))

if __name__ == "__main__":
    main()
