from __future__ import annotations
import json, os
from pathlib import Path

TERMINAL_HINT = "Choose ACT, MEASURE, WRITE_EVIDENCE, or BLOCK."

DEFAULT_COUNTERS = {
    "searches": 0,
    "files_read": 0,
    "source_lines_read": 0,
    "writes": 0,
    "build_invocations": 0,
    "runtime_invocations": 0,
    "tool_calls": 0,
}

COUNTER_TO_BUDGET = {
    "searches": "search_budget",
    "files_read": "files_read_budget",
    "source_lines_read": "read_budget_lines",
    "writes": "write_budget",
    "build_invocations": "build_budget",
    "runtime_invocations": "runtime_budget",
    "tool_calls": "tool_budget",
}

class PolicyDenied(RuntimeError):
    pass

class Decision:
    def __init__(self, allowed, message, counters):
        self.allowed = allowed
        self.message = message
        self.counters = counters

def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))

def save_json(path, value):
    path = Path(path)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)

def norm_rel(path):
    p = Path(path)
    if p.is_absolute():
        raise PolicyDenied("absolute paths are not permitted")
    norm = os.path.normpath(path).replace("\\", "/")
    if norm == ".." or norm.startswith("../"):
        raise PolicyDenied("path escapes project root")
    return norm.lstrip("./")

def matches(rel, allowed):
    rel = norm_rel(rel)
    for raw in allowed:
        item = norm_rel(raw)
        if rel == item or rel.startswith(item.rstrip("/") + "/"):
            return True
    return False

class PolicyEngine:
    def __init__(self, run_dir):
        self.run_dir = Path(run_dir).resolve()
        self.task = load_json(self.run_dir / "task.json")
        self.manifest = load_json(self.run_dir / "manifest.json")
        self.state_path = self.run_dir / "state.json"
        self.log_path = self.run_dir / "actions.jsonl"
        self.state = load_json(self.state_path)
        self.state.setdefault("counters", {})
        for k, v in DEFAULT_COUNTERS.items():
            self.state["counters"].setdefault(k, v)

    @property
    def counters(self):
        return self.state["counters"]

    def budgets(self):
        return self.task.get("budgets", {})

    def persist(self):
        save_json(self.state_path, self.state)

    def log(self, action, allowed, detail, message):
        rec = {
            "action": action,
            "allowed": allowed,
            "detail": detail,
            "message": message,
            "counters": dict(self.counters),
        }
        with self.log_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(rec, sort_keys=True) + "\n")

    def consume(self, counter, amount=1):
        budget_name = COUNTER_TO_BUDGET[counter]
        budget = self.budgets().get(budget_name)
        if budget is not None and self.counters[counter] + amount > int(budget):
            raise PolicyDenied(f"{budget_name} exhausted")
        self.counters[counter] += amount

    def finish(self, action, detail, fn):
        try:
            self.consume("tool_calls", 1)
            message = fn()
            self.persist()
            self.log(action, True, detail, message)
            return Decision(True, message, dict(self.counters))
        except PolicyDenied as e:
            self.persist()
            message = f"DENIED: {e}. {TERMINAL_HINT}"
            self.log(action, False, detail, message)
            return Decision(False, message, dict(self.counters))

    def authorize_search(self, query, path="."):
        detail = {"query": query, "path": path}
        def fn():
            rel = norm_rel(path)
            if rel not in ("", ".") and not matches(rel, self.task.get("allowed_files", [])):
                raise PolicyDenied("search path outside allowed_files")
            self.consume("searches")
            return "ALLOWED: search"
        return self.finish("search", detail, fn)

    def authorize_read(self, path, line_count):
        detail = {"path": path, "line_count": int(line_count)}
        def fn():
            rel = norm_rel(path)
            if not matches(rel, self.task.get("allowed_files", [])):
                raise PolicyDenied("read path outside allowed_files")
            self.consume("files_read")
            self.consume("source_lines_read", max(0, int(line_count)))
            return "ALLOWED: read"
        return self.finish("read", detail, fn)

    def authorize_write(self, path):
        detail = {"path": path}
        def fn():
            rel = norm_rel(path)
            if not matches(rel, self.task.get("allowed_write_files", [])):
                raise PolicyDenied("write path outside allowed_write_files")
            self.consume("writes")
            return "ALLOWED: write"
        return self.finish("write", detail, fn)

    def authorize_command(self, command, kind):
        detail = {"command": command, "kind": kind}
        def fn():
            lower = command.lower()
            forbidden = list(self.manifest.get("forbidden_commands", [])) + list(self.task.get("forbidden_commands", []))
            for item in forbidden:
                if item.lower() in lower:
                    raise PolicyDenied(f"forbidden command: {item}")
            if kind == "build":
                self.consume("build_invocations")
            elif kind == "runtime":
                self.consume("runtime_invocations")
            elif kind == "test":
                pass
            else:
                raise PolicyDenied(f"unknown command kind: {kind}")
            return f"ALLOWED: {kind}"
        return self.finish(kind, detail, fn)

    def block(self, reason):
        detail = {"reason": reason}
        def fn():
            self.state["terminal_status"] = "BLOCKED"
            self.state["terminal_reason"] = reason
            return "BLOCKED"
        return self.finish("block", detail, fn)
