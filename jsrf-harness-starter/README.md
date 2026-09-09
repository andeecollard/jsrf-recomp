# JSRF Harness Starter

A deliberately small, mechanical supervisor for running bounded Pi/Qwen tasks on the JSRF static recompilation project.

The core idea is simple:

- The **human/ChatGPT** chooses the milestone.
- The **task YAML** defines scope, budgets, allowed files, build/run commands, and success checks.
- **Pi/Qwen** is a disposable worker for one bounded task.
- The **harness** checks scope and artifacts mechanically.
- Evidence is written to disk so the next worker does not need the previous chat context.

This starter does not yet intercept Pi's individual tool calls. It implements the outer mechanical loop first: task specification, fresh worker prompt generation, transcript capture, diff checks, artifact checks, and validation commands. It is intentionally easy to extend later with stricter tool-call gating.

## Layout

```
jsrf-harness-starter/
├── README.md
├── harness.py
├── project.example.yaml
├── prompts/
│   └── worker.txt
├── tasks/
│   ├── metal-m1a-capture.yaml
│   └── metal-m1b-select.yaml
├── evidence/
├── runs/
└── scripts/
    └── check_scope.py
```

## Requirements

- Python 3.10+
- PyYAML (`python3 -m pip install pyyaml`)
- Git
- Pi installed and available as `pi`
- Your local Qwen model configured in Pi

## Configure

Copy:

```bash
cp project.example.yaml project.yaml
```

Edit `project.yaml` and set `worktree` if necessary.

The supplied path is already set to the current JSRF worktree:

```text
/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/upstream_xboxrecomp_clean
```

## First experiment

The first task is intentionally narrower than the previous failed Metal investigation. It only asks Qwen to add a bounded diagnostic and capture up to ten successful draws.

Generate the worker prompt:

```bash
python3 harness.py prepare tasks/metal-m1a-capture.yaml
```

This creates a run directory under `runs/` containing `prompt.txt`.

### Run with Pi manually

For the first version, manual launch is safest and easiest to inspect:

```bash
pi
```

Then paste `/qwen-task-discipline`, followed by the generated `prompt.txt`.

After Qwen exits, save/paste its transcript to the run directory if desired, then validate:

```bash
python3 harness.py validate runs/<run-id>
```

The validator checks:

- required evidence artifact exists;
- only permitted files changed;
- forbidden destructive Git operations are not part of the task;
- requested validation commands can be run explicitly;
- a machine-readable `result.json` is written.

## Why tasks are split

Do not combine research, instrumentation, candidate selection, architecture design, and Metal implementation in one Qwen context.

Recommended sequence:

1. `metal-m1a-capture` — instrument and capture.
2. `metal-m1b-select` — read only the capture and select one candidate.
3. M2A — identify decoded-state interface.
4. M2B — specify minimum Metal subset.
5. M3A — create minimal Metal infrastructure.
6. M3B — route one selected guest draw.
7. M4 — validate against software output.

Start a fresh Pi/Qwen context for each task.

## Task contract

Each YAML can specify:

- `id`, `type`, `objective`
- `allowed_files`
- `allowed_write_files`
- `required_artifacts`
- `search_budget`, `read_budget_lines`, `tool_budget`, `token_budget`
- `build`
- `runtime`
- `success_checks`
- `forbidden_patterns`
- `worker_instructions`

The budgets are currently injected into the prompt and recorded in the run manifest. A later version can enforce them by wrapping/intercepting Pi tools.

## Result states

Workers should terminate as one of:

- `DONE` — requested bounded task completed.
- `BLOCKED` — task cannot be completed inside its permitted scope.
- `FAILED` — attempted task produced a concrete failure.

A bounded `BLOCKED` result is preferable to repository-wide archaeology.

## Next development step

Once this outer loop is useful, add a Pi extension or MCP-like wrapper that exposes only:

```
READ
SEARCH
PATCH
BUILD
RUN_CAPTURE
TEST
WRITE_EVIDENCE
BLOCK
```

and mechanically refuses calls after budgets are exhausted. That is the point where prompt rules become actual enforcement.
