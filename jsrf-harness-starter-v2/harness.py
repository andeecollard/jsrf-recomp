#!/usr/bin/env python3
from __future__ import annotations
import argparse, datetime as dt, json, subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent

def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))

def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")

def git_status(root):
    try:
        out = subprocess.check_output(["git", "status", "--porcelain=v1"], cwd=root, text=True)
    except Exception:
        return []
    rows = []
    for line in out.splitlines():
        if len(line) >= 4:
            rows.append({"status": line[:2], "path": line[3:]})
    return rows

def allowed(path, allow):
    p = path.replace("\\", "/").lstrip("./")
    for a in allow:
        a = a.replace("\\", "/").lstrip("./")
        if p == a or p.startswith(a.rstrip("/") + "/"):
            return True
    return False

def prepare(task_path):
    config = load(HERE / "project.json")
    task = load(task_path)
    budgets = dict(config.get("defaults", {}))
    budgets.update(task.get("budgets", {}))
    task["budgets"] = budgets
    root = (HERE / config.get("project_root", "..")).resolve()

    run_id = f"{task['id']}-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}"
    run = HERE / "runs" / run_id
    run.mkdir(parents=True)

    save(run / "task.json", task)
    save(run / "manifest.json", {
        "run_id": run_id,
        "project_root": str(root),
        "harness_root": str(HERE),
        "model": config.get("model"),
        "forbidden_commands": config.get("forbidden_commands", [])
    })
    save(run / "state.json", {
        "counters": {},
        "terminal_status": None
    })
    save(run / "baseline_git_status.json", git_status(root))
    (run / "actions.jsonl").write_text("", encoding="utf-8")

    worker = (HERE / "prompts" / "worker.txt").read_text(encoding="utf-8")
    (run / "prompt.txt").write_text(worker + "\n\nACTIVE TASK\n\n" + task.get("prompt", "") + "\n", encoding="utf-8")

    print(run)
    print(f'export JSRF_HARNESS_RUN="{run}"')

def validate(run):
    manifest = load(run / "manifest.json")
    task = load(run / "task.json")
    state = load(run / "state.json")
    before = {(x["status"], x["path"]) for x in load(run / "baseline_git_status.json")}
    after = {(x["status"], x["path"]) for x in git_status(Path(manifest["project_root"]))}
    delta = sorted(after - before)
    violations = [{"status": s, "path": p} for s, p in delta if not allowed(p, task.get("allowed_write_files", []))]

    artifacts = []
    for rel in task.get("required_artifacts", []):
        artifacts.append({"path": rel, "exists": (Path(manifest["project_root"]) / rel).exists()})

    result = {
        "scope_ok": not violations,
        "scope_violations": violations,
        "artifact_checks": artifacts,
        "counters": state.get("counters", {}),
        "terminal_status": state.get("terminal_status"),
        "passed": not violations and all(x["exists"] for x in artifacts)
    }
    save(run / "result.json", result)
    print(json.dumps(result, indent=2, sort_keys=True))
    raise SystemExit(0 if result["passed"] else 2)

def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare"); p.add_argument("task")
    v = sub.add_parser("validate"); v.add_argument("run")
    args = ap.parse_args()
    if args.cmd == "prepare": prepare(Path(args.task).resolve())
    else: validate(Path(args.run).resolve())

if __name__ == "__main__":
    main()
