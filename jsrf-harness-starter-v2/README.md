# JSRF Harness Starter v2

A small mechanical supervisor for bounded Pi/Qwen work on the JSRF static recompilation project.

This version separates policy enforcement from Pi integration:

    human / ChatGPT
          |
      task manifest
          |
      policy engine   <- deterministic, tested
          |
      thin Pi adapter
          |
    restricted worker tools
          |
      JSRF repository

## What v2 adds

- dependency-free Python policy engine
- JSON task manifests (no PyYAML required)
- persistent per-run counters
- JSONL audit log for allowed and denied actions
- allowed-read and allowed-write path enforcement
- search, read-line, write, build, runtime and total-tool budgets
- forbidden-command checks
- baseline git status captured at prepare time
- validation against changes made after prepare, not all pre-existing dirty files
- synthetic enforcement test
- thin Pi adapter scaffold
- separate Metal M1A and M1B tasks

## Important boundary

This is not an OS security sandbox. It is designed to stop a cooperative local model from wandering.

For bounded worker sessions, expose only the `jsrf_*` tools. Do not give the worker unrestricted bash/read/edit tools if you want policy enforcement to be authoritative.

## Recommended location

Place this folder inside:

    upstream_xboxrecomp_clean/jsrf-harness-starter-v2/

Then exclude it locally:

    echo "jsrf-harness-starter-v2/" >> .git/info/exclude

## First test

From inside `jsrf-harness-starter-v2/`:

    python3 tests/test_policy.py

Expected:

    PASS: synthetic enforcement policy

## Prepare a synthetic run

    python3 harness.py prepare tasks/synthetic-enforcement.json

The command creates:

    runs/<run-id>/
      task.json
      manifest.json
      state.json
      actions.jsonl
      baseline_git_status.json
      prompt.txt

Then:

    python3 scripts/run_synthetic.py runs/<run-id>
    python3 harness.py validate runs/<run-id>

## Worker task types

- AUDIT: read/search only
- MEASURE: bounded diagnostic + runtime capture
- PATCH: narrow code change + focused validation
- VALIDATE: tests/runtime only
- CLASSIFY: evidence-only decision

A bounded BLOCKED result is a successful outcome. Scope expansion is not.

## Pi integration

`ext/jsrf-worker.ts` is deliberately a thin adapter scaffold. It should:

- load the active run from `JSRF_HARNESS_RUN`
- register:
  - jsrf_read
  - jsrf_search
  - jsrf_patch
  - jsrf_build
  - jsrf_run_capture
  - jsrf_test
  - jsrf_write_evidence
  - jsrf_block
- consult `policy/cli.py` before every action
- execute the real action only after an ALLOWED decision
- call `setActiveTools(...)` so unrestricted tools are not active in bounded worker sessions

Do not test the extension with standalone Node as a proxy for Pi's loader. Wire and test it inside Pi in a separate bounded integration task.

## Recommended sequence

1. Prove the policy engine.
2. Prove the synthetic prepared-task flow.
3. Wire/test the Pi adapter in a fresh Qwen context.
4. Run Metal M1A capture.
5. Start another fresh context for Metal M1B selection.

Do not combine M1A and M1B.
