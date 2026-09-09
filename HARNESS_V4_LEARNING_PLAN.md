# Harness v4 learning plan

## Boundary

Keep v3's deterministic policy engine, restricted Pi tools, JSONL trajectory and
`outcome.json`. Do not add autonomous code changes, embeddings, model training,
or self-modifying prompts. The first v4 increment should be an offline,
reviewable layer over completed runs.

## Smallest useful flow

    run task
      -> retain actions.jsonl + outcome.json + declared artifacts
      -> assign one deterministic outcome class
      -> propose at most one reusable lesson
      -> retrieve a few lessons by exact tags
      -> promote a recurring lesson only through a tested harness patch

### 1. Retain trajectory and outcome

Treat the existing run directory as the immutable record. Add a small
`finalize` command that validates the run and appends one index row containing
the run id, task id/type, outcome class, failed stage, touched files and artifact
paths. Never copy full build or runtime output into the index.

### 2. Classify deterministically

Start with rules over existing metadata, in priority order:

- `INVALID_TASK` or `UNSAFE_RUN_PATH`
- `SCOPE_VIOLATION`
- `BUDGET_EXHAUSTED`
- `BUILD_FAILED`
- `RUNTIME_TIMEOUT`, `RUNTIME_CRASH` or `RUNTIME_FAILED`
- `ARTIFACT_MISSING`
- `BLOCKED_WITHIN_SCOPE`
- `PASS`

This directly covers Qwen's common scope drift, repeated-search budget use,
forgotten state, long-running children and shell cascades. Classification must
not invoke a model.

### 3. Extract one candidate lesson

Run a separate, bounded `extract-lesson` task only after finalization. Its input
is the compact outcome plus the last few denied/failed actions, not the whole
repository or raw logs. It emits one JSON record:

```json
{
  "trigger": "runtime task launches the JSRF software renderer",
  "lesson": "include RECOMP_PB_EXEC=1 in the declared runtime command",
  "evidence_runs": ["metal-m1a-capture-..."],
  "tags": ["runtime", "jsrf", "pb-exec"],
  "confidence": "candidate"
}
```

If evidence is insufficient, emit no lesson. Stop after one record so the
worker cannot continue researching after the task is already decided.

### 4. Retrieve without embeddings

Use exact task type, stage and explicit tags from the task manifest. Return at
most three lessons, ordered by validated recurrence count and recency. Inject
them as advisory text after the authoritative task rules. Exact-tag retrieval
is transparent, testable and sufficient before semantic search is justified.

### 5. Promote only recurring validated lessons

A candidate becomes `validated` only when the same rule explains at least two
independent runs and a deterministic regression fixture reproduces the failure.
Promotion into policy or task templates must be a normal reviewed patch with:

1. a failing fixture,
2. the smallest harness change,
3. all policy and cleanup tests passing,
4. one real Pi run confirming no new scope or termination regression.

The learning layer may propose that patch but must never apply or approve it.

## First implementation slice

Add only three files in v4: `classify.py`, `lessons.jsonl`, and
`tests/test_classify.py`. Have `harness.py finalize` call the classifier and
append the compact index. Defer model-assisted lesson extraction until the
classifier has stable fixtures for all classes above.

## Required regression fixtures

- a read/write outside scope is rejected and classified once;
- repeated searches exhaust the budget and later searches stay denied;
- a fresh policy process reloads counters and terminal state;
- a completed artifact causes the worker prompt to stop;
- a runtime spawning a grandchild times out and leaves neither process alive;
- a failed build prevents runtime, while a successful build retry unlocks it;
- two matching validated lessons are retrieved, while unrelated lessons are not.
