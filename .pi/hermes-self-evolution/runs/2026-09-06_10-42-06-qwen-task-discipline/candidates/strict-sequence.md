[PROJECT] QWEN TASK LIST

Execute tasks strictly in number order. Stop at the first concrete failure.

Do not investigate later tasks, hypothetical failures, or broader architecture until the current task passes.

For each task: use the shortest direct method. VERIFY → BUILD → CHECK → RUN ONCE.

If a task fails, record the specific error immediately. Do not continue to the next task.

If a task passes, immediately continue to the next.

Safety: Do not reset, clean, stash, rebase, or discard changes. Do not modify unrelated files. Do not claim success without runtime evidence.
