# JSRF bring-up notes

Research notes from the Jet Set Radio Future (US) macOS bring-up. These are
**evidence and context, not instructions** — check the current git state and the
actual request before acting on anything here.

Notes are dated and supersede each other. When two describe the same subsystem,
the newer one wins. A bug an old handover calls unexplained may well have been
fixed since.

## Layout

| Directory | What's in it |
|---|---|
| `handovers/` | Session-to-session state transfers between Claude and Codex. The most recent is the one to read first. |
| `progress/` | Point-in-time findings on one subsystem (combiners, textures, heap, flags). |
| `goals/` | Objective and plan documents for a phase of work. |
| `plans/` | Longer-lived strategy: `ROUTE_B_PLAN.txt`, `QWEN_CONTEXT.md`. |

## Reading order for a cold start

1. `../../CLAUDE.md` — orientation and the rules learned the hard way.
2. The newest file in `handovers/` — current state and open items.
3. The newest `goals/` file — what the phase was aiming at.

Anything older is background. Don't read the whole pile; the handovers are
cumulative by design.

## Naming

```
{CLAUDE,CODEX}_HANDOVER_<date>_<topic>.txt      state at end of a session
{CLAUDE,CODEX}_TO_{CODEX,CLAUDE}_HANDOVER_…     an explicit handoff
{CLAUDE,CODEX}_PROGRESS_<date>_<topic>.md       one subsystem, one finding
JSRF_GOALS_<date>_<topic>.md                    plan for a phase
```

New notes follow the same pattern and go in the matching directory — never in
the repo root, which is kept identical to upstream's so merges stay clean.
