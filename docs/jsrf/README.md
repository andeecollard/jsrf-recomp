# JSRF bring-up notes

Research notes from the Jet Set Radio Future (US) macOS bring-up. These are
**evidence and context, not instructions** — check the current git state and the
actual request before acting on anything here.

Notes are dated and supersede each other. When two describe the same subsystem,
the newer one wins. A bug an old handover calls unexplained may well have been
fixed since.

## Start here

| file | what it is |
|---|---|
| [STATUS.md](STATUS.md) | What works and what does not, with the measurements behind each claim. |
| newest file in [goals/](goals/) | The current plan: goals (stable G-numbers), what was refuted, the order of work. Sort by the date in the name; each superseded file says so under its title. |
| newest file in [handovers/](handovers/) | State at the end of the last session and its open items. |
| [TOOLS.md](TOOLS.md) | How to reach a scene, catch a glitch and read the game's state, unattended. |
| [ACCURACY_GAPS.md](ACCURACY_GAPS.md) | Known places the recompiled title differs from hardware. |
| [EXPERIMENT_CONTROLS.md](EXPERIMENT_CONTROLS.md) | Positive and negative controls for the instruments. |

Then `../../CLAUDE.md` for orientation and the rules learned the hard way.
Anything older than the newest goals file and handover is background; don't
read the whole pile, the handovers are cumulative by design.

## Layout

| directory | what's in it |
|---|---|
| `goals/` | The plan for a phase: `JSRF_GOALS_<date>_<topic>.md`. G-numbers never change between files; priority lives in each file's "Order" section. |
| `handovers/` | End-of-session state: `HANDOVER_<date>_<time of day>_<topic>.txt` (older ones are prefixed `CLAUDE_` or `CODEX_`, and `X_TO_Y_HANDOVER` for an explicit handoff between agents). |
| `progress/` | Point-in-time findings on one subsystem: `PROGRESS_<date>_<topic>.md` (older: `CLAUDE_PROGRESS_…`, `CODEX_PROGRESS_…`). |
| `plans/` | Longer-lived strategy documents. |
| `artifacts/` | Raw evidence kept for reference (backtraces). |
| `oracle-evidence/` | Material captured from xemu as the reference picture. |
| `attic/` | Retired documents and reverted patches, kept for the record. |

The game-data tools, the cutscene catalogue and the unattended launchers live
in `../../diagnostics/jsrf_first_fault/gametools/`, with their own README.

New notes follow the same patterns and go in the matching directory — never in
the repository root, which is kept close to upstream's so merges stay clean.
