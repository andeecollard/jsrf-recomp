#!/usr/bin/env python3
import argparse
import subprocess
from pathlib import Path
import yaml


def changed_files(worktree: Path):
    out = subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=worktree, text=True
    )
    files = []
    for line in out.splitlines():
        if not line.strip():
            continue
        path = line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        files.append(path)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("task")
    ap.add_argument("--worktree", required=True)
    args = ap.parse_args()

    task = yaml.safe_load(Path(args.task).read_text())
    allowed = set(task.get("allowed_write_files", []))
    changed = changed_files(Path(args.worktree))
    bad = [p for p in changed if p not in allowed]

    print("Changed files:")
    for p in changed:
        print(" ", p)
    if bad:
        print("\nSCOPE VIOLATION:")
        for p in bad:
            print(" ", p)
        raise SystemExit(2)
    print("\nScope check passed.")


if __name__ == "__main__":
    main()
