# Current traversal verification goals

Scope: verify the historical execution/deletion corruption on the current build, using the canonical mapping. Preserve existing work; no renderer or save-data extension.

1. [x] Confirm mapping and distinguish Exec1/deletion from drawing (REPORT.md).
2. [x] Reassess historical AddRef attribution and recorded repair (FOLLOWUP.md).
3. [x] Build the current diagnostic harness and run its regression suite: 21/21 pass.
4. [x] Run a 180-second current-build check with existing tree and startup diagnostics enabled; record termination and progress.
5. [x] If a fault reproduces, establish its actual instruction/dispatch before proposing a fix. Otherwise document the bounded non-reproduction and instrumentation limits.
6. [x] Record a single next action supported by the result.

Run: codex-canonical-verify-01. Binary SHA-256: 6a4f0f5357c06f0cb28a1d1f5c42ff635eb0cff0056db89fabb9964693be744d.

Completed: the runner reported bounded-stop=180.0s, no failure reproduced. See CURRENT_RUN.md for evidence and limits.
