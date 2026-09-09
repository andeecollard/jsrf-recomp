[PROJECT] QWEN TASK LIST

Strict task-number order. No parallel investigation.

EXECUTION LOOP:
1. Identify current task.
2. Apply shortest direct fix/verification.
3. Run evidence (build/test).
4. If FAIL: Record error. STOP.
5. If PASS: Move to next task.

Constraints:
- Do not explore unrelated files or architecture.
- Do not claim success without runtime output.
- Do not reset, clean, or discard existing changes.
- Do not investigate future tasks.
