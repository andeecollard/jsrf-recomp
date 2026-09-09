#!/usr/bin/env python3
from __future__ import annotations
import json, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "policy"))
from engine import PolicyEngine

def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

def main():
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        project = td / "project"; project.mkdir()
        run = td / "run"; run.mkdir()
        (project / "allowed").mkdir()
        (project / "evidence").mkdir()
        (project / "outside").mkdir()
        (project / "allowed" / "input.txt").write_text("a\nb\nc\n")

        save(run / "task.json", {
            "allowed_files": ["allowed/"],
            "allowed_write_files": ["evidence/result.txt"],
            "budgets": {
                "search_budget": 2,
                "files_read_budget": 1,
                "read_budget_lines": 10,
                "write_budget": 1,
                "build_budget": 0,
                "runtime_budget": 0,
                "tool_budget": 8
            }
        })
        save(run / "manifest.json", {"project_root": str(project), "forbidden_commands": ["git reset"]})
        save(run / "state.json", {"counters": {}, "terminal_status": None})
        (run / "actions.jsonl").write_text("")

        e = PolicyEngine(run)
        assert e.authorize_search("one", "allowed").allowed
        assert e.authorize_search("two", "allowed").allowed
        assert not e.authorize_search("three", "allowed").allowed
        assert e.authorize_read("allowed/input.txt", 3).allowed
        assert not e.authorize_read("outside/secret.txt", 1).allowed
        assert e.authorize_write("evidence/result.txt").allowed
        assert not e.authorize_write("allowed/input.txt").allowed

        records = [json.loads(x) for x in (run / "actions.jsonl").read_text().splitlines() if x.strip()]
        assert len(records) == 7
        assert sum(1 for x in records if x["allowed"]) == 4
        assert sum(1 for x in records if not x["allowed"]) == 3

    print("PASS: synthetic enforcement policy")

if __name__ == "__main__":
    main()
