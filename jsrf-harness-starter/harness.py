#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import shutil
import subprocess
from pathlib import Path

try:
    import yaml
except ImportError:
    raise SystemExit("PyYAML is required: python3 -m pip install pyyaml")

ROOT = Path(__file__).resolve().parent


def load_yaml(path: Path):
    return yaml.safe_load(path.read_text())


def load_project():
    p = ROOT / "project.yaml"
    if not p.exists():
        p = ROOT / "project.example.yaml"
    return load_yaml(p)


def render_prompt(task, project):
    worker = (ROOT / "prompts" / "worker.txt").read_text().rstrip()
    b = task.get("budgets", {})
    lines = [
        worker,
        "",
        "TASK CONTRACT",
        f"ID: {task['id']}",
        f"TYPE: {task.get('type', 'WORK')}",
        f"WORKTREE: {project['worktree']}",
        f"OBJECTIVE: {task['objective']}",
        "",
        "ENFORCED/RECORDED BUDGETS",
        f"search_budget: {b.get('search_budget', project['policy'].get('default_search_budget'))}",
        f"read_budget_lines: {b.get('read_budget_lines', project['policy'].get('default_read_budget_lines'))}",
        f"tool_budget: {b.get('tool_budget', project['policy'].get('default_tool_budget'))}",
        f"token_budget: {b.get('token_budget', project['policy'].get('default_token_budget'))}",
        "",
        "ALLOWED FILES",
    ]
    lines += [f"- {x}" for x in task.get("allowed_files", [])]
    lines += ["", "ALLOWED WRITE FILES"]
    lines += [f"- {x}" for x in task.get("allowed_write_files", [])]
    if task.get("build"):
        lines += ["", "BUILD", f"cwd: {task['build']['cwd']}", f"command: {task['build']['command']}"]
    if task.get("runtime"):
        r = task["runtime"]
        lines += ["", "RUNTIME", f"cwd: {r['cwd']}", f"command: {r['command']}", f"timeout_seconds: {r.get('timeout_seconds', 30)}"]
    lines += ["", "TASK-SPECIFIC INSTRUCTIONS", task.get("worker_instructions", "").rstrip()]
    return "\n".join(lines) + "\n"


def make_run_id(task_id):
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    return f"{stamp}-{task_id}"


def prepare(task_path: Path):
    project = load_project()
    task = load_yaml(task_path)
    run_id = make_run_id(task["id"])
    run = ROOT / "runs" / run_id
    run.mkdir(parents=True)
    shutil.copy2(task_path, run / "task.yaml")
    (run / "prompt.txt").write_text(render_prompt(task, project))
    manifest = {
        "run_id": run_id,
        "task": task["id"],
        "created_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "worktree": project["worktree"],
        "status": "PREPARED",
    }
    (run / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(run)
    print(f"Prompt: {run / 'prompt.txt'}")


def git_changed(worktree: Path):
    p = subprocess.run(["git", "status", "--porcelain"], cwd=worktree, text=True, capture_output=True)
    p.check_returncode()
    files = []
    for line in p.stdout.splitlines():
        if not line.strip():
            continue
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        files.append(path)
    return files


def validate(run_path: Path):
    project = load_project()
    task = load_yaml(run_path / "task.yaml")
    worktree = Path(project["worktree"])
    allowed = set(task.get("allowed_write_files", []))
    changed = git_changed(worktree)
    violations = [p for p in changed if p not in allowed]

    checks = []
    for item in task.get("success_checks", []):
        if "artifact_exists" in item:
            rel = item["artifact_exists"]
            ok = (worktree / rel).exists()
            checks.append({"check": f"artifact_exists:{rel}", "ok": ok})
        elif "artifact_contains" in item:
            needle = item["artifact_contains"]
            artifacts = task.get("required_artifacts", [])
            ok = any((worktree / rel).exists() and needle in (worktree / rel).read_text(errors="replace") for rel in artifacts)
            checks.append({"check": f"artifact_contains:{needle}", "ok": ok})

    result = {
        "task": task["id"],
        "changed_files": changed,
        "scope_violations": violations,
        "checks": checks,
        "passed": not violations and all(c["ok"] for c in checks),
    }
    (run_path / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 2)


def main():
    ap = argparse.ArgumentParser(description="JSRF bounded worker harness starter")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("prepare")
    p.add_argument("task")

    v = sub.add_parser("validate")
    v.add_argument("run")

    args = ap.parse_args()
    if args.cmd == "prepare":
        prepare(Path(args.task))
    elif args.cmd == "validate":
        validate(Path(args.run))


if __name__ == "__main__":
    main()
